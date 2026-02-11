/************************************************************
 * FIELDLINK PUMP CONTROLLER
 * Board: ESP32-S3 POE ETH 8DI 8DO (Waveshare)
 * Version: 2.10.0
 *
 * Project-specific code for pump control.
 * Uses FieldLinkCore shared library for board I/O,
 * networking, MQTT, OTA, and web server base.
 ************************************************************/

#include <FieldLinkCore.h>
#include <Preferences.h>
#include "secrets.h"

/* ================= PROJECT CONFIG ================= */

#define FW_NAME    "ESP32 Pump Controller"
#define FW_VERSION "2.10.0"
#define HW_TYPE    "PUMP_ESP32S3"

// Bench test mode - disable protections that require pump/load
#define BENCH_TEST_MODE false

// Timing intervals
#define TELEMETRY_INTERVAL_MS  2000
#define SENSOR_READ_INTERVAL_MS 500

// State detection hysteresis
#define HYSTERESIS_CURRENT     1.0
#define STATE_DEBOUNCE_COUNT   3

// Fault handling
#define FAULT_AUTO_RESET_MS    0

// Button debounce
#define DEBOUNCE_MS  50

// Protection defaults
#define RUN_THRESHOLD  5.0
#define START_TIMEOUT  10000

// DO channel assignments for pump controller
#define DO_CONTACTOR_CH  0   // Main contactor relay (DO1)
#define DO_RUN_LED_CH    1   // RUN indicator (DO2)
#define DO_FAULT_LED_CH  2   // FAULT indicator (DO3)
#define DO_FAULT_CH      4   // Fault alarm output (DO5)

/* ================= STATE MACHINE ================= */

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

// MQTT publish failure tracking
int mqttPublishFailCount = 0;
#define MAX_MQTT_PUBLISH_FAILURES 3

/* ================= PROTECTION CONFIG ================= */

// Configurable protection thresholds (stored in NVS)
float maxCurrentThreshold = 120.0;
float dryCurrentThreshold = 0.5;

// Protection enable flags
bool overcurrentProtectionEnabled = true;
bool dryRunProtectionEnabled = true;

// Fault delay settings (stored in NVS, in seconds)
uint32_t overcurrentDelayS = 0;
uint32_t dryrunDelayS = 0;

// Fault delay timers
unsigned long overcurrentStartTime = 0;
bool overcurrentConditionActive = false;
unsigned long dryrunStartTime = 0;
bool dryrunConditionActive = false;

// Contactor feedback (DI4 monitors aux contact)
bool contactorConfirmed = false;

/* ================= SCHEDULE CONFIG ================= */

bool scheduleEnabled = false;
uint8_t scheduleStartHour = 6;
uint8_t scheduleStartMinute = 0;
uint8_t scheduleEndHour = 18;
uint8_t scheduleEndMinute = 0;
uint8_t scheduleDays = 0x7F;
bool wasWithinSchedule = false;

// Ruraflex TOU settings
bool ruraflexEnabled = false;

/* ================= TIMING ================= */

unsigned long lastTelemetryTime = 0;
unsigned long lastSensorReadTime = 0;

// Manual button state
bool lastStartButtonState = false;
bool lastStopButtonState = true;  // NC button default
unsigned long lastStartDebounceTime = 0;
unsigned long lastStopDebounceTime = 0;

// Local/Remote mode
bool remoteMode = true;

// Digital input states
uint8_t diStatus = 0;

// Contactor tracking
static bool lastDOState = false;

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
void saveProtectionConfig();
void saveScheduleConfig();
void saveRuraflexConfig();

/* ================= STATE FUNCTIONS ================= */

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
    faultCurrentA = fl_Ia;
    faultCurrentB = fl_Ib;
    faultCurrentC = fl_Ic;

    startCommand = false;
    fl_setDO(DO_CONTACTOR_CH, false);
    fl_setDO(DO_FAULT_CH, true);

    Serial.printf("!!! FAULT TRIGGERED: %s !!!\n", faultTypeToString(type));
    Serial.printf("Currents at fault: Ia=%.2f Ib=%.2f Ic=%.2f\n", fl_Ia, fl_Ib, fl_Ic);

    // Send notification
    String payload = "{\"device_id\":\"";
    payload += fl_deviceId;
    payload += "\"}";
    fl_sendNotification(payload.c_str());
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
    fl_setDO(DO_FAULT_CH, false);
    Serial.println("Fault cleared. Ready to restart.");
  }
}

