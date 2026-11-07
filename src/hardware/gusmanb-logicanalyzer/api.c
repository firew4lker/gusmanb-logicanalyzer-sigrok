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

#include <config.h>
#include <string.h>
#include "protocol.h"

/*
 * The Pico's USB stdio only sends data while DTR is asserted. The baud
 * rate is irrelevant for USB CDC.
 */
#define SERIALCOMM "115200/8n1/dtr=1/rts=1"

#define DEFAULT_SAMPLERATE	SR_MHZ(1)
#define DEFAULT_LIMIT_SAMPLES	10000
#define DEFAULT_CAPTURE_RATIO	10

static const uint32_t scanopts[] = {
	SR_CONF_CONN,
	SR_CONF_SERIALCOMM,
};

static const uint32_t drvopts[] = {
	SR_CONF_LOGIC_ANALYZER,
};

static const uint32_t devopts[] = {
	SR_CONF_CONN | SR_CONF_GET,
	SR_CONF_SAMPLERATE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_LIMIT_SAMPLES | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_CAPTURE_RATIO | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_TRIGGER_MATCH | SR_CONF_LIST,
};

static const int32_t trigger_matches[] = {
	SR_TRIGGER_ZERO,
	SR_TRIGGER_ONE,
	SR_TRIGGER_RISING,
	SR_TRIGGER_FALLING,
};

static void clear_helper(struct dev_context *devc)
{
	g_free(devc->board);
	g_free(devc->samplerates);
	g_free(devc->rx_buf);
	g_free(devc->out_buf);
}

static int dev_clear(const struct sr_dev_driver *di)
{
	return std_dev_clear_with_callback(di,
		(std_dev_clear_callback)clear_helper);
}

/* Probe one serial port. Returns a new device instance, or NULL. */
static struct sr_dev_inst *probe_port(const char *conn, const char *serialcomm)
{
	struct sr_dev_inst *sdi;
	struct sr_serial_dev_inst *serial;
	struct dev_context *devc;
	char *comm;
	char name[8];
	unsigned int i;
	int ret, attempt;

	/*
	 * Front ends may pass their own serialcomm (PulseView sends e.g.
	 * "921600/8n1"). Make sure DTR/RTS are asserted anyway, the firmware
	 * stays silent without DTR.
	 */
	if (!strstr(serialcomm, "dtr="))
		comm = g_strdup_printf("%s/dtr=1%s", serialcomm,
			strstr(serialcomm, "rts=") ? "" : "/rts=1");
	else
		comm = g_strdup(serialcomm);
	serial = sr_serial_dev_inst_new(conn, comm);
	g_free(comm);
	sr_info("Probing %s.", conn);
	if (serial_open(serial, SERIAL_RDWR) != SR_OK) {
		sr_serial_dev_inst_free(serial);
		return NULL;
	}

	devc = g_malloc0(sizeof(*devc));
	/*
	 * Right after power-up the firmware waits up to ~3 s before it
	 * answers, so retry once.
	 */
	ret = SR_ERR;
	for (attempt = 0; attempt < 2 && ret != SR_OK; attempt++)
		ret = gla_identify(serial, devc);
	serial_close(serial);

	if (ret != SR_OK) {
		sr_dbg("No LogicAnalyzer found on %s.", conn);
		clear_helper(devc);
		g_free(devc);
		sr_serial_dev_inst_free(serial);
		return NULL;
	}

	if (devc->fw_major < GLA_MIN_MAJOR ||
	    (devc->fw_major == GLA_MIN_MAJOR && devc->fw_minor < GLA_MIN_MINOR)) {
		sr_err("Firmware V%d_%d is not supported, please update to "
			"V%d_%d or later.", devc->fw_major, devc->fw_minor,
			GLA_MIN_MAJOR, GLA_MIN_MINOR);
		clear_helper(devc);
		g_free(devc);
		sr_serial_dev_inst_free(serial);
		return NULL;
	}

