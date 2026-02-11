#ifndef FL_MODBUS_H
#define FL_MODBUS_H

#include <Arduino.h>

// Current/voltage validation limits
#define FL_MIN_VALID_CURRENT   -0.5
#define FL_MAX_VALID_CURRENT   500.0
#define FL_MAX_VALID_VOLTAGE   500.0
#define FL_MAX_MODBUS_FAILURES 5

// Sensor data (updated by readSensors)
extern float fl_Va, fl_Vb, fl_Vc;
extern float fl_Ia, fl_Ib, fl_Ic;
extern bool  fl_sensorOnline;
extern int   fl_modbusFailCount;

// Initialize RS485 and ModbusMaster
void fl_initModbus();

// Read voltages and currents from power meter
// Returns true if reading was successful
bool fl_readSensors();

// Utility functions
float fl_registersToFloat(uint16_t high, uint16_t low);
bool fl_isValidCurrent(float current);
bool fl_isValidVoltage(float voltage);

#endif // FL_MODBUS_H
