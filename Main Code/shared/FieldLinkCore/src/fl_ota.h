#ifndef FL_OTA_H
#define FL_OTA_H

#include <Arduino.h>

// Perform remote firmware update via HTTP download
void fl_performRemoteFirmwareUpdate(const char* firmwareUrl);

// Setup ArduinoOTA for wireless updates
void fl_setupArduinoOTA();

// Set OTA password (call before fl_setupArduinoOTA)
void fl_setOtaPassword(const char* password);

// Set firmware info for identification
void fl_setFirmwareInfo(const char* name, const char* version, const char* hwType);

// Firmware info accessors
const char* fl_getFwName();
const char* fl_getFwVersion();
const char* fl_getHwType();

#endif