float getMaxCurrent() {
  float maxI = fl_Ia;
  if (fl_Ib > maxI) maxI = fl_Ib;
  if (fl_Ic > maxI) maxI = fl_Ic;
  return maxI;
}

PumpState evaluateState() {
  float maxCurrent = getMaxCurrent();
  unsigned long now = millis();

  // Overcurrent detection with configurable delay
  if (overcurrentProtectionEnabled && (fl_Ia > maxCurrentThreshold || fl_Ib > maxCurrentThreshold || fl_Ic > maxCurrentThreshold)) {
    if (!overcurrentConditionActive) {
      overcurrentConditionActive = true;
      overcurrentStartTime = now;
      Serial.printf("Overcurrent condition started (delay=%lus)\n", overcurrentDelayS);
    }
    if (overcurrentDelayS == 0 || (now - overcurrentStartTime) >= (overcurrentDelayS * 1000)) {
      return FAULT;
    }
  } else {
    if (overcurrentConditionActive) {
      Serial.println("Overcurrent condition cleared");
      overcurrentConditionActive = false;
    }
  }

  #if !BENCH_TEST_MODE
  // Dry run detection with configurable delay
  if (dryRunProtectionEnabled && dryCurrentThreshold > 0 && startCommand && state == RUNNING) {
    if (maxCurrent < dryCurrentThreshold) {
      if (!dryrunConditionActive) {
        dryrunConditionActive = true;
        dryrunStartTime = now;
        Serial.printf("Dry run condition started (delay=%lus)\n", dryrunDelayS);
      }
      if (dryrunDelayS == 0 || (now - dryrunStartTime) >= (dryrunDelayS * 1000)) {
        return FAULT;
      }
    } else {
      if (dryrunConditionActive) {
        Serial.println("Dry run condition cleared");
        dryrunConditionActive = false;
      }
    }
  } else {
    dryrunConditionActive = false;
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

  if (!fl_sensorOnline && fl_modbusFailCount >= FL_MAX_MODBUS_FAILURES) {
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

/* ================= NVS CONFIG ================= */

void loadProtectionConfig() {
  Preferences prefs;
  prefs.begin("protection", true);
  overcurrentProtectionEnabled = prefs.getBool("overcurrent", true);
  dryRunProtectionEnabled = prefs.getBool("dryrun", true);
  maxCurrentThreshold = prefs.getFloat("max_current", 120.0);
  dryCurrentThreshold = prefs.getFloat("dry_current", 0.5);
  overcurrentDelayS = prefs.getULong("oc_delay", 0);
  dryrunDelayS = prefs.getULong("dr_delay", 0);
  prefs.end();
  Serial.printf("Protection config loaded: max=%.1fA, dry=%.1fA, oc_delay=%lus, dr_delay=%lus\n",
                maxCurrentThreshold, dryCurrentThreshold, overcurrentDelayS, dryrunDelayS);
}

void saveProtectionConfig() {
  Preferences prefs;
  prefs.begin("protection", false);
  prefs.putBool("overcurrent", overcurrentProtectionEnabled);
  prefs.putBool("dryrun", dryRunProtectionEnabled);
  prefs.putFloat("max_current", maxCurrentThreshold);
  prefs.putFloat("dry_current", dryCurrentThreshold);
  prefs.putULong("oc_delay", overcurrentDelayS);
  prefs.putULong("dr_delay", dryrunDelayS);
  prefs.end();
  Serial.printf("Protection config saved: max=%.1fA, dry=%.1fA, oc_delay=%lus, dr_delay=%lus\n",
                maxCurrentThreshold, dryCurrentThreshold, overcurrentDelayS, dryrunDelayS);
}

void loadScheduleConfig() {
  Preferences prefs;
  prefs.begin("schedule", true);
  scheduleEnabled = prefs.getBool("enabled", false);
  scheduleStartHour = prefs.getUChar("startH", 6);
  scheduleStartMinute = prefs.getUChar("startM", 0);
  scheduleEndHour = prefs.getUChar("endH", 18);
  scheduleEndMinute = prefs.getUChar("endM", 0);
  scheduleDays = prefs.getUChar("days", 0x7F);
  prefs.end();
  Serial.println("Schedule config loaded");
}

void saveScheduleConfig() {
  Preferences prefs;
  prefs.begin("schedule", false);
  prefs.putBool("enabled", scheduleEnabled);
  prefs.putUChar("startH", scheduleStartHour);
  prefs.putUChar("startM", scheduleStartMinute);
  prefs.putUChar("endH", scheduleEndHour);
  prefs.putUChar("endM", scheduleEndMinute);
  prefs.putUChar("days", scheduleDays);
  prefs.end();
  Serial.println("Schedule config saved");
}

void loadRuraflexConfig() {
  Preferences prefs;
  prefs.begin("ruraflex", true);
  ruraflexEnabled = prefs.getBool("enabled", false);
  prefs.end();
  Serial.println("Ruraflex config loaded");
}

void saveRuraflexConfig() {
  Preferences prefs;
  prefs.begin("ruraflex", false);
  prefs.putBool("enabled", ruraflexEnabled);
  prefs.end();
  Serial.println("Ruraflex config saved");
}

/* ================= RURAFLEX TOU ================= */

bool isWithinRuraflex() {
  if (!ruraflexEnabled) return true;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10)) return true;

  int month = timeinfo.tm_mon + 1;
  int dayOfWeek = timeinfo.tm_wday;
  int hour = timeinfo.tm_hour;
  int mins = timeinfo.tm_min;
  int nowMins = hour * 60 + mins;

  bool isHighDemandSeason = (month >= 6 && month <= 8);
  bool isWeekday = (dayOfWeek >= 1 && dayOfWeek <= 5);

  bool isPeak = false;
  bool isStandard = false;

  if (isWeekday) {
    if (isHighDemandSeason) {
      isPeak = (nowMins >= 360 && nowMins < 480) || (nowMins >= 1020 && nowMins < 1200);
      isStandard = (nowMins >= 480 && nowMins < 1020) || (nowMins >= 1200 && nowMins < 1320);
    } else {
      isPeak = (nowMins >= 420 && nowMins < 540) || (nowMins >= 1020 && nowMins < 1200);
      isStandard = (nowMins >= 360 && nowMins < 420) || (nowMins >= 540 && nowMins < 1020) || (nowMins >= 1200 && nowMins < 1320);
    }
  } else {
    isPeak = false;
    isStandard = (nowMins >= 420 && nowMins < 720) || (nowMins >= 1080 && nowMins < 1200);
  }

  bool isOffPeak = !isPeak && !isStandard;
  return isOffPeak;
}

bool isWithinSchedule() {
  if (ruraflexEnabled) {
    return isWithinRuraflex();
  }

  if (!scheduleEnabled) return true;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10)) return true;

  if (!(scheduleDays & (1 << timeinfo.tm_wday))) {
    return false;
  }

  int nowMins = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int startMins = scheduleStartHour * 60 + scheduleStartMinute;
  int endMins = scheduleEndHour * 60 + scheduleEndMinute;

  if (startMins <= endMins) {
    return nowMins >= startMins && nowMins < endMins;
  } else {
    return nowMins >= startMins || nowMins < endMins;
  }
}

