/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2026 firewalker <firew4lker@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Protocol summary (firmware V6_0, see Firmware/LogicAnalyzer_V2 in the
 * upstream repository):
 *
 * Host -> device: binary frames "55 AA <cmd> <payload> AA 55". Payload
 * bytes AA, 55 and F0 are escaped as F0 followed by (byte ^ F0).
 * Device -> host: ASCII lines terminated by '\n', except for capture data.
 *
 * After "CAPTURE_STARTED" and once the trigger fired and the capture
 * completed, the device sends: u32 sample count (LE), the samples (1, 2
 * or 4 bytes LE each, bit i = i-th requested channel), u8 timestamp
 * count, and (only if that count is > 1) count * u32 timestamps.
 */

#include <config.h>
#include <string.h>
#include "protocol.h"

/* Maximum quiet time while sample data is being transferred. */
#define DATA_STALL_TIMEOUT_US	(5 * 1000 * 1000)
/* Read buffer size for the receive callback. */
#define RX_READ_SIZE		(64 * 1024)

SR_PRIV size_t gla_frame_encode(const uint8_t *payload, size_t len,
	uint8_t *out)
{
	size_t i, n;

	n = 0;
	out[n++] = 0x55;
	out[n++] = 0xaa;
	for (i = 0; i < len; i++) {
		if (payload[i] == 0xaa || payload[i] == 0x55 || payload[i] == 0xf0) {
			out[n++] = 0xf0;
			out[n++] = payload[i] ^ 0xf0;
		} else {
			out[n++] = payload[i];
		}
	}
	out[n++] = 0xaa;
	out[n++] = 0x55;

	return n;
}

/*
 * Resynchronisation preamble sent before every frame.
 *
 * The firmware's frame parser can be left holding a stray 0x55 (a
 * "start" byte), e.g. after a host program was killed while a capture
 * was armed: the next frame's first byte is consumed as the cancel
 * request and its trailing "AA 55" leaves the parser waiting for 0xAA.
 * From then on every "55 AA ..." frame is rejected and the device seems
 * dead. "AA 55 00" brings the parser back to idle from every state:
 *  - idle:            AA ignored, 55 starts, 00 aborts         -> idle
 *  - holding 55:      AA 55 forms an (unknown) frame, 00 ignored -> idle
 *                     (the device answers ERR_UNKNOWN_MSG, which is skipped)
 *  - inside a frame:  AA 55 terminates it, 00 ignored           -> idle
 *  - armed capture:   AA is eaten as the cancel byte, 55 00     -> idle
 */
static const uint8_t resync_preamble[] = { 0xaa, 0x55, 0x00 };

SR_PRIV int gla_send_command(struct sr_serial_dev_inst *serial,
	const uint8_t *payload, size_t len)
{
	uint8_t *frame;
	size_t n;
	int ret;

	frame = g_malloc(sizeof(resync_preamble) + 2 * len + 4);
	memcpy(frame, resync_preamble, sizeof(resync_preamble));
	n = sizeof(resync_preamble);
	n += gla_frame_encode(payload, len, frame + n);
	ret = serial_write_blocking(serial, frame, n, serial_timeout(serial, n));
	g_free(frame);
	if (ret < 0 || (size_t)ret != n) {
		sr_err("Failed to send command 0x%02x.", payload[0]);
		return SR_ERR_IO;
	}
	serial_drain(serial);

	return SR_OK;
}

/*
 * Read one non-empty line (without the terminator). Returns its length,
 * 0 on timeout, or -1 on error.
 *
 * The Pico SDK's USB stdio sends "\r\n" line endings. serial_readline()
 * stops at the first CR or LF, so every CRLF yields an extra empty line,
 * which is skipped here.
 */
SR_PRIV int gla_read_line(struct sr_serial_dev_inst *serial, char *buf,
	size_t buflen, int timeout_ms)
{
	gint64 deadline;
	int len, remaining;

	deadline = g_get_monotonic_time() + timeout_ms * 1000LL;
	do {
		remaining = (deadline - g_get_monotonic_time()) / 1000;
		if (remaining <= 0)
			remaining = 1;
		len = buflen;
		if (serial_readline(serial, &buf, &len, remaining) != SR_OK)
			return -1;
		while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == '\n'))
			buf[--len] = '\0';
	} while (len == 0 && g_get_monotonic_time() < deadline);

	if (len > 0)
		sr_spew("Received line '%s'.", buf);

	return len;
}

