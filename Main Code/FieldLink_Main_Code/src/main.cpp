/************************************************************
 * ESP32-S3 WAVESHARE PUMP CONTROLLER
 * Board: ESP32-S3 POE ETH 8DI 8DO
 * Version: 1.6.0
 *
 * Features:
 * - WiFi Captive Portal for easy setup
 * - Unique Device ID from MAC address
 * - Cloud MQTT (HiveMQ) with TLS
 * - Built-in web dashboard + Cloud dashboard
 * - Modbus RS485 current sensing
 * - Overcurrent/dry-run/sensor fault protection
 * - Start failure timeout detection
 ************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ModbusMaster.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <nvs_flash.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>

/* ================= USER CONFIG ================= */

#define FW_NAME    "ESP32 Pump Controller"
#define FW_VERSION "1.6.0"

// Captive portal timeout (seconds) - how long to wait for user to configure WiFi
#define PORTAL_TIMEOUT_S    180

// Connection timeouts
#define WIFI_TIMEOUT_MS     30000
#define MQTT_TIMEOUT_MS     10000
#define MQTT_RETRY_INTERVAL 5000

// Watchdog timeout (seconds)
#define WDT_TIMEOUT_S       30

// Max payload size for MQTT callback safety
#define MAX_PAYLOAD_SIZE    128

// Timing intervals (non-blocking)
#define TELEMETRY_INTERVAL_MS  2000
#define SENSOR_READ_INTERVAL_MS 500

// State detection hysteresis
#define HYSTERESIS_CURRENT     1.0
#define STATE_DEBOUNCE_COUNT   3

// Current validation limits
#define MIN_VALID_CURRENT      -0.5
#define MAX_VALID_CURRENT      500.0

// Fault handling
#define MAX_MODBUS_FAILURES    5
#define FAULT_AUTO_RESET_MS    0

// HiveMQ Cloud Settings
#define MQTT_HOST "a5c598acdbdc4abba053799bcefb73d0.s1.eu.hivemq.cloud"
#define MQTT_PORT 8883
#define MQTT_USER "fieldlogicuser1"
#define MQTT_PASS "@Shadow69"

// Preferences for non-WiFi settings
Preferences preferences;

// WiFiManager instance
WiFiManager wifiManager;

// Device ID (generated from MAC address)
char DEVICE_ID[16] = "";
char AP_NAME[32] = "";
char TOPIC_TELEMETRY[64] = "";
char TOPIC_COMMAND[64] = "";
char TOPIC_SUBSCRIBE[64] = "";  // Wildcard for subscriptions

// RS485
#define RS485_RX  18
#define RS485_TX  17
#define RS485_DE  21
#define BAUDRATE  9600
#define MODBUS_ID 1

// WAVESHARE I2C PINS (for TCA9554 I/O expander)
#define I2C_SDA   42
#define I2C_SCL   41
#define TCA9554_ADDR  0x20  // I2C address of TCA9554 for digital outputs

// DO CHANNELS (0-7 for 8 outputs)
#define DO_CONTACTOR_CH  0   // Main contactor relay
#define DO_RUN_LED_CH    1   // RUN indicator (green)
#define DO_FAULT_LED_CH  2   // FAULT indicator (red)

// WAVESHARE DIGITAL INPUT PINS (directly connected to ESP32 GPIOs)
#define DI1_PIN  4   // START button (NO - Normally Open)
#define DI2_PIN  5   // STOP button (NC - Normally Closed)
#define DI3_PIN  6   // LOCAL/REMOTE selector (LOW=LOCAL, HIGH=REMOTE)
#define DI4_PIN  7
#define DI5_PIN  8
#define DI6_PIN  9
#define DI7_PIN  10
#define DI8_PIN  11

// Button debounce
#define DEBOUNCE_MS  50

// Protection
#define RUN_THRESHOLD  5.0
#define MAX_CURRENT 120.0
#define DRY_CURRENT    0.5
#define START_TIMEOUT  10000

/* =============================================== */

// Use secure client for HiveMQ Cloud
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);
ModbusMaster node;
HardwareSerial RS485(2);

// Web server on port 80
AsyncWebServer server(80);

// DO register
uint8_t do_state = 0;

// Currents
float Ia = 0, Ib = 0, Ic = 0;

// State machine
enum PumpState { STOPPED, RUNNING, FAULT };
enum FaultType { NO_FAULT, OVERCURRENT, DRY_RUN, SENSOR_FAULT };

PumpState state = STOPPED;
PumpState pendingState = STOPPED;
FaultType faultType = NO_FAULT;
bool startCommand = false;
unsigned long startCommandTime = 0;

// State debouncing
int stateDebounceCounter = 0;

// Fault tracking
unsigned long faultTimestamp = 0;
float faultCurrentA = 0, faultCurrentB = 0, faultCurrentC = 0;

// Modbus error tracking
int modbusFailCount = 0;
bool sensorOnline = false;

// Connection state
unsigned long lastMqttRetry = 0;
bool wifiConnected = false;
bool mqttConnected = false;
bool configLoaded = false;

// Non-blocking timing
unsigned long lastTelemetryTime = 0;
unsigned long lastSensorReadTime = 0;

// Manual button state (with debouncing)
bool lastStartButtonState = false;
bool lastStopButtonState = true;  // NC button - default closed (DI2 to GND)
unsigned long lastStartDebounceTime = 0;
unsigned long lastStopDebounceTime = 0;

// Local/Remote mode (DI3: LOW=LOCAL, HIGH=REMOTE)
bool remoteMode = true;  // Default to remote if no selector connected

// Digital input states for telemetry
uint8_t diStatus = 0;  // Bit field for DI1-DI8

