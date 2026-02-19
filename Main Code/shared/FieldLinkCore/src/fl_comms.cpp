#include "fl_comms.h"
#include "fl_pins.h"
#include "fl_storage.h"
#include "fl_ota.h"
#include <ArduinoJson.h>

// Network clients
static WiFiClientSecure fl_espClientSecure;
static WiFiClient fl_espClientInsecure;
static EthernetClient fl_ethClient;
PubSubClient fl_mqtt;

// Ethernet MAC address (will be derived from WiFi MAC)
static byte ethMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};

// Connection state
bool fl_mqttConnected = false;
bool fl_wifiConnected = false;
bool fl_ethernetConnected = false;
bool fl_useEthernet = false;
bool fl_configLoaded = false;
unsigned long fl_lastMqttActivity = 0;

int fl_mqttPublishFailCount = 0;

WiFiManager fl_wifiManager;

// Internal state
static unsigned long lastMqttRetry = 0;
static int mqttConnectFailCount = 0;

// Project callback
static fl_mqtt_callback_t _mqttProjectCallback = nullptr;

void fl_setMqttCallback(fl_mqtt_callback_t callback) {
  _mqttProjectCallback = callback;
}

// Internal MQTT callback - handles UPDATE_FIRMWARE, forwards rest to project
static void internalMqttCallback(char* topic, byte* payload, unsigned int length) {
  if (length >= FL_MAX_PAYLOAD_SIZE) {
    Serial.println("MQTT payload too large, ignoring");
    return;
  }

  // Only process command topic
  if (strcmp(topic, fl_TOPIC_COMMAND) != 0) {
    return;
  }

  // Update activity tracker
  fl_lastMqttActivity = millis();

  char cmd[FL_MAX_PAYLOAD_SIZE];
  memcpy(cmd, payload, length);
  cmd[length] = '\0';

  Serial.print("MQTT CMD: "); Serial.println(cmd);

  // Try parsing as JSON for UPDATE_FIRMWARE
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, cmd);

  if (!error) {
    const char* command = doc["command"];
    if (command && strcmp(command, "UPDATE_FIRMWARE") == 0) {
      const char* firmwareUrl = doc["url"];
      if (firmwareUrl) {
        // Notify project callback first (e.g., stop pump for safety)
        if (_mqttProjectCallback) {
          _mqttProjectCallback(cmd, length);
        }
        Serial.printf("Remote firmware update requested: %s\n", firmwareUrl);
        fl_mqtt.publish(fl_TOPIC_TELEMETRY, "{\"status\":\"updating\"}");
        fl_performRemoteFirmwareUpdate(firmwareUrl);
      } else {
        Serial.println("UPDATE_FIRMWARE command missing 'url' parameter");
      }
      return;  // Handled internally
    }
  }

  // Forward everything else to project callback
  if (_mqttProjectCallback) {
    _mqttProjectCallback(cmd, length);
  }
}