/* ================= MQTT HANDLER ================= */

void pumpMqttHandler(const char* cmd, unsigned int length) {
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
    startCommand = false;
    fl_setDO(DO_CONTACTOR_CH, false);
    if (state != FAULT) {
      state = STOPPED;
    }
    Serial.println("Stop command accepted");
  }
  else if (strcmp(cmd, "RESET") == 0) {
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

      if (command && strcmp(command, "SET_PROTECTION") == 0) {
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
      else if (command && strcmp(command, "SET_DELAYS") == 0) {
        if (doc.containsKey("overcurrent_delay_s")) {
          uint32_t val = doc["overcurrent_delay_s"];
          if (val <= 30) overcurrentDelayS = val;
        }
        if (doc.containsKey("dryrun_delay_s")) {
          uint32_t val = doc["dryrun_delay_s"];
          if (val <= 30) dryrunDelayS = val;
        }
        saveProtectionConfig();
        Serial.printf("Delays updated: overcurrent=%lus, dryrun=%lus\n", overcurrentDelayS, dryrunDelayS);
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
        if (ruraflexEnabled && scheduleEnabled) {
          scheduleEnabled = false;
          saveScheduleConfig();
        }
        saveRuraflexConfig();
        Serial.println("Ruraflex updated via MQTT");
      }
      else if (command && strcmp(command, "GET_SETTINGS") == 0) {
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
        resp["overcurrent_delay_s"] = overcurrentDelayS;
        resp["dryrun_delay_s"] = dryrunDelayS;
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 10)) {
          char timeStr[9];
          strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
          resp["current_time"] = timeStr;
        }
        char buf[512];
        serializeJson(resp, buf);
        fl_mqtt.publish(fl_topicTelemetry, buf);
        Serial.println("Settings sent via MQTT");
      }
    }
  }
}