	gla_build_samplerates(devc);
	devc->cur_samplerate = DEFAULT_SAMPLERATE;
	if (devc->cur_samplerate > devc->max_samplerate)
		devc->cur_samplerate = devc->max_samplerate;
	devc->limit_samples = DEFAULT_LIMIT_SAMPLES;
	devc->capture_ratio = DEFAULT_CAPTURE_RATIO;
	devc->rx_state = GLA_RX_IDLE;

	sdi = g_malloc0(sizeof(*sdi));
	sdi->status = SR_ST_INACTIVE;
	sdi->vendor = g_strdup("gusmanb");
	sdi->model = g_strdup_printf("LogicAnalyzer %s", devc->board);
	sdi->version = g_strdup_printf("V%d_%d", devc->fw_major, devc->fw_minor);
	sdi->inst_type = SR_INST_SERIAL;
	sdi->conn = serial;
	sdi->connection_id = g_strdup(conn);
	sdi->priv = devc;

	for (i = 0; i < devc->num_channels; i++) {
		g_snprintf(name, sizeof(name), "D%u", i);
		sr_channel_new(sdi, i, SR_CHANNEL_LOGIC, TRUE, name);
	}

	return sdi;
}

/*
 * With conn= only that port is probed. Without it, every serial port with
 * the analyzer's USB ID (pid.codes 1209:3020, set by the firmware) is
 * probed, so "sigrok-cli --scan" and PulseView find it on any port.
 */
static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
	struct sr_config *src;
	struct sr_dev_inst *sdi;
	GSList *l, *ports, *devices;
	const char *conn, *serialcomm;

	conn = serialcomm = NULL;
	for (l = options; l; l = l->next) {
		src = l->data;
		switch (src->key) {
		case SR_CONF_CONN:
			conn = g_variant_get_string(src->data, NULL);
			break;
		case SR_CONF_SERIALCOMM:
			serialcomm = g_variant_get_string(src->data, NULL);
			break;
		}
	}
	if (!serialcomm)
		serialcomm = SERIALCOMM;

	devices = NULL;
	if (conn) {
		if ((sdi = probe_port(conn, serialcomm)))
			devices = g_slist_append(devices, sdi);
	} else {
		ports = sr_serial_find_usb(GLA_USB_VID, GLA_USB_PID);
		for (l = ports; l; l = l->next)
			if ((sdi = probe_port(l->data, serialcomm)))
				devices = g_slist_append(devices, sdi);
		g_slist_free_full(ports, g_free);
	}

	return std_scan_complete(di, devices);
}

