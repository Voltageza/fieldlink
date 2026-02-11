#include "fl_comms.h"
#include "fl_storage.h"
#include "fl_ota.h"
#include "fl_board.h"
#include "fl_pins.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <Ethernet.h>
#include <Preferences.h>

// Connection state
bool fl_wifiConnected = false;
bool fl_ethernetConnected = false;
bool fl_mqttConnected = false;
bool fl_useEthernet = false;
bool fl_configLoaded = false;
unsigned long fl_lastMqttActivity = 0;
int fl_mqttPublishFailCount = 0;

// Internal state
static unsigned long _lastMqttRetry = 0;
static int _mqttConnectFailCount = 0;
static FL_MqttHandler _projectMqttHandler = nullptr;

// Firmware info (set during begin)
static const char* _fwName = "";
static const char* _fwVersion = "";
static const char* _hwType = "";

// Clients
static WiFiClientSecure _espClientSecure;
static WiFiClient _espClientInsecure;
static EthernetClient _ethClient;
static byte _ethMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
PubSubClient fl_mqtt;

// WiFiManager (used internally)
static WiFiManager _wifiManager;

void fl_setMqttHandler(FL_MqttHandler handler) {
  _projectMqttHandler = handler;
}

void fl_setFirmwareInfo(const char* name, const char* version, const char* hwType) {
  _fwName = name;
  _fwVersion = version;
  _hwType = hwType;
}

const char* fl_getFwName()    { return _fwName; }
const char* fl_getFwVersion() { return _fwVersion; }
const char* fl_getHwType()    { return _hwType; }

// Internal MQTT callback - dispatches to project handler
static void _mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (length >= FL_MAX_PAYLOAD_SIZE) {
    Serial.println("MQTT payload too large, ignoring");
    return;
  }

  if (strcmp(topic, fl_topicCommand) != 0) {
    return;
  }

  fl_lastMqttActivity = millis();

  char cmd[FL_MAX_PAYLOAD_SIZE];
  memcpy(cmd, payload, length);
  cmd[length] = '\0';

  Serial.print("MQTT CMD: "); Serial.println(cmd);

  // Handle core commands
  // Try parsing as JSON for UPDATE_FIRMWARE
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, cmd);
  if (!error) {
    const char* command = doc["command"];
    if (command && strcmp(command, "UPDATE_FIRMWARE") == 0) {
      const char* firmwareUrl = doc["url"];
      if (firmwareUrl) {
        Serial.printf("Remote firmware update requested: %s\n", firmwareUrl);
        // Stop any project operations (project should handle via its own safety)
        fl_mqtt.publish(fl_topicTelemetry, "{\"status\":\"updating\"}");
        fl_performRemoteOTA(firmwareUrl);
      } else {
        Serial.println("UPDATE_FIRMWARE command missing 'url' parameter");
      }
      return;
    }
  }

  // Forward to project handler
  if (_projectMqttHandler) {
    _projectMqttHandler(cmd, length);
  }
}

