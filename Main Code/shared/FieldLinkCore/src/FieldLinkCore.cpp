#include "FieldLinkCore.h"
#include <ArduinoOTA.h>

void fl_begin() {
  // CRITICAL: I2C bus recovery - release stuck bus from previous crash
  fl_i2cBusRecovery();

  // Initialize I2C and outputs FIRST to prevent floating pins
  Wire.begin(FL_I2C_SDA, FL_I2C_SCL);
  fl_initDO();

  // Serial
  Serial.begin(115200);
  delay(3000);  // Wait for USB CDC to enumerate

  // Initialize NVS
  fl_initNVS();

  Serial.println("Type 'HELP' for serial commands");

  // Initialize digital inputs
  fl_initDI();

  // Initialize RS485 + Modbus
  fl_initModbus();
}

void fl_tick() {
  // Handle OTA updates
  ArduinoOTA.handle();

  // Handle serial commands
  fl_handleSerial();

  // MQTT reconnect and loop
  if (fl_configLoaded) {
    fl_reconnectMQTT();
    fl_mqtt.loop();
  }

  // Read digital inputs
  fl_readDI();
}