static int parse_uint_line(const char *line, const char *prefix,
	uint64_t *value)
{
	size_t plen;
	char *end;
	guint64 v;

	plen = strlen(prefix);
	if (strncmp(line, prefix, plen) != 0)
		return SR_ERR_DATA;
	v = g_ascii_strtoull(line + plen, &end, 10);
	if (end == line + plen || *end != '\0')
		return SR_ERR_DATA;
	*value = v;

	return SR_OK;
}

SR_PRIV int gla_identify(struct sr_serial_dev_inst *serial,
	struct dev_context *devc)
{
	static const uint8_t cmd[] = { GLA_CMD_IDENTIFY };
	char line[128];
	GRegex *re;
	GMatchInfo *mi;
	gboolean found;
	uint64_t freq, blast, buffer, channels;
	char *s;
	int i, len;

	serial_flush(serial);
	if (gla_send_command(serial, cmd, sizeof(cmd)) != SR_OK)
		return SR_ERR_IO;

	re = g_regex_new("^LOGIC_ANALYZER_(.+)_V([0-9]+)_([0-9]+)$", 0, 0, NULL);
	found = FALSE;
	/* Skip a few unrelated lines (e.g. leftovers of an earlier session). */
	for (i = 0; i < 8 && !found; i++) {
		len = gla_read_line(serial, line, sizeof(line),
			GLA_IDENTIFY_TIMEOUT_MS);
		if (len <= 0) {
			if (len < 0 || i > 0)
				break;
			continue;
		}
		if (g_regex_match(re, line, 0, &mi)) {
			g_free(devc->board);
			devc->board = g_match_info_fetch(mi, 1);
			s = g_match_info_fetch(mi, 2);
			devc->fw_major = atoi(s);
			g_free(s);
			s = g_match_info_fetch(mi, 3);
			devc->fw_minor = atoi(s);
			g_free(s);
			found = TRUE;
		}
		g_match_info_free(mi);
	}
	g_regex_unref(re);

	if (!found) {
		sr_dbg("No valid identification response.");
		return SR_ERR_DATA;
	}

	if (gla_read_line(serial, line, sizeof(line), GLA_RESPONSE_TIMEOUT_MS) <= 0 ||
	    parse_uint_line(line, "FREQ:", &freq) != SR_OK)
		goto bad_info;
	if (gla_read_line(serial, line, sizeof(line), GLA_RESPONSE_TIMEOUT_MS) <= 0 ||
	    parse_uint_line(line, "BLASTFREQ:", &blast) != SR_OK)
		goto bad_info;
	if (gla_read_line(serial, line, sizeof(line), GLA_RESPONSE_TIMEOUT_MS) <= 0 ||
	    parse_uint_line(line, "BUFFER:", &buffer) != SR_OK)
		goto bad_info;
	if (gla_read_line(serial, line, sizeof(line), GLA_RESPONSE_TIMEOUT_MS) <= 0 ||
	    parse_uint_line(line, "CHANNELS:", &channels) != SR_OK)
		goto bad_info;

	if (freq == 0 || buffer == 0 || channels == 0 ||
	    channels > GLA_MAX_CHANNELS) {
		sr_err("Implausible device parameters (freq %" PRIu64
			", buffer %" PRIu64 ", channels %" PRIu64 ").",
			freq, buffer, channels);
		return SR_ERR_DATA;
	}

	devc->max_samplerate = freq;
	devc->blast_samplerate = blast;
	devc->buffer_size = buffer;
	devc->num_channels = channels;

	sr_info("Found LogicAnalyzer %s, firmware V%d_%d, %" PRIu64
		" channels, %" PRIu64 " byte buffer, max %" PRIu64
		" Hz (blast %" PRIu64 " Hz).", devc->board, devc->fw_major,
		devc->fw_minor, channels, buffer, freq, blast);

	return SR_OK;

bad_info:
	sr_err("Invalid device information response ('%s').", line);
	return SR_ERR_DATA;
}