/* ================= DASHBOARD HTML ================= */

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>FieldLink Pump Controller</title>
  <link href="https://fonts.googleapis.com/css2?family=Chakra+Petch:wght@400;600;700&family=JetBrains+Mono:wght@400;500;600&display=swap" rel="stylesheet">
  <script src="https://unpkg.com/mqtt/dist/mqtt.min.js"></script>
  <style>
    :root {
      --bg-primary: #0a0e14;
      --bg-secondary: #111821;
      --bg-card: #151c28;
      --border-color: #1e2a3a;
      --text-primary: #e4e8ef;
      --text-secondary: #6b7a8f;
      --text-muted: #3d4a5c;
      --accent-cyan: #00d4ff;
      --status-running: #00ff88;
      --status-stopped: #6b7a8f;
      --status-fault: #ff4757;
    }
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body {
      font-family: 'JetBrains Mono', monospace;
      background: var(--bg-primary);
      color: var(--text-primary);
      min-height: 100vh;
    }
    body::before {
      content: '';
      position: fixed;
      top: 0; left: 0; right: 0; bottom: 0;
      background-image:
        linear-gradient(rgba(0, 212, 255, 0.03) 1px, transparent 1px),
        linear-gradient(90deg, rgba(0, 212, 255, 0.03) 1px, transparent 1px);
      background-size: 50px 50px;
      pointer-events: none;
    }
    .container { max-width: 1200px; margin: 0 auto; padding: 20px; position: relative; z-index: 1; }
    .header {
      display: flex; justify-content: space-between; align-items: center;
      margin-bottom: 24px; padding-bottom: 20px; border-bottom: 1px solid var(--border-color);
    }
    .logo { display: flex; align-items: center; gap: 12px; }
    .logo-icon {
      width: 42px; height: 42px;
      background: linear-gradient(135deg, var(--accent-cyan) 0%, #0088aa 100%);
      border-radius: 10px; display: flex; align-items: center; justify-content: center;
      font-family: 'Chakra Petch', sans-serif; font-weight: 700; font-size: 18px;
      color: var(--bg-primary); box-shadow: 0 4px 20px rgba(0, 212, 255, 0.3);
    }
    .logo-text { font-family: 'Chakra Petch', sans-serif; font-size: 24px; font-weight: 700; }
    .logo-text span { color: var(--accent-cyan); }
    .connection-status {
      display: flex; align-items: center; gap: 8px; padding: 8px 14px;
      background: var(--bg-card); border: 1px solid var(--border-color);
      border-radius: 6px; font-size: 12px;
    }
    .status-dot { width: 8px; height: 8px; border-radius: 50%; background: var(--status-fault); }
    .status-dot.connected { background: var(--status-running); box-shadow: 0 0 10px var(--status-running); }
    .grid { display: grid; grid-template-columns: 1fr 280px; gap: 20px; }
    .card {
      background: var(--bg-card); border: 1px solid var(--border-color);
      border-radius: 12px; padding: 20px; margin-bottom: 20px;
    }
    .card-title {
      font-family: 'Chakra Petch', sans-serif; font-size: 12px; font-weight: 600;
      text-transform: uppercase; letter-spacing: 1.5px; color: var(--text-secondary);
      margin-bottom: 16px;
    }
    .state-display { text-align: center; padding: 30px 16px; }
    .state-indicator {
      display: inline-flex; align-items: center; gap: 14px; padding: 16px 40px;
      border-radius: 12px; background: var(--bg-secondary); border: 2px solid var(--border-color);
    }
    .state-indicator.running { border-color: var(--status-running); box-shadow: 0 0 30px rgba(0, 255, 136, 0.2); }
    .state-indicator.fault { border-color: var(--status-fault); box-shadow: 0 0 30px rgba(255, 71, 87, 0.3); animation: pulse 1.5s infinite; }
    @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.7; } }
    .state-icon { width: 20px; height: 20px; border-radius: 50%; }
    .state-icon.running { background: var(--status-running); box-shadow: 0 0 15px var(--status-running); }
    .state-icon.stopped { background: var(--status-stopped); }
    .state-icon.fault { background: var(--status-fault); box-shadow: 0 0 15px var(--status-fault); }
    .state-text { font-family: 'Chakra Petch', sans-serif; font-size: 28px; font-weight: 700; letter-spacing: 3px; }
    .state-text.running { color: var(--status-running); }
    .state-text.stopped { color: var(--status-stopped); }
    .state-text.fault { color: var(--status-fault); }
    .current-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 16px; }
    .current-card {
      background: var(--bg-secondary); border: 1px solid var(--border-color);
      border-radius: 10px; padding: 20px; text-align: center;
    }
    .current-label { font-size: 12px; color: var(--text-secondary); margin-bottom: 6px; }
    .current-value { font-size: 36px; font-weight: 600; color: var(--accent-cyan); line-height: 1; margin-bottom: 4px; }
    .current-unit { font-size: 12px; color: var(--text-muted); }
    .controls { display: flex; flex-direction: column; gap: 10px; }
    .btn {
      font-family: 'Chakra Petch', sans-serif; font-size: 14px; font-weight: 600;
      letter-spacing: 1.5px; padding: 14px 20px; border: none; border-radius: 10px;
      cursor: pointer; text-transform: uppercase;
    }
    .btn-start { background: linear-gradient(135deg, #00aa66, #00ff88); color: var(--bg-primary); }
    .btn-stop { background: linear-gradient(135deg, #cc3344, #ff4757); color: white; }
    .btn-reset { background: var(--bg-secondary); color: var(--text-primary); border: 1px solid var(--border-color); }
    .btn:disabled { opacity: 0.5; cursor: not-allowed; }
    .status-list { display: flex; flex-direction: column; gap: 12px; }
    .status-item {
      display: flex; justify-content: space-between; align-items: center;
      padding: 12px; background: var(--bg-secondary); border-radius: 8px;
    }
    .status-label { font-size: 12px; color: var(--text-secondary); }
    .status-badge {
      padding: 3px 8px; border-radius: 4px; font-size: 10px; font-weight: 600;
      text-transform: uppercase;
    }
    .status-badge.online { background: rgba(0, 255, 136, 0.15); color: var(--status-running); }
    .status-badge.offline { background: rgba(255, 71, 87, 0.15); color: var(--status-fault); }
    .status-badge.active { background: rgba(0, 212, 255, 0.15); color: var(--accent-cyan); }
    .status-badge.inactive { background: rgba(107, 122, 143, 0.15); color: var(--text-secondary); }
    .uptime-display { text-align: center; padding: 16px; }
    .uptime-value { font-size: 28px; font-weight: 500; letter-spacing: 2px; }
    .uptime-label { font-size: 10px; color: var(--text-muted); margin-top: 6px; }
    .fault-banner {
      display: none; background: rgba(255, 71, 87, 0.15); border: 1px solid var(--status-fault);
      border-radius: 10px; padding: 12px; text-align: center; margin-bottom: 20px;
    }
    .fault-banner.visible { display: block; }
    .fault-banner-text { color: var(--status-fault); font-family: 'Chakra Petch', sans-serif; font-weight: 600; font-size: 13px; }
    @media (max-width: 900px) { .grid { grid-template-columns: 1fr; } }
    @media (max-width: 600px) { .current-grid { grid-template-columns: 1fr; } }
  </style>
</head>
<body>
  <div class="container">
    <header class="header">
      <div class="logo">
        <div class="logo-icon">FL</div>
        <div class="logo-text">Field<span>Link</span></div>
      </div>
      <div class="connection-status">
        <div class="status-dot" id="mqttStatus"></div>
        <span id="mqttStatusText">Connecting...</span>
      </div>
    </header>
    <div class="fault-banner" id="faultBanner">
      <div class="fault-banner-text" id="faultText">FAULT DETECTED</div>
    </div>
    <div class="grid">
      <div class="main">
        <div class="card">
          <div class="card-title">Pump Status</div>
          <div class="state-display">
            <div class="state-indicator stopped" id="stateIndicator">
              <div class="state-icon stopped" id="stateIcon"></div>
              <div class="state-text stopped" id="stateText">---</div>
            </div>
          </div>
        </div>
        <div class="card">
          <div class="card-title">Phase Currents</div>
          <div class="current-grid">
            <div class="current-card">
              <div class="current-label">Phase A</div>
              <div class="current-value" id="currentA">--</div>
              <div class="current-unit">Amps</div>
            </div>
            <div class="current-card">
              <div class="current-label">Phase B</div>
              <div class="current-value" id="currentB">--</div>
              <div class="current-unit">Amps</div>
            </div>
            <div class="current-card">
              <div class="current-label">Phase C</div>
              <div class="current-value" id="currentC">--</div>
              <div class="current-unit">Amps</div>
            </div>
          </div>
        </div>
      </div>
      <div class="side">
        <div class="card">
          <div class="card-title">Controls</div>
          <div class="controls">
            <button class="btn btn-start" id="btnStart" onclick="sendCommand('START')">Start Pump</button>
            <button class="btn btn-stop" id="btnStop" onclick="sendCommand('STOP')">Stop Pump</button>
            <button class="btn btn-reset" id="btnReset" onclick="sendCommand('RESET')">Reset Fault</button>
          </div>
        </div>
        <div class="card">
          <div class="card-title">System Info</div>
          <div class="status-list">
            <div class="status-item">
              <span class="status-label">Sensor</span>
              <span class="status-badge offline" id="sensorStatus">OFFLINE</span>
            </div>
            <div class="status-item">
              <span class="status-label">Command</span>
              <span class="status-badge inactive" id="cmdStatus">INACTIVE</span>
            </div>
          </div>
          <div class="uptime-display">
            <div class="uptime-value" id="uptime">--:--:--</div>
            <div class="uptime-label">UPTIME</div>
          </div>
        </div>
      </div>
    </div>
  </div>
  <script>
    const MQTT_BROKER = 'wss://a5c598acdbdc4abba053799bcefb73d0.s1.eu.hivemq.cloud:8884/mqtt';
    const MQTT_USER = 'fieldlogicuser1';
    const MQTT_PASS = '@Shadow69';
    let TOPIC_TELEMETRY = '';
    let TOPIC_COMMAND = '';
    let DEVICE_ID = '';
    let client = null, isConnected = false;
    const el = {
      mqttStatus: document.getElementById('mqttStatus'),
      mqttStatusText: document.getElementById('mqttStatusText'),
      stateIndicator: document.getElementById('stateIndicator'),
      stateIcon: document.getElementById('stateIcon'),
      stateText: document.getElementById('stateText'),
      currentA: document.getElementById('currentA'),
      currentB: document.getElementById('currentB'),
      currentC: document.getElementById('currentC'),
      sensorStatus: document.getElementById('sensorStatus'),
      cmdStatus: document.getElementById('cmdStatus'),
      uptime: document.getElementById('uptime'),
      faultBanner: document.getElementById('faultBanner'),
      faultText: document.getElementById('faultText'),
      btnStart: document.getElementById('btnStart'),
      btnReset: document.getElementById('btnReset')
    };
    function formatUptime(s) {
      const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
      return `${h.toString().padStart(2,'0')}:${m.toString().padStart(2,'0')}:${sec.toString().padStart(2,'0')}`;
    }
    function updateState(state) {
      const s = state.toLowerCase();
      el.stateIndicator.className = 'state-indicator ' + s;
      el.stateIcon.className = 'state-icon ' + s;
      el.stateText.className = 'state-text ' + s;
      el.stateText.textContent = state;
      el.faultBanner.classList.toggle('visible', s === 'fault');
      el.btnStart.disabled = s === 'fault';
      el.btnReset.disabled = s !== 'fault';
    }
    function updateTelemetry(data) {
      try {
        const t = JSON.parse(data);
        el.currentA.textContent = parseFloat(t.Ia).toFixed(1);
        el.currentB.textContent = parseFloat(t.Ib).toFixed(1);
        el.currentC.textContent = parseFloat(t.Ic).toFixed(1);
        updateState(t.state);
        if (t.fault) el.faultText.textContent = 'FAULT: ' + t.fault;
        el.sensorStatus.textContent = t.sensor ? 'ONLINE' : 'OFFLINE';
        el.sensorStatus.className = 'status-badge ' + (t.sensor ? 'online' : 'offline');
        el.cmdStatus.textContent = t.cmd ? 'ACTIVE' : 'INACTIVE';
        el.cmdStatus.className = 'status-badge ' + (t.cmd ? 'active' : 'inactive');
        el.uptime.textContent = formatUptime(t.uptime);
      } catch (e) { console.error('Parse error:', e); }
    }
    function sendCommand(cmd) {
      if (client && isConnected) {
        client.publish(TOPIC_COMMAND, cmd);
        console.log('Sent:', cmd);
      } else { alert('Not connected'); }
    }
    async function fetchDeviceInfo() {
      try {
        const response = await fetch('/api/device');
        const data = await response.json();
        DEVICE_ID = data.device_id;
        TOPIC_TELEMETRY = data.topic_telemetry;
        TOPIC_COMMAND = data.topic_command;
        document.title = 'FieldLink - ' + DEVICE_ID;
        return true;
      } catch (e) {
        console.error('Failed to fetch device info:', e);
        return false;
      }
    }
    async function connect() {
      el.mqttStatusText.textContent = 'Loading...';
      if (!await fetchDeviceInfo()) {
        el.mqttStatusText.textContent = 'Device Error';
        return;
      }
      el.mqttStatusText.textContent = 'Connecting...';
      client = mqtt.connect(MQTT_BROKER, {
        username: MQTT_USER,
        password: MQTT_PASS,
        clientId: 'local_' + DEVICE_ID + '_' + Math.random().toString(16).substr(2, 8),
        reconnectPeriod: 5000
      });
      client.on('connect', () => {
        isConnected = true;
        el.mqttStatus.classList.add('connected');
        el.mqttStatusText.textContent = 'Connected';
        client.subscribe(TOPIC_TELEMETRY);
      });
      client.on('message', (topic, msg) => { if (topic === TOPIC_TELEMETRY) updateTelemetry(msg.toString()); });
      client.on('close', () => {
        isConnected = false;
        el.mqttStatus.classList.remove('connected');
        el.mqttStatusText.textContent = 'Disconnected';
      });
      client.on('reconnect', () => { el.mqttStatusText.textContent = 'Reconnecting...'; });
    }
    document.addEventListener('DOMContentLoaded', connect);
  </script>
</body>
</html>
)rawliteral";

/* ================= FORWARD DECLARATIONS ================= */

void resetFault();
void triggerFault(FaultType type);
const char* faultTypeToString(FaultType ft);

/* ================= DO DRIVER (TCA9554 I2C) ================= */

void writeDO() {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(0x01);  // Output port register
  Wire.write(do_state);
  Wire.endTransmission();
}

void initDO() {
  // Configure TCA9554: set all pins as outputs
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(0x03);  // Configuration register
  Wire.write(0x00);  // All pins as outputs (0 = output)
  Wire.endTransmission();

  // Initialize all outputs to OFF (active-low: 0xFF = all OFF)
  do_state = 0xFF;
  writeDO();
  Serial.println("TCA9554 I/O expander initialized");
}

void setDO(uint8_t ch, bool on) {
  // Active-low outputs: clear bit to turn ON, set bit to turn OFF
  if (on) do_state &= ~(1 << ch);
  else    do_state |=  (1 << ch);
  writeDO();
}

/* ================= RS485 ================= */

void preTransmission() { digitalWrite(RS485_DE, HIGH); }
void postTransmission(){ digitalWrite(RS485_DE, LOW); }

/* ================= MODBUS ================= */

float registersToFloat(uint16_t high, uint16_t low) {
  uint32_t combined = ((uint32_t)high << 16) | low;
  float result;
  memcpy(&result, &combined, sizeof(float));
  return result;
}

bool isValidCurrent(float current) {
  if (isnan(current) || isinf(current)) return false;
  if (current < MIN_VALID_CURRENT || current > MAX_VALID_CURRENT) return false;
  return true;
}

bool readCurrents() {
  uint8_t result = node.readInputRegisters(0x0006, 6);

  if (result != node.ku8MBSuccess) {
    modbusFailCount++;
    if (modbusFailCount >= MAX_MODBUS_FAILURES) {
      if (sensorOnline) {
        Serial.println("ERROR: Modbus sensor offline!");
        sensorOnline = false;
      }
    }
    return false;
  }

  if (!sensorOnline) {
    Serial.println("Modbus sensor online");
  }
  modbusFailCount = 0;
  sensorOnline = true;

  float newIa = registersToFloat(node.getResponseBuffer(0), node.getResponseBuffer(1));
  float newIb = registersToFloat(node.getResponseBuffer(2), node.getResponseBuffer(3));
  float newIc = registersToFloat(node.getResponseBuffer(4), node.getResponseBuffer(5));

  if (!isValidCurrent(newIa) || !isValidCurrent(newIb) || !isValidCurrent(newIc)) {
    Serial.printf("WARNING: Invalid current reading: Ia=%.2f Ib=%.2f Ic=%.2f\n", newIa, newIb, newIc);
    return false;
  }

  Ia = newIa;
  Ib = newIb;
  Ic = newIc;

  return true;
}

/* ================= MQTT ================= */

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (length >= MAX_PAYLOAD_SIZE) {
    Serial.println("MQTT payload too large, ignoring");
    return;
  }

  // Only process command topic
  if (strcmp(topic, TOPIC_COMMAND) != 0) {
    return;
  }

  char cmd[MAX_PAYLOAD_SIZE];
  memcpy(cmd, payload, length);
  cmd[length] = '\0';

  Serial.print("MQTT CMD: "); Serial.println(cmd);

  if (strcmp(cmd, "START") == 0) {
    if (!remoteMode) {
      Serial.println("MQTT START ignored - in LOCAL mode");
    } else if (state == FAULT) {
      Serial.println("Cannot START while in FAULT state. Send RESET first.");
    } else {
      startCommand = true;
      startCommandTime = millis();
      Serial.println("Start command accepted (REMOTE mode)");
    }
  }
  else if (strcmp(cmd, "STOP") == 0) {
    // STOP always works for safety (both LOCAL and REMOTE mode)
    startCommand = false;
    setDO(DO_CONTACTOR_CH, false);
    if (state != FAULT) {
      state = STOPPED;
    }
    Serial.println("Stop command accepted");
  }
  else if (strcmp(cmd, "RESET") == 0) {
    // RESET works in any mode
    if (state == FAULT) {
      Serial.println("Fault reset requested");
      resetFault();
    } else {
      Serial.println("No fault to reset");
    }
  }
  else if (strcmp(cmd, "STATUS") == 0) {
    lastTelemetryTime = 0;
  }
}

/* ================= STATE ================= */

const char* faultTypeToString(FaultType ft) {
  switch (ft) {
    case OVERCURRENT:   return "OVERCURRENT";
    case DRY_RUN:       return "DRY_RUN";
    case SENSOR_FAULT:  return "SENSOR_FAULT";
    default:            return "NONE";
  }
}

void triggerFault(FaultType type) {
  if (state != FAULT) {
    state = FAULT;
    faultType = type;
    faultTimestamp = millis();
    faultCurrentA = Ia;
    faultCurrentB = Ib;
    faultCurrentC = Ic;

    startCommand = false;
    setDO(DO_CONTACTOR_CH, false);

    Serial.printf("!!! FAULT TRIGGERED: %s !!!\n", faultTypeToString(type));
    Serial.printf("Currents at fault: Ia=%.2f Ib=%.2f Ic=%.2f\n", Ia, Ib, Ic);
  }
}

void resetFault() {
  if (state == FAULT) {
    Serial.printf("Clearing fault: %s\n", faultTypeToString(faultType));
    state = STOPPED;
    faultType = NO_FAULT;
    pendingState = STOPPED;
    stateDebounceCounter = 0;
    startCommand = false;
    Serial.println("Fault cleared. Ready to restart.");
  }
}

float getMaxCurrent() {
  float maxI = Ia;
  if (Ib > maxI) maxI = Ib;
  if (Ic > maxI) maxI = Ic;
  return maxI;
}

PumpState evaluateState() {
  float maxCurrent = getMaxCurrent();

  if (Ia > MAX_CURRENT || Ib > MAX_CURRENT || Ic > MAX_CURRENT) {
    return FAULT;
  }

  if (DRY_CURRENT > 0 && startCommand && state == RUNNING) {
    if (maxCurrent < DRY_CURRENT) {
      return FAULT;
    }
  }

  if (START_TIMEOUT > 0 && startCommand && state != RUNNING) {
    if (millis() - startCommandTime > START_TIMEOUT) {
      Serial.println("Start failure timeout - pump did not start");
      return FAULT;
    }
  }

  if (state == RUNNING) {
    if (maxCurrent < (RUN_THRESHOLD - HYSTERESIS_CURRENT)) {
      return STOPPED;
    }
    return RUNNING;
  } else {
    if (maxCurrent > RUN_THRESHOLD) {
      return RUNNING;
    }
    return STOPPED;
  }
}

void updateState() {
  if (state == FAULT) {
    if (FAULT_AUTO_RESET_MS > 0 && (millis() - faultTimestamp) > FAULT_AUTO_RESET_MS) {
      Serial.println("Auto-resetting fault after timeout");
      resetFault();
    }
    return;
  }

  if (!sensorOnline && modbusFailCount >= MAX_MODBUS_FAILURES) {
    triggerFault(SENSOR_FAULT);
    return;
  }

  PumpState targetState = evaluateState();

  if (targetState == FAULT) {
    float maxCurrent = getMaxCurrent();
    if (maxCurrent > MAX_CURRENT) {
      triggerFault(OVERCURRENT);
    } else {
      triggerFault(DRY_RUN);
    }
    return;
  }

  if (targetState != state) {
    if (targetState == pendingState) {
      stateDebounceCounter++;
      if (stateDebounceCounter >= STATE_DEBOUNCE_COUNT) {
        state = targetState;
        stateDebounceCounter = 0;
        Serial.printf("State changed to: %s\n",
          state == RUNNING ? "RUNNING" : "STOPPED");
      }
    } else {
      pendingState = targetState;
      stateDebounceCounter = 1;
    }
  } else {
    stateDebounceCounter = 0;
    pendingState = state;
  }
}

/* ================= SERIAL COMMANDS ================= */

void handleSerialConfig() {
  if (!Serial.available()) return;

  String input = Serial.readStringUntil('\n');
  input.trim();

  if (input == "WIFI_RESET") {
    Serial.println("\n=== WIFI RESET ===");
    Serial.println("Clearing saved WiFi credentials...");
    wifiManager.resetSettings();
    Serial.println("WiFi credentials cleared! Restarting into setup mode...");
    delay(1000);
    ESP.restart();
  }
  else if (input == "STATUS") {
    Serial.println("\n=== SYSTEM STATUS ===");
    Serial.printf("Firmware: %s v%s\n", FW_NAME, FW_VERSION);
    Serial.printf("Device ID: %s\n", DEVICE_ID);
    Serial.printf("Setup AP: %s\n", AP_NAME);
    Serial.printf("Uptime: %lu seconds\n", millis() / 1000);
    Serial.println("\n--- Connectivity ---");
    Serial.printf("WiFi: %s\n", wifiConnected ? "Connected" : "Disconnected");
    if (wifiConnected) {
      Serial.printf("SSID: %s\n", WiFi.SSID().c_str());
      Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
      Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
    }
    Serial.printf("MQTT: %s\n", mqttConnected ? "Connected" : "Disconnected");
    Serial.printf("Sensor: %s\n", sensorOnline ? "Online" : "Offline");
    Serial.println("\n--- MQTT Topics ---");
    Serial.printf("Telemetry: %s\n", TOPIC_TELEMETRY);
    Serial.printf("Command: %s\n", TOPIC_COMMAND);
    Serial.println("\n--- Pump State ---");
    Serial.printf("State: %s\n", state==RUNNING?"RUNNING":state==FAULT?"FAULT":"STOPPED");
    Serial.printf("Start Command: %s\n", startCommand ? "Yes" : "No");
    Serial.printf("Currents: Ia=%.2f Ib=%.2f Ic=%.2f A\n", Ia, Ib, Ic);
    if (state == FAULT) {
      Serial.printf("Fault Type: %s\n", faultTypeToString(faultType));
    }
  }
  else if (input == "FAULT_RESET" || input == "CLEAR") {
    if (state == FAULT) {
      resetFault();
    } else {
      Serial.println("No fault to clear");
    }
  }
  else if (input == "START") {
    if (state == FAULT) {
      Serial.println("Cannot start while in FAULT. Use FAULT_RESET first.");
    } else {
      startCommand = true;
      startCommandTime = millis();
      Serial.println("Start command issued");
    }
  }
  else if (input == "STOP") {
    startCommand = false;
    setDO(DO_CONTACTOR_CH, false);
    Serial.println("Stop command issued");
  }
  else if (input == "REBOOT") {
    Serial.println("Rebooting...");
    delay(500);
    ESP.restart();
  }
  else if (input == "FACTORY_RESET") {
    Serial.println("Clearing all settings and restarting...");
    wifiManager.resetSettings();  // Clear WiFi credentials
    preferences.begin("fieldlink", false);
    preferences.clear();
    preferences.end();
    Serial.println("All settings cleared! Device will restart in setup mode...");
    delay(500);
    ESP.restart();
  }
  else if (input == "HELP") {
    Serial.println("\n=== SERIAL COMMANDS ===");
    Serial.println("STATUS       - Show system status");
    Serial.println("START        - Start pump");
    Serial.println("STOP         - Stop pump");
    Serial.println("FAULT_RESET  - Clear fault condition");
    Serial.println("WIFI_RESET   - Clear WiFi and restart setup portal");
    Serial.println("REBOOT       - Restart device");
    Serial.println("FACTORY_RESET- Clear all settings");
    Serial.println("HELP         - Show this help");
  }
}

/* ================= CONNECTION ================= */

bool connectMQTT() {
  if (!wifiConnected) return false;

  Serial.printf("Connecting to MQTT: %s:%d\n", MQTT_HOST, MQTT_PORT);

  // Configure TLS - skip certificate verification for simplicity
  espClient.setInsecure();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  unsigned long startTime = millis();
  while (!mqtt.connected()) {
    if (mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS)) {
      mqtt.subscribe(TOPIC_SUBSCRIBE);  // Wildcard subscription
      Serial.printf("MQTT connected as %s!\n", DEVICE_ID);
      Serial.printf("Subscribed to: %s\n", TOPIC_SUBSCRIBE);
      mqttConnected = true;
      return true;
    }

    if (millis() - startTime > MQTT_TIMEOUT_MS) {
      Serial.printf("MQTT connection TIMEOUT (rc=%d)\n", mqtt.state());
      mqttConnected = false;
      return false;
    }
    delay(500);
  }

  mqttConnected = true;
  return true;
}

void reconnectMQTT() {
  // Check WiFi status
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiConnected) {
      Serial.println("WiFi disconnected!");
      wifiConnected = false;
      mqttConnected = false;
    }
    // WiFiManager will handle reconnection automatically
    // Just wait for it to reconnect
    return;
  }

  // WiFi is connected
  if (!wifiConnected) {
    Serial.printf("WiFi reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
    wifiConnected = true;
  }

  // Handle MQTT reconnection
  if (!mqtt.connected()) {
    unsigned long now = millis();
    if (now - lastMqttRetry > MQTT_RETRY_INTERVAL) {
      lastMqttRetry = now;
      Serial.println("Attempting MQTT reconnect...");

      if (mqtt.connect(DEVICE_ID, MQTT_USER, MQTT_PASS)) {
        mqtt.subscribe(TOPIC_SUBSCRIBE);  // Wildcard subscription
        Serial.printf("MQTT reconnected as %s!\n", DEVICE_ID);
        mqttConnected = true;
      } else {
        Serial.printf("MQTT reconnect failed, rc=%d\n", mqtt.state());
        mqttConnected = false;
      }
    }
  } else {
    mqttConnected = true;
  }
}