/* ================= SERIAL HANDLER ================= */

bool pumpSerialHandler(const String& input) {
  if (input == "START") {
    if (state == FAULT) {
      Serial.println("Cannot start while in FAULT. Use FAULT_RESET first.");
    } else {
      startCommand = true;
      startCommandTime = millis();
      Serial.println("Start command issued");
    }
    return true;
  }
  else if (input == "STOP") {
    startCommand = false;
    fl_setDO(DO_CONTACTOR_CH, false);
    Serial.println("Stop command issued");
    return true;
  }
  else if (input == "FAULT_RESET" || input == "CLEAR") {
    if (state == FAULT) {
      resetFault();
    } else {
      Serial.println("No fault to clear");
    }
    return true;
  }
  else if (input == "TEST_FAULT") {
    Serial.println("Testing fault trigger...");
    Serial.printf("do_state BEFORE: 0x%02X\n", fl_do_state);
    triggerFault(SENSOR_FAULT);
    Serial.printf("do_state AFTER:  0x%02X\n", fl_do_state);
    Serial.printf("DO_FAULT_CH = %d, expected bit = 0x%02X\n", DO_FAULT_CH, (1 << DO_FAULT_CH));
    return true;
  }
  else if (input == "STATUS") {
    // Add pump-specific status info (base STATUS is handled by shared lib)
    Serial.println("\n--- Pump State ---");
    Serial.printf("State: %s\n", state==RUNNING?"RUNNING":state==FAULT?"FAULT":"STOPPED");
    Serial.printf("Start Command: %s\n", startCommand ? "Yes" : "No");
    Serial.printf("Mode: %s\n", remoteMode ? "REMOTE" : "LOCAL");
    Serial.printf("Voltages: Va=%.1f Vb=%.1f Vc=%.1f V\n", fl_Va, fl_Vb, fl_Vc);
    Serial.printf("Currents: Ia=%.2f Ib=%.2f Ic=%.2f A\n", fl_Ia, fl_Ib, fl_Ic);
    if (state == FAULT) {
      Serial.printf("Fault Type: %s\n", faultTypeToString(faultType));
    }
    return false;  // Let base STATUS also run
  }
  else if (input == "HELP") {
    // Add pump-specific help (base HELP is handled by shared lib)
    Serial.println("\n=== SERIAL COMMANDS (Pump) ===");
    Serial.println("START        - Start pump");
    Serial.println("STOP         - Stop pump");
    Serial.println("FAULT_RESET  - Clear fault condition");
    Serial.println("TEST_FAULT   - Test fault alarm output");
    return false;  // Already called from base HELP
  }
  return false;  // Not handled
}

/* ================= PUMP WEB ROUTES ================= */

