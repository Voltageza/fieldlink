#ifndef FL_SERIAL_H
#define FL_SERIAL_H

#include <Arduino.h>

// Project serial command handler callback
// Return true if the command was handled, false to fall through to base handler
typedef bool (*FL_SerialHandler)(const String& input);
void fl_setSerialHandler(FL_SerialHandler handler);

// Process serial input (call from tick or loop)
void fl_handleSerial();

#endif // FL_SERIAL_H
