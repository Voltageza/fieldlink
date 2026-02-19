#ifndef FL_COMMS_H
#define FL_COMMS_H

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Ethernet.h>
#include <SPI.h>

// Connection timeouts
#define FL_PORTAL_TIMEOUT_S       180
#define FL_WIFI_TIMEOUT_MS        30000
#define FL_MQTT_TIMEOUT_MS        10000
#define FL_MQTT_RETRY_INTERVAL    5000
#define FL_MQTT_KEEPALIVE_S       30
#define FL_MQTT_STALE_TIMEOUT_MS  90000
#define FL_MAX_PAYLOAD_SIZE       1024
#define FL_MAX_MQTT_PUBLISH_FAILURES 3
#define FL_MAX_MQTT_CONNECT_FAILURES 3

// MQTT client
extern PubSubClient fl_mqtt;

// Connection state
extern bool fl_mqttConnected;
extern bool fl_wifiConnected;
extern bool fl_ethernetConnected;
extern bool fl_useEthernet;
extern bool fl_configLoaded;
extern unsigned long fl_lastMqttActivity;

// MQTT publish failure tracking
extern int fl_mqttPublishFailCount;

// WiFiManager instance
extern WiFiManager fl_wifiManager;

// MQTT command callback type: receives (command_string, length)
typedef void (*fl_mqtt_callback_t)(const char* cmd, unsigned int length);

// Set project MQTT command callback
void fl_setMqttCallback(fl_mqtt_callback_t callback);

// Initialize network (Ethernet first, WiFi fallback)
void fl_initNetwork();

// Configure NTP time sync
void fl_initNTP(long gmtOffsetSec);

// Connect to MQTT broker
bool fl_connectMQTT();

// MQTT reconnect logic (call in loop via fl_tick)
void fl_reconnectMQTT();

// Initialize Ethernet
bool fl_initEthernet();

#endif
