#ifndef FIELDLINKCORE_H
#define FIELDLINKCORE_H

// FieldLinkCore - Shared board support library for FieldLink IoT devices
// Waveshare ESP32-S3 POE-ETH-8DI-8DO board

#include "fl_pins.h"
#include "fl_board.h"
#include "fl_modbus.h"
#include "fl_storage.h"
#include "fl_comms.h"
#include "fl_ota.h"
#include "fl_web.h"
#include "fl_telegram.h"
#include "fl_serial.h"

// Initialize hardware: I2C recovery, TCA9554 DO, DI pins, NVS, RS485/Modbus, Serial
void fl_begin();

// Tick: OTA handle, serial commands, MQTT reconnect+loop, DI read
void fl_tick();

#endif
