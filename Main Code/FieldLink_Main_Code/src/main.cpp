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
#include "esp_wifi.h"  // For esp_wifi_restore() to clear rogue AP config
#include <ESPAsyncWebServer.h>
#include <Wire.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <time.h>
#include <SPI.h>
#include <Ethernet.h>

/* ================= USER CONFIG ================= */

#define FW_NAME    "ESP32 Pump Controller"
#define FW_VERSION "2.8.1"

// BENCH TEST MODE - disable protections that require pump/load
#define BENCH_TEST_MODE false  // Set to true for bench testing without pump
#define HW_TYPE    "PUMP_ESP32S3"  // Hardware type for firmware management

// Captive portal timeout (seconds) - how long to wait for user to configure WiFi
#define PORTAL_TIMEOUT_S    180

// Connection timeouts
#define WIFI_TIMEOUT_MS     30000
#define MQTT_TIMEOUT_MS     10000
#define MQTT_RETRY_INTERVAL 5000
#define MQTT_KEEPALIVE_S    30       // MQTT keepalive interval (seconds)
#define MQTT_STALE_TIMEOUT_MS 90000  // Force reconnect if no successful publish for 90s

// Watchdog timeout (seconds)
#define WDT_TIMEOUT_S       30

// Max payload size for MQTT callback safety (increased for UPDATE_FIRMWARE commands)
#define MAX_PAYLOAD_SIZE    512

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

// Default MQTT Broker (HiveMQ Cloud)
// Load credentials from secrets.h (not committed to git)
#include "secrets.h"

// Configurable MQTT settings (stored in NVS)
char mqtt_host[128] = "";
uint16_t mqtt_port = DEFAULT_MQTT_PORT;
char mqtt_user[64] = "";
char mqtt_pass[64] = "";
bool mqtt_use_tls = true;

// Preferences for non-WiFi settings
Preferences preferences;

// WiFiManager instance
WiFiManager wifiManager;

// Device ID (generated from MAC address)
char DEVICE_ID[16] = "";
char AP_NAME[32] = "";
char TOPIC_TELEMETRY[64] = "";
char TOPIC_COMMAND[64] = "";
char TOPIC_STATUS[64] = "";     // Online/offline status (LWT)
char TOPIC_SUBSCRIBE[64] = "";  // Wildcard for subscriptions

// RS485
#define RS485_RX  18
#define RS485_TX  17
#define RS485_DE  21
#define BAUDRATE  9600
#define MODBUS_ID 1

// W5500 Ethernet (SPI)
#define ETH_CS    16
#define ETH_SCLK  15
#define ETH_MOSI  13
#define ETH_MISO  14
#define ETH_INT   12
#define ETH_RST   39

// WAVESHARE I2C PINS (for TCA9554 I/O expander)
#define I2C_SDA   42
#define I2C_SCL   41
#define TCA9554_ADDR  0x20  // I2C address of TCA9554 for digital outputs

// DO CHANNELS (0-7 for 8 outputs)
#define DO_CONTACTOR_CH  0   // Main contactor relay
#define DO_RUN_LED_CH    1   // RUN indicator (green)
#define DO_FAULT_LED_CH  2   // FAULT indicator (red)
#define DO_FAULT_CH      4   // Fault alarm output (physical DO5)

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
#define START_TIMEOUT  10000

// Configurable protection thresholds (stored in NVS)
float maxCurrentThreshold = 120.0;  // Overcurrent trip threshold (A)
float dryCurrentThreshold = 0.5;    // Dry run detection threshold (A)

// Protection enable flags
bool overcurrentProtectionEnabled = true;
bool dryRunProtectionEnabled = true;

// Schedule settings
bool scheduleEnabled = false;
uint8_t scheduleStartHour = 6;
uint8_t scheduleStartMinute = 0;
uint8_t scheduleEndHour = 18;
uint8_t scheduleEndMinute = 0;
uint8_t scheduleDays = 0x7F;  // Bitmask: bit0=Sun, bit1=Mon...bit6=Sat (0x7F = all days)
bool wasWithinSchedule = false;  // Track previous state for auto-start detection

// Ruraflex TOU settings (Eskom South Africa)
// When enabled, pump runs ONLY during off-peak hours
// Season auto-detected: June-Aug = High Demand, Sept-May = Low Demand
bool ruraflexEnabled = false;

/* =============================================== */

// MQTT clients (TLS and non-TLS)
WiFiClientSecure espClientSecure;
WiFiClient espClientInsecure;
EthernetClient ethClient;  // For Ethernet (non-TLS only with W5500)
PubSubClient mqtt;

// Ethernet MAC address (will be derived from WiFi MAC)
byte ethMac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED};
ModbusMaster node;
HardwareSerial RS485(2);

// Web server on port 80
AsyncWebServer server(80);

// DO register (0xFF = all OFF for active-low outputs)
uint8_t do_state = 0xFF;

// Currents
float Ia = 0, Ib = 0, Ic = 0;

// Voltages
float Va = 0, Vb = 0, Vc = 0;

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

// MQTT publish failure tracking
int mqttPublishFailCount = 0;
#define MAX_MQTT_PUBLISH_FAILURES 3

// MQTT connection failure tracking (for Ethernet->WiFi fallback)
int mqttConnectFailCount = 0;
#define MAX_MQTT_CONNECT_FAILURES 3
bool sensorOnline = false;

// Connection state
unsigned long lastMqttRetry = 0;
unsigned long lastMqttActivity = 0;  // Last successful MQTT publish/receive
bool ethernetConnected = false;
bool wifiConnected = false;
bool mqttConnected = false;
bool configLoaded = false;
bool useEthernet = false;  // True if using Ethernet, false if using WiFi

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
          <div class="card-title">Phase Voltages</div>
          <div class="current-grid">
            <div class="current-card">
              <div class="current-label">Phase A</div>
              <div class="current-value" id="voltageA">--</div>
              <div class="current-unit">Volts</div>
            </div>
            <div class="current-card">
              <div class="current-label">Phase B</div>
              <div class="current-value" id="voltageB">--</div>
              <div class="current-unit">Volts</div>
            </div>
            <div class="current-card">
              <div class="current-label">Phase C</div>
              <div class="current-value" id="voltageC">--</div>
              <div class="current-unit">Volts</div>
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
    const MQTT_BROKER = 'wss://)" DEFAULT_MQTT_HOST R"(:8884/mqtt';
    const MQTT_USER = ')" DEFAULT_MQTT_USER R"(';
    const MQTT_PASS = ')" DEFAULT_MQTT_PASS R"(';
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
      voltageA: document.getElementById('voltageA'),
      voltageB: document.getElementById('voltageB'),
      voltageC: document.getElementById('voltageC'),
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
        el.voltageA.textContent = parseFloat(t.Va).toFixed(1);
        el.voltageB.textContent = parseFloat(t.Vb).toFixed(1);
        el.voltageC.textContent = parseFloat(t.Vc).toFixed(1);
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
void sendFaultNotification();
void performRemoteFirmwareUpdate(const char* firmwareUrl);

/* ================= DO DRIVER (TCA9554 I2C) ================= */

void writeDO() {
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(0x01);  // Output port register
  Wire.write(do_state);
  Wire.endTransmission();
}

void initDO() {
  // CRITICAL: Set output values BEFORE configuring as outputs
  // This prevents glitches when pins transition from input to output

  // Step 1: Write output port register first (0xFF = all OFF for active-low)
  do_state = 0xFF;
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(0x01);  // Output port register
  Wire.write(do_state);
  Wire.endTransmission();

  // Step 2: Set polarity inversion to none
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(0x02);  // Polarity inversion register
  Wire.write(0x00);  // No inversion
  Wire.endTransmission();

  // Step 3: NOW configure all pins as outputs
  Wire.beginTransmission(TCA9554_ADDR);
  Wire.write(0x03);  // Configuration register
  Wire.write(0x00);  // All pins as outputs (0 = output)
  Wire.endTransmission();

  // Step 4: Write output state again to ensure it's correct
  writeDO();
  Serial.println("TCA9554 I/O expander initialized");
}