SR_PRIV void gla_build_samplerates(struct dev_context *devc)
{
	static const uint64_t steps[] = { 1, 2, 5 };
	GArray *rates;
	uint64_t decade, rate, min_rate;
	unsigned int i;

	/* Same lower limit as the official software (16-bit PIO divider). */
	min_rate = (devc->max_samplerate * 2) / 65535;
	if (min_rate < 1)
		min_rate = 1;

	rates = g_array_new(FALSE, FALSE, sizeof(uint64_t));
	for (decade = 1; decade <= devc->max_samplerate; decade *= 10) {
		for (i = 0; i < ARRAY_SIZE(steps); i++) {
			rate = decade * steps[i];
			if (rate >= min_rate && rate <= devc->max_samplerate)
				g_array_append_val(rates, rate);
		}
	}
	if (rates->len == 0 ||
	    g_array_index(rates, uint64_t, rates->len - 1) != devc->max_samplerate)
		g_array_append_val(rates, devc->max_samplerate);
	/* Blast mode: a single, fixed, higher samplerate. */
	if (devc->blast_samplerate > devc->max_samplerate)
		g_array_append_val(rates, devc->blast_samplerate);

	g_free(devc->samplerates);
	devc->num_samplerates = rates->len;
	devc->samplerates = (uint64_t *)g_array_free(rates, FALSE);
}

SR_PRIV uint64_t gla_max_samples(const struct dev_context *devc,
	enum gla_capture_mode mode)
{
	switch (mode) {
	case GLA_MODE_8CH:
		return devc->buffer_size;
	case GLA_MODE_16CH:
		return devc->buffer_size / 2;
	default:
		return devc->buffer_size / 4;
	}
}

/*
 * The capture mode depends on the highest enabled channel, not on the
 * number of channels: in 8 (16) channel mode only the first 8 (16)
 * inputs are stored by the DMA.
 */
SR_PRIV enum gla_capture_mode gla_mode_for_channels(const struct sr_dev_inst *sdi)
{
	const struct sr_channel *ch;
	const GSList *l;
	int highest;

	highest = 0;
	for (l = sdi->channels; l; l = l->next) {
		ch = l->data;
		if (ch->enabled && ch->index > highest)
			highest = ch->index;
	}
	if (highest < 8)
		return GLA_MODE_8CH;
	if (highest < 16)
		return GLA_MODE_16CH;
	return GLA_MODE_24CH;
}

static gint match_cmp(gconstpointer a, gconstpointer b)
{
	const struct sr_trigger_match *ma = a, *mb = b;

	return ma->channel->index - mb->channel->index;
}

/*
 * Map the sigrok trigger onto one of the hardware trigger types:
 *
 *  - one channel, ONE/RISING or ZERO/FALLING: edge trigger ("simple"
 *    trigger, any channel). Note: the hardware is level sensitive, it
 *    fires as soon as the channel has the requested level.
 *  - several channels, all ONE/ZERO, consecutive, within channels 0-15:
 *    "fast" pattern trigger if base + count <= 5, else "complex" pattern
 *    trigger. Both need GPIO0 and GPIO1 to be connected.
 *  - no trigger: the firmware always waits for a trigger, so the capture
 *    is triggered on "EXT input low" (trigger channel 24, GPIO1). That pin
 *    idles low (pull-down, connected to the idle trigger output GPIO0), and
 *    the level-sensitive trigger therefore fires immediately.
 */
static void set_immediate_trigger(struct dev_context *devc)
{
	devc->trigger.type = GLA_TRIGGER_EDGE;
	devc->trigger.immediate = TRUE;
	devc->trigger.channel = GLA_EXT_TRIGGER_CHANNEL;
	devc->trigger.inverted = 1;
	sr_dbg("No trigger configured, starting immediately (EXT input low).");
}

