/*
 * Copyright (C) 2025 Rinigus https://github.com/rinigus
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "bt-monitor.h"

#include <gutil_log.h>

#define BLUEZ_SERVICE "org.bluez"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"
#define DBUS_OBJECT_MANAGER_INTERFACE "org.freedesktop.DBus.ObjectManager"
#define DEVICE_INTERFACE "org.bluez.Device1"

static void bt_print_device_state(GDBusConnection *conn,
                                  const gchar *object_path) {
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(
      conn, BLUEZ_SERVICE, object_path, DBUS_PROPERTIES_INTERFACE, "GetAll",
      g_variant_new("(s)", DEVICE_INTERFACE), G_VARIANT_TYPE("(a{sv})"),
      G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);

  if (error) {
    GINFO("Device %s no longer exists (disconnected)", object_path);
    g_error_free(error);
    return;
  }

  GVariant *props;
  g_variant_get(reply, "(@a{sv})", &props);

  gboolean connected = FALSE, resolved = FALSE;
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

  GINFO("Device %s connected: %s, services: %s", object_path,
        connected ? "true" : "false", resolved ? "resolved" : "unresolved");

  if (connected && resolved) {
    v = g_variant_lookup_value(props, "UUIDs", NULL);
    if (v) {
      GVariantIter iter;
      const gchar *uuid;
      g_variant_iter_init(&iter, v);
      while (g_variant_iter_next(&iter, "&s", &uuid)) {
        GINFO("  UUID: %s", uuid);
      }
      g_variant_unref(v);
    }
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
      bt_print_device_state(conn, object_path);
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
  GVariantIter iter;
  g_variant_iter_init(&iter, parameters);
  GVariant *object_v = g_variant_iter_next_value(&iter);
  const gchar *object = g_variant_get_string(object_v, NULL);
  GVariant *interfaces = g_variant_iter_next_value(&iter);
  g_variant_unref(object_v);

  // Check if DEVICE_INTERFACE was added
  if (g_variant_lookup_value(interfaces, DEVICE_INTERFACE, NULL)) {
    bt_print_device_state(conn, object);
  }

  g_variant_unref(interfaces);
}

static void on_interfaces_removed(GDBusConnection *conn,
                                  const gchar *sender_name,
                                  const gchar *object_path,
                                  const gchar *interface_name,
                                  const gchar *signal_name,
                                  GVariant *parameters, gpointer user_data) {
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
      bt_print_device_state(conn, object);
      break;
    }
  }

  g_variant_unref(ifaces);
}

void bt_monitor_init(GDBusConnection *conn) {
  g_dbus_connection_signal_subscribe(
      conn, BLUEZ_SERVICE, DBUS_PROPERTIES_INTERFACE, "PropertiesChanged", NULL,
      NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_properties_changed, NULL, NULL);

  g_dbus_connection_signal_subscribe(
      conn, BLUEZ_SERVICE, DBUS_OBJECT_MANAGER_INTERFACE, "InterfacesAdded",
      NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_interfaces_added, NULL, NULL);

  g_dbus_connection_signal_subscribe(
      conn, BLUEZ_SERVICE, DBUS_OBJECT_MANAGER_INTERFACE, "InterfacesRemoved",
      NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_interfaces_removed, NULL, NULL);

  GINFO("Monitoring BlueZ devices...");
}