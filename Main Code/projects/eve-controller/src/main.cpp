/************************************************************
 * FieldLink Eve 3-Pump Controller
 * Board: ESP32-S3 POE ETH 8DI 8DO (Waveshare)
 * Version: 1.0.0
 *
 * Uses FieldLinkCore shared library for board support,
 * networking, MQTT, OTA, and web server.
 *
 * Controls 3 independent pumps from a single energy meter:
 *   L1 (Va/Ia) = Pump 1
 *   L2 (Vb/Ib) = Pump 2
 *   L3 (Vc/Ic) = Pump 3
 *
 * Features:
 * - Per-pump overcurrent/dry-run protection with NVS storage
 * - Shared schedule and Ruraflex TOU control
 * - Per-pump and aggregate MQTT commands
 * - Embedded 3-pump web dashboard
 * - Remote-only control (no local buttons)
 ************************************************************/

#include <FieldLinkCore.h>
#include <ArduinoJson.h>
#include "secrets.h"

/* ================= PROJECT CONFIG ================= */

#define FW_NAME    "ESP32 Eve 3-Pump Controller"
#define FW_VERSION "1.0.0"
#define HW_TYPE    "EVE_ESP32S3"

#define NUM_PUMPS 3

// Timing intervals (non-blocking)
#define TELEMETRY_INTERVAL_MS   2000
#define SENSOR_READ_INTERVAL_MS 500

// State detection hysteresis
#define HYSTERESIS_CURRENT      1.0
#define STATE_DEBOUNCE_COUNT    3

// Fault handling
#define FAULT_AUTO_RESET_MS     0

// Protection
#define RUN_THRESHOLD   5.0
#define START_TIMEOUT   10000

/* ================= PUMP STATE ================= */

enum PumpState { STOPPED, RUNNING, FAULT };
enum FaultType { NO_FAULT, OVERCURRENT, DRY_RUN, SENSOR_FAULT };

struct Pump {
  uint8_t id;              // 1, 2, 3

  // DO channels
  uint8_t doContactor;     // DO0, DO1, DO2
  uint8_t doFaultAlarm;    // DO4, DO5, DO6

  // DI feedback
  uint8_t diFeedbackBit;   // bit position in fl_diStatus

  // Sensor pointers (to fl_Va/Ia, fl_Vb/Ib, fl_Vc/Ic)
  float* voltage;
  float* current;

  // State machine
  PumpState state;
  PumpState pendingState;
  FaultType faultType;
  bool startCommand;
  unsigned long startCommandTime;
  int stateDebounceCounter;
  bool contactorConfirmed;
  bool lastDOState;

  // Fault tracking
  unsigned long faultTimestamp;
  float faultCurrent;

  // Per-pump protection thresholds (NVS-stored)
  float maxCurrentThreshold;
  float dryCurrentThreshold;
  bool overcurrentEnabled;
  bool dryRunEnabled;
  uint32_t overcurrentDelayS;
  uint32_t dryrunDelayS;

  // Fault delay timers
  unsigned long overcurrentStartTime;
  bool overcurrentConditionActive;
  unsigned long dryrunStartTime;
  bool dryrunConditionActive;

  // NVS namespace
  char nvsNamespace[8];    // "prot_p1", "prot_p2", "prot_p3"
};

Pump pumps[NUM_PUMPS];

// Schedule settings (shared across all pumps)
bool scheduleEnabled = false;
uint8_t scheduleStartHour = 6;
uint8_t scheduleStartMinute = 0;
uint8_t scheduleEndHour = 18;
uint8_t scheduleEndMinute = 0;
uint8_t scheduleDays = 0x7F;  // Bitmask: bit0=Sun...bit6=Sat (0x7F = all days)
bool wasWithinSchedule = false;

// Ruraflex TOU settings (Eskom South Africa)
bool ruraflexEnabled = false;

// Non-blocking timing
unsigned long lastTelemetryTime = 0;
unsigned long lastSensorReadTime = 0;

