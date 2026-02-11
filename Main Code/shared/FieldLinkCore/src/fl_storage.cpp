#include "fl_storage.h"
#include <Preferences.h>
#include <WiFi.h>
#include <nvs_flash.h>
#include "esp_wifi.h"

// Default MQTT credentials (set by project before begin())
static const char* _defaultMqttHost = "";
static uint16_t    _defaultMqttPort = 8883;
static const char* _defaultMqttUser = "";
static const char* _defaultMqttPass = "";

char fl_deviceId[16] = "";
char fl_apName[32] = "";
char fl_topicTelemetry[64] = "";
char fl_topicCommand[64] = "";
char fl_topicStatus[64] = "";
char fl_topicSubscribe[64] = "";

char     fl_mqttHost[128] = "";
uint16_t fl_mqttPort = 8883;
char     fl_mqttUser[64] = "";
char     fl_mqttPass[64] = "";
bool     fl_mqttUseTls = true;

void fl_setDefaultMqtt(const char* host, uint16_t port, const char* user, const char* pass) {
  _defaultMqttHost = host;
  _defaultMqttPort = port;
  _defaultMqttUser = user;
  _defaultMqttPass = pass;
}

void fl_initNVS() {
  Serial.println("Initializing NVS...");
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.println("Erasing NVS...");
    nvs_flash_erase();
    nvs_flash_init();
  }
}

void fl_generateDeviceId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);

  snprintf(fl_deviceId, sizeof(fl_deviceId), "FL-%02X%02X%02X", mac[3], mac[4], mac[5]);
  snprintf(fl_apName, sizeof(fl_apName), "FieldLink-%02X%02X%02X", mac[3], mac[4], mac[5]);
  snprintf(fl_topicTelemetry, sizeof(fl_topicTelemetry), "fieldlink/%s/telemetry", fl_deviceId);
  snprintf(fl_topicCommand, sizeof(fl_topicCommand), "fieldlink/%s/command", fl_deviceId);
  snprintf(fl_topicStatus, sizeof(fl_topicStatus), "fieldlink/%s/status", fl_deviceId);
  snprintf(fl_topicSubscribe, sizeof(fl_topicSubscribe), "fieldlink/%s/#", fl_deviceId);
}

void fl_printDeviceInfo() {
  Serial.println("\n========================================");
  Serial.printf("  DEVICE ID: %s\n", fl_deviceId);
  Serial.println("========================================");
  Serial.printf("  WiFi AP Name: %s\n", fl_apName);
  Serial.printf("  Telemetry Topic: %s\n", fl_topicTelemetry);
  Serial.printf("  Command Topic:   %s\n", fl_topicCommand);
  Serial.println("========================================\n");
}

void fl_loadMqttConfig() {
  Preferences prefs;
  prefs.begin("mqtt", true);

  String host = prefs.getString("host", "");
  if (host.length() > 0) {
    strncpy(fl_mqttHost, host.c_str(), sizeof(fl_mqttHost) - 1);
  } else {
    strncpy(fl_mqttHost, _defaultMqttHost, sizeof(fl_mqttHost) - 1);
  }

  fl_mqttPort = prefs.getUShort("port", _defaultMqttPort);
  fl_mqttUseTls = prefs.getBool("tls", true);

  String user = prefs.getString("user", "");
  String pass = prefs.getString("pass", "");

  if (user.length() > 0) {
    strncpy(fl_mqttUser, user.c_str(), sizeof(fl_mqttUser) - 1);
    strncpy(fl_mqttPass, pass.c_str(), sizeof(fl_mqttPass) - 1);
  } else {
    strncpy(fl_mqttUser, _defaultMqttUser, sizeof(fl_mqttUser) - 1);
    strncpy(fl_mqttPass, _defaultMqttPass, sizeof(fl_mqttPass) - 1);
  }

  prefs.end();

  Serial.println("MQTT Config loaded:");
  Serial.printf("  Host: %s:%d\n", fl_mqttHost, fl_mqttPort);
  Serial.printf("  User: %s\n", fl_mqttUser);
  Serial.printf("  TLS: %s\n", fl_mqttUseTls ? "yes" : "no");
}

void fl_saveMqttConfig() {
  Preferences prefs;
  prefs.begin("mqtt", false);
  prefs.putString("host", fl_mqttHost);
  prefs.putUShort("port", fl_mqttPort);
  prefs.putString("user", fl_mqttUser);
  prefs.putString("pass", fl_mqttPass);
  prefs.putBool("tls", fl_mqttUseTls);
  prefs.end();
  Serial.println("MQTT Config saved");
}

void fl_resetMqttConfig() {
  Preferences prefs;
  prefs.begin("mqtt", false);
  prefs.clear();
  prefs.end();

  strncpy(fl_mqttHost, _defaultMqttHost, sizeof(fl_mqttHost) - 1);
  fl_mqttPort = _defaultMqttPort;
  strncpy(fl_mqttUser, _defaultMqttUser, sizeof(fl_mqttUser) - 1);
  strncpy(fl_mqttPass, _defaultMqttPass, sizeof(fl_mqttPass) - 1);
  fl_mqttUseTls = true;

  Serial.println("MQTT Config reset to defaults");
}

void fl_wifiRestoreFix() {
  Preferences prefs;
  prefs.begin("fieldlink", false);
  bool wifiRestoreDone = prefs.getBool("wifi_restored", false);
  if (!wifiRestoreDone) {
    Serial.println("First boot: clearing rogue AP config from NVS...");
    esp_wifi_restore();
    prefs.putBool("wifi_restored", true);
    Serial.println("WiFi config cleared. This will only happen once.");
  }
  prefs.end();
}