static int convert_trigger(const struct sr_dev_inst *sdi, gboolean blast)
{
	struct dev_context *devc;
	struct sr_trigger *trigger;
	struct sr_trigger_stage *stage;
	struct sr_trigger_match *match;
	GSList *matches, *l;
	unsigned int count, base, i;
	uint16_t value;
	int ret;

	devc = sdi->priv;
	memset(&devc->trigger, 0, sizeof(devc->trigger));

	trigger = sr_session_trigger_get(sdi->session);
	if (!trigger || !trigger->stages) {
		set_immediate_trigger(devc);
		return SR_OK;
	}
	if (g_slist_length(trigger->stages) > 1) {
		sr_err("This device supports only a single trigger stage.");
		return SR_ERR_ARG;
	}
	stage = trigger->stages->data;

	matches = NULL;
	for (l = stage->matches; l; l = l->next) {
		match = l->data;
		if (!match->match)
			continue;
		matches = g_slist_insert_sorted(matches, match, match_cmp);
	}
	count = g_slist_length(matches);
	ret = SR_OK;

	if (count == 0) {
		/* A trigger without matches (e.g. all cleared in PulseView). */
		set_immediate_trigger(devc);
	} else if (count == 1) {
		match = matches->data;
		if (match->channel->index >= (int)devc->num_channels) {
			sr_err("Invalid trigger channel %d.", match->channel->index);
			ret = SR_ERR_ARG;
		} else if (match->match == SR_TRIGGER_ONE ||
				match->match == SR_TRIGGER_RISING) {
			devc->trigger.type = GLA_TRIGGER_EDGE;
			devc->trigger.channel = match->channel->index;
			devc->trigger.inverted = 0;
		} else if (match->match == SR_TRIGGER_ZERO ||
				match->match == SR_TRIGGER_FALLING) {
			devc->trigger.type = GLA_TRIGGER_EDGE;
			devc->trigger.channel = match->channel->index;
			devc->trigger.inverted = 1;
		} else {
			sr_err("Unsupported trigger match type on %s.",
				match->channel->name);
			ret = SR_ERR_ARG;
		}
	} else if (blast) {
		sr_err("Blast mode (%" PRIu64 " Hz) supports only a single "
			"channel trigger.", devc->blast_samplerate);
		ret = SR_ERR_ARG;
	} else {
		base = ((struct sr_trigger_match *)matches->data)->channel->index;
		value = 0;
		for (l = matches, i = 0; l; l = l->next, i++) {
			match = l->data;
			if ((unsigned int)match->channel->index != base + i) {
				sr_err("Pattern triggers must use consecutive "
					"channels.");
				ret = SR_ERR_ARG;
				break;
			}
			if (match->match == SR_TRIGGER_ONE) {
				value |= 1 << i;
			} else if (match->match != SR_TRIGGER_ZERO) {
				sr_err("Pattern triggers (more than one channel) "
					"support only 0/1 levels, no edges.");
				ret = SR_ERR_ARG;
				break;
			}
		}
		if (ret == SR_OK && base + count > GLA_COMPLEX_MAX_BITS) {
			sr_err("Pattern triggers must be within D0-D15.");
			ret = SR_ERR_ARG;
		}
		if (ret == SR_OK) {
			devc->trigger.type = (base + count <= GLA_FAST_MAX_BITS) ?
				GLA_TRIGGER_FAST : GLA_TRIGGER_COMPLEX;
			devc->trigger.channel = base;
			devc->trigger.count = count;
			devc->trigger.value = value;
		}
	}
	g_slist_free(matches);

	if (ret == SR_OK && !devc->trigger.immediate) {
		if (devc->trigger.type == GLA_TRIGGER_EDGE) {
			sr_dbg("Edge trigger on channel %d, %s.",
				devc->trigger.channel,
				devc->trigger.inverted ? "low" : "high");
		} else {
			sr_dbg("%s pattern trigger: channels %d-%d, value 0x%04x.",
				devc->trigger.type == GLA_TRIGGER_FAST ?
				"Fast" : "Complex", devc->trigger.channel,
				devc->trigger.channel + devc->trigger.count - 1,
				devc->trigger.value);
			sr_info("Pattern triggers need GPIO0 and GPIO1 to be "
				"connected on the device.");
		}
	}

	return ret;
}