bool fl_initEthernet() {
  Serial.println("\n=== Initializing Ethernet ===");

  // Reset W5500
  pinMode(FL_ETH_RST, OUTPUT);
  digitalWrite(FL_ETH_RST, LOW);
  delay(50);
  digitalWrite(FL_ETH_RST, HIGH);
  delay(50);

  // Initialize SPI with custom pins
  SPI.begin(FL_ETH_SCLK, FL_ETH_MISO, FL_ETH_MOSI, FL_ETH_CS);
  Ethernet.init(FL_ETH_CS);

  // Get MAC from WiFi and modify for Ethernet
  WiFi.macAddress(ethMac);
  ethMac[0] = (ethMac[0] | 0x02) & 0xFE;  // Set locally administered, clear multicast

  Serial.printf("Ethernet MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                ethMac[0], ethMac[1], ethMac[2], ethMac[3], ethMac[4], ethMac[5]);

  Serial.println("Requesting IP via DHCP...");
  if (Ethernet.begin(ethMac, 10000)) {  // 10 second timeout
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

static void maintainEthernet() {
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

void fl_initNetwork() {
  // === NETWORK PRIORITY: Ethernet first, WiFi fallback ===

  if (fl_initEthernet()) {
    Serial.println("Using Ethernet as primary connection");
    fl_configLoaded = true;

    // Disable WiFi completely when using Ethernet
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi disabled (Ethernet active)");
  } else {
    // Ethernet failed, use WiFi
    Serial.println("Ethernet not available, using WiFi...");

    fl_wifiManager.setConfigPortalTimeout(FL_PORTAL_TIMEOUT_S);
    fl_wifiManager.setAPCallback([](WiFiManager *mgr) {
      Serial.println("\n*** WIFI SETUP MODE ***");
      Serial.printf("Connect to WiFi network: %s\n", fl_AP_NAME);
      Serial.println("Then open http://192.168.4.1 in your browser");
      Serial.println("Or wait for the captive portal to appear automatically");
    });
    fl_wifiManager.setSaveConfigCallback([]() {
      Serial.println("WiFi credentials saved!");
    });

    Serial.println("Connecting to WiFi...");
    if (fl_wifiManager.autoConnect(fl_AP_NAME)) {
      Serial.printf("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
      fl_wifiConnected = true;
      fl_configLoaded = true;
    }
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

  if (!fl_configLoaded) {
    Serial.println("Failed to connect to network. Restarting...");
    delay(3000);
    ESP.restart();
  }

  Serial.printf("\n=== Network: %s ===\n", fl_useEthernet ? "ETHERNET (priority)" : "WiFi");
  Serial.printf("IP Address: %s\n", fl_useEthernet ? Ethernet.localIP().toString().c_str() : WiFi.localIP().toString().c_str());
}

void fl_initNTP(long gmtOffsetSec) {
  configTime(gmtOffsetSec, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("NTP configured");
}

bool fl_connectMQTT() {
  if (!fl_ethernetConnected && !fl_wifiConnected) return false;

  Serial.printf("Connecting to MQTT: %s:%d (TLS: %s, via %s)\n",
                fl_mqtt_host, fl_mqtt_port, fl_mqtt_use_tls ? "yes" : "no",
                fl_useEthernet ? "Ethernet" : "WiFi");

  // Configure client based on connection type and TLS setting
  if (fl_useEthernet) {
    if (fl_mqtt_use_tls) {
      Serial.println("WARNING: TLS not supported over Ethernet, using non-TLS on port 1883");
      fl_mqtt_port = 1883;
    }
    fl_mqtt.setClient(fl_ethClient);
  } else if (fl_mqtt_use_tls) {
    fl_espClientSecure.setInsecure();  // Skip certificate verification
    fl_mqtt.setClient(fl_espClientSecure);
  } else {
    fl_mqtt.setClient(fl_espClientInsecure);
  }

  fl_mqtt.setServer(fl_mqtt_host, fl_mqtt_port);
  fl_mqtt.setBufferSize(FL_MAX_PAYLOAD_SIZE);
  fl_mqtt.setKeepAlive(FL_MQTT_KEEPALIVE_S);
  fl_mqtt.setCallback(internalMqttCallback);

  unsigned long startTime = millis();
  while (!fl_mqtt.connected()) {
    // Connect with Last Will and Testament (LWT)
    if (fl_mqtt.connect(fl_DEVICE_ID, fl_mqtt_user, fl_mqtt_pass, fl_TOPIC_STATUS, 0, true, "offline")) {
      fl_mqtt.subscribe(fl_TOPIC_SUBSCRIBE);
      fl_mqtt.publish(fl_TOPIC_STATUS, "online", true);
      fl_lastMqttActivity = millis();
      Serial.printf("MQTT connected as %s!\n", fl_DEVICE_ID);
      Serial.printf("Subscribed to: %s\n", fl_TOPIC_SUBSCRIBE);
      Serial.printf("Status topic: %s (LWT enabled)\n", fl_TOPIC_STATUS);
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
    maintainEthernet();
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
    if (now - lastMqttRetry > FL_MQTT_RETRY_INTERVAL) {
      lastMqttRetry = now;
      Serial.printf("Attempting MQTT reconnect via %s...\n", fl_useEthernet ? "Ethernet" : "WiFi");

      // Reconfigure client for current network
      if (fl_useEthernet) {
        fl_mqtt.setClient(fl_ethClient);
      } else if (fl_mqtt_use_tls) {
        fl_espClientSecure.setInsecure();
        fl_mqtt.setClient(fl_espClientSecure);
      } else {
        fl_mqtt.setClient(fl_espClientInsecure);
      }

      fl_mqtt.setServer(fl_mqtt_host, fl_useEthernet ? 1883 : fl_mqtt_port);
      fl_mqtt.setBufferSize(FL_MAX_PAYLOAD_SIZE);
      fl_mqtt.setKeepAlive(FL_MQTT_KEEPALIVE_S);

      // Connect with LWT
      if (fl_mqtt.connect(fl_DEVICE_ID, fl_mqtt_user, fl_mqtt_pass, fl_TOPIC_STATUS, 0, true, "offline")) {
        fl_mqtt.subscribe(fl_TOPIC_SUBSCRIBE);
        fl_mqtt.publish(fl_TOPIC_STATUS, "online", true);
        fl_lastMqttActivity = millis();
        Serial.printf("MQTT reconnected as %s!\n", fl_DEVICE_ID);
        fl_mqttConnected = true;
        mqttConnectFailCount = 0;
      } else {
        Serial.printf("MQTT reconnect failed, rc=%d\n", fl_mqtt.state());
        fl_mqttConnected = false;
        mqttConnectFailCount++;

        // If on Ethernet and MQTT keeps failing, fall back to WiFi
        if (fl_useEthernet && mqttConnectFailCount >= FL_MAX_MQTT_CONNECT_FAILURES) {
          Serial.println("MQTT over Ethernet failed repeatedly - switching to WiFi for TLS support");
          mqttConnectFailCount = 0;
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

    // Staleness detection
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
