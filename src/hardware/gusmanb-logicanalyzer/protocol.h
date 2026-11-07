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

#ifndef LIBSIGROK_HARDWARE_GUSMANB_LOGICANALYZER_PROTOCOL_H
#define LIBSIGROK_HARDWARE_GUSMANB_LOGICANALYZER_PROTOCOL_H

#include <stdint.h>
#include <glib.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"

#define LOG_PREFIX "gusmanb-la"

/*
 * gusmanb LogicAnalyzer (https://github.com/gusmanb/logicanalyzer),
 * RP2040/RP2350 based, firmware V6_0 and later.
 */

/* USB ID of the firmware's CDC device (pid.codes). */
#define GLA_USB_VID		0x1209
#define GLA_USB_PID		0x3020

#define GLA_MIN_MAJOR		6
#define GLA_MIN_MINOR		0

#define GLA_MAX_CHANNELS	24
/* Trigger channel index of the external trigger input (chained mode). */
#define GLA_EXT_TRIGGER_CHANNEL	24

/* Commands (first byte inside a frame). */
#define GLA_CMD_IDENTIFY	0x00
#define GLA_CMD_CAPTURE		0x01
#define GLA_CMD_BLINK_ON	0x05
#define GLA_CMD_BLINK_OFF	0x06
/* Any byte sent while a capture is pending cancels it. */
#define GLA_CANCEL_BYTE		0xff

/* Trigger types understood by the firmware. */
enum gla_trigger_type {
	GLA_TRIGGER_EDGE = 0,
	GLA_TRIGGER_COMPLEX = 1,
	GLA_TRIGGER_FAST = 2,
	GLA_TRIGGER_BLAST = 3,
};

/* Capture modes: 8, 16 or 24 channels (1, 2 or 4 bytes per sample). */
enum gla_capture_mode {
	GLA_MODE_8CH = 0,
	GLA_MODE_16CH = 1,
	GLA_MODE_24CH = 2,
};

/* Size of the packed CAPTURE_REQUEST structure of the firmware. */
#define GLA_REQUEST_SIZE	48

/* Limits mirrored from the official software (SharedDriver). */
#define GLA_MIN_PRE_SAMPLES	2
#define GLA_MIN_POST_SAMPLES	2
#define GLA_MAX_PRE_DIVISOR	10	/* pre <= max_samples / 10 */
#define GLA_FAST_MAX_BITS	5	/* fast trigger: base + count <= 5 */
#define GLA_COMPLEX_MAX_BITS	16	/* complex trigger: channels 0..15 */
/* Trigger latencies in device clock cycles at the max samplerate. */
#define GLA_FAST_TRIGGER_DELAY		3.0
#define GLA_COMPLEX_TRIGGER_DELAY	5.0

/* Samples per SR_DF_LOGIC packet. */
#define GLA_CHUNK_SAMPLES	(16 * 1024)

/* Timeouts. */
#define GLA_IDENTIFY_TIMEOUT_MS	3000
#define GLA_RESPONSE_TIMEOUT_MS	10000
/* The firmware polls for a cancel request about every 2 s. */
#define GLA_CANCEL_WAIT_MS	2500

enum gla_rx_state {
	GLA_RX_IDLE,
	GLA_RX_LENGTH,		/* Waiting for the u32 sample count. */
	GLA_RX_SAMPLES,		/* Receiving sample data. */
	GLA_RX_STAMP_COUNT,	/* Waiting for the u8 timestamp count. */
	GLA_RX_STAMPS,		/* Skipping burst timestamps (unused). */
	GLA_RX_DONE,
};

struct gla_trigger_config {
	enum gla_trigger_type type;
	gboolean immediate;	/* No sigrok trigger: start right away. */
	uint8_t channel;	/* Edge: channel. Pattern: base channel. */
	uint8_t inverted;	/* Edge only: trigger on low/falling. */
	uint8_t count;		/* Pattern only: number of bits. */
	uint16_t value;		/* Pattern only: bit 0 = base channel. */
};

struct dev_context {
	/* Device information (from the identify response). */
	char *board;
	int fw_major, fw_minor;
	uint64_t max_samplerate;
	uint64_t blast_samplerate;
	uint32_t buffer_size;
	unsigned int num_channels;
	uint64_t *samplerates;
	unsigned int num_samplerates;

	/* Settings. */
	uint64_t cur_samplerate;
	uint64_t limit_samples;
	uint64_t capture_ratio;

	/* Acquisition parameters. */
	struct gla_trigger_config trigger;
	enum gla_capture_mode mode;
	unsigned int bytes_per_sample;	/* On the wire: 1, 2 or 4. */
	unsigned int unitsize;		/* In SR_DF_LOGIC packets. */
	uint8_t chan_map[GLA_MAX_CHANNELS]; /* Wire bit i -> channel index. */
	unsigned int num_capture_channels;
	uint32_t req_pre, req_post;	/* As sent to the device. */
	uint64_t trigger_at;		/* Sample index of the trigger. */
	uint64_t num_samples;		/* Samples to send to the session. */
	uint32_t skip_samples;		/* Leading wire samples to discard. */
	gboolean blast_fixup;		/* Drop the bogus last blast sample. */

	/* Receive state. */
	enum gla_rx_state rx_state;
	uint8_t hdr[4];
	unsigned int hdr_len;
	uint32_t wire_samples;		/* Sample count announced by device. */
	uint32_t wire_received;		/* Samples received so far. */
	uint32_t skip_left;		/* Leading samples still to discard. */
	uint64_t samples_sent;		/* Samples sent to the session. */
	gboolean trigger_sent;
	uint32_t stamps_left;		/* Timestamp bytes left to skip. */
	gint64 last_rx;			/* Monotonic time of the last data. */
	uint8_t *rx_buf;		/* Partial sample bytes. */
	unsigned int rx_len;
	uint8_t *out_buf;		/* Converted samples for SR_DF_LOGIC. */
};

SR_PRIV size_t gla_frame_encode(const uint8_t *payload, size_t len,
	uint8_t *out);
SR_PRIV int gla_send_command(struct sr_serial_dev_inst *serial,
	const uint8_t *payload, size_t len);
SR_PRIV int gla_read_line(struct sr_serial_dev_inst *serial, char *buf,
	size_t buflen, int timeout_ms);
SR_PRIV int gla_identify(struct sr_serial_dev_inst *serial,
	struct dev_context *devc);
SR_PRIV void gla_build_samplerates(struct dev_context *devc);
SR_PRIV uint64_t gla_max_samples(const struct dev_context *devc,
	enum gla_capture_mode mode);
SR_PRIV enum gla_capture_mode gla_mode_for_channels(const struct sr_dev_inst *sdi);
SR_PRIV int gla_prepare_acquisition(const struct sr_dev_inst *sdi);
SR_PRIV void gla_build_request(const struct dev_context *devc,
	uint8_t req[GLA_REQUEST_SIZE]);
SR_PRIV int gla_start_capture(const struct sr_dev_inst *sdi);
SR_PRIV int gla_receive_data(int fd, int revents, void *cb_data);
SR_PRIV void gla_abort_acquisition(const struct sr_dev_inst *sdi);

#endif
