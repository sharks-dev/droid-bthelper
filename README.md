# Droid BTHelper

This service monitors BlueZ D-Bus signals for Bluetooth device connection events and forwards
information to Android services to enable headset support.

The service mimics similar services in Android audio stack that enable SCO support for headset 
Bluetooth devices by calling android.hardware.audio service.