/* ================= DASHBOARD HTML ================= */

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>FieldLink Eve 3-Pump Controller</title>
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
    .pump-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 20px; margin-bottom: 20px; }
    .pump-card {
      background: var(--bg-card); border: 1px solid var(--border-color);
      border-radius: 12px; padding: 20px;
    }
    .pump-card.fault { border-color: var(--status-fault); box-shadow: 0 0 20px rgba(255, 71, 87, 0.2); }
    .pump-card.running { border-color: var(--status-running); box-shadow: 0 0 15px rgba(0, 255, 136, 0.1); }
    .card {
      background: var(--bg-card); border: 1px solid var(--border-color);
      border-radius: 12px; padding: 20px; margin-bottom: 20px;
    }
    .card-title {
      font-family: 'Chakra Petch', sans-serif; font-size: 12px; font-weight: 600;
      text-transform: uppercase; letter-spacing: 1.5px; color: var(--text-secondary);
      margin-bottom: 16px;
    }
    .state-indicator {
      display: inline-flex; align-items: center; gap: 10px; padding: 10px 20px;
      border-radius: 8px; background: var(--bg-secondary); border: 2px solid var(--border-color);
      margin-bottom: 12px; width: 100%; justify-content: center;
    }
    .state-indicator.running { border-color: var(--status-running); box-shadow: 0 0 20px rgba(0, 255, 136, 0.15); }
    .state-indicator.fault { border-color: var(--status-fault); box-shadow: 0 0 20px rgba(255, 71, 87, 0.2); animation: pulse 1.5s infinite; }
    @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.7; } }
    .state-icon { width: 14px; height: 14px; border-radius: 50%; }
    .state-icon.running { background: var(--status-running); box-shadow: 0 0 10px var(--status-running); }
    .state-icon.stopped { background: var(--status-stopped); }
    .state-icon.fault { background: var(--status-fault); box-shadow: 0 0 10px var(--status-fault); }
    .state-text { font-family: 'Chakra Petch', sans-serif; font-size: 18px; font-weight: 700; letter-spacing: 2px; }
    .state-text.running { color: var(--status-running); }
    .state-text.stopped { color: var(--status-stopped); }
    .state-text.fault { color: var(--status-fault); }
    .reading-row { display: flex; justify-content: space-between; padding: 8px 0; border-bottom: 1px solid var(--border-color); }
    .reading-row:last-child { border-bottom: none; }
    .reading-label { font-size: 12px; color: var(--text-secondary); }
    .reading-value { font-size: 14px; font-weight: 600; color: var(--accent-cyan); }
    .reading-value.fault-text { color: var(--status-fault); }
    .pump-controls { display: flex; gap: 8px; margin-top: 12px; }
    .btn {
      font-family: 'Chakra Petch', sans-serif; font-size: 11px; font-weight: 600;
      letter-spacing: 1px; padding: 10px 12px; border: none; border-radius: 8px;
      cursor: pointer; text-transform: uppercase; flex: 1;
    }
    .btn-start { background: linear-gradient(135deg, #00aa66, #00ff88); color: var(--bg-primary); }
    .btn-stop { background: linear-gradient(135deg, #cc3344, #ff4757); color: white; }
    .btn-reset { background: var(--bg-secondary); color: var(--text-primary); border: 1px solid var(--border-color); }
    .btn:disabled { opacity: 0.5; cursor: not-allowed; }
    .aggregate-controls { display: flex; gap: 12px; }
    .aggregate-controls .btn { flex: 1; padding: 14px 20px; font-size: 13px; }
    .status-row { display: flex; gap: 20px; }
    .status-row .card { flex: 1; }
    .status-list { display: flex; flex-direction: column; gap: 10px; }
    .status-item {
      display: flex; justify-content: space-between; align-items: center;
      padding: 10px; background: var(--bg-secondary); border-radius: 8px;
    }
    .status-label { font-size: 12px; color: var(--text-secondary); }
    .status-badge {
      padding: 3px 8px; border-radius: 4px; font-size: 10px; font-weight: 600;
      text-transform: uppercase;
    }
    .status-badge.online { background: rgba(0, 255, 136, 0.15); color: var(--status-running); }
    .status-badge.offline { background: rgba(255, 71, 87, 0.15); color: var(--status-fault); }
    .uptime-value { font-size: 24px; font-weight: 500; letter-spacing: 2px; text-align: center; }
    .uptime-label { font-size: 10px; color: var(--text-muted); margin-top: 4px; text-align: center; }
    @media (max-width: 900px) { .pump-grid { grid-template-columns: 1fr; } .status-row { flex-direction: column; } .aggregate-controls { flex-direction: column; } }
  </style>
</head>
<body>
  <div class="container">
    <header class="header">
      <div class="logo">
        <div class="logo-icon">FL</div>
        <div class="logo-text">Field<span>Link</span> Eve</div>
      </div>
      <div class="connection-status">
        <div class="status-dot" id="mqttStatus"></div>
        <span id="mqttStatusText">Connecting...</span>
      </div>
    </header>
    <div class="pump-grid">
      <div class="pump-card" id="pumpCard1">
        <div class="card-title">Pump 1 (L1)</div>
        <div class="state-indicator stopped" id="si1">
          <div class="state-icon stopped" id="icon1"></div>
          <div class="state-text stopped" id="st1">---</div>
        </div>
        <div class="reading-row"><span class="reading-label">Voltage</span><span class="reading-value" id="v1">--</span></div>
        <div class="reading-row"><span class="reading-label">Current</span><span class="reading-value" id="i1">--</span></div>
        <div class="reading-row"><span class="reading-label">Contactor</span><span class="reading-value" id="cf1">--</span></div>
        <div class="reading-row"><span class="reading-label">Fault</span><span class="reading-value fault-text" id="f1">--</span></div>
        <div class="pump-controls">
          <button class="btn btn-start" onclick="sendCmd('START',1)">Start</button>
          <button class="btn btn-stop" onclick="sendCmd('STOP',1)">Stop</button>
          <button class="btn btn-reset" onclick="sendCmd('RESET',1)">Reset</button>
        </div>
      </div>
      <div class="pump-card" id="pumpCard2">
        <div class="card-title">Pump 2 (L2)</div>
        <div class="state-indicator stopped" id="si2">
          <div class="state-icon stopped" id="icon2"></div>
          <div class="state-text stopped" id="st2">---</div>
        </div>
        <div class="reading-row"><span class="reading-label">Voltage</span><span class="reading-value" id="v2">--</span></div>
        <div class="reading-row"><span class="reading-label">Current</span><span class="reading-value" id="i2">--</span></div>
        <div class="reading-row"><span class="reading-label">Contactor</span><span class="reading-value" id="cf2">--</span></div>
        <div class="reading-row"><span class="reading-label">Fault</span><span class="reading-value fault-text" id="f2">--</span></div>
        <div class="pump-controls">
          <button class="btn btn-start" onclick="sendCmd('START',2)">Start</button>
          <button class="btn btn-stop" onclick="sendCmd('STOP',2)">Stop</button>
          <button class="btn btn-reset" onclick="sendCmd('RESET',2)">Reset</button>
        </div>
      </div>
      <div class="pump-card" id="pumpCard3">
        <div class="card-title">Pump 3 (L3)</div>
        <div class="state-indicator stopped" id="si3">
          <div class="state-icon stopped" id="icon3"></div>
          <div class="state-text stopped" id="st3">---</div>
        </div>
        <div class="reading-row"><span class="reading-label">Voltage</span><span class="reading-value" id="v3">--</span></div>
        <div class="reading-row"><span class="reading-label">Current</span><span class="reading-value" id="i3">--</span></div>
        <div class="reading-row"><span class="reading-label">Contactor</span><span class="reading-value" id="cf3">--</span></div>
        <div class="reading-row"><span class="reading-label">Fault</span><span class="reading-value fault-text" id="f3">--</span></div>
        <div class="pump-controls">
          <button class="btn btn-start" onclick="sendCmd('START',3)">Start</button>
          <button class="btn btn-stop" onclick="sendCmd('STOP',3)">Stop</button>
          <button class="btn btn-reset" onclick="sendCmd('RESET',3)">Reset</button>
        </div>
      </div>
    </div>
    <div class="card">
      <div class="card-title">All Pumps</div>
      <div class="aggregate-controls">
        <button class="btn btn-start" onclick="sendAll('START_ALL')">Start All</button>
        <button class="btn btn-stop" onclick="sendAll('STOP_ALL')">Stop All</button>
        <button class="btn btn-reset" onclick="sendAll('RESET_ALL')">Reset All</button>
      </div>
    </div>
    <div class="status-row">
      <div class="card">
        <div class="card-title">System Info</div>
        <div class="status-list">
          <div class="status-item">
            <span class="status-label">Sensor</span>
            <span class="status-badge offline" id="sensorStatus">OFFLINE</span>
          </div>
          <div class="status-item">
            <span class="status-label">Network</span>
            <span class="status-badge" id="networkStatus">--</span>
          </div>
        </div>
      </div>
      <div class="card">
        <div class="card-title">Uptime</div>
        <div style="padding: 12px 0;">
          <div class="uptime-value" id="uptime">--:--:--</div>
          <div class="uptime-label">UPTIME</div>
        </div>
      </div>
    </div>
  </div>
  <script>
    const MQTT_BROKER = 'wss://)" DEFAULT_MQTT_HOST R"(:8884/mqtt';
    const MQTT_USER = ')" DEFAULT_MQTT_USER R"(';
    const MQTT_PASS = ')" DEFAULT_MQTT_PASS R"(';
    let TOPIC_TELEMETRY = '', TOPIC_COMMAND = '', DEVICE_ID = '';
    let client = null, isConnected = false;
    function formatUptime(s) {
      const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
      return `${h.toString().padStart(2,'0')}:${m.toString().padStart(2,'0')}:${sec.toString().padStart(2,'0')}`;
    }
    function updatePumpCard(n, state, voltage, current, fault, cf) {
      const s = state.toLowerCase();
      const card = document.getElementById('pumpCard' + n);
      card.className = 'pump-card' + (s === 'fault' ? ' fault' : s === 'running' ? ' running' : '');
      const si = document.getElementById('si' + n);
      si.className = 'state-indicator ' + s;
      document.getElementById('icon' + n).className = 'state-icon ' + s;
      const st = document.getElementById('st' + n);
      st.className = 'state-text ' + s;
      st.textContent = state;
      document.getElementById('v' + n).textContent = parseFloat(voltage).toFixed(1) + ' V';
      document.getElementById('i' + n).textContent = parseFloat(current).toFixed(2) + ' A';
      document.getElementById('cf' + n).textContent = cf ? 'CONFIRMED' : 'OFF';
      document.getElementById('f' + n).textContent = fault || 'NONE';
    }
    function updateTelemetry(data) {
      try {
        const t = JSON.parse(data);
        updatePumpCard(1, t.s1, t.V1, t.I1, t.f1, t.cf1);
        updatePumpCard(2, t.s2, t.V2, t.I2, t.f2, t.cf2);
        updatePumpCard(3, t.s3, t.V3, t.I3, t.f3, t.cf3);
        const se = document.getElementById('sensorStatus');
        se.textContent = t.sensor ? 'ONLINE' : 'OFFLINE';
        se.className = 'status-badge ' + (t.sensor ? 'online' : 'offline');
        const ne = document.getElementById('networkStatus');
        ne.textContent = t.network || '--';
        ne.className = 'status-badge online';
        document.getElementById('uptime').textContent = formatUptime(t.uptime);
      } catch (e) { console.error('Parse error:', e); }
    }
    function sendCmd(cmd, pump) {
      if (client && isConnected) {
        client.publish(TOPIC_COMMAND, JSON.stringify({command: cmd, pump: pump}));
      } else { alert('Not connected'); }
    }
    function sendAll(cmd) {
      if (client && isConnected) {
        client.publish(TOPIC_COMMAND, JSON.stringify({command: cmd}));
      } else { alert('Not connected'); }
    }
    async function fetchDeviceInfo() {
      try {
        const r = await fetch('/api/device');
        const d = await r.json();
        DEVICE_ID = d.device_id;
        TOPIC_TELEMETRY = d.topic_telemetry;
        TOPIC_COMMAND = d.topic_command;
        document.title = 'FieldLink Eve - ' + DEVICE_ID;
        return true;
      } catch (e) { console.error('Device info error:', e); return false; }
    }
    async function connect() {
      document.getElementById('mqttStatusText').textContent = 'Loading...';
      if (!await fetchDeviceInfo()) { document.getElementById('mqttStatusText').textContent = 'Device Error'; return; }
      document.getElementById('mqttStatusText').textContent = 'Connecting...';
      client = mqtt.connect(MQTT_BROKER, {
        username: MQTT_USER, password: MQTT_PASS,
        clientId: 'local_' + DEVICE_ID + '_' + Math.random().toString(16).substr(2, 8),
        reconnectPeriod: 5000
      });
      client.on('connect', () => {
        isConnected = true;
        document.getElementById('mqttStatus').classList.add('connected');
        document.getElementById('mqttStatusText').textContent = 'Connected';
        client.subscribe(TOPIC_TELEMETRY);
      });
      client.on('message', (topic, msg) => { if (topic === TOPIC_TELEMETRY) updateTelemetry(msg.toString()); });
      client.on('close', () => {
        isConnected = false;
        document.getElementById('mqttStatus').classList.remove('connected');
        document.getElementById('mqttStatusText').textContent = 'Disconnected';
      });
      client.on('reconnect', () => { document.getElementById('mqttStatusText').textContent = 'Reconnecting...'; });
    }
    document.addEventListener('DOMContentLoaded', connect);
  </script>
</body>
</html>
)rawliteral";

/* ================= FORWARD DECLARATIONS ================= */

void initPumps();
void triggerFault(Pump& p, FaultType type);
void resetFault(Pump& p);
const char* faultTypeToString(FaultType ft);
const char* stateToString(PumpState s);
PumpState evaluatePumpState(Pump& p);
void updatePumpState(Pump& p);
void loadPumpProtection(Pump& p);
void savePumpProtection(Pump& p);
void loadScheduleConfig();
void saveScheduleConfig();
void loadRuraflexConfig();
void saveRuraflexConfig();
bool isWithinSchedule();

/* ================= PUMP INITIALIZATION ================= */

static void initPump(Pump& p, uint8_t id, uint8_t doCont, uint8_t doFault,
                     uint8_t diBit, float* volt, float* curr, const char* nvs) {
  p.id = id;
  p.doContactor = doCont;
  p.doFaultAlarm = doFault;
  p.diFeedbackBit = diBit;
  p.voltage = volt;
  p.current = curr;
  p.state = STOPPED;
  p.pendingState = STOPPED;
  p.faultType = NO_FAULT;
  p.startCommand = false;
  p.startCommandTime = 0;
  p.stateDebounceCounter = 0;
  p.contactorConfirmed = false;
  p.lastDOState = false;
  p.faultTimestamp = 0;
  p.faultCurrent = 0;
  p.maxCurrentThreshold = 120.0;
  p.dryCurrentThreshold = 0.5;
  p.overcurrentEnabled = true;
  p.dryRunEnabled = true;
  p.overcurrentDelayS = 0;
  p.dryrunDelayS = 0;
  p.overcurrentStartTime = 0;
  p.overcurrentConditionActive = false;
  p.dryrunStartTime = 0;
  p.dryrunConditionActive = false;
  strncpy(p.nvsNamespace, nvs, sizeof(p.nvsNamespace) - 1);
  p.nvsNamespace[sizeof(p.nvsNamespace) - 1] = '\0';
}

void initPumps() {
  // Pump 1: L1 = Va/Ia, DO0 contactor, DO4 fault alarm, DI1 feedback (bit 0)
  initPump(pumps[0], 1, 0, 4, 0, &fl_Va, &fl_Ia, "prot_p1");
  // Pump 2: L2 = Vb/Ib, DO1 contactor, DO5 fault alarm, DI2 feedback (bit 1)
  initPump(pumps[1], 2, 1, 5, 1, &fl_Vb, &fl_Ib, "prot_p2");
  // Pump 3: L3 = Vc/Ic, DO2 contactor, DO6 fault alarm, DI3 feedback (bit 2)
  initPump(pumps[2], 3, 2, 6, 2, &fl_Vc, &fl_Ic, "prot_p3");
}

/* ================= STATE FUNCTIONS ================= */

const char* faultTypeToString(FaultType ft) {
  switch (ft) {
    case OVERCURRENT:   return "OVERCURRENT";
    case DRY_RUN:       return "DRY_RUN";
    case SENSOR_FAULT:  return "SENSOR_FAULT";
    default:            return "";
  }
}

const char* stateToString(PumpState s) {
  switch (s) {
    case RUNNING: return "RUNNING";
    case FAULT:   return "FAULT";
    default:      return "STOPPED";
  }
}

void triggerFault(Pump& p, FaultType type) {
  if (p.state != FAULT) {
    p.state = FAULT;
    p.faultType = type;
    p.faultTimestamp = millis();
    p.faultCurrent = *(p.current);

    p.startCommand = false;
    fl_setDO(p.doContactor, false);
    fl_setDO(p.doFaultAlarm, true);

    Serial.printf("!!! PUMP %d FAULT: %s (I=%.2fA) !!!\n", p.id, faultTypeToString(type), p.faultCurrent);
  }
}

void resetFault(Pump& p) {
  if (p.state == FAULT) {
    Serial.printf("Pump %d: Clearing fault: %s\n", p.id, faultTypeToString(p.faultType));
    p.state = STOPPED;
    p.faultType = NO_FAULT;
    p.pendingState = STOPPED;
    p.stateDebounceCounter = 0;
    p.startCommand = false;
    fl_setDO(p.doFaultAlarm, false);
    Serial.printf("Pump %d: Fault cleared. Ready to restart.\n", p.id);
  }
}

PumpState evaluatePumpState(Pump& p) {
  float current = *(p.current);
  unsigned long now = millis();

  // Overcurrent detection with configurable delay
  if (p.overcurrentEnabled && current > p.maxCurrentThreshold) {
    if (!p.overcurrentConditionActive) {
      p.overcurrentConditionActive = true;
      p.overcurrentStartTime = now;
      Serial.printf("Pump %d: Overcurrent condition started (delay=%lus)\n", p.id, p.overcurrentDelayS);
    }
    if (p.overcurrentDelayS == 0 || (now - p.overcurrentStartTime) >= (p.overcurrentDelayS * 1000)) {
      return FAULT;
    }
  } else {
    if (p.overcurrentConditionActive) {
      Serial.printf("Pump %d: Overcurrent condition cleared\n", p.id);
      p.overcurrentConditionActive = false;
    }
  }

  // Dry run detection with configurable delay
  if (p.dryRunEnabled && p.dryCurrentThreshold > 0 && p.startCommand && p.state == RUNNING) {
    if (current < p.dryCurrentThreshold) {
      if (!p.dryrunConditionActive) {
        p.dryrunConditionActive = true;
        p.dryrunStartTime = now;
        Serial.printf("Pump %d: Dry run condition started (delay=%lus)\n", p.id, p.dryrunDelayS);
      }
      if (p.dryrunDelayS == 0 || (now - p.dryrunStartTime) >= (p.dryrunDelayS * 1000)) {
        return FAULT;
      }
    } else {
      if (p.dryrunConditionActive) {
        Serial.printf("Pump %d: Dry run condition cleared\n", p.id);
        p.dryrunConditionActive = false;
      }
    }
  } else {
    p.dryrunConditionActive = false;
  }

  // Start failure timeout
  if (START_TIMEOUT > 0 && p.startCommand && p.state != RUNNING) {
    if (millis() - p.startCommandTime > START_TIMEOUT) {
      Serial.printf("Pump %d: Start failure timeout\n", p.id);
      return FAULT;
    }
  }

  if (p.state == RUNNING) {
    if (current < (RUN_THRESHOLD - HYSTERESIS_CURRENT)) {
      return STOPPED;
    }
    return RUNNING;
  } else {
    if (current > RUN_THRESHOLD) {
      return RUNNING;
    }
    return STOPPED;
  }
}

void updatePumpState(Pump& p) {
  if (p.state == FAULT) {
    if (FAULT_AUTO_RESET_MS > 0 && (millis() - p.faultTimestamp) > FAULT_AUTO_RESET_MS) {
      Serial.printf("Pump %d: Auto-resetting fault\n", p.id);
      resetFault(p);
    }
    return;
  }

  // Sensor fault is shared — one Modbus sensor for all 3
  if (!fl_sensorOnline && fl_modbusFailCount >= FL_MAX_MODBUS_FAILURES) {
    triggerFault(p, SENSOR_FAULT);
    return;
  }

  PumpState targetState = evaluatePumpState(p);

  if (targetState == FAULT) {
    float current = *(p.current);
    if (current > p.maxCurrentThreshold) {
      triggerFault(p, OVERCURRENT);
    } else {
      triggerFault(p, DRY_RUN);
    }
    return;
  }

  if (targetState != p.state) {
    if (targetState == p.pendingState) {
      p.stateDebounceCounter++;
      if (p.stateDebounceCounter >= STATE_DEBOUNCE_COUNT) {
        p.state = targetState;
        p.stateDebounceCounter = 0;
        Serial.printf("Pump %d: State changed to %s\n", p.id, stateToString(p.state));
      }
    } else {
      p.pendingState = targetState;
      p.stateDebounceCounter = 1;
    }
  } else {
    p.stateDebounceCounter = 0;
    p.pendingState = p.state;
  }
}

/* ================= PROTECTION CONFIG (per-pump NVS) ================= */

void loadPumpProtection(Pump& p) {
  fl_preferences.begin(p.nvsNamespace, true);
  p.overcurrentEnabled = fl_preferences.getBool("oc_en", true);
  p.dryRunEnabled = fl_preferences.getBool("dr_en", true);
  p.maxCurrentThreshold = fl_preferences.getFloat("max_i", 120.0);
  p.dryCurrentThreshold = fl_preferences.getFloat("dry_i", 0.5);
  p.overcurrentDelayS = fl_preferences.getULong("oc_delay", 0);
  p.dryrunDelayS = fl_preferences.getULong("dr_delay", 0);
  fl_preferences.end();
  Serial.printf("Pump %d protection: max=%.1fA, dry=%.1fA, oc_delay=%lus, dr_delay=%lus\n",
                p.id, p.maxCurrentThreshold, p.dryCurrentThreshold, p.overcurrentDelayS, p.dryrunDelayS);
}

void savePumpProtection(Pump& p) {
  fl_preferences.begin(p.nvsNamespace, false);
  fl_preferences.putBool("oc_en", p.overcurrentEnabled);
  fl_preferences.putBool("dr_en", p.dryRunEnabled);
  fl_preferences.putFloat("max_i", p.maxCurrentThreshold);
  fl_preferences.putFloat("dry_i", p.dryCurrentThreshold);
  fl_preferences.putULong("oc_delay", p.overcurrentDelayS);
  fl_preferences.putULong("dr_delay", p.dryrunDelayS);
  fl_preferences.end();
  Serial.printf("Pump %d protection saved\n", p.id);
}

/* ================= SCHEDULE CONFIG ================= */

void loadScheduleConfig() {
  fl_preferences.begin("schedule", true);
  scheduleEnabled = fl_preferences.getBool("enabled", false);
  scheduleStartHour = fl_preferences.getUChar("startH", 6);
  scheduleStartMinute = fl_preferences.getUChar("startM", 0);
  scheduleEndHour = fl_preferences.getUChar("endH", 18);
  scheduleEndMinute = fl_preferences.getUChar("endM", 0);
  scheduleDays = fl_preferences.getUChar("days", 0x7F);
  fl_preferences.end();
  Serial.println("Schedule config loaded");
}

void saveScheduleConfig() {
  fl_preferences.begin("schedule", false);
  fl_preferences.putBool("enabled", scheduleEnabled);
  fl_preferences.putUChar("startH", scheduleStartHour);
  fl_preferences.putUChar("startM", scheduleStartMinute);
  fl_preferences.putUChar("endH", scheduleEndHour);
  fl_preferences.putUChar("endM", scheduleEndMinute);
  fl_preferences.putUChar("days", scheduleDays);
  fl_preferences.end();
  Serial.println("Schedule config saved");
}

/* ================= RURAFLEX CONFIG ================= */

void loadRuraflexConfig() {
  fl_preferences.begin("ruraflex", true);
  ruraflexEnabled = fl_preferences.getBool("enabled", false);
  fl_preferences.end();
  Serial.println("Ruraflex config loaded");
}

void saveRuraflexConfig() {
  fl_preferences.begin("ruraflex", false);
  fl_preferences.putBool("enabled", ruraflexEnabled);
  fl_preferences.end();
  Serial.println("Ruraflex config saved");
}

// Ruraflex TOU time checking (Eskom South Africa 2025/26)
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

/* ================= MQTT CALLBACK ================= */

void eveMqttCallback(const char* cmd, unsigned int length) {
  // Handle UPDATE_FIRMWARE notification (library handles actual update)
  {
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, cmd);
    if (!error) {
      const char* command = doc["command"];
      if (!command) goto not_json;

      if (strcmp(command, "UPDATE_FIRMWARE") == 0) {
        // Stop all pumps for safety during update
        for (int i = 0; i < NUM_PUMPS; i++) {
          pumps[i].startCommand = false;
          fl_setDO(pumps[i].doContactor, false);
        }
        return;
      }

      // Per-pump commands
      if (strcmp(command, "START") == 0) {
        int pump = doc["pump"] | 0;
        if (pump >= 1 && pump <= NUM_PUMPS) {
          Pump& p = pumps[pump - 1];
          if (p.state == FAULT) {
            Serial.printf("Pump %d: Cannot START while in FAULT\n", p.id);
          } else {
            p.startCommand = true;
            p.startCommandTime = millis();
            Serial.printf("Pump %d: Start command accepted\n", p.id);
          }
        }
        return;
      }

      if (strcmp(command, "STOP") == 0) {
        int pump = doc["pump"] | 0;
        if (pump >= 1 && pump <= NUM_PUMPS) {
          Pump& p = pumps[pump - 1];
          p.startCommand = false;
          fl_setDO(p.doContactor, false);
          if (p.state != FAULT) p.state = STOPPED;
          Serial.printf("Pump %d: Stop command accepted\n", p.id);
        }
        return;
      }

      if (strcmp(command, "RESET") == 0) {
        int pump = doc["pump"] | 0;
        if (pump >= 1 && pump <= NUM_PUMPS) {
          resetFault(pumps[pump - 1]);
        }
        return;
      }

      // Aggregate commands
      if (strcmp(command, "START_ALL") == 0) {
        for (int i = 0; i < NUM_PUMPS; i++) {
          if (pumps[i].state != FAULT) {
            pumps[i].startCommand = true;
            pumps[i].startCommandTime = millis();
          }
        }
        Serial.println("START_ALL accepted");
        return;
      }

      if (strcmp(command, "STOP_ALL") == 0) {
        for (int i = 0; i < NUM_PUMPS; i++) {
          pumps[i].startCommand = false;
          fl_setDO(pumps[i].doContactor, false);
          if (pumps[i].state != FAULT) pumps[i].state = STOPPED;
        }
        Serial.println("STOP_ALL accepted");
        return;
      }

      if (strcmp(command, "RESET_ALL") == 0) {
        for (int i = 0; i < NUM_PUMPS; i++) {
          resetFault(pumps[i]);
        }
        Serial.println("RESET_ALL accepted");
        return;
      }

      // Per-pump threshold settings
      if (strcmp(command, "SET_THRESHOLDS") == 0) {
        int pump = doc["pump"] | 0;
        if (pump >= 1 && pump <= NUM_PUMPS) {
          Pump& p = pumps[pump - 1];
          if (doc.containsKey("max_current")) {
            float val = doc["max_current"];
            if (val >= 1.0 && val <= 500.0) p.maxCurrentThreshold = val;
          }
          if (doc.containsKey("dry_current")) {
            float val = doc["dry_current"];
            if (val >= 0.0 && val <= 50.0) p.dryCurrentThreshold = val;
          }
          savePumpProtection(p);
          Serial.printf("Pump %d: Thresholds updated max=%.1fA dry=%.1fA\n",
                        p.id, p.maxCurrentThreshold, p.dryCurrentThreshold);
        }
        return;
      }

      if (strcmp(command, "SET_PROTECTION") == 0) {
        int pump = doc["pump"] | 0;
        if (pump >= 1 && pump <= NUM_PUMPS) {
          Pump& p = pumps[pump - 1];
          if (doc.containsKey("overcurrent_enabled"))
            p.overcurrentEnabled = doc["overcurrent_enabled"];
          if (doc.containsKey("dryrun_enabled"))
            p.dryRunEnabled = doc["dryrun_enabled"];
          savePumpProtection(p);
          Serial.printf("Pump %d: Protection updated\n", p.id);
        }
        return;
      }

      if (strcmp(command, "SET_DELAYS") == 0) {
        int pump = doc["pump"] | 0;
        if (pump >= 1 && pump <= NUM_PUMPS) {
          Pump& p = pumps[pump - 1];
          if (doc.containsKey("overcurrent_delay_s")) {
            uint32_t val = doc["overcurrent_delay_s"];
            if (val <= 30) p.overcurrentDelayS = val;
          }
          if (doc.containsKey("dryrun_delay_s")) {
            uint32_t val = doc["dryrun_delay_s"];
            if (val <= 30) p.dryrunDelayS = val;
          }
          savePumpProtection(p);
          Serial.printf("Pump %d: Delays updated oc=%lus dr=%lus\n",
                        p.id, p.overcurrentDelayS, p.dryrunDelayS);
        }
        return;
      }

      // Shared schedule commands
      if (strcmp(command, "SET_SCHEDULE") == 0) {
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
        return;
      }

      if (strcmp(command, "SET_RURAFLEX") == 0) {
        if (doc.containsKey("enabled"))
          ruraflexEnabled = doc["enabled"];
        if (ruraflexEnabled && scheduleEnabled) {
          scheduleEnabled = false;
          saveScheduleConfig();
        }
        saveRuraflexConfig();
        Serial.println("Ruraflex updated via MQTT");
        return;
      }

      // Query settings
      if (strcmp(command, "GET_SETTINGS") == 0) {
        StaticJsonDocument<1024> resp;
        resp["type"] = "settings";

        // Per-pump protection
        for (int i = 0; i < NUM_PUMPS; i++) {
          char key[8];
          snprintf(key, sizeof(key), "p%d", i + 1);
          JsonObject pObj = resp.createNestedObject(key);
          pObj["overcurrent_enabled"] = pumps[i].overcurrentEnabled;
          pObj["dryrun_enabled"] = pumps[i].dryRunEnabled;
          pObj["max_current"] = pumps[i].maxCurrentThreshold;
          pObj["dry_current"] = pumps[i].dryCurrentThreshold;
          pObj["overcurrent_delay_s"] = pumps[i].overcurrentDelayS;
          pObj["dryrun_delay_s"] = pumps[i].dryrunDelayS;
        }

        // Shared schedule
        resp["schedule_enabled"] = scheduleEnabled;
        resp["schedule_start_hour"] = scheduleStartHour;
        resp["schedule_start_minute"] = scheduleStartMinute;
        resp["schedule_end_hour"] = scheduleEndHour;
        resp["schedule_end_minute"] = scheduleEndMinute;
        resp["schedule_days"] = scheduleDays;
        resp["ruraflex_enabled"] = ruraflexEnabled;

        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 10)) {
          char timeStr[9];
          strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
          resp["current_time"] = timeStr;
        }

        char buf[1024];
        serializeJson(resp, buf);
        fl_mqtt.publish(fl_TOPIC_TELEMETRY, buf);
        Serial.println("Settings sent via MQTT");
        return;
      }

      if (strcmp(command, "STATUS") == 0) {
        lastTelemetryTime = 0;
        return;
      }
    }
  }

not_json:
  // Simple string commands not wrapped in JSON (fallback)
  Serial.printf("MQTT: Unrecognized command: %s\n", cmd);
}

