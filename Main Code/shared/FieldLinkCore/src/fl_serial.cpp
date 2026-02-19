#include "fl_serial.h"
#include "fl_board.h"
#include "fl_modbus.h"
#include "fl_storage.h"
#include "fl_comms.h"
#include "fl_ota.h"
#include "fl_pins.h"
#include <WiFi.h>

static fl_serial_callback_t _serialProjectCallback = nullptr;

void fl_setSerialCallback(fl_serial_callback_t callback) {
  _serialProjectCallback = callback;
}

void fl_handleSerial() {
  if (!Serial.available()) return;

  String input = Serial.readStringUntil('\n');
  input.trim();

  if (input == "WIFI_RESET") {
    Serial.println("\n=== WIFI RESET ===");
    Serial.println("Clearing saved WiFi credentials...");
    fl_wifiManager.resetSettings();
    Serial.println("WiFi credentials cleared! Restarting into setup mode...");
    delay(1000);
    ESP.restart();
  }
  else if (input == "STATUS") {
    Serial.println("\n=== SYSTEM STATUS ===");
    Serial.printf("Firmware: %s v%s\n", fl_getFwName(), fl_getFwVersion());
    Serial.printf("Device ID: %s\n", fl_DEVICE_ID);
    Serial.printf("Setup AP: %s\n", fl_AP_NAME);
    Serial.printf("Uptime: %lu seconds\n", millis() / 1000);
    Serial.println("\n--- Connectivity ---");
    Serial.printf("WiFi: %s\n", fl_wifiConnected ? "Connected" : "Disconnected");
    if (fl_wifiConnected) {
      Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
      Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    }
    Serial.printf("MQTT: %s\n", fl_mqttConnected ? "Connected" : "Disconnected");
    if (fl_mqttConnected && fl_lastMqttActivity > 0) {
      Serial.printf("MQTT last activity: %lu seconds ago\n", (millis() - fl_lastMqttActivity) / 1000);
    }
    Serial.printf("Sensor: %s\n", fl_sensorOnline ? "Online" : "Offline");
    Serial.println("\n--- MQTT Topics ---");
    Serial.printf("Telemetry: %s\n", fl_TOPIC_TELEMETRY);
    Serial.printf("Command: %s\n", fl_TOPIC_COMMAND);
    Serial.printf("Status: %s (LWT)\n", fl_TOPIC_STATUS);
    // Forward to project for additional pump-specific status info
    if (_serialProjectCallback) {
      _serialProjectCallback(input);
    }
  }
  else if (input == "REBOOT") {
    Serial.println("Rebooting...");
    delay(500);
    ESP.restart();
  }
  else if (input == "FACTORY_RESET") {
    Serial.println("Clearing all settings and restarting...");
    fl_wifiManager.resetSettings();
    fl_preferences.begin("fieldlink", false);
    fl_preferences.clear();
    fl_preferences.end();
    Serial.println("All settings cleared! Device will restart in setup mode...");
    delay(500);
    ESP.restart();
  }
  else if (input == "I2CTEST") {
    Serial.println("Testing I2C TCA9554...");
    Wire.beginTransmission(FL_TCA9554_ADDR);
    uint8_t err = Wire.endTransmission();
    Serial.printf("I2C probe result: %d (0=OK)\n", err);

    Wire.beginTransmission(FL_TCA9554_ADDR);
    Wire.write(0x01);  // Output port register
    Wire.endTransmission();
    Wire.requestFrom(FL_TCA9554_ADDR, (uint8_t)1);
    if (Wire.available()) {
      uint8_t val = Wire.read();
      Serial.printf("TCA9554 output register: 0x%02X (expected: 0x%02X)\n", val, fl_do_state);
    } else {
      Serial.println("Failed to read from TCA9554");
    }
  }
  else if (input.startsWith("DO") && input.length() >= 4) {
    // DOxON or DOxOFF where x is 1-8
    int ch = input.charAt(2) - '1';  // Convert '1'-'8' to 0-7
    bool on = input.endsWith("ON");
    if (ch >= 0 && ch < 8) {
      fl_setDO(ch, on);
      Serial.printf("DO%d set to %s (channel %d, do_state=0x%02X)\n", ch+1, on?"ON":"OFF", ch, fl_do_state);
    }
  }
  else if (input == "HELP") {
    Serial.println("\n=== SERIAL COMMANDS ===");
    Serial.println("STATUS       - Show system status");
    Serial.println("REBOOT       - Restart device");
    Serial.println("WIFI_RESET   - Clear WiFi and restart setup portal");
    Serial.println("FACTORY_RESET- Clear all settings");
    Serial.println("DOxON/DOxOFF - Control any DO (x=1-8)");
    Serial.println("I2CTEST      - Test I2C communication with TCA9554");
    // Forward to project for additional help text
    if (_serialProjectCallback) {
      _serialProjectCallback(input);
    }
  }
  else {
    // Forward unknown commands to project
    if (_serialProjectCallback) {
      _serialProjectCallback(input);
    }
  }
}
