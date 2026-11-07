/*
 * Start a capture on a gusmanb LogicAnalyzer and stop it with
 * sr_session_stop() after a delay, the way PulseView's Stop button does.
 *
 * Build: cc -o stoptest stoptest.c $(pkg-config --cflags --libs libsigrok)
 * Usage: stoptest CONN SERIALCOMM DELAY_MS [TRIGGER_CH]
 * Exit code 0 if the session ended cleanly (SR_DF_END received).
 */

#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <libsigrok/libsigrok.h>

static gboolean got_end;
static uint64_t got_samples;

static void datafeed_in(const struct sr_dev_inst *sdi,
	const struct sr_datafeed_packet *packet, void *cb_data)
{
	const struct sr_datafeed_logic *logic;

	(void)sdi;
	(void)cb_data;

	if (packet->type == SR_DF_LOGIC) {
		logic = packet->payload;
		got_samples += logic->length / logic->unitsize;
	} else if (packet->type == SR_DF_END) {
		got_end = TRUE;
	}
}

static gboolean stop_cb(gpointer data)
{
	fprintf(stderr, "stoptest: calling sr_session_stop()\n");
	sr_session_stop(data);
	return G_SOURCE_REMOVE;
}

static void stopped_cb(void *data)
{
	g_main_loop_quit(data);
}

int main(int argc, char **argv)
{
	struct sr_context *ctx;
	struct sr_session *session;
	struct sr_dev_driver **drivers, *driver;
	struct sr_dev_inst *sdi;
	struct sr_trigger *trigger;
	struct sr_trigger_stage *stage;
	GSList *opts, *devs;
	struct sr_config *src_conn, *src_comm;
	GMainLoop *loop;
	int i;

	if (argc != 4 && argc != 5) {
		fprintf(stderr, "usage: %s CONN SERIALCOMM DELAY_MS [TRIGGER_CH]\n",
			argv[0]);
		return 2;
	}

	sr_init(&ctx);
	driver = NULL;
	drivers = sr_driver_list(ctx);
	for (i = 0; drivers[i]; i++)
		if (!g_strcmp0(drivers[i]->name, "gusmanb-logicanalyzer"))
			driver = drivers[i];
	if (!driver || sr_driver_init(ctx, driver) != SR_OK)
		return 2;

	src_conn = g_malloc0(sizeof(*src_conn));
	src_conn->key = SR_CONF_CONN;
	src_conn->data = g_variant_new_string(argv[1]);
	src_comm = g_malloc0(sizeof(*src_comm));
	src_comm->key = SR_CONF_SERIALCOMM;
	src_comm->data = g_variant_new_string(argv[2]);
	opts = g_slist_append(g_slist_append(NULL, src_conn), src_comm);
	devs = sr_driver_scan(driver, opts);
	if (!devs) {
		fprintf(stderr, "stoptest: no device\n");
		return 2;
	}
	sdi = devs->data;

	sr_session_new(ctx, &session);
	if (sr_dev_open(sdi) != SR_OK)
		return 2;
	sr_session_dev_add(session, sdi);
	sr_config_set(sdi, NULL, SR_CONF_LIMIT_SAMPLES, g_variant_new_uint64(1000));

	trigger = sr_trigger_new(NULL);
	stage = sr_trigger_stage_add(trigger);
	sr_trigger_match_add(stage, g_slist_nth_data(sr_dev_inst_channels_get(sdi),
		argc == 5 ? atoi(argv[4]) : 0), SR_TRIGGER_RISING, 0);
	sr_session_trigger_set(session, trigger);

	sr_session_datafeed_callback_add(session, datafeed_in, NULL);
	loop = g_main_loop_new(NULL, FALSE);
	sr_session_stopped_callback_set(session, stopped_cb, loop);

	if (sr_session_start(session) != SR_OK) {
		fprintf(stderr, "stoptest: start failed\n");
		return 1;
	}
	g_timeout_add(atoi(argv[3]), stop_cb, session);
	g_main_loop_run(loop);

	fprintf(stderr, "stoptest: end=%d samples=%" G_GUINT64_FORMAT "\n",
		got_end, got_samples);
	sr_session_destroy(session);
	sr_exit(ctx);

	return got_end ? 0 : 1;
}
