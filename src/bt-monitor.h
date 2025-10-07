/*
 * Copyright (C) 2025 Rinigus https://github.com/rinigus
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef BT_MONITOR_H
#define BT_MONITOR_H

#include <gio/gio.h>

void bt_monitor_init(GDBusConnection *conn);

#endif /* BT_MONITOR_H */