/* ================= SERIAL CALLBACK ================= */

void eveSerialCallback(const String& input) {
  if (input == "STATUS") {
    Serial.println("\n--- Eve 3-Pump Status ---");
    for (int i = 0; i < NUM_PUMPS; i++) {
      Pump& p = pumps[i];
      Serial.printf("Pump %d: %s | V=%.1f I=%.2f | cmd=%s | cf=%s",
                    p.id, stateToString(p.state), *(p.voltage), *(p.current),
                    p.startCommand ? "ON" : "OFF",
                    p.contactorConfirmed ? "YES" : "NO");
      if (p.state == FAULT) {
        Serial.printf(" | fault=%s", faultTypeToString(p.faultType));
      }
      Serial.println();
    }
    Serial.printf("Sensor: %s | Schedule: %s | Ruraflex: %s\n",
                  fl_sensorOnline ? "ONLINE" : "OFFLINE",
                  scheduleEnabled ? "ON" : "OFF",
                  ruraflexEnabled ? "ON" : "OFF");
  }
  else if (input == "HELP") {
    Serial.println("START1/2/3   - Start individual pump");
    Serial.println("STOP1/2/3    - Stop individual pump");
    Serial.println("FAULT_RESET1/2/3 - Clear pump fault");
    Serial.println("STARTALL     - Start all pumps");
    Serial.println("STOPALL      - Stop all pumps");
    Serial.println("RESETALL     - Reset all faults");
    Serial.println("STATUS       - Show all pump states");
  }
  else if (input.startsWith("START") && input.length() == 6) {
    int n = input.charAt(5) - '0';
    if (n >= 1 && n <= NUM_PUMPS) {
      Pump& p = pumps[n - 1];
      if (p.state == FAULT) {
        Serial.printf("Pump %d: Cannot start while in FAULT\n", p.id);
      } else {
        p.startCommand = true;
        p.startCommandTime = millis();
        Serial.printf("Pump %d: Start command issued\n", p.id);
      }
    }
  }
  else if (input.startsWith("STOP") && input.length() == 5) {
    int n = input.charAt(4) - '0';
    if (n >= 1 && n <= NUM_PUMPS) {
      Pump& p = pumps[n - 1];
      p.startCommand = false;
      fl_setDO(p.doContactor, false);
      Serial.printf("Pump %d: Stop command issued\n", p.id);
    }
  }
  else if (input.startsWith("FAULT_RESET") && input.length() == 12) {
    int n = input.charAt(11) - '0';
    if (n >= 1 && n <= NUM_PUMPS) {
      resetFault(pumps[n - 1]);
    }
  }
  else if (input == "STARTALL") {
    for (int i = 0; i < NUM_PUMPS; i++) {
      if (pumps[i].state != FAULT) {
        pumps[i].startCommand = true;
        pumps[i].startCommandTime = millis();
      }
    }
    Serial.println("All pumps: Start command issued");
  }
  else if (input == "STOPALL") {
    for (int i = 0; i < NUM_PUMPS; i++) {
      pumps[i].startCommand = false;
      fl_setDO(pumps[i].doContactor, false);
    }
    Serial.println("All pumps: Stop command issued");
  }
  else if (input == "RESETALL") {
    for (int i = 0; i < NUM_PUMPS; i++) {
      resetFault(pumps[i]);
    }
    Serial.println("All pump faults reset");
  }
}