/* ================= DEVICE ID ================= */

void generateDeviceId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);

  // Format: FL-XXYYZZ (last 3 bytes of MAC)
  snprintf(DEVICE_ID, sizeof(DEVICE_ID), "FL-%02X%02X%02X", mac[3], mac[4], mac[5]);

  // AP name for captive portal
  snprintf(AP_NAME, sizeof(AP_NAME), "FieldLink-%02X%02X%02X", mac[3], mac[4], mac[5]);

  // Generate topic strings
  snprintf(TOPIC_TELEMETRY, sizeof(TOPIC_TELEMETRY), "fieldlink/%s/telemetry", DEVICE_ID);
  snprintf(TOPIC_COMMAND, sizeof(TOPIC_COMMAND), "fieldlink/%s/command", DEVICE_ID);
  snprintf(TOPIC_SUBSCRIBE, sizeof(TOPIC_SUBSCRIBE), "fieldlink/%s/#", DEVICE_ID);  // Wildcard
}

void printDeviceInfo() {
  Serial.println("\n========================================");
  Serial.printf("  DEVICE ID: %s\n", DEVICE_ID);
  Serial.println("========================================");
  Serial.printf("  WiFi AP Name: %s\n", AP_NAME);
  Serial.printf("  Telemetry Topic: %s\n", TOPIC_TELEMETRY);
  Serial.printf("  Command Topic:   %s\n", TOPIC_COMMAND);
  Serial.println("========================================\n");
}

