#include "fl_modbus.h"
#include "fl_pins.h"
#include <ModbusMaster.h>

float fl_Va = 0, fl_Vb = 0, fl_Vc = 0;
float fl_Ia = 0, fl_Ib = 0, fl_Ic = 0;
bool  fl_sensorOnline = false;
int   fl_modbusFailCount = 0;

static HardwareSerial RS485(2);
static ModbusMaster node;

static void preTransmission()  { digitalWrite(RS485_DE, HIGH); }
static void postTransmission() { digitalWrite(RS485_DE, LOW); }

void fl_initModbus() {
  pinMode(RS485_DE, OUTPUT);
  digitalWrite(RS485_DE, LOW);

  RS485.begin(RS485_BAUDRATE, SERIAL_8N1, RS485_RX, RS485_TX);
  node.begin(MODBUS_ID, RS485);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);
  Serial.println("RS485/Modbus initialized");
}

float fl_registersToFloat(uint16_t high, uint16_t low) {
  uint32_t combined = ((uint32_t)high << 16) | low;
  float result;
  memcpy(&result, &combined, sizeof(float));
  return result;
}

bool fl_isValidCurrent(float current) {
  if (isnan(current) || isinf(current)) return false;
  if (current < FL_MIN_VALID_CURRENT || current > FL_MAX_VALID_CURRENT) return false;
  return true;
}

bool fl_isValidVoltage(float voltage) {
  if (isnan(voltage) || isinf(voltage)) return false;
  if (voltage < 0 || voltage > FL_MAX_VALID_VOLTAGE) return false;
  return true;
}

bool fl_readSensors() {
  // Read voltage (0x0000-0x0005) and current (0x0006-0x000B) in one transaction
  uint8_t result = node.readInputRegisters(0x0000, 12);

  if (result != node.ku8MBSuccess) {
    fl_modbusFailCount++;
    if (fl_modbusFailCount >= FL_MAX_MODBUS_FAILURES) {
      if (fl_sensorOnline) {
        Serial.println("ERROR: Modbus sensor offline!");
        fl_sensorOnline = false;
      }
    }
    return false;
  }

  if (!fl_sensorOnline) {
    Serial.println("Modbus sensor online");
  }
  fl_modbusFailCount = 0;
  fl_sensorOnline = true;

  // Parse voltages (registers 0-5)
  float newVa = fl_registersToFloat(node.getResponseBuffer(0), node.getResponseBuffer(1));
  float newVb = fl_registersToFloat(node.getResponseBuffer(2), node.getResponseBuffer(3));
  float newVc = fl_registersToFloat(node.getResponseBuffer(4), node.getResponseBuffer(5));

  // Parse currents (registers 6-11)
  float newIa = fl_registersToFloat(node.getResponseBuffer(6), node.getResponseBuffer(7));
  float newIb = fl_registersToFloat(node.getResponseBuffer(8), node.getResponseBuffer(9));
  float newIc = fl_registersToFloat(node.getResponseBuffer(10), node.getResponseBuffer(11));

  // Validate voltages
  if (fl_isValidVoltage(newVa) && fl_isValidVoltage(newVb) && fl_isValidVoltage(newVc)) {
    fl_Va = newVa;
    fl_Vb = newVb;
    fl_Vc = newVc;
  }

  // Validate currents
  if (!fl_isValidCurrent(newIa) || !fl_isValidCurrent(newIb) || !fl_isValidCurrent(newIc)) {
    Serial.printf("WARNING: Invalid current reading: Ia=%.2f Ib=%.2f Ic=%.2f\n", newIa, newIb, newIc);
    return false;
  }

  fl_Ia = newIa;
  fl_Ib = newIb;
  fl_Ic = newIc;

  return true;
}
