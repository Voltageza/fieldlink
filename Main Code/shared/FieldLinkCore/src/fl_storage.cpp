#include "fl_storage.h"
#include <WiFi.h>
#include <nvs_flash.h>
#include "esp_wifi.h"

char fl_DEVICE_ID[16] = "";
char fl_AP_NAME[32] = "";
char fl_TOPIC_TELEMETRY[64] = "";
char fl_TOPIC_COMMAND[64] = "";
char fl_TOPIC_STATUS[64] = "";
char fl_TOPIC_SUBSCRIBE[64] = "";

char fl_mqtt_host[128] = "";
uint16_t fl_mqtt_port = 8883;
char fl_mqtt_user[64] = "";
char fl_mqtt_pass[64] = "";
bool fl_mqtt_use_tls = true;

Preferences fl_preferences;

// Default MQTT values (set via fl_setMqttDefaults)
static char _default_mqtt_host[128] = "";
static uint16_t _default_mqtt_port = 8883;
static char _default_mqtt_user[64] = "";
static char _default_mqtt_pass[64] = "";

void fl_setMqttDefaults(const char* host, uint16_t port, const char* user, const char* pass) {
  strncpy(_default_mqtt_host, host, sizeof(_default_mqtt_host) - 1);
  _default_mqtt_port = port;
  strncpy(_default_mqtt_user, user, sizeof(_default_mqtt_user) - 1);
  strncpy(_default_mqtt_pass, pass, sizeof(_default_mqtt_pass) - 1);
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

void fl_checkWifiRestore() {
  fl_preferences.begin("fieldlink", false);
  bool wifiRestoreDone = fl_preferences.getBool("wifi_restored", false);
  if (!wifiRestoreDone) {
    Serial.println("First boot: clearing rogue AP config from NVS...");
    esp_wifi_restore();
    fl_preferences.putBool("wifi_restored", true);
    Serial.println("WiFi config cleared. This will only happen once.");
  }
  fl_preferences.end();
  WiFi.persistent(false);
  delay(100);
}

void fl_generateDeviceId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);

  snprintf(fl_DEVICE_ID, sizeof(fl_DEVICE_ID), "FL-%02X%02X%02X", mac[3], mac[4], mac[5]);
  snprintf(fl_AP_NAME, sizeof(fl_AP_NAME), "FieldLink-%02X%02X%02X", mac[3], mac[4], mac[5]);
  snprintf(fl_TOPIC_TELEMETRY, sizeof(fl_TOPIC_TELEMETRY), "fieldlink/%s/telemetry", fl_DEVICE_ID);
  snprintf(fl_TOPIC_COMMAND, sizeof(fl_TOPIC_COMMAND), "fieldlink/%s/command", fl_DEVICE_ID);
  snprintf(fl_TOPIC_STATUS, sizeof(fl_TOPIC_STATUS), "fieldlink/%s/status", fl_DEVICE_ID);
  snprintf(fl_TOPIC_SUBSCRIBE, sizeof(fl_TOPIC_SUBSCRIBE), "fieldlink/%s/#", fl_DEVICE_ID);
}

void fl_printDeviceInfo() {
  Serial.println("\n========================================");
  Serial.printf("  DEVICE ID: %s\n", fl_DEVICE_ID);
  Serial.println("========================================");
  Serial.printf("  WiFi AP Name: %s\n", fl_AP_NAME);
  Serial.printf("  Telemetry Topic: %s\n", fl_TOPIC_TELEMETRY);
  Serial.printf("  Command Topic:   %s\n", fl_TOPIC_COMMAND);
  Serial.println("========================================\n");
}

void fl_loadMqttConfig() {
  fl_preferences.begin("mqtt", true);  // Read-only

  String host = fl_preferences.getString("host", "");
  if (host.length() > 0) {
    strncpy(fl_mqtt_host, host.c_str(), sizeof(fl_mqtt_host) - 1);
  } else {
    strncpy(fl_mqtt_host, _default_mqtt_host, sizeof(fl_mqtt_host) - 1);
  }

  fl_mqtt_port = fl_preferences.getUShort("port", _default_mqtt_port);
  fl_mqtt_use_tls = fl_preferences.getBool("tls", true);

  String user = fl_preferences.getString("user", "");
  String pass = fl_preferences.getString("pass", "");

  if (user.length() > 0) {
    strncpy(fl_mqtt_user, user.c_str(), sizeof(fl_mqtt_user) - 1);
    strncpy(fl_mqtt_pass, pass.c_str(), sizeof(fl_mqtt_pass) - 1);
  } else {
    strncpy(fl_mqtt_user, _default_mqtt_user, sizeof(fl_mqtt_user) - 1);
    strncpy(fl_mqtt_pass, _default_mqtt_pass, sizeof(fl_mqtt_pass) - 1);
  }

  fl_preferences.end();

  Serial.println("MQTT Config loaded:");
  Serial.printf("  Host: %s:%d\n", fl_mqtt_host, fl_mqtt_port);
  Serial.printf("  User: %s\n", fl_mqtt_user);
  Serial.printf("  TLS: %s\n", fl_mqtt_use_tls ? "yes" : "no");
}

void fl_saveMqttConfig() {
  fl_preferences.begin("mqtt", false);  // Read-write
  fl_preferences.putString("host", fl_mqtt_host);
  fl_preferences.putUShort("port", fl_mqtt_port);
  fl_preferences.putString("user", fl_mqtt_user);
  fl_preferences.putString("pass", fl_mqtt_pass);
  fl_preferences.putBool("tls", fl_mqtt_use_tls);
  fl_preferences.end();
  Serial.println("MQTT Config saved");
}

void fl_resetMqttConfig() {
  fl_preferences.begin("mqtt", false);
  fl_preferences.clear();
  fl_preferences.end();

  strncpy(fl_mqtt_host, _default_mqtt_host, sizeof(fl_mqtt_host) - 1);
  fl_mqtt_port = _default_mqtt_port;
  strncpy(fl_mqtt_user, _default_mqtt_user, sizeof(fl_mqtt_user) - 1);
  strncpy(fl_mqtt_pass, _default_mqtt_pass, sizeof(fl_mqtt_pass) - 1);
  fl_mqtt_use_tls = true;

  Serial.println("MQTT Config reset to defaults");
}