/* ================= WEB SERVER ================= */

void setupWebServer() {
  // Serve dashboard
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", DASHBOARD_HTML);
  });

  // API endpoint for status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<256> doc;
    doc["Ia"] = Ia;
    doc["Ib"] = Ib;
    doc["Ic"] = Ic;
    doc["state"] = (state == RUNNING ? "RUNNING" : state == FAULT ? "FAULT" : "STOPPED");
    doc["cmd"] = startCommand;
    doc["sensor"] = sensorOnline;
    doc["uptime"] = millis() / 1000;
    if (state == FAULT) {
      doc["fault"] = faultTypeToString(faultType);
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // API endpoint for commands
  server.on("/api/command", HTTP_POST, [](AsyncWebServerRequest *request){
    if (request->hasParam("cmd", true)) {
      String cmd = request->getParam("cmd", true)->value();
      if (cmd == "START" && state != FAULT) {
        startCommand = true;
        startCommandTime = millis();
        request->send(200, "text/plain", "OK");
      } else if (cmd == "STOP") {
        startCommand = false;
        setDO(DO_CONTACTOR_CH, false);
        request->send(200, "text/plain", "OK");
      } else if (cmd == "RESET" && state == FAULT) {
        resetFault();
        request->send(200, "text/plain", "OK");
      } else {
        request->send(400, "text/plain", "Invalid command");
      }
    } else {
      request->send(400, "text/plain", "Missing cmd parameter");
    }
  });

  // API endpoint for device info
  server.on("/api/device", HTTP_GET, [](AsyncWebServerRequest *request){
    StaticJsonDocument<512> doc;
    doc["device_id"] = DEVICE_ID;
    doc["firmware"] = FW_VERSION;
    doc["name"] = FW_NAME;
    doc["ip"] = WiFi.localIP().toString();
    doc["mac"] = WiFi.macAddress();
    doc["rssi"] = WiFi.RSSI();
    doc["mqtt_connected"] = mqttConnected;
    doc["topic_telemetry"] = TOPIC_TELEMETRY;
    doc["topic_command"] = TOPIC_COMMAND;
    doc["dashboard_url"] = String("https://voltageza.github.io/fieldlink-dashboard/?device=") + DEVICE_ID;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  server.begin();
  Serial.println("Web server started on port 80");
}

/* ================= SETUP ================= */

void setup() {
  Serial.begin(115200);
  delay(3000);  // Wait for USB CDC to enumerate

  Serial.println("\n\n*** ESP32 BOOT ***");
  Serial.println(FW_NAME);
  Serial.printf("Version: %s\n", FW_VERSION);
  Serial.flush();

  // Initialize NVS
  Serial.println("Initializing NVS...");
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.println("Erasing NVS...");
    nvs_flash_erase();
    nvs_flash_init();
  }

  Serial.println("Type 'HELP' for serial commands");

  // Initialize I2C for TCA9554 I/O expander
  Wire.begin(I2C_SDA, I2C_SCL);
  initDO();

  // Initialize digital inputs (with internal pull-up)
  pinMode(DI1_PIN, INPUT_PULLUP);  // START button (NO)
  pinMode(DI2_PIN, INPUT_PULLUP);  // STOP button (NC)
  pinMode(DI3_PIN, INPUT_PULLUP);
  pinMode(DI4_PIN, INPUT_PULLUP);
  pinMode(DI5_PIN, INPUT_PULLUP);
  pinMode(DI6_PIN, INPUT_PULLUP);
  pinMode(DI7_PIN, INPUT_PULLUP);
  pinMode(DI8_PIN, INPUT_PULLUP);
  Serial.println("Digital inputs initialized");

  // Initialize RS485
  pinMode(RS485_DE, OUTPUT);
  digitalWrite(RS485_DE, LOW);

  RS485.begin(BAUDRATE, SERIAL_8N1, RS485_RX, RS485_TX);
  node.begin(MODBUS_ID, RS485);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  // Generate device ID from MAC address (before WiFi connects)
  WiFi.mode(WIFI_STA);
  generateDeviceId();
  printDeviceInfo();

  // Configure WiFiManager
  wifiManager.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  wifiManager.setAPCallback([](WiFiManager *mgr) {
    Serial.println("\n*** WIFI SETUP MODE ***");
    Serial.printf("Connect to WiFi network: %s\n", AP_NAME);
    Serial.println("Then open http://192.168.4.1 in your browser");
    Serial.println("Or wait for the captive portal to appear automatically");
  });
  wifiManager.setSaveConfigCallback([]() {
    Serial.println("WiFi credentials saved!");
  });

  // Try to connect to WiFi, or start captive portal
  Serial.println("Connecting to WiFi...");
  if (wifiManager.autoConnect(AP_NAME)) {
    // Connected successfully
    Serial.printf("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
    wifiConnected = true;
    configLoaded = true;

    // Start web server
    setupWebServer();

    // Connect to cloud MQTT
    connectMQTT();
  } else {
    Serial.println("Failed to connect to WiFi. Restarting...");
    delay(3000);
    ESP.restart();
  }

  Serial.println("Setup complete. Entering main loop...");
}

/* ================= LOOP ================= */

static bool lastDOState = false;

void loop() {
  unsigned long now = millis();

  handleSerialConfig();

  if (configLoaded) {
    reconnectMQTT();
    mqtt.loop();
  }

  // ===== READ ALL DIGITAL INPUTS =====
  diStatus = 0;
  if (!digitalRead(DI1_PIN)) diStatus |= 0x01;  // DI1 active (inverted)
  if (!digitalRead(DI2_PIN)) diStatus |= 0x02;  // DI2 active (inverted)
  if (!digitalRead(DI3_PIN)) diStatus |= 0x04;  // DI3 active (inverted)
  if (!digitalRead(DI4_PIN)) diStatus |= 0x08;  // DI4 active (inverted)
  if (!digitalRead(DI5_PIN)) diStatus |= 0x10;  // DI5 active (inverted)
  if (!digitalRead(DI6_PIN)) diStatus |= 0x20;  // DI6 active (inverted)
  if (!digitalRead(DI7_PIN)) diStatus |= 0x40;  // DI7 active (inverted)
  if (!digitalRead(DI8_PIN)) diStatus |= 0x80;  // DI8 active (inverted)

  // ===== LOCAL/REMOTE MODE (DI3) =====
  // LOW (active) = LOCAL mode, HIGH (inactive) = REMOTE mode
  remoteMode = digitalRead(DI3_PIN);  // HIGH = remote, LOW = local

  // ===== MANUAL BUTTON HANDLING =====
  // START button (NO - Normally Open): wired between DI1 and GND
  // With pull-up: HIGH when not pressed, LOW when pressed
  bool startButtonReading = !digitalRead(DI1_PIN);  // Invert: true when pressed

  if (startButtonReading != lastStartButtonState) {
    if ((now - lastStartDebounceTime) > DEBOUNCE_MS) {
      lastStartDebounceTime = now;
      lastStartButtonState = startButtonReading;

      if (startButtonReading) {
        // START button pressed
        if (remoteMode) {
          Serial.println("Manual START ignored - in REMOTE mode");
        } else if (state == FAULT) {
          Serial.println("Manual START ignored - clear fault first");
        } else if (!startCommand) {
          startCommand = true;
          startCommandTime = now;
          Serial.println("Manual START button pressed (LOCAL mode)");
        }
      }
    }
  }

  // STOP button (NC - Normally Closed): wired between DI2 and GND
  // NC normal state: DI2 connected to GND = pump can run
  // NC triggered: DI2 open (wire broken or button pressed) = STOP
  // STOP always works in both modes (safety / fail-safe)
  bool stopButtonActive = !digitalRead(DI2_PIN);  // true when DI2 connected to GND

  if (stopButtonActive != lastStopButtonState) {
    if ((now - lastStopDebounceTime) > DEBOUNCE_MS) {
      lastStopDebounceTime = now;
      lastStopButtonState = stopButtonActive;

      if (!stopButtonActive) {
        // NC button opened (wire broken or button pressed) - works in ANY mode
        if (startCommand) {
          startCommand = false;
          setDO(DO_CONTACTOR_CH, false);
          Serial.println("Manual STOP button pressed");
        }
      }
    }
  }

  // ===== UPDATE INDICATOR OUTPUTS =====
  setDO(DO_RUN_LED_CH, state == RUNNING);
  setDO(DO_FAULT_LED_CH, state == FAULT);

  if (now - lastSensorReadTime >= SENSOR_READ_INTERVAL_MS) {
    lastSensorReadTime = now;
    readCurrents();
    updateState();

    bool desiredDO = (startCommand && state != FAULT);
    if (desiredDO != lastDOState) {
      setDO(DO_CONTACTOR_CH, desiredDO);
      Serial.printf("Contactor: %s\n", desiredDO ? "ON" : "OFF");
      lastDOState = desiredDO;
    }
  }

  if (now - lastTelemetryTime >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryTime = now;

    if (mqttConnected && mqtt.connected()) {
      StaticJsonDocument<384> doc;

      doc["Ia"] = round(Ia * 100) / 100.0;
      doc["Ib"] = round(Ib * 100) / 100.0;
      doc["Ic"] = round(Ic * 100) / 100.0;
      doc["state"] = (state == RUNNING ? "RUNNING" : state == FAULT ? "FAULT" : "STOPPED");
      doc["cmd"] = startCommand;

      if (state == FAULT) {
        doc["fault"] = faultTypeToString(faultType);
      }

      doc["sensor"] = sensorOnline;
      doc["uptime"] = now / 1000;
      doc["mode"] = remoteMode ? "REMOTE" : "LOCAL";
      doc["di"] = diStatus;
      doc["do"] = do_state;

      char buf[384];
      serializeJson(doc, buf);
      mqtt.publish(TOPIC_TELEMETRY, buf);
    }
  }

  delay(10);
}
