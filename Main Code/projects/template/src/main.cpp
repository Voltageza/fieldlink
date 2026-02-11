/************************************************************
 * FIELDLINK PROJECT TEMPLATE
 * Board: ESP32-S3 POE ETH 8DI 8DO (Waveshare)
 *
 * Starter template showing how to use FieldLinkCore.
 * Copy this project folder and customize for your application.
 ************************************************************/

#include <FieldLinkCore.h>
#include "secrets.h"

#define FW_NAME    "FieldLink Template"
#define FW_VERSION "1.0.0"
#define HW_TYPE    "TEMPLATE_ESP32S3"

/* ================= MQTT HANDLER ================= */

void myMqttHandler(const char* cmd, unsigned int length) {
  // Handle project-specific MQTT commands here
  Serial.printf("Project MQTT command: %s\n", cmd);
}

/* ================= SERIAL HANDLER ================= */

bool mySerialHandler(const String& input) {
  // Handle project-specific serial commands here
  // Return true if handled, false to fall through to base handler
  if (input == "HELLO") {
    Serial.println("Hello from template project!");
    return true;
  }
  return false;
}

/* ================= SETUP ================= */

void setup() {
  // Configure shared library with secrets
  FieldLink::setWebAuth(WEB_AUTH_USER, WEB_AUTH_PASS);
  FieldLink::setOtaPassword(OTA_PASSWORD);
  FieldLink::setDefaultMqtt(DEFAULT_MQTT_HOST, DEFAULT_MQTT_PORT,
                             DEFAULT_MQTT_USER, DEFAULT_MQTT_PASS);
  FieldLink::setNotificationUrl(NOTIFICATION_WEBHOOK_URL);

  // Initialize all board subsystems
  FieldLink::begin(FW_NAME, FW_VERSION, HW_TYPE);

  // Register project handlers
  FieldLink::setMqttHandler(myMqttHandler);
  FieldLink::setSerialHandler(mySerialHandler);

  // Add project-specific web routes here
  // AsyncWebServer& server = FieldLink::getWebServer();
  // server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) { ... });

  // Start web server (after all routes are registered)
  FieldLink::startWebServer();

  Serial.println("Template project ready.");
}

/* ================= LOOP ================= */

void loop() {
  // Shared library tick (OTA, serial, MQTT)
  FieldLink::tick();

  // Read digital inputs
  uint8_t di = fl_readDI();

  // Read sensors periodically
  // fl_readSensors();

  // Your project logic here

  delay(10);
}
