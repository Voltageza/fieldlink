#ifndef FL_STORAGE_H
#define FL_STORAGE_H

#include <Arduino.h>

// Device identity (generated from MAC address)
extern char fl_deviceId[16];      // FL-XXYYZZ
extern char fl_apName[32];        // FieldLink-XXYYZZ
extern char fl_topicTelemetry[64];
extern char fl_topicCommand[64];
extern char fl_topicStatus[64];
extern char fl_topicSubscribe[64];

// MQTT config (stored in NVS, loaded at boot)
extern char     fl_mqttHost[128];
extern uint16_t fl_mqttPort;
extern char     fl_mqttUser[64];
extern char     fl_mqttPass[64];
extern bool     fl_mqttUseTls;

// Initialize NVS flash
void fl_initNVS();

// Generate device ID and topic strings from WiFi MAC
void fl_generateDeviceId();

// Print device info to Serial
void fl_printDeviceInfo();

// MQTT config management (NVS namespace: "mqtt")
void fl_loadMqttConfig();
void fl_saveMqttConfig();
void fl_resetMqttConfig();

// WiFi restore fix (one-time, NVS namespace: "fieldlink")
void fl_wifiRestoreFix();

#endif // FL_STORAGE_H