SR_PRIV int gla_prepare_acquisition(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct sr_channel *ch;
	GSList *l;
	gboolean blast;
	uint64_t max_samples, limit, pre, post, max_pre;
	double sample_period, delay, delay_period;
	unsigned int n;
	int offset, ret;

	devc = sdi->priv;

	/* Channels: wire bit i is the i-th enabled channel (ascending). */
	n = 0;
	for (l = sdi->channels; l; l = l->next) {
		ch = l->data;
		if (!ch->enabled || ch->index >= GLA_MAX_CHANNELS)
			continue;
		devc->chan_map[n++] = ch->index;
	}
	if (n == 0) {
		sr_err("No channels enabled.");
		return SR_ERR_ARG;
	}
	devc->num_capture_channels = n;
	devc->mode = gla_mode_for_channels(sdi);
	devc->bytes_per_sample = (devc->mode == GLA_MODE_8CH) ? 1 :
		(devc->mode == GLA_MODE_16CH) ? 2 : 4;
	devc->unitsize = (devc->num_channels + 7) / 8;

	max_samples = gla_max_samples(devc, devc->mode);
	limit = devc->limit_samples;
	if (limit > max_samples) {
		sr_warn("Requested %" PRIu64 " samples, but only %" PRIu64
			" fit in the device buffer with channels up to D%d; "
			"capturing %" PRIu64 " samples.", limit, max_samples,
			devc->chan_map[n - 1], max_samples);
		limit = max_samples;
	}
	if (limit < GLA_MIN_PRE_SAMPLES + GLA_MIN_POST_SAMPLES) {
		sr_err("Sample limit too small (minimum %d).",
			GLA_MIN_PRE_SAMPLES + GLA_MIN_POST_SAMPLES);
		return SR_ERR_ARG;
	}

	blast = devc->cur_samplerate > devc->max_samplerate;
	if ((ret = convert_trigger(sdi, blast)) != SR_OK)
		return ret;

	devc->blast_fixup = FALSE;
	devc->skip_samples = 0;
	if (blast) {
		/*
		 * Blast mode has no pre-trigger samples. Firmware 6.0 sends
		 * the buffer shifted by one (first sample lost, last sample
		 * bogus): request one more sample and drop the last one.
		 */
		devc->trigger.type = GLA_TRIGGER_BLAST;
		if (devc->capture_ratio > 0 && !devc->trigger.immediate)
			sr_info("Blast mode has no pre-trigger samples, "
				"ignoring the capture ratio.");
		devc->req_pre = 0;
		devc->blast_fixup = TRUE;
		if (limit + 1 <= max_samples) {
			devc->req_post = limit + 1;
			devc->num_samples = limit;
		} else {
			devc->req_post = limit;
			devc->num_samples = limit - 1;
		}
		devc->trigger_at = devc->trigger.immediate ? G_MAXUINT64 : 0;
		return SR_OK;
	}

	if (devc->trigger.immediate) {
		/*
		 * The trigger fires at once, so the pre-trigger part of the
		 * buffer is never filled (the firmware zeroes it): request the
		 * minimum and discard it. No SR_DF_TRIGGER is sent.
		 */
		pre = GLA_MIN_PRE_SAMPLES;
		devc->req_pre = pre;
		devc->req_post = MIN(limit, max_samples - pre);
		devc->skip_samples = pre;
		devc->num_samples = devc->req_post;
		devc->trigger_at = G_MAXUINT64;
		return SR_OK;
	}

	pre = limit * devc->capture_ratio / 100;
	max_pre = max_samples / GLA_MAX_PRE_DIVISOR;
	if (pre > max_pre) {
		sr_info("Pre-trigger samples limited to %" PRIu64
			" (10%% of the buffer).", max_pre);
		pre = max_pre;
	}
	if (pre < GLA_MIN_PRE_SAMPLES)
		pre = GLA_MIN_PRE_SAMPLES;
	post = limit - pre;
	if (post < GLA_MIN_POST_SAMPLES) {
		sr_err("Not enough post-trigger samples.");
		return SR_ERR_ARG;
	}

	devc->trigger_at = pre;
	devc->num_samples = limit;
	devc->req_pre = pre;
	devc->req_post = post;

	if (devc->trigger.type == GLA_TRIGGER_FAST ||
	    devc->trigger.type == GLA_TRIGGER_COMPLEX) {
		/*
		 * Compensate the trigger latency like the official software
		 * does: move the trigger point by the delay of the trigger
		 * state machine.
		 */
		sample_period = 1e9 / devc->cur_samplerate;
		delay = (devc->trigger.type == GLA_TRIGGER_FAST) ?
			GLA_FAST_TRIGGER_DELAY : GLA_COMPLEX_TRIGGER_DELAY;
		delay_period = (1.0 / devc->max_samplerate) * 1e9 * delay;
		offset = (int)(delay_period / sample_period + 0.3 + 0.5);
		if (post < (uint64_t)offset + GLA_MIN_POST_SAMPLES) {
			sr_err("Not enough post-trigger samples.");
			return SR_ERR_ARG;
		}
		devc->req_pre = pre + offset;
		devc->req_post = post - offset;
	}

	return SR_OK;
}

static void write_le16(uint8_t *p, uint16_t v)
{
	p[0] = v & 0xff;
	p[1] = v >> 8;
}

