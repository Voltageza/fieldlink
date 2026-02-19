#include "fl_modbus.h"
#include "fl_pins.h"

float fl_Va = 0, fl_Vb = 0, fl_Vc = 0;
float fl_Ia = 0, fl_Ib = 0, fl_Ic = 0;
bool fl_sensorOnline = false;
int fl_modbusFailCount = 0;

ModbusMaster fl_modbusNode;
static HardwareSerial fl_RS485(2);

static void preTransmission()  { digitalWrite(FL_RS485_DE, HIGH); }
static void postTransmission() { digitalWrite(FL_RS485_DE, LOW); }

static float registersToFloat(uint16_t high, uint16_t low) {
  uint32_t combined = ((uint32_t)high << 16) | low;
  float result;
  memcpy(&result, &combined, sizeof(float));
  return result;
}

static bool isValidCurrent(float current) {
  if (isnan(current) || isinf(current)) return false;
  if (current < FL_MIN_VALID_CURRENT || current > FL_MAX_VALID_CURRENT) return false;
  return true;
}

static bool isValidVoltage(float voltage) {
  if (isnan(voltage) || isinf(voltage)) return false;
  if (voltage < 0 || voltage > 500) return false;
  return true;
}

void fl_initModbus() {
  pinMode(FL_RS485_DE, OUTPUT);
  digitalWrite(FL_RS485_DE, LOW);

  fl_RS485.begin(FL_RS485_BAUD, SERIAL_8N1, FL_RS485_RX, FL_RS485_TX);
  fl_modbusNode.begin(FL_MODBUS_ID, fl_RS485);
  fl_modbusNode.preTransmission(preTransmission);
  fl_modbusNode.postTransmission(postTransmission);
}

bool fl_readSensors() {
  // Read voltage (0x0000-0x0005) and current (0x0006-0x000B) in one transaction
  uint8_t result = fl_modbusNode.readInputRegisters(0x0000, 12);

  if (result != fl_modbusNode.ku8MBSuccess) {
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
  float newVa = registersToFloat(fl_modbusNode.getResponseBuffer(0), fl_modbusNode.getResponseBuffer(1));
  float newVb = registersToFloat(fl_modbusNode.getResponseBuffer(2), fl_modbusNode.getResponseBuffer(3));
  float newVc = registersToFloat(fl_modbusNode.getResponseBuffer(4), fl_modbusNode.getResponseBuffer(5));

  // Parse currents (registers 6-11)
  float newIa = registersToFloat(fl_modbusNode.getResponseBuffer(6), fl_modbusNode.getResponseBuffer(7));
  float newIb = registersToFloat(fl_modbusNode.getResponseBuffer(8), fl_modbusNode.getResponseBuffer(9));
  float newIc = registersToFloat(fl_modbusNode.getResponseBuffer(10), fl_modbusNode.getResponseBuffer(11));

  // Validate voltages
  if (isValidVoltage(newVa) && isValidVoltage(newVb) && isValidVoltage(newVc)) {
    fl_Va = newVa;
    fl_Vb = newVb;
    fl_Vc = newVc;
  }

  // Validate currents
  if (!isValidCurrent(newIa) || !isValidCurrent(newIb) || !isValidCurrent(newIc)) {
    Serial.printf("WARNING: Invalid current reading: Ia=%.2f Ib=%.2f Ic=%.2f\n", newIa, newIb, newIc);
    return false;
  }

  fl_Ia = newIa;
  fl_Ib = newIb;
  fl_Ic = newIc;

  return true;
}
