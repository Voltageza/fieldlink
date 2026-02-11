#include "FieldLinkCore.h"
#include <WiFi.h>
#include <Ethernet.h>

// Forward declarations from other modules
void fl_setFirmwareInfo(const char* name, const char* version, const char* hwType);
void fl_setDefaultMqtt(const char* host, uint16_t port, const char* user, const char* pass);

namespace FieldLink {

void setDefaultMqtt(const char* host, uint16_t port, const char* user, const char* pass) {
  ::fl_setDefaultMqtt(host, port, user, pass);
}

void begin(const char* fwName, const char* fwVersion, const char* hwType,
           bool benchTestMode) {
  // Store firmware info for web/serial
  fl_setFirmwareInfo(fwName, fwVersion, hwType);

  // Phase 1: Hardware initialization
  fl_initI2C();
  fl_initDO();

  Serial.begin(115200);
  delay(3000);  // Wait for USB CDC to enumerate

  Serial.println("\n\n*** ESP32 BOOT ***");
  Serial.println(fwName);
  Serial.printf("Version: %s\n", fwVersion);
  if (benchTestMode) {
    Serial.println("*** BENCH TEST MODE ***");
  }
  Serial.flush();

  fl_initNVS();
  Serial.println("Type 'HELP' for serial commands");

  fl_initDI();
  fl_initModbus();

  // Phase 2: Network initialization
  WiFi.mode(WIFI_STA);
  fl_wifiRestoreFix();
  WiFi.persistent(false);
  delay(100);

  fl_generateDeviceId();
  fl_printDeviceInfo();

  if (!fl_initNetwork()) {
    Serial.println("Failed to connect to network. Restarting...");
    delay(3000);
    ESP.restart();
  }

  // Force disable any rogue AP
  if (!fl_useEthernet) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    Serial.println("Soft AP disabled, WiFi in STA mode");
  } else {
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi disabled (Ethernet mode)");
  }

  // Phase 3: Services
  // Configure NTP for time sync (GMT+2 for South Africa)
  configTime(2 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("NTP configured");

  fl_loadMqttConfig();
  fl_setupBaseWebRoutes();
  fl_connectMQTT();
  fl_initArduinoOTA();

  Serial.printf("\n=== Network: %s ===\n", fl_useEthernet ? "ETHERNET (priority)" : "WiFi");
  Serial.printf("IP Address: %s\n",
    fl_useEthernet ? Ethernet.localIP().toString().c_str() : WiFi.localIP().toString().c_str());
  Serial.println("Core initialization complete.");
}

void tick() {
  fl_handleOTA();
  fl_handleSerial();

  if (fl_configLoaded) {
    fl_reconnectMQTT();
    fl_mqtt.loop();
  }
}

} // namespace FieldLink