static void write_le32(uint8_t *p, uint32_t v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

/*
 * CAPTURE_REQUEST as laid out by the firmware (natural alignment):
 *  0 triggerType, 1 trigger, 2 inverted/count, 4 triggerValue (u16),
 *  6 channels[24], 30 channelCount, 32 frequency, 36 preSamples,
 *  40 postSamples, 44 loopCount, 45 measure, 46 captureMode.
 */
SR_PRIV void gla_build_request(const struct dev_context *devc,
	uint8_t req[GLA_REQUEST_SIZE])
{
	unsigned int i;

	memset(req, 0, GLA_REQUEST_SIZE);
	req[0] = devc->trigger.type;
	req[1] = devc->trigger.channel;
	if (devc->trigger.type == GLA_TRIGGER_FAST ||
	    devc->trigger.type == GLA_TRIGGER_COMPLEX) {
		req[2] = devc->trigger.count;
		write_le16(&req[4], devc->trigger.value);
	} else {
		req[2] = devc->trigger.inverted;
	}
	for (i = 0; i < devc->num_capture_channels; i++)
		req[6 + i] = devc->chan_map[i];
	req[30] = devc->num_capture_channels;
	write_le32(&req[32], devc->cur_samplerate);
	write_le32(&req[36], devc->req_pre);
	write_le32(&req[40], devc->req_post);
	req[44] = 0;	/* loopCount: burst mode not supported. */
	req[45] = 0;	/* measure: burst timestamps not supported. */
	req[46] = devc->mode;
}

/*
 * serial_readline() stopped at the CR of "CAPTURE_STARTED\r\n"; the LF is
 * still pending and must not be taken as the first byte of the sample
 * count. The firmware waits 100 ms before it sends the count, so a byte
 * arriving within 50 ms belongs to the line ending. Should it be anything
 * else (a firmware sending bare "\n"), keep it as the first header byte.
 */
static void consume_line_end(struct sr_serial_dev_inst *serial,
	struct dev_context *devc)
{
	uint8_t b;

	if (serial_read_blocking(serial, &b, 1, 50) != 1)
		return;
	if (b != '\n') {
		devc->hdr[0] = b;
		devc->hdr_len = 1;
	}
}

SR_PRIV int gla_start_capture(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct sr_serial_dev_inst *serial;
	uint8_t payload[1 + GLA_REQUEST_SIZE];
	char line[64];
	gint64 deadline;
	int len, remaining;

	devc = sdi->priv;
	serial = sdi->conn;

	payload[0] = GLA_CMD_CAPTURE;
	gla_build_request(devc, &payload[1]);

	sr_dbg("Capture: type %d, trigger ch %d, %u channels, mode %d, "
		"%" PRIu64 " Hz, pre %u, post %u.", devc->trigger.type,
		devc->trigger.channel, devc->num_capture_channels, devc->mode,
		devc->cur_samplerate, devc->req_pre, devc->req_post);

	serial_flush(serial);
	if (gla_send_command(serial, payload, sizeof(payload)) != SR_OK)
		return SR_ERR_IO;

	deadline = g_get_monotonic_time() + GLA_RESPONSE_TIMEOUT_MS * 1000LL;
	while ((remaining = (deadline - g_get_monotonic_time()) / 1000) > 0) {
		len = gla_read_line(serial, line, sizeof(line), remaining);
		if (len < 0)
			return SR_ERR_IO;
		if (len == 0)
			continue;
		if (!strcmp(line, "CAPTURE_STARTED")) {
			consume_line_end(serial, devc);
			return SR_OK;
		}
		if (!strcmp(line, "CAPTURE_ERROR")) {
			sr_err("The device rejected the capture parameters.");
			return SR_ERR_ARG;
		}
		sr_dbg("Ignoring unexpected line '%s'.", line);
	}
	sr_err("Timeout waiting for the capture to start.");

	return SR_ERR_TIMEOUT;
}

static void send_trigger(const struct sr_dev_inst *sdi)
{
	struct sr_datafeed_packet packet;

	/* std_session_send_df_trigger() does not exist in 0.5.x. */
	packet.type = SR_DF_TRIGGER;
	packet.payload = NULL;
	sr_session_send(sdi, &packet);
}

static void flush_logic(const struct sr_dev_inst *sdi, unsigned int count)
{
	struct dev_context *devc;
	struct sr_datafeed_packet packet;
	struct sr_datafeed_logic logic;

	devc = sdi->priv;
	if (count == 0)
		return;
	logic.length = count * devc->unitsize;
	logic.unitsize = devc->unitsize;
	logic.data = devc->out_buf;
	packet.type = SR_DF_LOGIC;
	packet.payload = &logic;
	sr_session_send(sdi, &packet);
}

/* Convert and forward a run of complete wire samples. */
static void process_samples(const struct sr_dev_inst *sdi,
	const uint8_t *data, unsigned int nsamples)
{
	struct dev_context *devc;
	unsigned int i, b, bps, pending;
	uint32_t wire, out;
	uint8_t *p;

	devc = sdi->priv;
	bps = devc->bytes_per_sample;
	pending = 0;

	for (i = 0; i < nsamples; i++, data += bps) {
		if (devc->skip_left > 0) {
			devc->skip_left--;	/* Unfilled pre-trigger samples. */
			continue;
		}
		if (devc->samples_sent >= devc->num_samples)
			continue;	/* Excess (e.g. bogus blast sample). */

		if (!devc->trigger_sent && devc->samples_sent == devc->trigger_at) {
			flush_logic(sdi, pending);
			pending = 0;
			send_trigger(sdi);
			devc->trigger_sent = TRUE;
		}

		wire = data[0];
		if (bps > 1)
			wire |= (uint32_t)data[1] << 8;
		if (bps > 2)
			wire |= ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);

		out = 0;
		for (b = 0; b < devc->num_capture_channels; b++)
			if (wire & (1u << b))
				out |= 1u << devc->chan_map[b];

		p = devc->out_buf + pending * devc->unitsize;
		for (b = 0; b < devc->unitsize; b++)
			p[b] = (out >> (8 * b)) & 0xff;

		devc->samples_sent++;
		if (++pending == GLA_CHUNK_SAMPLES) {
			flush_logic(sdi, pending);
			pending = 0;
		}
	}
	flush_logic(sdi, pending);
}

