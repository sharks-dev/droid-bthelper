/*
 * Copyright (C) 2025 Rinigus https://github.com/rinigus
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BT_MONITOR_H
#define BT_MONITOR_H

#include <gio/gio.h>

#include "audio-hal.h"

typedef void (*BtMonitorOnConnection)(const gchar *address, gboolean connected,
                                      gpointer user_data);

typedef struct {
  GDBusConnection *conn;
  GHashTable *address_to_connected; // char* -> gboolean (connected headset)
  GHashTable *object_to_address;    // char* -> char* (object path to address)
  BtMonitorOnConnection on_connection;
  gpointer user_data;
} BtMonitorState;

BtMonitorState *bt_monitor_init(GDBusConnection *conn,
                                BtMonitorOnConnection on_connection,
                                gpointer user_data);
void bt_monitor_free(BtMonitorState *state);

#endif /* BT_MONITOR_H */