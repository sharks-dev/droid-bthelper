/*
 * Copyright (C) 2025 Rinigus https://github.com/rinigus
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BT_MONITOR_H
#define BT_MONITOR_H

#include <gio/gio.h>

typedef void (*BtMonitorOnConnected)(const gchar *address, gpointer user_data);
typedef void (*BtMonitorOnDisconnected)(const gchar *address,
                                        gpointer user_data);

typedef struct {
  GDBusConnection *conn;
  GHashTable *address_to_connected; // char* -> gboolean (connected headset)
  GHashTable *object_to_address;    // char* -> char* (object path to address)
  BtMonitorOnConnected on_connected;
  BtMonitorOnDisconnected on_disconnected;
  gpointer user_data;
} BtMonitorState;

BtMonitorState *bt_monitor_init(GDBusConnection *conn,
                                BtMonitorOnConnected on_connected,
                                BtMonitorOnDisconnected on_disconnected,
                                gpointer user_data);
void bt_monitor_free(BtMonitorState *state);

#endif /* BT_MONITOR_H */