void setDO(uint8_t ch, bool on) {
  uint8_t old_state = do_state;
  // Active-low outputs: clear bit to turn ON, set bit to turn OFF
  if (on) do_state &= ~(1 << ch);
  else    do_state |=  (1 << ch);

  // Only write if state actually changed
  if (do_state != old_state) {
    writeDO();
  }
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

bool isValidVoltage(float voltage) {
  if (isnan(voltage) || isinf(voltage)) return false;
  if (voltage < 0 || voltage > 500) return false;  // 0-500V range
  return true;
}

bool readCurrents() {
  // Read voltage (0x0000-0x0005) and current (0x0006-0x000B) in one transaction
  uint8_t result = node.readInputRegisters(0x0000, 12);

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

  // Parse voltages (registers 0-5)
  float newVa = registersToFloat(node.getResponseBuffer(0), node.getResponseBuffer(1));
  float newVb = registersToFloat(node.getResponseBuffer(2), node.getResponseBuffer(3));
  float newVc = registersToFloat(node.getResponseBuffer(4), node.getResponseBuffer(5));

  // Parse currents (registers 6-11)
  float newIa = registersToFloat(node.getResponseBuffer(6), node.getResponseBuffer(7));
  float newIb = registersToFloat(node.getResponseBuffer(8), node.getResponseBuffer(9));
  float newIc = registersToFloat(node.getResponseBuffer(10), node.getResponseBuffer(11));

  // Validate voltages
  if (isValidVoltage(newVa) && isValidVoltage(newVb) && isValidVoltage(newVc)) {
    Va = newVa;
    Vb = newVb;
    Vc = newVc;
  }

  // Validate currents
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

// Forward declarations
void saveProtectionConfig();
void saveScheduleConfig();
void saveRuraflexConfig();

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (length >= MAX_PAYLOAD_SIZE) {
    Serial.println("MQTT payload too large, ignoring");
    return;
  }

  // Only process command topic
  if (strcmp(topic, TOPIC_COMMAND) != 0) {
    return;
  }

  // Update activity tracker - we received a valid message
  lastMqttActivity = millis();

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
  else {
    // Try parsing as JSON for complex commands
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, cmd);

    if (!error) {
      const char* command = doc["command"];

      if (command && strcmp(command, "UPDATE_FIRMWARE") == 0) {
        const char* firmwareUrl = doc["url"];

        if (firmwareUrl) {
          Serial.printf("Remote firmware update requested: %s\n", firmwareUrl);

          // Stop pump for safety during update
          startCommand = false;
          setDO(DO_CONTACTOR_CH, false);

          // Publish update status
          mqtt.publish(TOPIC_TELEMETRY, "{\"status\":\"updating\"}");

          // Perform update (will restart device on success)
          performRemoteFirmwareUpdate(firmwareUrl);
        } else {
          Serial.println("UPDATE_FIRMWARE command missing 'url' parameter");
        }
      }
      else if (command && strcmp(command, "SET_PROTECTION") == 0) {
        if (doc.containsKey("overcurrent_enabled"))
          overcurrentProtectionEnabled = doc["overcurrent_enabled"];
        if (doc.containsKey("dryrun_enabled"))
          dryRunProtectionEnabled = doc["dryrun_enabled"];
        saveProtectionConfig();
        Serial.println("Protection settings updated via MQTT");
      }
      else if (command && strcmp(command, "SET_THRESHOLDS") == 0) {
        if (doc.containsKey("max_current")) {
          float val = doc["max_current"];
          if (val >= 1.0 && val <= 500.0) maxCurrentThreshold = val;
        }
        if (doc.containsKey("dry_current")) {
          float val = doc["dry_current"];
          if (val >= 0.0 && val <= 50.0) dryCurrentThreshold = val;
        }
        saveProtectionConfig();
        Serial.printf("Thresholds updated: max=%.1fA, dry=%.1fA\n", maxCurrentThreshold, dryCurrentThreshold);
      }
      else if (command && strcmp(command, "SET_SCHEDULE") == 0) {
        if (doc.containsKey("enabled"))
          scheduleEnabled = doc["enabled"];
        if (doc.containsKey("start_hour"))
          scheduleStartHour = doc["start_hour"];
        if (doc.containsKey("start_minute"))
          scheduleStartMinute = doc["start_minute"];
        if (doc.containsKey("end_hour"))
          scheduleEndHour = doc["end_hour"];
        if (doc.containsKey("end_minute"))
          scheduleEndMinute = doc["end_minute"];
        if (doc.containsKey("days"))
          scheduleDays = doc["days"];
        saveScheduleConfig();
        Serial.println("Schedule updated via MQTT");
      }
      else if (command && strcmp(command, "SET_RURAFLEX") == 0) {
        if (doc.containsKey("enabled"))
          ruraflexEnabled = doc["enabled"];
        // Disable custom schedule when Ruraflex is enabled
        if (ruraflexEnabled && scheduleEnabled) {
          scheduleEnabled = false;
          saveScheduleConfig();
        }
        saveRuraflexConfig();
        Serial.println("Ruraflex updated via MQTT");
      }
      else if (command && strcmp(command, "GET_SETTINGS") == 0) {
        // Respond with current schedule, ruraflex and protection settings
        StaticJsonDocument<512> resp;
        resp["type"] = "settings";
        resp["schedule_enabled"] = scheduleEnabled;
        resp["schedule_start_hour"] = scheduleStartHour;
        resp["schedule_start_minute"] = scheduleStartMinute;
        resp["schedule_end_hour"] = scheduleEndHour;
        resp["schedule_end_minute"] = scheduleEndMinute;
        resp["schedule_days"] = scheduleDays;
        resp["ruraflex_enabled"] = ruraflexEnabled;
        resp["overcurrent_protection"] = overcurrentProtectionEnabled;
        resp["dryrun_protection"] = dryRunProtectionEnabled;
        resp["max_current"] = maxCurrentThreshold;
        resp["dry_current"] = dryCurrentThreshold;
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 10)) {
          char timeStr[9];
          strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
          resp["current_time"] = timeStr;
        }
        char buf[512];
        serializeJson(resp, buf);
        mqtt.publish(TOPIC_TELEMETRY, buf);
        Serial.println("Settings sent via MQTT");
      }
    }
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

