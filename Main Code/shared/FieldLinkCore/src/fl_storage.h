#ifndef FL_STORAGE_H
#define FL_STORAGE_H

#include <Arduino.h>
#include <Preferences.h>

// Device identification
extern char fl_DEVICE_ID[16];
extern char fl_AP_NAME[32];
extern char fl_TOPIC_TELEMETRY[64];
extern char fl_TOPIC_COMMAND[64];
extern char fl_TOPIC_STATUS[64];
extern char fl_TOPIC_SUBSCRIBE[64];

// MQTT configuration (loaded from NVS, fallback to defaults)
extern char fl_mqtt_host[128];
extern uint16_t fl_mqtt_port;
extern char fl_mqtt_user[64];
extern char fl_mqtt_pass[64];
extern bool fl_mqtt_use_tls;

// Preferences instance
extern Preferences fl_preferences;

// Set default MQTT credentials (call before fl_loadMqttConfig)
void fl_setMqttDefaults(const char* host, uint16_t port, const char* user, const char* pass);

// Initialize NVS
void fl_initNVS();

// One-time WiFi restore fix
void fl_checkWifiRestore();

// Generate device ID and topic strings from MAC
void fl_generateDeviceId();

// Print device info to serial
void fl_printDeviceInfo();

// MQTT config management
void fl_loadMqttConfig();
void fl_saveMqttConfig();
void fl_resetMqttConfig();

#endif
