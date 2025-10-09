/*
 * Copyright (C) 2025 Rinigus https://github.com/rinigus
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef AUDIO_HAL_H
#define AUDIO_HAL_H

#include <gbinder.h>
#include <glib.h>

typedef struct {
  GBinderServiceManager *sm;
  GBinderRemoteObject *factory_remote;
  GBinderClient *factory_client;
  GBinderRemoteObject *device_remote;
  GBinderClient *device_client;
} AudioHalState;

AudioHalState *audio_hal_init(void);
void audio_hal_free(AudioHalState *state);
void audio_hal_notify_connected(const char *address, gboolean connected,
                                gpointer user_data);

#endif /* AUDIO_HAL_H */