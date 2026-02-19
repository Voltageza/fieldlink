#ifndef FL_SERIAL_H
#define FL_SERIAL_H

#include <Arduino.h>

// Serial command callback type
typedef void (*fl_serial_callback_t)(const String& cmd);

// Set project serial command callback
void fl_setSerialCallback(fl_serial_callback_t callback);

// Handle serial commands (call in loop via fl_tick)
void fl_handleSerial();

#endif