static void handle_bytes(const struct sr_dev_inst *sdi, const uint8_t *data,
	size_t len)
{
	struct dev_context *devc;
	unsigned int bps, take, n;
	uint32_t left;

	devc = sdi->priv;
	bps = devc->bytes_per_sample;

	while (len > 0 && devc->rx_state != GLA_RX_DONE) {
		switch (devc->rx_state) {
		case GLA_RX_LENGTH:
			devc->hdr[devc->hdr_len++] = *data++;
			len--;
			if (devc->hdr_len < 4)
				break;
			devc->wire_samples = devc->hdr[0] | (devc->hdr[1] << 8) |
				(devc->hdr[2] << 16) | ((uint32_t)devc->hdr[3] << 24);
			devc->wire_received = 0;
			sr_dbg("Device sends %u samples.", devc->wire_samples);
			if (devc->wire_samples != devc->req_pre + devc->req_post)
				sr_warn("Device sends %u samples, expected %u.",
					devc->wire_samples,
					devc->req_pre + devc->req_post);
			devc->rx_state = devc->wire_samples ?
				GLA_RX_SAMPLES : GLA_RX_STAMP_COUNT;
			break;
		case GLA_RX_SAMPLES:
			/* Complete a partially received sample first. */
			if (devc->rx_len > 0) {
				take = MIN(bps - devc->rx_len, len);
				memcpy(devc->rx_buf + devc->rx_len, data, take);
				devc->rx_len += take;
				data += take;
				len -= take;
				if (devc->rx_len < bps)
					break;
				process_samples(sdi, devc->rx_buf, 1);
				devc->rx_len = 0;
				devc->wire_received++;
			}
			left = devc->wire_samples - devc->wire_received;
			n = MIN(len / bps, left);
			if (n > 0) {
				process_samples(sdi, data, n);
				devc->wire_received += n;
				data += (size_t)n * bps;
				len -= (size_t)n * bps;
			}
			if (devc->wire_received == devc->wire_samples) {
				devc->rx_state = GLA_RX_STAMP_COUNT;
			} else if (len > 0 && len < bps) {
				memcpy(devc->rx_buf, data, len);
				devc->rx_len = len;
				len = 0;
			}
			break;
		case GLA_RX_STAMP_COUNT:
			n = *data++;
			len--;
			if (n > 1) {
				/* Burst timestamps; not used, skip them. */
				devc->stamps_left = n * 4;
				devc->rx_state = GLA_RX_STAMPS;
			} else {
				devc->rx_state = GLA_RX_DONE;
			}
			break;
		case GLA_RX_STAMPS:
			take = MIN(devc->stamps_left, len);
			devc->stamps_left -= take;
			data += take;
			len -= take;
			if (devc->stamps_left == 0)
				devc->rx_state = GLA_RX_DONE;
			break;
		default:
			len = 0;
			break;
		}
	}
	if (len > 0)
		sr_dbg("Ignoring %zu trailing bytes.", len);
}

