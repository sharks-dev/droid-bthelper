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

static void on_properties_changed(GDBusConnection *conn,
                                  const gchar *sender_name,
                                  const gchar *object_path,
                                  const gchar *interface_name,
                                  const gchar *signal_name,
                                  GVariant *parameters, gpointer user_data) {
  GINFO("Received PropertiesChanged signal on %s from %s, parameters type: %s",
        interface_name, sender_name, g_variant_get_type_string(parameters));

  GVariantIter iter;
  g_variant_iter_init(&iter, parameters);
  GVariant *iface_v = g_variant_iter_next_value(&iter);
  const gchar *iface = g_variant_get_string(iface_v, NULL);
  GVariant *changed = g_variant_iter_next_value(&iter);
  GVariant *invalidated = g_variant_iter_next_value(&iter);
  g_variant_unref(iface_v);

  if (g_strcmp0(iface, DEVICE_INTERFACE) == 0) {
    GVariant *value = g_variant_lookup_value(changed, "Connected", NULL);
    if (value) {
      gboolean connected = g_variant_get_boolean(value);
      GINFO("Device %s %s", object_path,
            connected ? "connected" : "disconnected");
      g_variant_unref(value);
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
  GINFO("Received InterfacesAdded signal on %s from %s, parameters type: %s",
        interface_name, sender_name, g_variant_get_type_string(parameters));

  GVariantIter iter;
  g_variant_iter_init(&iter, parameters);
  GVariant *object_v = g_variant_iter_next_value(&iter);
  const gchar *object = g_variant_get_string(object_v, NULL);
  GVariant *interfaces = g_variant_iter_next_value(&iter);
  g_variant_unref(object_v);
  GINFO("Interfaces added at %s", object);
  g_variant_unref(interfaces);
}

static void on_interfaces_removed(GDBusConnection *conn,
                                  const gchar *sender_name,
                                  const gchar *object_path,
                                  const gchar *interface_name,
                                  const gchar *signal_name,
                                  GVariant *parameters, gpointer user_data) {
  GINFO("Received InterfacesRemoved signal on %s from %s, parameters type: %s",
        interface_name, sender_name, g_variant_get_type_string(parameters));

  GVariantIter iter;
  g_variant_iter_init(&iter, parameters);
  GVariant *object_v = g_variant_iter_next_value(&iter);
  const gchar *object = g_variant_get_string(object_v, NULL);
  GVariant *ifaces = g_variant_iter_next_value(&iter);
  g_variant_unref(object_v);
  GINFO("Interfaces removed at %s", object);
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