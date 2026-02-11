#include "fl_serial.h"
#include "fl_board.h"
#include "fl_modbus.h"
#include "fl_storage.h"
#include "fl_comms.h"
#include <WiFi.h>
#include <Ethernet.h>

// Forward declarations from fl_comms.cpp
const char* fl_getFwName();
const char* fl_getFwVersion();

static FL_SerialHandler _projectSerialHandler = nullptr;

void fl_setSerialHandler(FL_SerialHandler handler) {
  _projectSerialHandler = handler;
}

void fl_handleSerial() {
  if (!Serial.available()) return;

  String input = Serial.readStringUntil('\n');
  input.trim();
  if (input.length() == 0) return;

  // Let project handler try first for project-specific commands
  if (_projectSerialHandler && _projectSerialHandler(input)) {
    return;  // Project handled it
  }

  // Base commands
  if (input == "STATUS") {
    Serial.println("\n=== SYSTEM STATUS ===");
    Serial.printf("Firmware: %s v%s\n", fl_getFwName(), fl_getFwVersion());
    Serial.printf("Device ID: %s\n", fl_deviceId);
    Serial.printf("Setup AP: %s\n", fl_apName);
    Serial.printf("Uptime: %lu seconds\n", millis() / 1000);
    Serial.println("\n--- Connectivity ---");
    Serial.printf("WiFi: %s\n", fl_wifiConnected ? "Connected" : "Disconnected");
    if (fl_wifiConnected) {
      Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
      Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    }
    if (fl_useEthernet) {
      Serial.printf("Ethernet: %s\n", fl_ethernetConnected ? "Connected" : "Disconnected");
      if (fl_ethernetConnected) {
        Serial.printf("Ethernet IP: %s\n", Ethernet.localIP().toString().c_str());
      }
    }
    Serial.printf("MQTT: %s\n", fl_mqttConnected ? "Connected" : "Disconnected");
    if (fl_mqttConnected && fl_lastMqttActivity > 0) {
      Serial.printf("MQTT last activity: %lu seconds ago\n", (millis() - fl_lastMqttActivity) / 1000);
    }
    Serial.printf("Sensor: %s\n", fl_sensorOnline ? "Online" : "Offline");
    Serial.println("\n--- MQTT Topics ---");
    Serial.printf("Telemetry: %s\n", fl_topicTelemetry);
    Serial.printf("Command: %s\n", fl_topicCommand);
    Serial.printf("Status: %s (LWT)\n", fl_topicStatus);
  }
  else if (input == "REBOOT") {
    Serial.println("Rebooting...");
    delay(500);
    ESP.restart();
  }
  else if (input == "FACTORY_RESET") {
    fl_factoryReset();
  }
  else if (input == "WIFI_RESET") {
    fl_wifiReset();
  }
  else if (input == "I2CTEST") {
    fl_i2cTest();
  }
  else if (input.startsWith("DO") && input.length() >= 4) {
    int ch = input.charAt(2) - '1';
    bool on = input.endsWith("ON");
    if (ch >= 0 && ch < 8) {
      fl_setDO(ch, on);
      Serial.printf("DO%d set to %s (channel %d, do_state=0x%02X)\n", ch+1, on?"ON":"OFF", ch, fl_do_state);
    }
  }
  else if (input == "HELP") {
    Serial.println("\n=== SERIAL COMMANDS (Base) ===");
    Serial.println("STATUS       - Show system status");
    Serial.println("DOxON/DOxOFF - Control digital output (x=1-8)");
    Serial.println("I2CTEST      - Test I2C communication with TCA9554");
    Serial.println("WIFI_RESET   - Clear WiFi and restart setup portal");
    Serial.println("REBOOT       - Restart device");
    Serial.println("FACTORY_RESET- Clear all settings");
    Serial.println("HELP         - Show this help");
    // Let project add its commands to HELP
    if (_projectSerialHandler) {
      _projectSerialHandler(String("HELP"));
    }
  }
  else {
    Serial.printf("Unknown command: %s (type HELP)\n", input.c_str());
  }
}