/* ================= EVE WEB ROUTES ================= */

void setupEveWebRoutes() {
  // API endpoint for 3-pump status
  fl_server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    StaticJsonDocument<512> doc;
    for (int i = 0; i < NUM_PUMPS; i++) {
      Pump& p = pumps[i];
      char vk[4], ik[4], sk[4], ck[4], fk[4], cfk[4];
      snprintf(vk, sizeof(vk), "V%d", p.id);
      snprintf(ik, sizeof(ik), "I%d", p.id);
      snprintf(sk, sizeof(sk), "s%d", p.id);
      snprintf(ck, sizeof(ck), "c%d", p.id);
      snprintf(fk, sizeof(fk), "f%d", p.id);
      snprintf(cfk, sizeof(cfk), "cf%d", p.id);
      doc[vk] = round(*(p.voltage) * 10) / 10.0;
      doc[ik] = round(*(p.current) * 100) / 100.0;
      doc[sk] = stateToString(p.state);
      doc[ck] = p.startCommand;
      doc[fk] = faultTypeToString(p.faultType);
      doc[cfk] = p.contactorConfirmed;
    }
    doc["sensor"] = fl_sensorOnline;
    doc["uptime"] = millis() / 1000;
    doc["network"] = fl_useEthernet ? "ETH" : "WiFi";
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // API endpoint for commands (forwards to MQTT handler)
  fl_server.on("/api/command", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    if (request->hasParam("cmd", true)) {
      String cmd = request->getParam("cmd", true)->value();
      eveMqttCallback(cmd.c_str(), cmd.length());
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Missing cmd parameter");
    }
  });

  // Per-pump protection settings API
  fl_server.on("/api/protection", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    StaticJsonDocument<512> doc;
    for (int i = 0; i < NUM_PUMPS; i++) {
      char key[4];
      snprintf(key, sizeof(key), "p%d", pumps[i].id);
      JsonObject pObj = doc.createNestedObject(key);
      pObj["overcurrent_enabled"] = pumps[i].overcurrentEnabled;
      pObj["dryrun_enabled"] = pumps[i].dryRunEnabled;
      pObj["max_current"] = pumps[i].maxCurrentThreshold;
      pObj["dry_current"] = pumps[i].dryCurrentThreshold;
      pObj["overcurrent_delay_s"] = pumps[i].overcurrentDelayS;
      pObj["dryrun_delay_s"] = pumps[i].dryrunDelayS;
    }
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  // Shared schedule settings API
  fl_server.on("/api/schedule", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    StaticJsonDocument<384> doc;
    doc["enabled"] = scheduleEnabled;
    doc["start_hour"] = scheduleStartHour;
    doc["start_minute"] = scheduleStartMinute;
    doc["end_hour"] = scheduleEndHour;
    doc["end_minute"] = scheduleEndMinute;
    doc["days"] = scheduleDays;
    doc["ruraflex_enabled"] = ruraflexEnabled;
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
}

/* ================= SETUP ================= */

void setup() {
  // Initialize hardware (I2C recovery, TCA9554, DI, NVS, RS485/Modbus, Serial)
  fl_begin();

  Serial.println("\n\n*** ESP32 BOOT ***");
  Serial.println(FW_NAME);
  Serial.printf("Version: %s\n", FW_VERSION);
  Serial.flush();

  // Initialize pump structs
  initPumps();

  // Set secrets via setters (library never includes secrets.h)
  fl_setMqttDefaults(DEFAULT_MQTT_HOST, DEFAULT_MQTT_PORT, DEFAULT_MQTT_USER, DEFAULT_MQTT_PASS);
  fl_setWebAuth(WEB_AUTH_USER, WEB_AUTH_PASS);
  fl_setWebhookUrl(NOTIFICATION_WEBHOOK_URL);
  fl_setFirmwareInfo(FW_NAME, FW_VERSION, HW_TYPE);
  fl_setOtaPassword(OTA_PASSWORD);

  // WiFi init and device ID
  WiFi.mode(WIFI_STA);
  fl_checkWifiRestore();
  fl_generateDeviceId();
  fl_printDeviceInfo();

  // Network: Ethernet first, WiFi fallback
  fl_initNetwork();

  // NTP for South Africa (GMT+2)
  fl_initNTP(2 * 3600);

  // Load configs from NVS
  fl_loadMqttConfig();
  for (int i = 0; i < NUM_PUMPS; i++) {
    loadPumpProtection(pumps[i]);
  }
  loadScheduleConfig();
  loadRuraflexConfig();

  // Initialize schedule state and auto-start if booting within schedule window
  wasWithinSchedule = isWithinSchedule();
  Serial.printf("Schedule init: currently %s schedule window\n", wasWithinSchedule ? "within" : "outside");
  if ((scheduleEnabled || ruraflexEnabled) && wasWithinSchedule) {
    for (int i = 0; i < NUM_PUMPS; i++) {
      pumps[i].startCommand = true;
    }
    Serial.println("Schedule: Boot within allowed hours, starting all pumps");
  }

  // Set callbacks
  fl_setMqttCallback(eveMqttCallback);
  fl_setSerialCallback(eveSerialCallback);

  // Web server: library routes + eve routes + start
  fl_setDashboardHtml(DASHBOARD_HTML);
  fl_setupWebRoutes();
  setupEveWebRoutes();
  fl_server.begin();
  Serial.println("Web server started on port 80");

  // Connect to cloud MQTT
  fl_connectMQTT();

  // Setup ArduinoOTA
  fl_setupArduinoOTA();

  Serial.println("Setup complete. Entering main loop...");
}

/* ================= LOOP ================= */

void loop() {
  unsigned long now = millis();

  // Library tick: OTA, serial, MQTT reconnect+loop, DI read
  fl_tick();

  // ===== CONTACTOR FEEDBACK (DI1-DI3) =====
  for (int i = 0; i < NUM_PUMPS; i++) {
    Pump& p = pumps[i];
    bool diFeedback = (fl_diStatus & (1 << p.diFeedbackBit)) != 0;
    bool contactorOn = (fl_do_state & (1 << p.doContactor)) == 0;  // Active low
    p.contactorConfirmed = contactorOn && diFeedback;
  }

  // ===== FORCE UNUSED DO OFF =====
  // DO mask 0x88 = bits 3 and 7 forced OFF (set to 1 = inactive)
  // Preserves bits 0,1,2 (contactors) and 4,5,6 (fault alarms)
  fl_do_state |= 0x88;
  fl_writeDO();

  // ===== SENSOR READ + STATE MACHINE (every 500ms) =====
  if (now - lastSensorReadTime >= SENSOR_READ_INTERVAL_MS) {
    lastSensorReadTime = now;
    fl_readSensors();

    for (int i = 0; i < NUM_PUMPS; i++) {
      updatePumpState(pumps[i]);
    }

    // Check schedule/Ruraflex (shared for all pumps)
    bool scheduleAllows = isWithinSchedule();
    if (scheduleEnabled || ruraflexEnabled) {
      if (scheduleAllows && !wasWithinSchedule) {
        for (int i = 0; i < NUM_PUMPS; i++) {
          if (pumps[i].state != FAULT) {
            pumps[i].startCommand = true;
          }
        }
        Serial.println("Schedule: Entering allowed hours, starting all pumps");
      }
      if (!scheduleAllows && wasWithinSchedule) {
        for (int i = 0; i < NUM_PUMPS; i++) {
          if (pumps[i].startCommand) {
            pumps[i].startCommand = false;
          }
        }
        Serial.println("Schedule: Outside allowed hours, stopping all pumps");
      }
      wasWithinSchedule = scheduleAllows;
    }

    // Update DO outputs per pump
    bool scheduleAllowsNow = isWithinSchedule();
    for (int i = 0; i < NUM_PUMPS; i++) {
      Pump& p = pumps[i];
      bool desiredDO = (p.startCommand && p.state != FAULT && scheduleAllowsNow);
      if (desiredDO != p.lastDOState) {
        fl_setDO(p.doContactor, desiredDO);
        Serial.printf("Pump %d contactor: %s\n", p.id, desiredDO ? "ON" : "OFF");
        p.lastDOState = desiredDO;
      }
    }
  }

  // ===== TELEMETRY PUBLISH (every 2000ms) =====
  if (now - lastTelemetryTime >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryTime = now;

    if (fl_mqttConnected && fl_mqtt.connected()) {
      StaticJsonDocument<768> doc;

      for (int i = 0; i < NUM_PUMPS; i++) {
        Pump& p = pumps[i];
        char vk[4], ik[4], sk[4], ck[4], fk[4], cfk[4];
        snprintf(vk, sizeof(vk), "V%d", p.id);
        snprintf(ik, sizeof(ik), "I%d", p.id);
        snprintf(sk, sizeof(sk), "s%d", p.id);
        snprintf(ck, sizeof(ck), "c%d", p.id);
        snprintf(fk, sizeof(fk), "f%d", p.id);
        snprintf(cfk, sizeof(cfk), "cf%d", p.id);
        doc[vk] = round(*(p.voltage) * 10) / 10.0;
        doc[ik] = round(*(p.current) * 100) / 100.0;
        doc[sk] = stateToString(p.state);
        doc[ck] = p.startCommand;
        doc[fk] = faultTypeToString(p.faultType);
        doc[cfk] = p.contactorConfirmed;
      }

      doc["sensor"] = fl_sensorOnline;
      doc["uptime"] = now / 1000;
      doc["network"] = fl_useEthernet ? "ETH" : "WiFi";
      doc["di"] = fl_diStatus;
      doc["do"] = fl_do_state;
      doc["hardware_type"] = HW_TYPE;
      doc["firmware_version"] = FW_VERSION;

      struct tm timeinfo;
      if (getLocalTime(&timeinfo, 10)) {
        char timeStr[9];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
        doc["time"] = timeStr;
      }

      char buf[1024];
      size_t len = serializeJson(doc, buf);

      bool published = fl_mqtt.publish(fl_TOPIC_TELEMETRY, buf);
      if (published) {
        fl_mqttPublishFailCount = 0;
        fl_lastMqttActivity = now;
      } else {
        fl_mqttPublishFailCount++;
        Serial.printf("MQTT publish failed (count=%d, len=%d)\n", fl_mqttPublishFailCount, len);

        if (fl_mqttPublishFailCount >= FL_MAX_MQTT_PUBLISH_FAILURES) {
          Serial.println("Too many publish failures - forcing MQTT reconnect");
          fl_mqtt.disconnect();
          fl_mqttConnected = false;
          fl_mqttPublishFailCount = 0;
        }
      }
    }
  }

  delay(10);
}
