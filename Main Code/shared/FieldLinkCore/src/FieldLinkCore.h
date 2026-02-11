#ifndef FIELDLINK_CORE_H
#define FIELDLINK_CORE_H

// FieldLinkCore - Shared board support library for
// Waveshare ESP32-S3 POE-ETH-8DI-8DO projects

// Include all sub-modules
#include "fl_pins.h"
#include "fl_board.h"
#include "fl_modbus.h"
#include "fl_storage.h"
#include "fl_comms.h"
#include "fl_ota.h"
#include "fl_web.h"
#include "fl_telegram.h"
#include "fl_serial.h"

#include <ArduinoJson.h>

namespace FieldLink {

  // === Configuration (call before begin) ===

  // Set web interface authentication credentials
  inline void setWebAuth(const char* user, const char* pass) {
    fl_setWebAuth(user, pass);
  }

  // Set ArduinoOTA password
  inline void setOtaPassword(const char* pass) {
    fl_setOtaPassword(pass);
  }

  // Set default MQTT broker credentials (used when NVS has no saved config)
  void setDefaultMqtt(const char* host, uint16_t port, const char* user, const char* pass);

  // Set Telegram/webhook notification URL
  inline void setNotificationUrl(const char* url) {
    fl_setNotificationUrl(url);
  }

  // === Initialization ===

  // Initialize all board subsystems
  // Call after configuration setters, before handlers
  void begin(const char* fwName, const char* fwVersion, const char* hwType,
             bool benchTestMode = false);

  // === Callbacks ===

  // Set project MQTT command handler
  inline void setMqttHandler(FL_MqttHandler handler) {
    fl_setMqttHandler(handler);
  }

  // Set project serial command handler
  inline void setSerialHandler(FL_SerialHandler handler) {
    fl_setSerialHandler(handler);
  }

  // === Web Server ===

  // Get the web server for adding project-specific routes
  inline AsyncWebServer& getWebServer() {
    return fl_getWebServer();
  }

  // Start the web server (call after all routes are added)
  inline void startWebServer() {
    fl_startWebServer();
  }

  // === Main Loop ===

  // Call every loop iteration
  // Handles: OTA, serial commands, MQTT reconnect + message loop
  void tick();
}

#endif // FIELDLINK_CORE_H