void setupPumpWebRoutes() {
  AsyncWebServer& server = FieldLink::getWebServer();

  // Dashboard (main page)
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    request->send(200, "text/html", DASHBOARD_HTML);
  });

  // API: pump status
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    StaticJsonDocument<384> doc;
    doc["Va"] = fl_Va;
    doc["Vb"] = fl_Vb;
    doc["Vc"] = fl_Vc;
    doc["Ia"] = fl_Ia;
    doc["Ib"] = fl_Ib;
    doc["Ic"] = fl_Ic;
    doc["state"] = (state == RUNNING ? "RUNNING" : state == FAULT ? "FAULT" : "STOPPED");
    doc["cmd"] = startCommand;
    doc["sensor"] = fl_sensorOnline;
    doc["uptime"] = millis() / 1000;
    if (state == FAULT) {
      doc["fault"] = faultTypeToString(faultType);
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // API: pump commands
  server.on("/api/command", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    if (request->hasParam("cmd", true)) {
      String cmd = request->getParam("cmd", true)->value();
      if (cmd == "START" && state != FAULT) {
        startCommand = true;
        startCommandTime = millis();
        request->send(200, "text/plain", "OK");
      } else if (cmd == "STOP") {
        startCommand = false;
        fl_setDO(DO_CONTACTOR_CH, false);
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

  // API: protection settings
  server.on("/api/protection", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    StaticJsonDocument<128> doc;
    doc["overcurrent_enabled"] = overcurrentProtectionEnabled;
    doc["dryrun_enabled"] = dryRunProtectionEnabled;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  server.on("/api/protection", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    if (request->hasParam("overcurrent_enabled", true))
      overcurrentProtectionEnabled = request->getParam("overcurrent_enabled", true)->value() == "true";
    if (request->hasParam("dryrun_enabled", true))
      dryRunProtectionEnabled = request->getParam("dryrun_enabled", true)->value() == "true";
    saveProtectionConfig();
    request->send(200, "text/plain", "Protection settings saved");
  });

  // API: schedule settings
  server.on("/api/schedule", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    StaticJsonDocument<384> doc;
    doc["enabled"] = scheduleEnabled;
    doc["start_hour"] = scheduleStartHour;
    doc["start_minute"] = scheduleStartMinute;
    doc["end_hour"] = scheduleEndHour;
    doc["end_minute"] = scheduleEndMinute;
    doc["days"] = scheduleDays;
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
      doc["current_day"] = timeinfo.tm_wday;
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  server.on("/api/schedule", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
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
}

/* ================= SETUP ================= */

void setup() {
  // Configure shared library with secrets
  FieldLink::setWebAuth(WEB_AUTH_USER, WEB_AUTH_PASS);
  FieldLink::setOtaPassword(OTA_PASSWORD);
  FieldLink::setDefaultMqtt(DEFAULT_MQTT_HOST, DEFAULT_MQTT_PORT,
                             DEFAULT_MQTT_USER, DEFAULT_MQTT_PASS);
  FieldLink::setNotificationUrl(NOTIFICATION_WEBHOOK_URL);

  // Initialize all board subsystems
  FieldLink::begin(FW_NAME, FW_VERSION, HW_TYPE, BENCH_TEST_MODE);

  // Load project-specific configs
  loadProtectionConfig();
  loadScheduleConfig();
  loadRuraflexConfig();

  // Register project handlers
  FieldLink::setMqttHandler(pumpMqttHandler);
  FieldLink::setSerialHandler(pumpSerialHandler);

  // Add pump-specific web routes
  setupPumpWebRoutes();

  // Start web server (after all routes are registered)
  FieldLink::startWebServer();

  // Initialize schedule state
  wasWithinSchedule = isWithinSchedule();
  Serial.printf("Schedule init: currently %s schedule window\n", wasWithinSchedule ? "within" : "outside");
  if ((scheduleEnabled || ruraflexEnabled) && wasWithinSchedule) {
    startCommand = true;
    Serial.println("Schedule: Boot within allowed hours, starting pump");
  }

  Serial.println("Pump controller setup complete. Entering main loop...");
}

/* ================= LOOP ================= */

void loop() {
  unsigned long now = millis();

  // Shared library tick (OTA, serial, MQTT)
  FieldLink::tick();

  // Read all digital inputs
  diStatus = fl_readDI();

  // Contactor feedback (DI4)
  bool di4Active = (diStatus & 0x08) != 0;
  bool contactorOn = (fl_do_state & (1 << DO_CONTACTOR_CH)) == 0;
  contactorConfirmed = contactorOn && di4Active;

  // Local/Remote mode (DI3)
  remoteMode = digitalRead(DI3_PIN);

  // START button (DI1 - NO)
  bool startButtonReading = !digitalRead(DI1_PIN);
  if (startButtonReading != lastStartButtonState) {
    if ((now - lastStartDebounceTime) > DEBOUNCE_MS) {
      lastStartDebounceTime = now;
      lastStartButtonState = startButtonReading;
      if (startButtonReading) {
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

  // STOP button (DI2 - NC)
  bool stopButtonActive = !digitalRead(DI2_PIN);
  if (stopButtonActive != lastStopButtonState) {
    if ((now - lastStopDebounceTime) > DEBOUNCE_MS) {
      lastStopDebounceTime = now;
      lastStopButtonState = stopButtonActive;
      if (!stopButtonActive) {
        if (startCommand) {
          startCommand = false;
          fl_setDO(DO_CONTACTOR_CH, false);
          Serial.println("Manual STOP button pressed");
        }
      }
    }
  }

  // Update indicator outputs
  // TEMPORARILY DISABLED - testing DO3 issue
  // fl_setDO(DO_RUN_LED_CH, state == RUNNING);
  // fl_setDO(DO_FAULT_LED_CH, state == FAULT);

  // Force unused outputs OFF, preserve contactor (bit 0) and fault alarm (bit 4)
  fl_do_state |= 0xEE;
  fl_writeDO();

  // Sensor reading and state machine
  if (now - lastSensorReadTime >= SENSOR_READ_INTERVAL_MS) {
    lastSensorReadTime = now;
    fl_readSensors();
    updateState();

    // Schedule/Ruraflex handling
    bool scheduleAllows = isWithinSchedule();
    if (scheduleEnabled || ruraflexEnabled) {
      if (scheduleAllows && !wasWithinSchedule && state != FAULT) {
        startCommand = true;
        Serial.println("Schedule: Entering allowed hours, starting pump");
      }
      if (!scheduleAllows && wasWithinSchedule && startCommand) {
        startCommand = false;
        Serial.println("Schedule: Outside allowed hours, stopping pump");
      }
      wasWithinSchedule = scheduleAllows;
    }

    bool desiredDO = (startCommand && state != FAULT && scheduleAllows);
    if (desiredDO != lastDOState) {
      fl_setDO(DO_CONTACTOR_CH, desiredDO);
      Serial.printf("Contactor: %s\n", desiredDO ? "ON" : "OFF");
      lastDOState = desiredDO;
    }
  }

  // Telemetry publishing
  if (now - lastTelemetryTime >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryTime = now;

    if (fl_mqttConnected && fl_mqtt.connected()) {
      StaticJsonDocument<512> doc;

      doc["Va"] = round(fl_Va * 10) / 10.0;
      doc["Vb"] = round(fl_Vb * 10) / 10.0;
      doc["Vc"] = round(fl_Vc * 10) / 10.0;
      doc["Ia"] = round(fl_Ia * 100) / 100.0;
      doc["Ib"] = round(fl_Ib * 100) / 100.0;
      doc["Ic"] = round(fl_Ic * 100) / 100.0;
      doc["state"] = (state == RUNNING ? "RUNNING" : state == FAULT ? "FAULT" : "STOPPED");
      doc["cmd"] = startCommand;

      if (state == FAULT) {
        doc["fault"] = faultTypeToString(faultType);
      }

      doc["sensor"] = fl_sensorOnline;
      doc["contactor_confirmed"] = contactorConfirmed;
      doc["uptime"] = now / 1000;
      doc["mode"] = remoteMode ? "REMOTE" : "LOCAL";
      doc["network"] = fl_useEthernet ? "ETH" : "WiFi";
      doc["di"] = diStatus;
      doc["do"] = fl_do_state;
      doc["hardware_type"] = HW_TYPE;
      doc["firmware_version"] = FW_VERSION;

      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 10)) {
        char timeStr[9];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
        doc["time"] = timeStr;
      }

      char buf[512];
      size_t len = serializeJson(doc, buf);

      bool published = fl_mqtt.publish(fl_topicTelemetry, buf);
      if (published) {
        mqttPublishFailCount = 0;
        fl_lastMqttActivity = now;
      } else {
        mqttPublishFailCount++;
        Serial.printf("MQTT publish failed (count=%d, len=%d)\n", mqttPublishFailCount, len);

        if (mqttPublishFailCount >= MAX_MQTT_PUBLISH_FAILURES) {
          Serial.println("Too many publish failures - forcing MQTT reconnect");
          fl_mqtt.disconnect();
          fl_mqttConnected = false;
          mqttPublishFailCount = 0;
        }
      }
    }
  }

  delay(10);
}
