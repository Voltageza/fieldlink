#ifndef FL_COMMS_H
#define FL_COMMS_H

#include <Arduino.h>
#include <PubSubClient.h>

// Connection timeouts
#define FL_PORTAL_TIMEOUT_S    180
#define FL_WIFI_TIMEOUT_MS     30000
#define FL_MQTT_TIMEOUT_MS     10000
#define FL_MQTT_RETRY_INTERVAL 5000
#define FL_MQTT_KEEPALIVE_S    30
#define FL_MQTT_STALE_TIMEOUT_MS 90000
#define FL_MAX_PAYLOAD_SIZE    512
#define FL_MAX_MQTT_PUBLISH_FAILURES 3
#define FL_MAX_MQTT_CONNECT_FAILURES 3

// Connection state
extern bool fl_wifiConnected;
extern bool fl_ethernetConnected;
extern bool fl_mqttConnected;
extern bool fl_useEthernet;
extern bool fl_configLoaded;
extern unsigned long fl_lastMqttActivity;

// MQTT publish failure tracking
extern int fl_mqttPublishFailCount;

// MQTT client
extern PubSubClient fl_mqtt;

// Project MQTT command handler callback
// Called for command topic messages not handled by the core library
typedef void (*FL_MqttHandler)(const char* cmd, unsigned int length);
void fl_setMqttHandler(FL_MqttHandler handler);

// Initialize Ethernet (W5500)
bool fl_initEthernet();

// Maintain Ethernet DHCP lease and link status
void fl_maintainEthernet();

// Initialize WiFi via WiFiManager captive portal
bool fl_initWiFi();

// Initialize network (tries Ethernet first, falls back to WiFi)
bool fl_initNetwork();

// Connect to MQTT broker
bool fl_connectMQTT();

// Reconnect MQTT with network failover
void fl_reconnectMQTT();

// Reset WiFi credentials and restart
void fl_wifiReset();

// Factory reset (clear WiFi + all NVS)
void fl_factoryReset();

#endif // FL_COMMS_H
