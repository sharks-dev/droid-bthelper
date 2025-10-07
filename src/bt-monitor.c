/*
 * Copyright (C) 2025 Rinigus https://github.com/rinigus
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "bt-monitor.h"

#include <glib.h>
#include <gutil_log.h>

#define BLUEZ_SERVICE "org.bluez"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define DBUS_OBJECT_MANAGER_INTERFACE "org.freedesktop.DBus.ObjectManager"
#define DEVICE_INTERFACE "org.bluez.Device1"

#define UUID_HSP "00001108-0000-1000-8000-00805F9B34FB"
#define UUID_HFP "0000111E-0000-1000-8000-00805F9B34FB"

static void bt_connection_status(BtMonitorState *state,
                                 const gchar *object_path, const gchar *address,
                                 gboolean connected) {
  const gchar *addr = address;

  if (!object_path) {
    GWARN("Cannot update status without object_path");
    return;
  }

  // If this is called without address, resolve via object_path
  if (!addr && object_path)
    addr = g_hash_table_lookup(state->object_to_address, object_path);

  if (!addr && connected) {
    GWARN("Inconsistent state: cannot set connected for unknown address (%s)",
          object_path);
    return;
  }

  // If disconnecting and no address found, just return as we processed it
  // earlier or it is untracked device
  if (!addr)
    return;

  gboolean was_connected =
      GPOINTER_TO_INT(g_hash_table_lookup(state->address_to_connected, addr));

  if (connected) {
    if (!g_hash_table_lookup(state->object_to_address, object_path)) {
      g_hash_table_insert(state->object_to_address, g_strdup(object_path),
                          g_strdup(addr));
    }

    if (!was_connected && state->on_connected)
      state->on_connected(addr, state->user_data);
  } else {
    if (was_connected && state->on_disconnected)
      state->on_disconnected(addr, state->user_data);

    // Remove from object mapping when disconnecting
    if (object_path)
      g_hash_table_remove(state->object_to_address, object_path);
  }

  // Update connection state
  g_hash_table_insert(state->address_to_connected, g_strdup(addr),
                      GINT_TO_POINTER(connected));
}

static void bt_update_device_state(BtMonitorState *state,
                                   const gchar *object_path) {
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(
      state->conn, BLUEZ_SERVICE, object_path, DBUS_PROPERTIES_INTERFACE,
      "GetAll", g_variant_new("(s)", DEVICE_INTERFACE),
      G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

  if (error) {
    GINFO("Device %s no longer exists (disconnected)", object_path);
    bt_connection_status(state, object_path, NULL, FALSE);
    g_error_free(error);
    return;
  }

  GVariant *props;
  g_variant_get(reply, "(@a{sv})", &props);

  gboolean connected = FALSE, resolved = FALSE, headset = FALSE;
  const gchar *address = NULL;
  GVariant *v;

  v = g_variant_lookup_value(props, "Connected", NULL);
  if (v) {
    connected = g_variant_get_boolean(v);
    g_variant_unref(v);
  }

  v = g_variant_lookup_value(props, "ServicesResolved", NULL);
  if (v) {
    resolved = g_variant_get_boolean(v);
    g_variant_unref(v);
  }

  v = g_variant_lookup_value(props, "Address", NULL);
  if (v) {
    address = g_variant_get_string(v, NULL);
    g_variant_unref(v);
  }

  if (connected && resolved) {
    v = g_variant_lookup_value(props, "UUIDs", NULL);
    if (v) {
      GVariantIter iter;
      const gchar *uuid;
      g_variant_iter_init(&iter, v);
      while (g_variant_iter_next(&iter, "&s", &uuid) && !headset) {
        headset = (g_ascii_strcasecmp(uuid, UUID_HSP) == 0 ||
                   g_ascii_strcasecmp(uuid, UUID_HFP) == 0);
        GINFO("  UUID: %s -> %d", uuid, headset);
      }
      g_variant_unref(v);
    }
  }

  gboolean status = connected && resolved && headset;

  GINFO("Device %s (%s): connected=%s, services=%s headset=%s", object_path,
        address ? address : "unknown", connected ? "true" : "false",
        resolved ? "resolved" : "unresolved", headset ? "yes" : "not");

  if (address) {
    bt_connection_status(state, object_path, address, status);
  }

  g_variant_unref(props);
  g_variant_unref(reply);
}

static void on_properties_changed(GDBusConnection *conn,
                                  const gchar *sender_name,
                                  const gchar *object_path,
                                  const gchar *interface_name,
                                  const gchar *signal_name,
                                  GVariant *parameters, gpointer user_data) {
  BtMonitorState *state = user_data;
  GVariantIter iter;
  GVariant *iface_v, *changed, *invalidated;
  const gchar *iface;

  g_variant_iter_init(&iter, parameters);
  iface_v = g_variant_iter_next_value(&iter);
  iface = g_variant_get_string(iface_v, NULL);
  changed = g_variant_iter_next_value(&iter);
  invalidated = g_variant_iter_next_value(&iter);
  g_variant_unref(iface_v);

  if (g_strcmp0(iface, DEVICE_INTERFACE) == 0) {
    // Check if any property we care about has changed
    if (g_variant_lookup_value(changed, "Connected", NULL) ||
        g_variant_lookup_value(changed, "UUIDs", NULL) ||
        g_variant_lookup_value(changed, "ServicesResolved", NULL)) {
      bt_update_device_state(state, object_path);
    }
  }

  g_variant_unref(changed);
  g_variant_unref(invalidated);
}

static void on_interfaces_added(GDBusConnection *conn, const gchar *sender_name,
                                const gchar *object_path,
                                const gchar *interface_name,
                                const gchar *signal_name, GVariant *parameters,
                                gpointer user_data) {
  BtMonitorState *state = user_data;
  GVariantIter iter;
  g_variant_iter_init(&iter, parameters);
  GVariant *object_v = g_variant_iter_next_value(&iter);
  const gchar *object = g_variant_get_string(object_v, NULL);
  GVariant *interfaces = g_variant_iter_next_value(&iter);
  g_variant_unref(object_v);

  // Check if DEVICE_INTERFACE was added
  if (g_variant_lookup_value(interfaces, DEVICE_INTERFACE, NULL)) {
    bt_update_device_state(state, object);
  }

  g_variant_unref(interfaces);
}

static void on_interfaces_removed(GDBusConnection *conn,
                                  const gchar *sender_name,
                                  const gchar *object_path,
                                  const gchar *interface_name,
                                  const gchar *signal_name,
                                  GVariant *parameters, gpointer user_data) {
  BtMonitorState *state = user_data;
  GVariantIter iter;
  g_variant_iter_init(&iter, parameters);
  GVariant *object_v = g_variant_iter_next_value(&iter);
  const gchar *object = g_variant_get_string(object_v, NULL);
  GVariant *ifaces = g_variant_iter_next_value(&iter);
  g_variant_unref(object_v);

  // Check if DEVICE_INTERFACE was removed
  GVariantIter iface_iter;
  g_variant_iter_init(&iface_iter, ifaces);
  const gchar *iface;
  while (g_variant_iter_next(&iface_iter, "&s", &iface)) {
    if (g_strcmp0(iface, DEVICE_INTERFACE) == 0) {
      bt_update_device_state(state, object);
      break;
    }
  }

  g_variant_unref(ifaces);
}

BtMonitorState *bt_monitor_init(GDBusConnection *conn,
                                BtMonitorOnConnected on_connected,
                                BtMonitorOnDisconnected on_disconnected,
                                gpointer user_data) {
  BtMonitorState *state = g_new(BtMonitorState, 1);
  state->conn = conn;
  state->address_to_connected =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  state->object_to_address =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  state->on_connected = on_connected;
  state->on_disconnected = on_disconnected;
  state->user_data = user_data;

  g_dbus_connection_signal_subscribe(
      conn, BLUEZ_SERVICE, DBUS_PROPERTIES_INTERFACE, "PropertiesChanged", NULL,
      NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_properties_changed, state, NULL);

  g_dbus_connection_signal_subscribe(
      conn, BLUEZ_SERVICE, DBUS_OBJECT_MANAGER_INTERFACE, "InterfacesAdded",
      NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_interfaces_added, state, NULL);

  g_dbus_connection_signal_subscribe(
      conn, BLUEZ_SERVICE, DBUS_OBJECT_MANAGER_INTERFACE, "InterfacesRemoved",
      NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_interfaces_removed, state, NULL);

  GINFO("Monitoring BlueZ devices...");
  return state;
}

void bt_monitor_free(BtMonitorState *state) {
  if (state) {
    g_hash_table_destroy(state->address_to_connected);
    g_hash_table_destroy(state->object_to_address);
    g_object_unref(state->conn);
    g_free(state);
  }
}