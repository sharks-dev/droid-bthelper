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

#define AUDIO_FACTORY_INTERFACE "android.hardware.audio@6.0::IDevicesFactory"
#define AUDIO_DEVICES_FACTORY_SERVICE AUDIO_FACTORY_INTERFACE "/default"
#define AUDIO_DEVICE_INTERFACE "android.hardware.audio@6.0::IDevice"

// android.hardware.audio.common@6.0::AudioDevice values (bitfield enum)
#define AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET 0x20u
#define AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET 0x80000008u
#define AUDIO_HARDWARE_MODULE_ID_PRIMARY "primary"

// Transaction codes (IDevice@6.0: setConnectedState is the 22nd method)
#define IDEVICES_FACTORY_OPEN_DEVICE 1 // Transaction code for openDevice method
#define IDEVICE_SET_CONNECTED_STATE 22

// result codes
#define RESULT_OK 0

// android.hardware.audio.common@6.0::DeviceAddress, HIDL memory layout:
//   AudioDevice device;          // uint32, offset 0
//   union { uint8_t mac[6]; uint8_t ipv4[4]; struct { int32 card, device; } alsa; }
//                                // 8 bytes, offset 4
//   string busAddress;           // offset 16
//   string rSubmixAddress;       // offset 32
// (The @7.0 layout with a string deviceType and safe_union id, which this
// code was originally written for, is not understood by a @6.0 HAL.)
typedef struct {
  guint32 device;
  guint8 address[8];
  guint32 padding;
  GBinderHidlString busAddress;
  GBinderHidlString rSubmixAddress;
} DeviceAddress;

G_STATIC_ASSERT(sizeof(DeviceAddress) == 48);

static const GBinderWriterField device_address_fields[] = {
    GBINDER_WRITER_FIELD_HIDL_STRING(DeviceAddress, busAddress),
    GBINDER_WRITER_FIELD_HIDL_STRING(DeviceAddress, rSubmixAddress),
    GBINDER_WRITER_FIELD_END()};

static const GBinderWriterType device_address_type = {
    GBINDER_WRITER_STRUCT_NAME_AND_SIZE(DeviceAddress), device_address_fields};

static gboolean audio_hal_set_connected_state(AudioHalState *state,
                                              guint32 device_type,
                                              const char *address,
                                              gboolean connected) {
  GINFO("setConnectedState with device_type=0x%08x, address=%s, %s",
        device_type, address, connected ? "connected" : "disconnected");

  GBinderLocalRequest *req = gbinder_client_new_request(state->device_client);
  GBinderWriter writer;

  gbinder_local_request_init_writer(req, &writer);

  // As in Android's HidlUtils::deviceAddressFromHal, the MAC is only carried
  // for A2DP devices; for SCO the HAL just receives the device type, which it
  // turns into set_parameters("connect=<device>").
  DeviceAddress *addr = gbinder_writer_new0(&writer, DeviceAddress);
  addr->device = device_type;
  addr->busAddress.data.str = "";
  addr->busAddress.len = 0;
  addr->busAddress.owns_buffer = FALSE;
  addr->rSubmixAddress.data.str = "";
  addr->rSubmixAddress.len = 0;
  addr->rSubmixAddress.owns_buffer = FALSE;

  gbinder_writer_append_struct(&writer, addr, &device_address_type, NULL);
  gbinder_writer_append_bool(&writer, connected);

  // Send transaction
  int status = 0;
  GBinderRemoteReply *reply = gbinder_client_transact_sync_reply(
      state->device_client, IDEVICE_SET_CONNECTED_STATE, req, &status);

  gbinder_local_request_unref(req);

  if (status != GBINDER_STATUS_OK || !reply) {
    GWARN("setConnectedState transact failed for 0x%08x (status %d)",
          device_type, status);
    if (reply)
      gbinder_remote_reply_unref(reply);
    return FALSE;
  }

  // HIDL reply: transport status (int32), then the Result return value
  GBinderReader reader;
  gbinder_remote_reply_init_reader(reply, &reader);

  gint32 hidl_status = -1;
  gint32 result = -1;
  if (!gbinder_reader_read_int32(&reader, &hidl_status) || hidl_status != 0 ||
      !gbinder_reader_read_int32(&reader, &result)) {
    GERR("Bad setConnectedState reply for 0x%08x (hidl status %d)",
         device_type, hidl_status);
    gbinder_remote_reply_unref(reply);
    return FALSE;
  }

  gbinder_remote_reply_unref(reply);

  if (result != RESULT_OK) {
    GERR("setConnectedState for 0x%08x returned Result %d", device_type,
         result);
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

  if (!audio_hal_set_connected_state(state, AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET, address,
                                     connected)) {
    return FALSE;
  }

  if (!audio_hal_set_connected_state(state, AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET, address,
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
