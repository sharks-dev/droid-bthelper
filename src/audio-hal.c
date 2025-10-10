/*
 * Copyright (C) 2025 Rinigus https://github.com/rinigus
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "audio-hal.h"

#include <gutil_log.h>
#include <string.h>

// Retry settings
#define MAX_INIT_RETRIES 60
#define INIT_RETRY_SLEEP_SEC 3

#define AUDIO_BINDER_DEVICE "/dev/hwbinder"

#define AUDIO_FACTORY_INTERFACE "android.hardware.audio@7.0::IDevicesFactory"
#define AUDIO_DEVICES_FACTORY_SERVICE AUDIO_FACTORY_INTERFACE "/default"
#define AUDIO_DEVICE_INTERFACE "android.hardware.audio@7.0::IDevice"

#define AUDIO_DEVICE_TYPE_OUT "AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET"
#define AUDIO_DEVICE_TYPE_IN "AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET"
#define AUDIO_HARDWARE_MODULE_ID_PRIMARY "primary"

// Transaction codes
#define IDEVICES_FACTORY_OPEN_DEVICE 1 // Transaction code for openDevice method
#define IDEVICE_SET_CONNECTED_STATE 22

// result codes
#define RESULT_OK 0

// DeviceAddress structure matching HIDL layout
typedef struct {
  GBinderHidlString deviceType;
  guint32 discriminator;
  guint32 padding;
  GBinderHidlString id;
} DeviceAddress;

static const GBinderWriterField device_address_fields[] = {
    GBINDER_WRITER_FIELD_HIDL_STRING(DeviceAddress, deviceType),
    GBINDER_WRITER_FIELD_HIDL_STRING(DeviceAddress, id),
    GBINDER_WRITER_FIELD_END()};

static const GBinderWriterType device_address_type = {
    GBINDER_WRITER_STRUCT_NAME_AND_SIZE(DeviceAddress), device_address_fields};

static gboolean audio_hal_set_connected_state(AudioHalState *state,
                                              const char *device_type,
                                              const char *address,
                                              gboolean connected) {
  GINFO("setConnectedState with device_type=%s, address=%s, %s", device_type,
        address, connected ? "connected" : "disconnected");

  GBinderLocalRequest *req = gbinder_client_new_request(state->device_client);
  GBinderWriter writer;

  gbinder_local_request_init_writer(req, &writer);

  DeviceAddress *addr = gbinder_writer_new0(&writer, DeviceAddress);
  addr->deviceType.data.str = (char *)device_type;
  addr->deviceType.len = strlen(device_type);
  addr->deviceType.owns_buffer = FALSE;

  addr->discriminator = 0; // id variant
  addr->padding = 0;

  addr->id.data.str = (char *)address;
  addr->id.len = strlen(address);
  addr->id.owns_buffer = FALSE;

  gbinder_writer_append_struct(&writer, addr, &device_address_type, NULL);
  gbinder_writer_append_bool(&writer, connected);

  // Send transaction
  int status = 0;
  GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
      state->device_client, IDEVICE_SET_CONNECTED_STATE, req, NULL);

  gbinder_local_request_unref(req);

  if (status != GBINDER_STATUS_OK || !reply) {
    GWARN("setConnectedState transact failed for %s", device_type);
    if (reply)
      gbinder_remote_reply_unref(reply);
    return FALSE;
  }

  // Read result code
  GBinderReader reader;
  gbinder_remote_reply_init_reader(reply, &reader);

  gint32 result = 0;
  gbinder_reader_read_int32(&reader, &result);

  gbinder_remote_reply_unref(reply);

  if (result != RESULT_OK) {
    GERR("Failed to read result from setConnectedState reply");
  }
  return result == RESULT_OK;
}

static gboolean audio_hal_notify_connected_worker(const char *address,
                                                  gboolean connected,
                                                  gpointer user_data) {
  AudioHalState *state = user_data;
  if (!state || !state->factory_client) {
    return FALSE;
  }

  if (!state->device_remote) {
    GINFO("Opening primary device");
    GBinderLocalRequest *req =
        gbinder_client_new_request(state->factory_client);

    // Write the device name parameter (PRIMARY)
    GBinderWriter writer;
    gbinder_local_request_init_writer(req, &writer);
    gbinder_writer_append_hidl_string(&writer,
                                      AUDIO_HARDWARE_MODULE_ID_PRIMARY);

    int status = 0;
    GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
        state->factory_client, IDEVICES_FACTORY_OPEN_DEVICE, req, &status);
    if (status != GBINDER_STATUS_OK || !reply) {
      GERR("Failed to open primary device");
      gbinder_local_request_unref(req);
      return FALSE;
    }

    GBinderReader reader;
    gbinder_remote_reply_init_reader(reply, &reader);

    gint32 result = 0;
    gbinder_reader_read_int32(&reader, &result);
    gbinder_reader_read_int32(&reader, &result);

    if (result != RESULT_OK) {
      GERR("Failed to read result");
      gbinder_remote_reply_unref(reply);
      gbinder_local_request_unref(req);
      return FALSE;
    }

    // Read the device object
    GBinderRemoteObject *device_obj = gbinder_reader_read_object(&reader);
    if (!device_obj) {
      GERR("Failed to get device remote object");
      gbinder_remote_reply_unref(reply);
      gbinder_local_request_unref(req);
      return FALSE;
    }
    state->device_remote = gbinder_remote_object_ref(device_obj);

    state->device_client =
        gbinder_client_new(state->device_remote, AUDIO_DEVICE_INTERFACE);

    gbinder_remote_reply_unref(reply);
    gbinder_local_request_unref(req);
    GINFO("Primary device opened");
  }

  if (!state->device_client) {
    return FALSE;
  }

  if (!audio_hal_set_connected_state(state, AUDIO_DEVICE_TYPE_OUT, address,
                                     connected)) {
    return FALSE;
  }

  if (!audio_hal_set_connected_state(state, AUDIO_DEVICE_TYPE_IN, address,
                                     connected)) {
    return FALSE;
  }

  return TRUE;
}

void audio_hal_notify_connected(const char *address, gboolean connected,
                                gpointer user_data) {
  if (!audio_hal_notify_connected_worker(address, connected, user_data))
    GWARN("Failed to set status to %s through AudioHal for %s",
          connected ? "connected" : "disconnected", address);
}

static AudioHalState *audio_hal_init_worker(void) {
  AudioHalState *state = g_new(AudioHalState, 1);
  memset(state, 0, sizeof(*state));

  state->sm = gbinder_servicemanager_new(AUDIO_BINDER_DEVICE);
  if (!state->sm) {
    GERR("Failed to create service manager");
    g_free(state);
    return NULL;
  }

  // Get the factory
  state->factory_remote = gbinder_servicemanager_get_service_sync(
      state->sm, AUDIO_DEVICES_FACTORY_SERVICE, NULL);
  if (!state->factory_remote) {
    GERR("Failed to get audio factory service");
    gbinder_servicemanager_unref(state->sm);
    g_free(state);
    return NULL;
  }

  gbinder_remote_object_ref(state->factory_remote);
  state->factory_client =
      gbinder_client_new(state->factory_remote, AUDIO_FACTORY_INTERFACE);

  if (!state->factory_client) {
    GERR("Failed to create factory client");
    gbinder_remote_object_unref(state->factory_remote);
    gbinder_servicemanager_unref(state->sm);
    g_free(state);
    return NULL;
  }

  GINFO("Audio HAL initialized");
  return state;
}

AudioHalState *audio_hal_init(void) {
  for (int i = 0; i < MAX_INIT_RETRIES; ++i) {
    GINFO("Trying to initialize Audio HAL (attempt %d/%d)", i + 1,
          MAX_INIT_RETRIES);
    AudioHalState *state = audio_hal_init_worker();
    if (state)
      return state;

    if (i < MAX_INIT_RETRIES - 1)
      sleep(INIT_RETRY_SLEEP_SEC);
  }
  GINFO("Failed to initialize Audio HAL after %d attempts", MAX_INIT_RETRIES);
  return NULL;
}

void audio_hal_free(AudioHalState *state) {
  if (state) {
    if (state->device_client)
      gbinder_client_unref(state->device_client);
    if (state->device_remote)
      gbinder_remote_object_unref(state->device_remote);
    if (state->factory_client)
      gbinder_client_unref(state->factory_client);
    if (state->factory_remote)
      gbinder_remote_object_unref(state->factory_remote);
    if (state->sm)
      gbinder_servicemanager_unref(state->sm);
    g_free(state);
  }
}