SR_PRIV int gla_receive_data(int fd, int revents, void *cb_data)
{
	struct sr_dev_inst *sdi;
	struct dev_context *devc;
	struct sr_serial_dev_inst *serial;
	uint8_t *buf;
	int len;
	gint64 now;

	(void)fd;

	if (!(sdi = cb_data) || !(devc = sdi->priv))
		return TRUE;
	serial = sdi->conn;
	now = g_get_monotonic_time();

	if (revents == G_IO_IN) {
		buf = g_malloc(RX_READ_SIZE);
		while ((len = serial_read_nonblocking(serial, buf, RX_READ_SIZE)) > 0) {
			devc->last_rx = now;
			handle_bytes(sdi, buf, len);
			if (devc->rx_state == GLA_RX_DONE)
				break;
		}
		g_free(buf);
		if (len < 0) {
			sr_err("Serial port read error.");
			sr_dev_acquisition_stop(sdi);
			return TRUE;
		}
	}

	if (devc->rx_state == GLA_RX_DONE) {
		if (!devc->trigger_sent && devc->samples_sent > 0)
			sr_dbg("Trigger point beyond the received samples.");
		if (devc->samples_sent < devc->num_samples)
			sr_warn("Received %" PRIu64 " of %" PRIu64 " samples.",
				devc->samples_sent, devc->num_samples);
		sr_dev_acquisition_stop(sdi);
		return TRUE;
	}

	/*
	 * While waiting for the trigger (no data yet) the device may stay
	 * silent forever. Once data started, a long pause is an error.
	 */
	if ((devc->rx_state != GLA_RX_LENGTH || devc->hdr_len > 0) &&
	    now - devc->last_rx > DATA_STALL_TIMEOUT_US) {
		sr_err("Device stopped sending data.");
		sr_dev_acquisition_stop(sdi);
	}

	return TRUE;
}

/* Read and discard input until the line has been quiet for quiet_ms. */
static void drain_input(struct sr_serial_dev_inst *serial, int quiet_ms,
	int max_ms)
{
	uint8_t buf[4096];
	gint64 start, last;
	int len;

	start = last = g_get_monotonic_time();
	while (g_get_monotonic_time() - start < max_ms * 1000LL) {
		len = serial_read_nonblocking(serial, buf, sizeof(buf));
		if (len > 0) {
			last = g_get_monotonic_time();
			continue;
		}
		if (len < 0 || g_get_monotonic_time() - last > quiet_ms * 1000LL)
			break;
		g_usleep(10 * 1000);
	}
	serial_flush(serial);
}

SR_PRIV void gla_abort_acquisition(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct sr_serial_dev_inst *serial;
	uint8_t cancel;

	devc = sdi->priv;
	serial = sdi->conn;

	/* Also called after a failed start; nothing to undo then. */
	if (devc->rx_state == GLA_RX_IDLE)
		return;

	serial_source_remove(sdi->session, serial);

	if (devc->rx_state == GLA_RX_LENGTH && devc->hdr_len == 0) {
		/* Still waiting for the trigger: ask the device to cancel. */
		sr_info("Cancelling the capture (this takes up to 2 s).");
		cancel = GLA_CANCEL_BYTE;
		serial_write_blocking(serial, &cancel, 1, serial_timeout(serial, 1));
		serial_drain(serial);
		g_usleep(GLA_CANCEL_WAIT_MS * 1000);
		/* The trigger may have fired meanwhile: discard that data. */
		drain_input(serial, 300, 10000);
	} else if (devc->rx_state != GLA_RX_DONE &&
			devc->rx_state != GLA_RX_IDLE) {
		/* Transfer in progress, it cannot be stopped: discard it. */
		drain_input(serial, 300, 10000);
	}

	if (devc->rx_state != GLA_RX_IDLE)
		std_session_send_df_end(sdi);

	devc->rx_state = GLA_RX_IDLE;
	g_free(devc->rx_buf);
	devc->rx_buf = NULL;
	g_free(devc->out_buf);
	devc->out_buf = NULL;
}
