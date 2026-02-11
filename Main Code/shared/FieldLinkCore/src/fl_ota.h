#ifndef FL_OTA_H
#define FL_OTA_H

#include <Arduino.h>

// Initialize ArduinoOTA for wireless firmware uploads
void fl_initArduinoOTA();

// Handle OTA in main loop
void fl_handleOTA();

// Perform remote firmware update via HTTP URL
void fl_performRemoteOTA(const char* firmwareUrl);

// Set OTA password (call before begin)
void fl_setOtaPassword(const char* pass);

#endif // FL_OTA_H
