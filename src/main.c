/*
 * Copyright (C) 2025 Rinigus https://github.com/rinigus
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gutil_log.h>

#include <gio/gio.h>
#include <glib-unix.h>

#include "audio-hal.h"
#include "bt-monitor.h"

// Command line options
static gboolean opt_verbose = FALSE;

static GOptionEntry option_entries[] = {{"verbose", 'v', 0, G_OPTION_ARG_NONE,
                                         &opt_verbose, "Enable verbose logging",
                                         NULL},
                                        {NULL}};

typedef struct app {
  GMainLoop *loop;
  AudioHalState *hal_state;
  BtMonitorState *bt_state;
  int ret;
} App;

static gboolean app_signal(gpointer user_data) {
  App *app = user_data;

  GINFO("Caught signal, shutting down...");
  g_main_loop_quit(app->loop);
  return G_SOURCE_CONTINUE;
}

static void app_run(App *app) {
  GError *error = NULL;

  GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
  if (!conn) {
    GERR("Failed to connect to system bus: %s", error->message);
    g_error_free(error);
    app->ret = 1;
    return;
  }

  app->hal_state = audio_hal_init();
  app->bt_state =
      bt_monitor_init(conn, audio_hal_notify_connected, app->hal_state);

  guint sigtrm = g_unix_signal_add(SIGTERM, app_signal, app);
  guint sigint = g_unix_signal_add(SIGINT, app_signal, app);

  app->loop = g_main_loop_new(NULL, FALSE);
  app->ret = 0;
  g_main_loop_run(app->loop);

  g_source_remove(sigtrm);
  g_source_remove(sigint);
  g_main_loop_unref(app->loop);
  bt_monitor_free(app->bt_state);
  audio_hal_free(app->hal_state);
}

static gboolean parse_options(int argc, char *argv[]) {
  GError *error = NULL;
  GOptionContext *context;

  context = g_option_context_new("- Droid Bluetooth Helper");
  g_option_context_add_main_entries(context, option_entries, NULL);
  g_option_context_set_summary(
      context, "Droid Bluetooth Helper service for monitoring BlueZ devices.");

  if (!g_option_context_parse(context, &argc, &argv, &error)) {
    g_printerr("Option parsing failed: %s\n", error->message);
    g_error_free(error);
    g_option_context_free(context);
    return FALSE;
  }

  g_option_context_free(context);
  return TRUE;
}

int main(int argc, char *argv[]) {
  App app;

  memset(&app, 0, sizeof(app));
  app.ret = 1;

  // Parse command line options
  if (!parse_options(argc, argv)) {
    return 1;
  }

  gutil_log_timestamp = FALSE;
  gutil_log_set_type(GLOG_TYPE_STDERR, "droid-bthelper");
  gutil_log_default.level =
      opt_verbose ? GLOG_LEVEL_VERBOSE : GLOG_LEVEL_DEFAULT;

  app_run(&app);

  return app.ret;
}