static int config_get(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;
	struct sr_serial_dev_inst *serial;

	(void)cg;

	if (!sdi)
		return SR_ERR_ARG;
	devc = sdi->priv;

	switch (key) {
	case SR_CONF_CONN:
		serial = sdi->conn;
		if (!serial || !serial->port)
			return SR_ERR_NA;
		*data = g_variant_new_string(serial->port);
		break;
	case SR_CONF_SAMPLERATE:
		*data = g_variant_new_uint64(devc->cur_samplerate);
		break;
	case SR_CONF_LIMIT_SAMPLES:
		*data = g_variant_new_uint64(devc->limit_samples);
		break;
	case SR_CONF_CAPTURE_RATIO:
		*data = g_variant_new_uint64(devc->capture_ratio);
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int config_set(uint32_t key, GVariant *data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;
	uint64_t value;

	(void)cg;

	devc = sdi->priv;

	switch (key) {
	case SR_CONF_SAMPLERATE:
		value = g_variant_get_uint64(data);
		/* Any rate up to the maximum works (fractional PIO divider). */
		if (value != devc->blast_samplerate &&
		    (value < devc->samplerates[0] || value > devc->max_samplerate)) {
			sr_err("Samplerate %" PRIu64 " Hz out of range (%" PRIu64
				" - %" PRIu64 " Hz, or %" PRIu64 " Hz blast mode).",
				value, devc->samplerates[0], devc->max_samplerate,
				devc->blast_samplerate);
			return SR_ERR_SAMPLERATE;
		}
		devc->cur_samplerate = value;
		break;
	case SR_CONF_LIMIT_SAMPLES:
		value = g_variant_get_uint64(data);
		if (value < GLA_MIN_PRE_SAMPLES + GLA_MIN_POST_SAMPLES ||
		    value > gla_max_samples(devc, GLA_MODE_8CH))
			return SR_ERR_ARG;
		devc->limit_samples = value;
		break;
	case SR_CONF_CAPTURE_RATIO:
		value = g_variant_get_uint64(data);
		if (value > 100)
			return SR_ERR_ARG;
		devc->capture_ratio = value;
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int config_list(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;

	switch (key) {
	case SR_CONF_SCAN_OPTIONS:
	case SR_CONF_DEVICE_OPTIONS:
		return STD_CONFIG_LIST(key, data, sdi, cg, scanopts, drvopts, devopts);
	case SR_CONF_SAMPLERATE:
		if (!sdi)
			return SR_ERR_ARG;
		devc = sdi->priv;
		*data = std_gvar_samplerates(devc->samplerates,
			devc->num_samplerates);
		break;
	case SR_CONF_LIMIT_SAMPLES:
		if (!sdi)
			return SR_ERR_ARG;
		devc = sdi->priv;
		*data = std_gvar_tuple_u64(GLA_MIN_PRE_SAMPLES + GLA_MIN_POST_SAMPLES,
			gla_max_samples(devc, gla_mode_for_channels(sdi)));
		break;
	case SR_CONF_TRIGGER_MATCH:
		*data = std_gvar_array_i32(ARRAY_AND_SIZE(trigger_matches));
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int dev_acquisition_start(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct sr_serial_dev_inst *serial;
	int ret;

	devc = sdi->priv;
	serial = sdi->conn;

	if ((ret = gla_prepare_acquisition(sdi)) != SR_OK)
		return ret;

	g_free(devc->rx_buf);
	g_free(devc->out_buf);
	devc->rx_buf = g_malloc0(4);
	devc->out_buf = g_malloc0(GLA_CHUNK_SAMPLES * devc->unitsize);
	devc->rx_len = 0;
	devc->hdr_len = 0;
	devc->wire_samples = devc->wire_received = 0;
	devc->skip_left = devc->skip_samples;
	devc->samples_sent = 0;
	devc->trigger_sent = FALSE;
	devc->stamps_left = 0;

	if ((ret = gla_start_capture(sdi)) != SR_OK) {
		g_free(devc->rx_buf);
		devc->rx_buf = NULL;
		g_free(devc->out_buf);
		devc->out_buf = NULL;
		return ret;
	}

	devc->rx_state = GLA_RX_LENGTH;
	devc->last_rx = g_get_monotonic_time();
	std_session_send_df_header(sdi);

	/* The device may wait for its trigger for a long time. */
	serial_source_add(sdi->session, serial, G_IO_IN, 100,
		gla_receive_data, (struct sr_dev_inst *)sdi);

	return SR_OK;
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi)
{
	gla_abort_acquisition(sdi);

	return SR_OK;
}

static struct sr_dev_driver gusmanb_logicanalyzer_driver_info = {
	.name = "gusmanb-logicanalyzer",
	.longname = "gusmanb LogicAnalyzer (RP2040/RP2350)",
	.api_version = 1,
	.init = std_init,
	.cleanup = std_cleanup,
	.scan = scan,
	.dev_list = std_dev_list,
	.dev_clear = dev_clear,
	.config_get = config_get,
	.config_set = config_set,
	.config_list = config_list,
	.dev_open = std_serial_dev_open,
	.dev_close = std_serial_dev_close,
	.dev_acquisition_start = dev_acquisition_start,
	.dev_acquisition_stop = dev_acquisition_stop,
	.context = NULL,
};
SR_REGISTER_DEV_DRIVER(gusmanb_logicanalyzer_driver_info);
