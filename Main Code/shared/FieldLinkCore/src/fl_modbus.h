#ifndef FL_MODBUS_H
#define FL_MODBUS_H

#include <Arduino.h>
#include <ModbusMaster.h>

// Sensor values
extern float fl_Va, fl_Vb, fl_Vc;
extern float fl_Ia, fl_Ib, fl_Ic;
extern bool fl_sensorOnline;
extern int fl_modbusFailCount;

// ModbusMaster instance
extern ModbusMaster fl_modbusNode;

// Current/voltage validation limits
#define FL_MIN_VALID_CURRENT  -0.5
#define FL_MAX_VALID_CURRENT  500.0
#define FL_MAX_MODBUS_FAILURES 5

// Initialize RS485 and ModbusMaster
void fl_initModbus();

// Read voltages and currents from power meter
bool fl_readSensors();

#endif