// Send fault notification via HTTP webhook (for Telegram alerts)
void sendFaultNotification() {
  if (!wifiConnected) {
    Serial.println("Cannot send notification - WiFi not connected");
    return;
  }

  HTTPClient http;
  http.begin(NOTIFICATION_WEBHOOK_URL);
  http.addHeader("Content-Type", "application/json");

  // Build JSON payload with device_id
  String payload = "{\"device_id\":\"";
  payload += DEVICE_ID;
  payload += "\"}";

  Serial.printf("Sending fault notification: %s\n", payload.c_str());

  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    Serial.printf("Notification sent, response code: %d\n", httpCode);
    if (httpCode == HTTP_CODE_OK) {
      String response = http.getString();
      Serial.printf("Response: %s\n", response.c_str());
    }
  } else {
    Serial.printf("Notification failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}

// Remote firmware update via HTTP download
void performRemoteFirmwareUpdate(const char* firmwareUrl) {
  if (!wifiConnected) {
    Serial.println("Cannot update - WiFi not connected");
    return;
  }

  Serial.println("===========================================");
  Serial.println("REMOTE FIRMWARE UPDATE STARTED");
  Serial.printf("URL: %s\n", firmwareUrl);
  Serial.println("===========================================");

  HTTPClient http;
  http.begin(firmwareUrl);

  int httpCode = http.GET();

  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Firmware download failed, HTTP code: %d\n", httpCode);
    http.end();
    return;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    Serial.println("Invalid content length");
    http.end();
    return;
  }

  Serial.printf("Firmware size: %d bytes\n", contentLength);

  bool canBegin = Update.begin(contentLength);
  if (!canBegin) {
    Serial.println("Not enough space for OTA");
    http.end();
    return;
  }

  WiFiClient * stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buff[128];
  int lastProgress = 0;

  Serial.println("Starting download...");

  while (http.connected() && (written < contentLength)) {
    size_t available = stream->available();

    if (available) {
      int bytesRead = stream->readBytes(buff, min(available, sizeof(buff)));

      if (bytesRead > 0) {
        size_t bytesWritten = Update.write(buff, bytesRead);

        if (bytesWritten != bytesRead) {
          Serial.println("Write error!");
          Update.abort();
          http.end();
          return;
        }

        written += bytesWritten;

        // Progress reporting
        int progress = (written * 100) / contentLength;
        if (progress != lastProgress && progress % 10 == 0) {
          Serial.printf("Progress: %d%%\n", progress);
          lastProgress = progress;
        }
      }
    }
    delay(1);
  }

  Serial.printf("Downloaded: %d bytes\n", written);

  if (written != contentLength) {
    Serial.println("Download incomplete!");
    Update.abort();
    http.end();
    return;
  }

  if (Update.end()) {
    Serial.println("===========================================");
    Serial.println("FIRMWARE UPDATE SUCCESS!");
    Serial.println("Device will restart in 3 seconds...");
    Serial.println("===========================================");

    http.end();
    delay(3000);
    ESP.restart();
  } else {
    Serial.println("Update failed!");
    Update.printError(Serial);
  }

  http.end();
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
    setDO(DO_FAULT_CH, true);  // Activate fault alarm output (DO5)

    Serial.printf("!!! FAULT TRIGGERED: %s !!!\n", faultTypeToString(type));
    Serial.printf("Currents at fault: Ia=%.2f Ib=%.2f Ic=%.2f\n", Ia, Ib, Ic);

    // TEMPORARILY DISABLED - testing if this causes DO3 issue
    // sendFaultNotification();
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
    setDO(DO_FAULT_CH, false);  // Deactivate fault alarm output (DO5)
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

  if (overcurrentProtectionEnabled && (Ia > maxCurrentThreshold || Ib > maxCurrentThreshold || Ic > maxCurrentThreshold)) {
    return FAULT;
  }

  #if !BENCH_TEST_MODE
  if (dryRunProtectionEnabled && dryCurrentThreshold > 0 && startCommand && state == RUNNING) {
    if (maxCurrent < dryCurrentThreshold) {
      return FAULT;
    }
  }

  if (START_TIMEOUT > 0 && startCommand && state != RUNNING) {
    if (millis() - startCommandTime > START_TIMEOUT) {
      Serial.println("Start failure timeout - pump did not start");
      return FAULT;
    }
  }
  #endif

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
    if (maxCurrent > maxCurrentThreshold) {
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
    if (mqttConnected && lastMqttActivity > 0) {
      Serial.printf("MQTT last activity: %lu seconds ago\n", (millis() - lastMqttActivity) / 1000);
    }
    Serial.printf("Sensor: %s\n", sensorOnline ? "Online" : "Offline");
    Serial.println("\n--- MQTT Topics ---");
    Serial.printf("Telemetry: %s\n", TOPIC_TELEMETRY);
    Serial.printf("Command: %s\n", TOPIC_COMMAND);
    Serial.printf("Status: %s (LWT)\n", TOPIC_STATUS);
    Serial.println("\n--- Pump State ---");
    Serial.printf("State: %s\n", state==RUNNING?"RUNNING":state==FAULT?"FAULT":"STOPPED");
    Serial.printf("Start Command: %s\n", startCommand ? "Yes" : "No");
    Serial.printf("Voltages: Va=%.1f Vb=%.1f Vc=%.1f V\n", Va, Vb, Vc);
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
  else if (input == "TEST_FAULT") {
    Serial.println("Testing fault trigger...");
    Serial.printf("do_state BEFORE: 0x%02X\n", do_state);
    triggerFault(SENSOR_FAULT);
    Serial.printf("do_state AFTER:  0x%02X\n", do_state);
    Serial.printf("DO_FAULT_CH = %d, expected bit = 0x%02X\n", DO_FAULT_CH, (1 << DO_FAULT_CH));
  }
  else if (input == "DO5ON") {
    Serial.println("Turning DO5 ON...");
    setDO(4, true);  // Channel 4 = physical DO5
    Serial.printf("do_state: 0x%02X\n", do_state);
  }
  else if (input == "DO5OFF") {
    Serial.println("Turning DO5 OFF...");
    setDO(4, false);
    Serial.printf("do_state: 0x%02X\n", do_state);
  }
  else if (input == "I2CTEST") {
    Serial.println("Testing I2C TCA9554...");
    Wire.beginTransmission(TCA9554_ADDR);
    uint8_t err = Wire.endTransmission();
    Serial.printf("I2C probe result: %d (0=OK)\n", err);

    // Try to read back output register
    Wire.beginTransmission(TCA9554_ADDR);
    Wire.write(0x01);  // Output port register
    Wire.endTransmission();
    Wire.requestFrom(TCA9554_ADDR, (uint8_t)1);
    if (Wire.available()) {
      uint8_t val = Wire.read();
      Serial.printf("TCA9554 output register: 0x%02X (expected: 0x%02X)\n", val, do_state);
    } else {
      Serial.println("Failed to read from TCA9554");
    }
  }
  else if (input.startsWith("DO") && input.length() >= 4) {
    // DOxON or DOxOFF where x is 1-8
    int ch = input.charAt(2) - '1';  // Convert '1'-'8' to 0-7
    bool on = input.endsWith("ON");
    if (ch >= 0 && ch < 8) {
      setDO(ch, on);
      Serial.printf("DO%d set to %s (channel %d, do_state=0x%02X)\n", ch+1, on?"ON":"OFF", ch, do_state);
    }
  }
  else if (input == "HELP") {
    Serial.println("\n=== SERIAL COMMANDS ===");
    Serial.println("STATUS       - Show system status");
    Serial.println("START        - Start pump");
    Serial.println("STOP         - Stop pump");
    Serial.println("FAULT_RESET  - Clear fault condition");
    Serial.println("TEST_FAULT   - Test fault alarm output");
    Serial.println("DO5ON/DO5OFF - Test DO5 directly");
    Serial.println("DOxON/DOxOFF - Control any DO (x=1-8)");
    Serial.println("I2CTEST      - Test I2C communication with TCA9554");
    Serial.println("WIFI_RESET   - Clear WiFi and restart setup portal");
    Serial.println("REBOOT       - Restart device");
    Serial.println("FACTORY_RESET- Clear all settings");
    Serial.println("HELP         - Show this help");
  }
}

/* ================= CONNECTION ================= */

bool initEthernet() {
  Serial.println("\n=== Initializing Ethernet ===");

  // Reset W5500
  pinMode(ETH_RST, OUTPUT);
  digitalWrite(ETH_RST, LOW);
  delay(50);
  digitalWrite(ETH_RST, HIGH);
  delay(50);

  // Initialize SPI with custom pins
  SPI.begin(ETH_SCLK, ETH_MISO, ETH_MOSI, ETH_CS);

  // Set CS pin
  Ethernet.init(ETH_CS);

  // Get MAC from WiFi and modify for Ethernet (change locally administered bit)
  WiFi.macAddress(ethMac);
  ethMac[0] = (ethMac[0] | 0x02) & 0xFE;  // Set locally administered, clear multicast

  Serial.printf("Ethernet MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                ethMac[0], ethMac[1], ethMac[2], ethMac[3], ethMac[4], ethMac[5]);

  // Try DHCP
  Serial.println("Requesting IP via DHCP...");
  if (Ethernet.begin(ethMac, 10000)) {  // 10 second timeout
    Serial.printf("Ethernet connected! IP: %s\n", Ethernet.localIP().toString().c_str());
    Serial.printf("Gateway: %s\n", Ethernet.gatewayIP().toString().c_str());
    Serial.printf("DNS: %s\n", Ethernet.dnsServerIP().toString().c_str());
    ethernetConnected = true;
    useEthernet = true;
    return true;
  } else {
    Serial.println("Ethernet DHCP failed - no cable or no DHCP server");
    ethernetConnected = false;
    return false;
  }
}

void maintainEthernet() {
  if (useEthernet) {
    switch (Ethernet.maintain()) {
      case 1: // Renew failed
        Serial.println("Ethernet DHCP renew failed");
        ethernetConnected = false;
        break;
      case 2: // Renew success
        Serial.println("Ethernet DHCP renewed");
        break;
      case 3: // Rebind failed
        Serial.println("Ethernet DHCP rebind failed");
        ethernetConnected = false;
        break;
      case 4: // Rebind success
        Serial.println("Ethernet DHCP rebind success");
        break;
    }

    // Check link status
    if (Ethernet.linkStatus() == LinkOFF) {
      if (ethernetConnected) {
        Serial.println("Ethernet cable disconnected!");
        ethernetConnected = false;
      }
    } else if (!ethernetConnected) {
      Serial.println("Ethernet cable reconnected, re-init...");
      initEthernet();
    }
  }
}

bool connectMQTT() {
  if (!ethernetConnected && !wifiConnected) return false;

  Serial.printf("Connecting to MQTT: %s:%d (TLS: %s, via %s)\n",
                mqtt_host, mqtt_port, mqtt_use_tls ? "yes" : "no",
                useEthernet ? "Ethernet" : "WiFi");

  // Configure client based on connection type and TLS setting
  if (useEthernet) {
    // Ethernet: W5500 doesn't support TLS natively, use non-TLS
    if (mqtt_use_tls) {
      Serial.println("WARNING: TLS not supported over Ethernet, using non-TLS on port 1883");
      mqtt_port = 1883;  // Force non-TLS port
    }
    mqtt.setClient(ethClient);
  } else if (mqtt_use_tls) {
    espClientSecure.setInsecure();  // Skip certificate verification
    mqtt.setClient(espClientSecure);
  } else {
    mqtt.setClient(espClientInsecure);
  }

  mqtt.setServer(mqtt_host, mqtt_port);
  mqtt.setBufferSize(512);  // Increase from default 256 for larger telemetry messages
  mqtt.setKeepAlive(MQTT_KEEPALIVE_S);  // Set keepalive for connection health
  mqtt.setCallback(mqttCallback);

  unsigned long startTime = millis();
  while (!mqtt.connected()) {
    // Connect with Last Will and Testament (LWT) for offline detection
    // LWT: willTopic, willQos, willRetain, willMessage
    if (mqtt.connect(DEVICE_ID, mqtt_user, mqtt_pass, TOPIC_STATUS, 0, true, "offline")) {
      mqtt.subscribe(TOPIC_SUBSCRIBE);  // Wildcard subscription
      mqtt.publish(TOPIC_STATUS, "online", true);  // Publish online status (retained)
      lastMqttActivity = millis();  // Initialize activity tracker
      Serial.printf("MQTT connected as %s!\n", DEVICE_ID);
      Serial.printf("Subscribed to: %s\n", TOPIC_SUBSCRIBE);
      Serial.printf("Status topic: %s (LWT enabled)\n", TOPIC_STATUS);
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
  // Check network status based on current mode
  bool networkOk = false;

  if (useEthernet) {
    // Maintain Ethernet DHCP lease
    maintainEthernet();
    networkOk = ethernetConnected;

    if (!networkOk) {
      // Ethernet failed, try to fall back to WiFi
      Serial.println("Ethernet down, checking WiFi...");
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Falling back to WiFi");
        useEthernet = false;
        wifiConnected = true;
        networkOk = true;
        // Need to reconfigure MQTT client for WiFi
        mqtt.disconnect();
        mqttConnected = false;
      }
    }
  } else {
    // Using WiFi
    if (WiFi.status() != WL_CONNECTED) {
      if (wifiConnected) {
        Serial.println("WiFi disconnected!");
        wifiConnected = false;
        mqttConnected = false;
      }
      // Try Ethernet as fallback
      if (initEthernet()) {
        Serial.println("Switched to Ethernet");
        networkOk = true;
        mqtt.disconnect();
        mqttConnected = false;
      }
    } else {
      if (!wifiConnected) {
        Serial.printf("WiFi reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
        wifiConnected = true;
      }
      networkOk = true;
    }
  }

  if (!networkOk) return;

  // Handle MQTT reconnection
  if (!mqtt.connected()) {
    unsigned long now = millis();
    if (now - lastMqttRetry > MQTT_RETRY_INTERVAL) {
      lastMqttRetry = now;
      Serial.printf("Attempting MQTT reconnect via %s...\n", useEthernet ? "Ethernet" : "WiFi");

      // Reconfigure client for current network
      if (useEthernet) {
        mqtt.setClient(ethClient);
      } else if (mqtt_use_tls) {
        espClientSecure.setInsecure();
        mqtt.setClient(espClientSecure);
      } else {
        mqtt.setClient(espClientInsecure);
      }

      mqtt.setServer(mqtt_host, useEthernet ? 1883 : mqtt_port);
      mqtt.setBufferSize(512);
      mqtt.setKeepAlive(MQTT_KEEPALIVE_S);

      // Connect with Last Will and Testament (LWT)
      if (mqtt.connect(DEVICE_ID, mqtt_user, mqtt_pass, TOPIC_STATUS, 0, true, "offline")) {
        mqtt.subscribe(TOPIC_SUBSCRIBE);  // Wildcard subscription
        mqtt.publish(TOPIC_STATUS, "online", true);  // Publish online status (retained)
        lastMqttActivity = millis();  // Reset activity tracker
        Serial.printf("MQTT reconnected as %s!\n", DEVICE_ID);
        mqttConnected = true;
        mqttConnectFailCount = 0;  // Reset failure count on success
      } else {
        Serial.printf("MQTT reconnect failed, rc=%d\n", mqtt.state());
        mqttConnected = false;
        mqttConnectFailCount++;

        // If on Ethernet and MQTT keeps failing (likely TLS issue), fall back to WiFi
        if (useEthernet && mqttConnectFailCount >= MAX_MQTT_CONNECT_FAILURES) {
          Serial.println("MQTT over Ethernet failed repeatedly - switching to WiFi for TLS support");
          mqttConnectFailCount = 0;
          useEthernet = false;
          ethernetConnected = false;  // Mark Ethernet as not usable for MQTT

          // Initialize WiFi
          WiFi.mode(WIFI_STA);
          WiFi.begin();  // Reconnect with saved credentials
          Serial.println("Connecting to WiFi...");

          unsigned long wifiStart = millis();
          while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
            delay(500);
            Serial.print(".");
          }

          if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\nWiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
            wifiConnected = true;
            WiFi.softAPdisconnect(true);  // Disable any AP
          } else {
            Serial.println("\nWiFi connection failed - will retry");
          }
        }
      }
    }
  } else {
    mqttConnected = true;

    // Staleness detection: if connected but no successful activity for too long, force reconnect
    unsigned long now = millis();
    if (lastMqttActivity > 0 && (now - lastMqttActivity > MQTT_STALE_TIMEOUT_MS)) {
      Serial.printf("MQTT connection stale (no activity for %lus) - forcing reconnect\n",
                    (now - lastMqttActivity) / 1000);
      mqtt.disconnect();
      mqttConnected = false;
      lastMqttActivity = 0;  // Reset to avoid immediate re-trigger
    }
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
  snprintf(TOPIC_STATUS, sizeof(TOPIC_STATUS), "fieldlink/%s/status", DEVICE_ID);
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

/* ================= MQTT CONFIG ================= */

void generateDefaultMqttCredentials() {
  // Generate unique password based on device ID
  // Format: FL-XXXXXX (where XXXXXX is from MAC)
  snprintf(mqtt_user, sizeof(mqtt_user), "%s", DEVICE_ID);
  snprintf(mqtt_pass, sizeof(mqtt_pass), "%s-key", DEVICE_ID);
}

void loadMqttConfig() {
  preferences.begin("mqtt", true);  // Read-only

  String host = preferences.getString("host", "");
  if (host.length() > 0) {
    strncpy(mqtt_host, host.c_str(), sizeof(mqtt_host) - 1);
  } else {
    strncpy(mqtt_host, DEFAULT_MQTT_HOST, sizeof(mqtt_host) - 1);
  }

  mqtt_port = preferences.getUShort("port", DEFAULT_MQTT_PORT);
  mqtt_use_tls = preferences.getBool("tls", true);

  String user = preferences.getString("user", "");
  String pass = preferences.getString("pass", "");

  if (user.length() > 0) {
    strncpy(mqtt_user, user.c_str(), sizeof(mqtt_user) - 1);
    strncpy(mqtt_pass, pass.c_str(), sizeof(mqtt_pass) - 1);
  } else {
    // Use default shared credentials for backward compatibility
    strncpy(mqtt_user, DEFAULT_MQTT_USER, sizeof(mqtt_user) - 1);
    strncpy(mqtt_pass, DEFAULT_MQTT_PASS, sizeof(mqtt_pass) - 1);
  }

  preferences.end();

  Serial.println("MQTT Config loaded:");
  Serial.printf("  Host: %s:%d\n", mqtt_host, mqtt_port);
  Serial.printf("  User: %s\n", mqtt_user);
  Serial.printf("  TLS: %s\n", mqtt_use_tls ? "yes" : "no");
}

void saveMqttConfig() {
  preferences.begin("mqtt", false);  // Read-write
  preferences.putString("host", mqtt_host);
  preferences.putUShort("port", mqtt_port);
  preferences.putString("user", mqtt_user);
  preferences.putString("pass", mqtt_pass);
  preferences.putBool("tls", mqtt_use_tls);
  preferences.end();
  Serial.println("MQTT Config saved");
}

void resetMqttConfig() {
  preferences.begin("mqtt", false);
  preferences.clear();
  preferences.end();

  // Reload defaults
  strncpy(mqtt_host, DEFAULT_MQTT_HOST, sizeof(mqtt_host) - 1);
  mqtt_port = DEFAULT_MQTT_PORT;
  strncpy(mqtt_user, DEFAULT_MQTT_USER, sizeof(mqtt_user) - 1);
  strncpy(mqtt_pass, DEFAULT_MQTT_PASS, sizeof(mqtt_pass) - 1);
  mqtt_use_tls = true;

  Serial.println("MQTT Config reset to defaults");
}

/* ================= PROTECTION CONFIG ================= */

void loadProtectionConfig() {
  preferences.begin("protection", true);
  overcurrentProtectionEnabled = preferences.getBool("overcurrent", true);
  dryRunProtectionEnabled = preferences.getBool("dryrun", true);
  maxCurrentThreshold = preferences.getFloat("max_current", 120.0);
  dryCurrentThreshold = preferences.getFloat("dry_current", 0.5);
  preferences.end();
  Serial.printf("Protection config loaded: max=%.1fA, dry=%.1fA\n", maxCurrentThreshold, dryCurrentThreshold);
}

void saveProtectionConfig() {
  preferences.begin("protection", false);
  preferences.putBool("overcurrent", overcurrentProtectionEnabled);
  preferences.putBool("dryrun", dryRunProtectionEnabled);
  preferences.putFloat("max_current", maxCurrentThreshold);
  preferences.putFloat("dry_current", dryCurrentThreshold);
  preferences.end();
  Serial.printf("Protection config saved: max=%.1fA, dry=%.1fA\n", maxCurrentThreshold, dryCurrentThreshold);
}

/* ================= SCHEDULE CONFIG ================= */

void loadScheduleConfig() {
  preferences.begin("schedule", true);
  scheduleEnabled = preferences.getBool("enabled", false);
  scheduleStartHour = preferences.getUChar("startH", 6);
  scheduleStartMinute = preferences.getUChar("startM", 0);
  scheduleEndHour = preferences.getUChar("endH", 18);
  scheduleEndMinute = preferences.getUChar("endM", 0);
  scheduleDays = preferences.getUChar("days", 0x7F);  // Default: all days
  preferences.end();
  Serial.println("Schedule config loaded");
}

void saveScheduleConfig() {
  preferences.begin("schedule", false);
  preferences.putBool("enabled", scheduleEnabled);
  preferences.putUChar("startH", scheduleStartHour);
  preferences.putUChar("startM", scheduleStartMinute);
  preferences.putUChar("endH", scheduleEndHour);
  preferences.putUChar("endM", scheduleEndMinute);
  preferences.putUChar("days", scheduleDays);
  preferences.end();
  Serial.println("Schedule config saved");
}

/* ================= RURAFLEX CONFIG ================= */

void loadRuraflexConfig() {
  preferences.begin("ruraflex", true);
  ruraflexEnabled = preferences.getBool("enabled", false);
  preferences.end();
  Serial.println("Ruraflex config loaded");
}

void saveRuraflexConfig() {
  preferences.begin("ruraflex", false);
  preferences.putBool("enabled", ruraflexEnabled);
  preferences.end();
  Serial.println("Ruraflex config saved");
}

// Ruraflex TOU time checking (Eskom South Africa 2025/26)
// Returns true if pump is allowed to run based on current TOU period
bool isWithinRuraflex() {
  if (!ruraflexEnabled) return true;  // Not enabled = always allowed

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10)) return true;  // NTP failed = allow

  int month = timeinfo.tm_mon + 1;  // tm_mon is 0-11
  int dayOfWeek = timeinfo.tm_wday; // 0=Sun, 1=Mon...6=Sat
  int hour = timeinfo.tm_hour;
  int mins = timeinfo.tm_min;
  int nowMins = hour * 60 + mins;

  // Determine season: High Demand = June-August, Low Demand = Sept-May
  bool isHighDemandSeason = (month >= 6 && month <= 8);

  // Determine day type: 0=Sunday, 1-5=Weekday, 6=Saturday
  bool isWeekday = (dayOfWeek >= 1 && dayOfWeek <= 5);
  bool isSaturday = (dayOfWeek == 6);
  bool isSunday = (dayOfWeek == 0);

  // Define time periods (in minutes from midnight)
  // High Demand Season (Winter: June-August)
  // Peak: 06:00-08:00 (360-480), 17:00-20:00 (1020-1200) - Weekdays only
  // Standard: 08:00-17:00 (480-1020), 20:00-22:00 (1200-1320) weekdays
  //           07:00-12:00 (420-720), 18:00-20:00 (1080-1200) weekends
  // Off-peak: 22:00-06:00 (1320-360) weekdays, rest of weekends

  // Low Demand Season (Summer: Sept-May)
  // Peak: 07:00-09:00 (420-540), 17:00-20:00 (1020-1200) - Weekdays only
  // Standard: 06:00-07:00 (360-420), 09:00-17:00 (540-1020), 20:00-22:00 (1200-1320) weekdays
  //           07:00-12:00 (420-720), 18:00-20:00 (1080-1200) weekends
  // Off-peak: 22:00-06:00 (1320-360) weekdays, rest of weekends

  bool isPeak = false;
  bool isStandard = false;

  if (isWeekday) {
    if (isHighDemandSeason) {
      // High demand weekday peaks: 06:00-08:00 and 17:00-20:00
      isPeak = (nowMins >= 360 && nowMins < 480) || (nowMins >= 1020 && nowMins < 1200);
      // Standard: 08:00-17:00 and 20:00-22:00
      isStandard = (nowMins >= 480 && nowMins < 1020) || (nowMins >= 1200 && nowMins < 1320);
    } else {
      // Low demand weekday peaks: 07:00-09:00 and 17:00-20:00
      isPeak = (nowMins >= 420 && nowMins < 540) || (nowMins >= 1020 && nowMins < 1200);
      // Standard: 06:00-07:00, 09:00-17:00, and 20:00-22:00
      isStandard = (nowMins >= 360 && nowMins < 420) || (nowMins >= 540 && nowMins < 1020) || (nowMins >= 1200 && nowMins < 1320);
    }
  } else {
    // Weekend (Saturday and Sunday) - no peak periods
    isPeak = false;
    // Standard: 07:00-12:00 and 18:00-20:00
    isStandard = (nowMins >= 420 && nowMins < 720) || (nowMins >= 1080 && nowMins < 1200);
  }

  // Off-peak is everything else (not peak and not standard)
  bool isOffPeak = !isPeak && !isStandard;

  // Pump runs ONLY during off-peak hours
  return isOffPeak;
}

bool isWithinSchedule() {
  // Ruraflex takes priority over custom schedule
  if (ruraflexEnabled) {
    return isWithinRuraflex();
  }

  if (!scheduleEnabled) return true;  // No schedule = always allowed

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10)) return true;  // NTP failed = allow (10ms timeout)

  // Check if today is an allowed day (tm_wday: 0=Sun, 1=Mon...6=Sat)
  if (!(scheduleDays & (1 << timeinfo.tm_wday))) {
    return false;  // Today is not a scheduled day
  }

  int nowMins = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int startMins = scheduleStartHour * 60 + scheduleStartMinute;
  int endMins = scheduleEndHour * 60 + scheduleEndMinute;

  if (startMins <= endMins) {
    return nowMins >= startMins && nowMins < endMins;
  } else {
    // Overnight schedule (e.g., 22:00 - 06:00)
    return nowMins >= startMins || nowMins < endMins;
  }
}

/* ================= WEB SERVER ================= */

// Authentication check helper
bool checkAuth(AsyncWebServerRequest *request) {
  if (!request->authenticate(WEB_AUTH_USER, WEB_AUTH_PASS)) {
    request->requestAuthentication();
    return false;
  }
  return true;
}

void setupWebServer() {
  // Serve dashboard (requires auth)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!checkAuth(request)) return;
    request->send_P(200, "text/html", DASHBOARD_HTML);
  });

  // API endpoint for status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!checkAuth(request)) return;
    StaticJsonDocument<384> doc;
    doc["Va"] = Va;
    doc["Vb"] = Vb;
    doc["Vc"] = Vc;
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
    if (!checkAuth(request)) return;
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
    if (!checkAuth(request)) return;
    StaticJsonDocument<512> doc;
    doc["device_id"] = DEVICE_ID;
    doc["hardware_type"] = HW_TYPE;
    doc["firmware"] = FW_VERSION;
    doc["name"] = FW_NAME;
    doc["ip"] = WiFi.localIP().toString();
    doc["mac"] = WiFi.macAddress();
    doc["rssi"] = WiFi.RSSI();
    doc["mqtt_connected"] = mqttConnected;
    doc["topic_telemetry"] = TOPIC_TELEMETRY;
    doc["topic_command"] = TOPIC_COMMAND;
    doc["topic_status"] = TOPIC_STATUS;
    doc["dashboard_url"] = String("https://voltageza.github.io/fieldlink-dashboard/?device=") + DEVICE_ID;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // API endpoint for MQTT config (GET)
  server.on("/api/mqtt", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!checkAuth(request)) return;
    StaticJsonDocument<256> doc;
    doc["host"] = mqtt_host;
    doc["port"] = mqtt_port;
    doc["user"] = mqtt_user;
    doc["pass"] = "********";  // Don't expose password
    doc["tls"] = mqtt_use_tls;
    doc["connected"] = mqttConnected;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // API endpoint for MQTT config (POST)
  server.on("/api/mqtt", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!checkAuth(request)) return;
    bool changed = false;

    if (request->hasParam("host", true)) {
      strncpy(mqtt_host, request->getParam("host", true)->value().c_str(), sizeof(mqtt_host) - 1);
      changed = true;
    }
    if (request->hasParam("port", true)) {
      mqtt_port = request->getParam("port", true)->value().toInt();
      changed = true;
    }
    if (request->hasParam("user", true)) {
      strncpy(mqtt_user, request->getParam("user", true)->value().c_str(), sizeof(mqtt_user) - 1);
      changed = true;
    }
    if (request->hasParam("pass", true)) {
      strncpy(mqtt_pass, request->getParam("pass", true)->value().c_str(), sizeof(mqtt_pass) - 1);
      changed = true;
    }
    if (request->hasParam("tls", true)) {
      mqtt_use_tls = request->getParam("tls", true)->value() == "true";
      changed = true;
    }

    if (changed) {
      saveMqttConfig();
      request->send(200, "text/plain", "Config saved. Rebooting...");
      delay(1000);
      ESP.restart();
    } else {
      request->send(400, "text/plain", "No parameters provided");
    }
  });

  // API endpoint to reset MQTT config
  server.on("/api/mqtt/reset", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!checkAuth(request)) return;
    resetMqttConfig();
    request->send(200, "text/plain", "Config reset. Rebooting...");
    delay(1000);
    ESP.restart();
  });

  // Protection settings API
  server.on("/api/protection", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!checkAuth(request)) return;
    StaticJsonDocument<128> doc;
    doc["overcurrent_enabled"] = overcurrentProtectionEnabled;
    doc["dryrun_enabled"] = dryRunProtectionEnabled;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  server.on("/api/protection", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!checkAuth(request)) return;
    if (request->hasParam("overcurrent_enabled", true))
      overcurrentProtectionEnabled = request->getParam("overcurrent_enabled", true)->value() == "true";
    if (request->hasParam("dryrun_enabled", true))
      dryRunProtectionEnabled = request->getParam("dryrun_enabled", true)->value() == "true";
    saveProtectionConfig();
    request->send(200, "text/plain", "Protection settings saved");
  });

  // Schedule settings API
  server.on("/api/schedule", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!checkAuth(request)) return;
    StaticJsonDocument<384> doc;
    doc["enabled"] = scheduleEnabled;
    doc["start_hour"] = scheduleStartHour;
    doc["start_minute"] = scheduleStartMinute;
    doc["end_hour"] = scheduleEndHour;
    doc["end_minute"] = scheduleEndMinute;
    doc["days"] = scheduleDays;  // Bitmask: bit0=Sun...bit6=Sat
    // Individual day flags for convenience
    JsonObject daysObj = doc.createNestedObject("days_detail");
    daysObj["sun"] = (scheduleDays & 0x01) != 0;
    daysObj["mon"] = (scheduleDays & 0x02) != 0;
    daysObj["tue"] = (scheduleDays & 0x04) != 0;
    daysObj["wed"] = (scheduleDays & 0x08) != 0;
    daysObj["thu"] = (scheduleDays & 0x10) != 0;
    daysObj["fri"] = (scheduleDays & 0x20) != 0;
    daysObj["sat"] = (scheduleDays & 0x40) != 0;
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10)) {
      char timeStr[9];
      strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
      doc["current_time"] = timeStr;
      doc["current_day"] = timeinfo.tm_wday;  // 0=Sun, 1=Mon...6=Sat
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  server.on("/api/schedule", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!checkAuth(request)) return;
    if (request->hasParam("enabled", true))
      scheduleEnabled = request->getParam("enabled", true)->value() == "true";
    if (request->hasParam("start_hour", true))
      scheduleStartHour = request->getParam("start_hour", true)->value().toInt();
    if (request->hasParam("start_minute", true))
      scheduleStartMinute = request->getParam("start_minute", true)->value().toInt();
    if (request->hasParam("end_hour", true))
      scheduleEndHour = request->getParam("end_hour", true)->value().toInt();
    if (request->hasParam("end_minute", true))
      scheduleEndMinute = request->getParam("end_minute", true)->value().toInt();
    if (request->hasParam("days", true))
      scheduleDays = request->getParam("days", true)->value().toInt();
    saveScheduleConfig();
    request->send(200, "text/plain", "Schedule saved");
  });

  // MQTT configuration page
  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!checkAuth(request)) return;
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>FieldLink - MQTT Config</title>
  <style>
    body { font-family: -apple-system, sans-serif; background: #1a1a2e; color: #eee; padding: 20px; }
    .container { max-width: 400px; margin: 0 auto; }
    h1 { color: #00d4ff; font-size: 24px; }
    .card { background: #16213e; border-radius: 12px; padding: 20px; margin: 20px 0; }
    label { display: block; margin: 15px 0 5px; color: #888; font-size: 12px; text-transform: uppercase; }
    input, select { width: 100%; padding: 12px; border: 1px solid #333; border-radius: 6px; background: #0f0f23; color: #fff; font-size: 16px; box-sizing: border-box; }
    input:focus { border-color: #00d4ff; outline: none; }
    button { width: 100%; padding: 14px; border: none; border-radius: 6px; font-size: 16px; font-weight: bold; cursor: pointer; margin-top: 10px; }
    .btn-primary { background: #00d4ff; color: #000; }
    .btn-danger { background: #ff4757; color: #fff; }
    .btn-secondary { background: #333; color: #fff; }
    .status { padding: 10px; border-radius: 6px; margin: 10px 0; text-align: center; }
    .status.connected { background: #00ff8820; color: #00ff88; }
    .status.disconnected { background: #ff475720; color: #ff4757; }
    .device-id { font-family: monospace; font-size: 20px; color: #00d4ff; text-align: center; padding: 10px; background: #0f0f23; border-radius: 6px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>FieldLink Config</h1>
    <div class="device-id" id="deviceId">Loading...</div>
    <div class="status disconnected" id="mqttStatus">MQTT: Checking...</div>

    <div class="card">
      <h3>MQTT Broker</h3>
      <label>Host</label>
      <input type="text" id="host" placeholder="broker.example.com">
      <label>Port</label>
      <input type="number" id="port" value="8883">
      <label>Username</label>
      <input type="text" id="user" placeholder="username">
      <label>Password</label>
      <input type="password" id="pass" placeholder="password">
      <label>Use TLS/SSL</label>
      <select id="tls">
        <option value="true">Yes (Port 8883)</option>
        <option value="false">No (Port 1883)</option>
      </select>
      <button class="btn-primary" onclick="saveConfig()">Save and Reboot</button>
      <button class="btn-danger" onclick="resetConfig()">Reset to Defaults</button>
    </div>

    <div class="card">
      <button class="btn-secondary" onclick="location.href='/update'">Firmware Update</button>
      <button class="btn-secondary" onclick="location.href='/'">Back to Dashboard</button>
    </div>
  </div>
  <script>
    async function loadConfig() {
      try {
        var res = await fetch('/api/mqtt');
        var cfg = await res.json();
        document.getElementById('host').value = cfg.host;
        document.getElementById('port').value = cfg.port;
        document.getElementById('user').value = cfg.user;
        document.getElementById('tls').value = cfg.tls ? 'true' : 'false';
        document.getElementById('mqttStatus').textContent = 'MQTT: ' + (cfg.connected ? 'Connected' : 'Disconnected');
        document.getElementById('mqttStatus').className = 'status ' + (cfg.connected ? 'connected' : 'disconnected');

        var devRes = await fetch('/api/device');
        var dev = await devRes.json();
        document.getElementById('deviceId').textContent = dev.device_id;
      } catch(e) { console.error(e); }
    }
    async function saveConfig() {
      var data = new URLSearchParams();
      data.append('host', document.getElementById('host').value);
      data.append('port', document.getElementById('port').value);
      data.append('user', document.getElementById('user').value);
      data.append('pass', document.getElementById('pass').value);
      data.append('tls', document.getElementById('tls').value);
      try {
        var res = await fetch('/api/mqtt', { method: 'POST', body: data });
        alert(await res.text());
      } catch(e) { alert('Error: ' + e); }
    }
    async function resetConfig() {
      if (confirm('Reset MQTT config to defaults?')) {
        try {
          var res = await fetch('/api/mqtt/reset', { method: 'POST' });
          alert(await res.text());
        } catch(e) { alert('Error: ' + e); }
      }
    }
    loadConfig();
  </script>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
  });

  // HTTP OTA Update endpoint
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>FieldLink - Firmware Update</title>
  <style>
    body { font-family: -apple-system, sans-serif; background: #1a1a2e; color: #eee; padding: 20px; }
    .container { max-width: 400px; margin: 0 auto; }
    h1 { color: #00d4ff; font-size: 24px; }
    .card { background: #16213e; border-radius: 12px; padding: 20px; margin: 20px 0; }
    .device-id { font-family: monospace; font-size: 20px; color: #00d4ff; text-align: center; padding: 10px; background: #0f0f23; border-radius: 6px; margin-bottom: 20px; }
    .version { text-align: center; color: #888; margin-bottom: 20px; }
    input[type="file"] { width: 100%; padding: 12px; border: 2px dashed #00d4ff; border-radius: 6px; background: #0f0f23; color: #fff; cursor: pointer; }
    input[type="file"]:hover { background: #1a1a3e; }
    button { width: 100%; padding: 14px; border: none; border-radius: 6px; font-size: 16px; font-weight: bold; cursor: pointer; margin-top: 10px; }
    .btn-primary { background: #00d4ff; color: #000; }
    .btn-secondary { background: #333; color: #fff; }
    .btn-primary:disabled { opacity: 0.5; cursor: not-allowed; }
    .progress { width: 100%; height: 30px; background: #0f0f23; border-radius: 6px; margin: 20px 0; overflow: hidden; display: none; }
    .progress-bar { height: 100%; background: linear-gradient(90deg, #00d4ff, #00ff88); width: 0%; transition: width 0.3s; text-align: center; line-height: 30px; color: #000; font-weight: bold; }
    .status { padding: 10px; border-radius: 6px; margin: 10px 0; text-align: center; display: none; }
    .status.success { background: #00ff8820; color: #00ff88; display: block; }
    .status.error { background: #ff475720; color: #ff4757; display: block; }
    .warning { background: #ff9f4320; color: #ff9f43; padding: 10px; border-radius: 6px; margin: 10px 0; font-size: 14px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>Firmware Update</h1>
    <div class="device-id" id="deviceId">Loading...</div>
    <div class="version">Current Version: <span id="version">--</span></div>

    <div class="card">
      <h3>Upload New Firmware</h3>
      <div class="warning">
        ⚠️ Warning: Device will restart after update. Ensure pump is stopped before proceeding.
      </div>
      <input type="file" id="fileInput" accept=".bin">
      <div class="progress" id="progressBar">
        <div class="progress-bar" id="progressBarFill">0%</div>
      </div>
      <div class="status" id="status"></div>
      <button class="btn-primary" id="uploadBtn" onclick="uploadFirmware()">Upload Firmware</button>
      <button class="btn-secondary" onclick="location.href='/config'">Back to Config</button>
    </div>
  </div>
  <script>
    async function loadInfo() {
      try {
        const res = await fetch('/api/device');
        const dev = await res.json();
        document.getElementById('deviceId').textContent = dev.device_id;
        document.getElementById('version').textContent = dev.firmware;
      } catch(e) { console.error(e); }
    }

    async function uploadFirmware() {
      const fileInput = document.getElementById('fileInput');
      const file = fileInput.files[0];

      if (!file) {
        alert('Please select a firmware file (.bin)');
        return;
      }

      if (!file.name.endsWith('.bin')) {
        alert('Please select a valid .bin firmware file');
        return;
      }

      if (!confirm('Upload firmware and restart device?')) return;

      const uploadBtn = document.getElementById('uploadBtn');
      const progressBar = document.getElementById('progressBar');
      const progressBarFill = document.getElementById('progressBarFill');
      const status = document.getElementById('status');

      uploadBtn.disabled = true;
      fileInput.disabled = true;
      progressBar.style.display = 'block';
      status.style.display = 'none';

      const formData = new FormData();
      formData.append('firmware', file);

      try {
        const xhr = new XMLHttpRequest();

        xhr.upload.addEventListener('progress', (e) => {
          if (e.lengthComputable) {
            const percent = (e.loaded / e.total) * 100;
            progressBarFill.style.width = percent + '%';
            progressBarFill.textContent = Math.round(percent) + '%';
          }
        });

        xhr.addEventListener('load', () => {
          if (xhr.status === 200) {
            status.className = 'status success';
            status.textContent = 'Update successful! Device will restart...';
            status.style.display = 'block';
            setTimeout(() => { location.href = '/'; }, 10000);
          } else {
            status.className = 'status error';
            status.textContent = 'Update failed: ' + xhr.responseText;
            status.style.display = 'block';
            uploadBtn.disabled = false;
            fileInput.disabled = false;
          }
        });

        xhr.addEventListener('error', () => {
          status.className = 'status error';
          status.textContent = 'Upload failed. Check connection.';
          status.style.display = 'block';
          uploadBtn.disabled = false;
          fileInput.disabled = false;
        });

        xhr.open('POST', '/api/update');
        xhr.send(formData);

      } catch(e) {
        status.className = 'status error';
        status.textContent = 'Error: ' + e.message;
        status.style.display = 'block';
        uploadBtn.disabled = false;
        fileInput.disabled = false;
      }
    }

    loadInfo();
  </script>
</body>
</html>
)rawliteral";
    request->send(200, "text/html", html);
  });

  // API endpoint for firmware upload (requires authentication)
  server.on("/api/update", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      if (!checkAuth(request)) return;
      // Request handler - called when upload is complete
      bool updateSuccess = !Update.hasError();

      AsyncWebServerResponse *response = request->beginResponse(200, "text/plain",
        updateSuccess ? "Update Success! Rebooting..." : "Update Failed!");
      response->addHeader("Connection", "close");
      request->send(response);

      if (updateSuccess) {
        delay(1000);
        ESP.restart();
      }
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      // Upload handler - called for each chunk of data
      if (!index) {
        Serial.printf("HTTP OTA Update Start: %s\n", filename.c_str());

        // Start update
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
          Update.printError(Serial);
          request->send(500, "text/plain", "Update init failed");
          return;
        }
      }

      // Write chunk
      if (Update.write(data, len) != len) {
        Update.printError(Serial);
        request->send(500, "text/plain", "Update write failed");
        return;
      }

      // Finish update
      if (final) {
        if (Update.end(true)) {
          Serial.printf("HTTP OTA Update Success: %u bytes\n", index + len);
        } else {
          Update.printError(Serial);
          request->send(500, "text/plain", "Update end failed");
        }
      }
    }
  );

  server.begin();
  Serial.println("Web server started on port 80");
}

/* ================= SETUP ================= */

void setup() {
  // CRITICAL: I2C bus recovery - release stuck bus from previous crash
  pinMode(I2C_SCL, OUTPUT);
  pinMode(I2C_SDA, INPUT_PULLUP);
  for (int i = 0; i < 9; i++) {
    digitalWrite(I2C_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(I2C_SCL, HIGH);
    delayMicroseconds(5);
  }
  pinMode(I2C_SCL, INPUT_PULLUP);
  delayMicroseconds(100);

  // CRITICAL: Initialize I2C and outputs FIRST to prevent floating pins
  Wire.begin(I2C_SDA, I2C_SCL);
  initDO();

  Serial.begin(115200);
  delay(3000);  // Wait for USB CDC to enumerate

  Serial.println("\n\n*** ESP32 BOOT ***");
  Serial.println(FW_NAME);
  Serial.printf("Version: %s\n", FW_VERSION);
  #if BENCH_TEST_MODE
  Serial.println("*** BENCH TEST MODE - DRY_RUN and START_TIMEOUT disabled ***");
  #endif
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

  // Generate device ID from MAC address (need WiFi initialized for MAC)
  WiFi.mode(WIFI_STA);     // Initialize WiFi subsystem

  // One-time fix for rogue "ESP32" AP issue - only runs once per device
  preferences.begin("fieldlink", false);
  bool wifiRestoreDone = preferences.getBool("wifi_restored", false);
  if (!wifiRestoreDone) {
    Serial.println("First boot: clearing rogue AP config from NVS...");
    esp_wifi_restore();      // Clear ALL stored WiFi config from NVS
    preferences.putBool("wifi_restored", true);
    Serial.println("WiFi config cleared. This will only happen once.");
  }
  preferences.end();

  WiFi.persistent(false);  // Don't save WiFi config to flash going forward
  delay(100);
  generateDeviceId();
  printDeviceInfo();

  // === NETWORK PRIORITY: Ethernet first, WiFi fallback ===

  // Try Ethernet first (has priority)
  if (initEthernet()) {
    Serial.println("Using Ethernet as primary connection");
    configLoaded = true;

    // Disable WiFi completely when using Ethernet
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi disabled (Ethernet active)");
  } else {
    // Ethernet failed, use WiFi
    Serial.println("Ethernet not available, using WiFi...");

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
    }
  }

  // Force disable any rogue AP - but only touch WiFi if we're using it
  if (!useEthernet) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);  // Ensure station-only mode
    Serial.println("Soft AP disabled, WiFi in STA mode");
  } else {
    // Keep WiFi completely off when using Ethernet
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi disabled (Ethernet mode)");
  }

  if (!configLoaded) {
    Serial.println("Failed to connect to network. Restarting...");
    delay(3000);
    ESP.restart();
  }

  // === Common initialization for both Ethernet and WiFi ===

  // Configure NTP for time sync (GMT+2 for South Africa)
  configTime(2 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("NTP configured");

  // Load MQTT config from NVS
  loadMqttConfig();

  // Load protection config from NVS
  loadProtectionConfig();

  // Load schedule config from NVS
  loadScheduleConfig();

  // Load Ruraflex config from NVS
  loadRuraflexConfig();

  // Initialize schedule state and auto-start if booting within schedule window
  wasWithinSchedule = isWithinSchedule();
  Serial.printf("Schedule init: currently %s schedule window\n", wasWithinSchedule ? "within" : "outside");
  if ((scheduleEnabled || ruraflexEnabled) && wasWithinSchedule) {
    startCommand = true;
    Serial.println("Schedule: Boot within allowed hours, starting pump");
  }

  // Start web server
  setupWebServer();

  // Connect to cloud MQTT
  connectMQTT();

  // Setup ArduinoOTA for wireless uploads (works over both Ethernet and WiFi)
  ArduinoOTA.setHostname(DEVICE_ID);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    String type = (ArduinoOTA.getCommand() == U_FLASH) ? "sketch" : "filesystem";
    Serial.println("OTA: Start updating " + type);
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA: Update complete!");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA Progress: %u%%\r", (progress / (total / 100)));
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });

  ArduinoOTA.begin();
  Serial.printf("ArduinoOTA ready. Hostname: %s\n", DEVICE_ID);

  Serial.printf("\n=== Network: %s ===\n", useEthernet ? "ETHERNET (priority)" : "WiFi");
  Serial.printf("IP Address: %s\n", useEthernet ? Ethernet.localIP().toString().c_str() : WiFi.localIP().toString().c_str());
  Serial.println("Setup complete. Entering main loop...");
}

/* ================= LOOP ================= */

static bool lastDOState = false;

void loop() {
  unsigned long now = millis();

  // Handle OTA updates
  ArduinoOTA.handle();

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
  // TEMPORARILY DISABLED - testing DO3 issue
  // setDO(DO_RUN_LED_CH, state == RUNNING);
  // setDO(DO_FAULT_LED_CH, state == FAULT);

  // Force unused outputs OFF, preserve contactor (bit 0) and fault alarm (bit 4)
  do_state |= 0xEE;  // 0xEE = 1110 1110, preserves bits 0 and 4
  writeDO();         // Always sync to hardware

  if (now - lastSensorReadTime >= SENSOR_READ_INTERVAL_MS) {
    lastSensorReadTime = now;
    readCurrents();
    updateState();

    // Check schedule/Ruraflex - auto-start when entering window, auto-stop when leaving
    bool scheduleAllows = isWithinSchedule();
    if (scheduleEnabled || ruraflexEnabled) {
      // Detect transition INTO schedule window → auto-start
      if (scheduleAllows && !wasWithinSchedule && state != FAULT) {
        startCommand = true;
        Serial.println("Schedule: Entering allowed hours, starting pump");
      }
      // Detect transition OUT OF schedule window → auto-stop
      if (!scheduleAllows && wasWithinSchedule && startCommand) {
        startCommand = false;
        Serial.println("Schedule: Outside allowed hours, stopping pump");
      }
      wasWithinSchedule = scheduleAllows;
    }

    bool desiredDO = (startCommand && state != FAULT && scheduleAllows);
    if (desiredDO != lastDOState) {
      setDO(DO_CONTACTOR_CH, desiredDO);
      Serial.printf("Contactor: %s\n", desiredDO ? "ON" : "OFF");
      lastDOState = desiredDO;
    }
  }

  if (now - lastTelemetryTime >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryTime = now;

    if (mqttConnected && mqtt.connected()) {
      StaticJsonDocument<512> doc;

      doc["Va"] = round(Va * 10) / 10.0;
      doc["Vb"] = round(Vb * 10) / 10.0;
      doc["Vc"] = round(Vc * 10) / 10.0;
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
      doc["network"] = useEthernet ? "ETH" : "WiFi";
      doc["di"] = diStatus;
      doc["do"] = do_state;
      doc["hardware_type"] = HW_TYPE;
      doc["firmware_version"] = FW_VERSION;

      // Add device time from NTP
      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 10)) {
        char timeStr[9];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
        doc["time"] = timeStr;
      }

      char buf[512];
      size_t len = serializeJson(doc, buf);

      bool published = mqtt.publish(TOPIC_TELEMETRY, buf);
      if (published) {
        mqttPublishFailCount = 0;  // Reset on success
        lastMqttActivity = now;    // Update activity tracker
      } else {
        mqttPublishFailCount++;
        Serial.printf("MQTT publish failed (count=%d, len=%d)\n", mqttPublishFailCount, len);

        if (mqttPublishFailCount >= MAX_MQTT_PUBLISH_FAILURES) {
          Serial.println("Too many publish failures - forcing MQTT reconnect");
          mqtt.disconnect();
          mqttConnected = false;
          mqttPublishFailCount = 0;
        }
      }
    }
  }

  delay(10);
}