bool fl_initEthernet() {
  Serial.println("\n=== Initializing Ethernet ===");

  pinMode(ETH_RST, OUTPUT);
  digitalWrite(ETH_RST, LOW);
  delay(50);
  digitalWrite(ETH_RST, HIGH);
  delay(50);

  SPI.begin(ETH_SCLK, ETH_MISO, ETH_MOSI, ETH_CS);
  Ethernet.init(ETH_CS);

  WiFi.macAddress(_ethMac);
  _ethMac[0] = (_ethMac[0] | 0x02) & 0xFE;

  Serial.printf("Ethernet MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                _ethMac[0], _ethMac[1], _ethMac[2], _ethMac[3], _ethMac[4], _ethMac[5]);

  Serial.println("Requesting IP via DHCP...");
  if (Ethernet.begin(_ethMac, 10000)) {
    Serial.printf("Ethernet connected! IP: %s\n", Ethernet.localIP().toString().c_str());
    Serial.printf("Gateway: %s\n", Ethernet.gatewayIP().toString().c_str());
    Serial.printf("DNS: %s\n", Ethernet.dnsServerIP().toString().c_str());
    fl_ethernetConnected = true;
    fl_useEthernet = true;
    return true;
  } else {
    Serial.println("Ethernet DHCP failed - no cable or no DHCP server");
    fl_ethernetConnected = false;
    return false;
  }
}

void fl_maintainEthernet() {
  if (fl_useEthernet) {
    switch (Ethernet.maintain()) {
      case 1:
        Serial.println("Ethernet DHCP renew failed");
        fl_ethernetConnected = false;
        break;
      case 2:
        Serial.println("Ethernet DHCP renewed");
        break;
      case 3:
        Serial.println("Ethernet DHCP rebind failed");
        fl_ethernetConnected = false;
        break;
      case 4:
        Serial.println("Ethernet DHCP rebind success");
        break;
    }

    if (Ethernet.linkStatus() == LinkOFF) {
      if (fl_ethernetConnected) {
        Serial.println("Ethernet cable disconnected!");
        fl_ethernetConnected = false;
      }
    } else if (!fl_ethernetConnected) {
      Serial.println("Ethernet cable reconnected, re-init...");
      fl_initEthernet();
    }
  }
}

bool fl_initWiFi() {
  Serial.println("Ethernet not available, using WiFi...");

  _wifiManager.setConfigPortalTimeout(FL_PORTAL_TIMEOUT_S);
  _wifiManager.setAPCallback([](WiFiManager *mgr) {
    Serial.println("\n*** WIFI SETUP MODE ***");
    Serial.printf("Connect to WiFi network: %s\n", fl_apName);
    Serial.println("Then open http://192.168.4.1 in your browser");
    Serial.println("Or wait for the captive portal to appear automatically");
  });
  _wifiManager.setSaveConfigCallback([]() {
    Serial.println("WiFi credentials saved!");
  });

  Serial.println("Connecting to WiFi...");
  if (_wifiManager.autoConnect(fl_apName)) {
    Serial.printf("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
    fl_wifiConnected = true;
    return true;
  }
  return false;
}

bool fl_initNetwork() {
  // Try Ethernet first
  if (fl_initEthernet()) {
    Serial.println("Using Ethernet as primary connection");
    fl_configLoaded = true;
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi disabled (Ethernet active)");
    return true;
  }

  // Fall back to WiFi
  if (fl_initWiFi()) {
    fl_configLoaded = true;
    return true;
  }

  return false;
}

bool fl_connectMQTT() {
  if (!fl_ethernetConnected && !fl_wifiConnected) return false;

  Serial.printf("Connecting to MQTT: %s:%d (TLS: %s, via %s)\n",
                fl_mqttHost, fl_mqttPort, fl_mqttUseTls ? "yes" : "no",
                fl_useEthernet ? "Ethernet" : "WiFi");

  if (fl_useEthernet) {
    if (fl_mqttUseTls) {
      Serial.println("WARNING: TLS not supported over Ethernet, using non-TLS on port 1883");
      fl_mqttPort = 1883;
    }
    fl_mqtt.setClient(_ethClient);
  } else if (fl_mqttUseTls) {
    _espClientSecure.setInsecure();
    fl_mqtt.setClient(_espClientSecure);
  } else {
    fl_mqtt.setClient(_espClientInsecure);
  }

  fl_mqtt.setServer(fl_mqttHost, fl_mqttPort);
  fl_mqtt.setBufferSize(512);
  fl_mqtt.setKeepAlive(FL_MQTT_KEEPALIVE_S);
  fl_mqtt.setCallback(_mqttCallback);

  unsigned long startTime = millis();
  while (!fl_mqtt.connected()) {
    if (fl_mqtt.connect(fl_deviceId, fl_mqttUser, fl_mqttPass, fl_topicStatus, 0, true, "offline")) {
      fl_mqtt.subscribe(fl_topicSubscribe);
      fl_mqtt.publish(fl_topicStatus, "online", true);
      fl_lastMqttActivity = millis();
      Serial.printf("MQTT connected as %s!\n", fl_deviceId);
      Serial.printf("Subscribed to: %s\n", fl_topicSubscribe);
      Serial.printf("Status topic: %s (LWT enabled)\n", fl_topicStatus);
      fl_mqttConnected = true;
      return true;
    }

    if (millis() - startTime > FL_MQTT_TIMEOUT_MS) {
      Serial.printf("MQTT connection TIMEOUT (rc=%d)\n", fl_mqtt.state());
      fl_mqttConnected = false;
      return false;
    }
    delay(500);
  }

  fl_mqttConnected = true;
  return true;
}

void fl_reconnectMQTT() {
  bool networkOk = false;

  if (fl_useEthernet) {
    fl_maintainEthernet();
    networkOk = fl_ethernetConnected;

    if (!networkOk) {
      Serial.println("Ethernet down, checking WiFi...");
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Falling back to WiFi");
        fl_useEthernet = false;
        fl_wifiConnected = true;
        networkOk = true;
        fl_mqtt.disconnect();
        fl_mqttConnected = false;
      }
    }
  } else {
    if (WiFi.status() != WL_CONNECTED) {
      if (fl_wifiConnected) {
        Serial.println("WiFi disconnected!");
        fl_wifiConnected = false;
        fl_mqttConnected = false;
      }
      if (fl_initEthernet()) {
        Serial.println("Switched to Ethernet");
        networkOk = true;
        fl_mqtt.disconnect();
        fl_mqttConnected = false;
      }
    } else {
      if (!fl_wifiConnected) {
        Serial.printf("WiFi reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
        fl_wifiConnected = true;
      }
      networkOk = true;
    }
  }

  if (!networkOk) return;

  if (!fl_mqtt.connected()) {
    unsigned long now = millis();
    if (now - _lastMqttRetry > FL_MQTT_RETRY_INTERVAL) {
      _lastMqttRetry = now;
      Serial.printf("Attempting MQTT reconnect via %s...\n", fl_useEthernet ? "Ethernet" : "WiFi");

      if (fl_useEthernet) {
        fl_mqtt.setClient(_ethClient);
      } else if (fl_mqttUseTls) {
        _espClientSecure.setInsecure();
        fl_mqtt.setClient(_espClientSecure);
      } else {
        fl_mqtt.setClient(_espClientInsecure);
      }

      fl_mqtt.setServer(fl_mqttHost, fl_useEthernet ? 1883 : fl_mqttPort);
      fl_mqtt.setBufferSize(512);
      fl_mqtt.setKeepAlive(FL_MQTT_KEEPALIVE_S);

      if (fl_mqtt.connect(fl_deviceId, fl_mqttUser, fl_mqttPass, fl_topicStatus, 0, true, "offline")) {
        fl_mqtt.subscribe(fl_topicSubscribe);
        fl_mqtt.publish(fl_topicStatus, "online", true);
        fl_lastMqttActivity = millis();
        Serial.printf("MQTT reconnected as %s!\n", fl_deviceId);
        fl_mqttConnected = true;
        _mqttConnectFailCount = 0;
      } else {
        Serial.printf("MQTT reconnect failed, rc=%d\n", fl_mqtt.state());
        fl_mqttConnected = false;
        _mqttConnectFailCount++;

        if (fl_useEthernet && _mqttConnectFailCount >= FL_MAX_MQTT_CONNECT_FAILURES) {
          Serial.println("MQTT over Ethernet failed repeatedly - switching to WiFi for TLS support");
          _mqttConnectFailCount = 0;
          fl_useEthernet = false;
          fl_ethernetConnected = false;

          WiFi.mode(WIFI_STA);
          WiFi.begin();
          Serial.println("Connecting to WiFi...");

          unsigned long wifiStart = millis();
          while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
            delay(500);
            Serial.print(".");
          }

          if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\nWiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
            fl_wifiConnected = true;
            WiFi.softAPdisconnect(true);
          } else {
            Serial.println("\nWiFi connection failed - will retry");
          }
        }
      }
    }
  } else {
    fl_mqttConnected = true;

    unsigned long now = millis();
    if (fl_lastMqttActivity > 0 && (now - fl_lastMqttActivity > FL_MQTT_STALE_TIMEOUT_MS)) {
      Serial.printf("MQTT connection stale (no activity for %lus) - forcing reconnect\n",
                    (now - fl_lastMqttActivity) / 1000);
      fl_mqtt.disconnect();
      fl_mqttConnected = false;
      fl_lastMqttActivity = 0;
    }
  }
}

void fl_wifiReset() {
  Serial.println("\n=== WIFI RESET ===");
  Serial.println("Clearing saved WiFi credentials...");
  _wifiManager.resetSettings();
  Serial.println("WiFi credentials cleared! Restarting into setup mode...");
  delay(1000);
  ESP.restart();
}

void fl_factoryReset() {
  Serial.println("Clearing all settings and restarting...");
  _wifiManager.resetSettings();
  Preferences prefs;
  prefs.begin("fieldlink", false);
  prefs.clear();
  prefs.end();
  prefs.begin("mqtt", false);
  prefs.clear();
  prefs.end();
  Serial.println("All settings cleared! Device will restart in setup mode...");
  delay(500);
  ESP.restart();
}
