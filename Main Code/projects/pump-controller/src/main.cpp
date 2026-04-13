/************************************************************
 * FieldLink Adam — Single-Motor 3-Phase Pump Controller
 * Board: ESP32-S3 POE ETH 8DI 8DO (Waveshare)
 * Version: 4.0.0-rc1
 *
 * Uses FieldLinkCore shared library for board support,
 * networking, MQTT, OTA, and web server.
 *
 * Controls ONE motor via a 3-phase contactor. The energy meter
 * measures all 3 phases of that single motor:
 *   L1 (Va/Ia), L2 (Vb/Ib), L3 (Vc/Ic) = same motor
 *
 * Protection:
 *  - Overcurrent on average line current
 *  - Phase imbalance (% deviation from average)
 *  - Phase loss (one phase drops while others still running)
 *  - Dry run (avg current below threshold while running)
 *  - Sensor fault (Modbus meter offline)
 *  - Start failure timeout
 *
 * All protection maths live in FieldLinkCore/fl_protection
 * and are covered by native unit tests in projects/fl-tests.
 ************************************************************/

#include <FieldLinkCore.h>
#include <ArduinoJson.h>
#include "secrets.h"

/* ================= PROJECT CONFIG ================= */

#define FW_NAME    "ESP32 Adam Single-Motor Controller"
#define FW_VERSION "4.0.0-rc1"
#define HW_TYPE    "PUMP_ESP32S3"

// Timing intervals (non-blocking)
#define TELEMETRY_INTERVAL_MS   2000
#define SENSOR_READ_INTERVAL_MS 500

// State detection hysteresis (on average current)
#define HYSTERESIS_CURRENT      1.0f
#define STATE_DEBOUNCE_COUNT    3

// Fault handling
#define FAULT_AUTO_RESET_MS     0

// Run detection
#define RUN_THRESHOLD           5.0f
#define START_TIMEOUT_MS        10000

// Adam uses DO0 (contactor) + DO4 (fault alarm). Everything else
// is forced off via this mask (bits 1,2,3,5,6,7 = 0xEE).
#define ADAM_DO_FORCE_OFF_MASK  0xEE

/* ================= MOTOR STATE ================= */

enum MotorState { STOPPED, RUNNING, FAULT };
enum FaultType {
  NO_FAULT,
  OVERCURRENT,
  PHASE_IMBALANCE,
  PHASE_LOSS,
  DRY_RUN,
  SENSOR_FAULT,
  START_FAILURE
};

struct Motor {
  // DO channels
  uint8_t doContactor;      // DO0
  uint8_t doFaultAlarm;     // DO4

  // DI feedback
  uint8_t diFeedbackBit;    // bit position in fl_diStatus (DI1 = bit 0)

  // State machine
  MotorState state;
  MotorState pendingState;
  FaultType faultType;
  bool startCommand;
  unsigned long startCommandTime;
  int stateDebounceCounter;
  bool contactorConfirmed;
  bool lastDOState;

  // Fault tracking
  unsigned long faultTimestamp;
  float faultCurrent;       // avg at moment of trip

  // Protection thresholds (NVS-stored, namespace "prot_adam")
  float maxCurrentA;        // overcurrent threshold on avg
  float dryCurrentA;        // dry-run threshold on avg
  float imbalancePct;       // phase imbalance threshold (%)
  float phaseLossA;         // phase considered "lost" below this
  bool overcurrentEnabled;
  bool imbalanceEnabled;
  bool phaseLossEnabled;
  bool dryRunEnabled;
  uint32_t overcurrentDelayS;
  uint32_t imbalanceDelayS;
  uint32_t phaseLossDelayS;
  uint32_t dryrunDelayS;

  // Protection debounce states (from fl_protection)
  fl_DebounceState ocDebounce;
  fl_DebounceState imbDebounce;
  fl_DebounceState plDebounce;
  fl_DebounceState drDebounce;

  // Schedule
  bool scheduleEnabled;
  uint8_t scheduleStartHour;
  uint8_t scheduleStartMinute;
  uint8_t scheduleEndHour;
  uint8_t scheduleEndMinute;
  uint8_t scheduleDays;     // Bitmask: bit0=Sun...bit6=Sat (0x7F = all days)
  bool wasWithinSchedule;
};

Motor motor;

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
  <title>FieldLink Adam — Single-Motor Controller</title>
  <link href="https://fonts.googleapis.com/css2?family=Chakra+Petch:wght@400;600;700&family=JetBrains+Mono:wght@400;500;600&display=swap" rel="stylesheet">
  <style>
    :root {
      --bg-primary: #0a0e14;
      --bg-card: #151c28;
      --border-color: #1e2a3a;
      --text-primary: #e4e8ef;
      --text-secondary: #6b7a8f;
      --accent-cyan: #00d4ff;
      --status-running: #00ff88;
      --status-stopped: #6b7a8f;
      --status-fault: #ff4757;
    }
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { font-family: 'JetBrains Mono', monospace; background: var(--bg-primary); color: var(--text-primary); min-height: 100vh; }
    .container { max-width: 900px; margin: 0 auto; padding: 20px; }
    .header { display: flex; justify-content: space-between; align-items: center; margin-bottom: 24px; padding-bottom: 20px; border-bottom: 1px solid var(--border-color); }
    .logo { font-family: 'Chakra Petch', sans-serif; font-size: 24px; font-weight: 700; }
    .logo span { color: var(--accent-cyan); }
    .card { background: var(--bg-card); border: 1px solid var(--border-color); border-radius: 12px; padding: 20px; margin-bottom: 20px; }
    .card.fault { border-color: var(--status-fault); box-shadow: 0 0 20px rgba(255, 71, 87, 0.2); }
    .card.running { border-color: var(--status-running); box-shadow: 0 0 15px rgba(0, 255, 136, 0.1); }
    .card-title { font-family: 'Chakra Petch', sans-serif; font-size: 12px; font-weight: 600; text-transform: uppercase; letter-spacing: 1.5px; color: var(--text-secondary); margin-bottom: 16px; }
    .state { font-family: 'Chakra Petch', sans-serif; font-size: 32px; font-weight: 700; letter-spacing: 2px; }
    .state.STOPPED { color: var(--status-stopped); }
    .state.RUNNING { color: var(--status-running); }
    .state.FAULT   { color: var(--status-fault); }
    .phase-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 14px; margin-top: 16px; }
    .phase { background: rgba(255,255,255,0.02); border: 1px solid var(--border-color); border-radius: 8px; padding: 14px; text-align: center; }
    .phase-label { font-size: 11px; color: var(--text-secondary); text-transform: uppercase; letter-spacing: 1px; }
    .phase-value { font-size: 22px; font-weight: 600; margin-top: 4px; }
    .phase-unit { font-size: 12px; color: var(--text-secondary); margin-left: 4px; }
    .metrics { display: grid; grid-template-columns: repeat(3, 1fr); gap: 14px; margin-top: 16px; }
    .metric { background: rgba(255,255,255,0.02); border: 1px solid var(--border-color); border-radius: 8px; padding: 12px; }
    .metric-label { font-size: 10px; color: var(--text-secondary); text-transform: uppercase; }
    .metric-value { font-size: 18px; font-weight: 600; margin-top: 2px; }
    .btn-row { display: flex; gap: 10px; margin-top: 16px; flex-wrap: wrap; }
    button { flex: 1; min-width: 100px; padding: 12px; background: var(--bg-card); border: 1px solid var(--border-color); color: var(--text-primary); font-family: inherit; font-size: 13px; border-radius: 8px; cursor: pointer; transition: all 0.2s; }
    button:hover { border-color: var(--accent-cyan); }
    button.start { border-color: var(--status-running); color: var(--status-running); }
    button.stop  { border-color: var(--status-stopped); }
    button.reset { border-color: var(--status-fault); color: var(--status-fault); }
    .fault-banner { margin-top: 14px; padding: 12px; background: rgba(255,71,87,0.1); border: 1px solid var(--status-fault); border-radius: 8px; color: var(--status-fault); font-weight: 600; }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <div class="logo">FieldLink <span>Adam</span></div>
      <div id="conn">Connecting...</div>
    </div>

    <div class="card" id="motorCard">
      <div class="card-title">Motor Status</div>
      <div class="state" id="state">---</div>
      <div id="faultBanner" class="fault-banner" style="display:none"></div>

      <div class="phase-grid">
        <div class="phase"><div class="phase-label">L1</div><div class="phase-value" id="V1">--<span class="phase-unit">V</span></div><div class="phase-value" id="I1">--<span class="phase-unit">A</span></div></div>
        <div class="phase"><div class="phase-label">L2</div><div class="phase-value" id="V2">--<span class="phase-unit">V</span></div><div class="phase-value" id="I2">--<span class="phase-unit">A</span></div></div>
        <div class="phase"><div class="phase-label">L3</div><div class="phase-value" id="V3">--<span class="phase-unit">V</span></div><div class="phase-value" id="I3">--<span class="phase-unit">A</span></div></div>
      </div>

      <div class="metrics">
        <div class="metric"><div class="metric-label">Avg Current</div><div class="metric-value" id="avgI">--A</div></div>
        <div class="metric"><div class="metric-label">Imbalance</div><div class="metric-value" id="imb">--%</div></div>
        <div class="metric"><div class="metric-label">Contactor FB</div><div class="metric-value" id="cf">--</div></div>
      </div>

      <div class="btn-row">
        <button class="start" onclick="send('START')">START</button>
        <button class="stop" onclick="send('STOP')">STOP</button>
        <button class="reset" onclick="send('RESET')">RESET FAULT</button>
      </div>
    </div>
  </div>

<script>
async function send(command) {
  const cmd = JSON.stringify({ command });
  await fetch('/api/command', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'cmd=' + encodeURIComponent(cmd)
  });
  refresh();
}

async function refresh() {
  try {
    const r = await fetch('/api/status');
    const d = await r.json();
    document.getElementById('conn').textContent = 'Connected';
    document.getElementById('state').textContent = d.state;
    document.getElementById('state').className = 'state ' + d.state;
    const card = document.getElementById('motorCard');
    card.classList.remove('fault', 'running');
    if (d.state === 'FAULT') card.classList.add('fault');
    else if (d.state === 'RUNNING') card.classList.add('running');

    document.getElementById('V1').innerHTML = d.Va.toFixed(1) + '<span class="phase-unit">V</span>';
    document.getElementById('V2').innerHTML = d.Vb.toFixed(1) + '<span class="phase-unit">V</span>';
    document.getElementById('V3').innerHTML = d.Vc.toFixed(1) + '<span class="phase-unit">V</span>';
    document.getElementById('I1').innerHTML = d.Ia.toFixed(2) + '<span class="phase-unit">A</span>';
    document.getElementById('I2').innerHTML = d.Ib.toFixed(2) + '<span class="phase-unit">A</span>';
    document.getElementById('I3').innerHTML = d.Ic.toFixed(2) + '<span class="phase-unit">A</span>';
    document.getElementById('avgI').textContent = d.avgI.toFixed(2) + ' A';
    document.getElementById('imb').textContent = d.imb.toFixed(1) + ' %';
    document.getElementById('cf').textContent = d.cf ? 'OK' : '-';

    const fb = document.getElementById('faultBanner');
    if (d.state === 'FAULT' && d.fault && d.fault !== 'NO_FAULT') {
      fb.style.display = 'block';
      fb.textContent = 'FAULT: ' + d.fault + ' @ ' + d.faultI.toFixed(2) + 'A';
    } else {
      fb.style.display = 'none';
    }
  } catch (e) {
    document.getElementById('conn').textContent = 'Offline';
  }
}
setInterval(refresh, 2000);
refresh();
</script>
</body>
</html>
)rawliteral";

/* ================= HELPERS ================= */

const char* stateToString(MotorState s) {
  switch (s) {
    case STOPPED: return "STOPPED";
    case RUNNING: return "RUNNING";
    case FAULT:   return "FAULT";
    default:      return "UNKNOWN";
  }
}

const char* faultTypeToString(FaultType t) {
  switch (t) {
    case NO_FAULT:        return "NO_FAULT";
    case OVERCURRENT:     return "OVERCURRENT";
    case PHASE_IMBALANCE: return "PHASE_IMBALANCE";
    case PHASE_LOSS:      return "PHASE_LOSS";
    case DRY_RUN:         return "DRY_RUN";
    case SENSOR_FAULT:    return "SENSOR_FAULT";
    case START_FAILURE:   return "START_FAILURE";
    default:              return "UNKNOWN";
  }
}

static inline float avgCurrent() {
  return fl_prot_average3(fl_Ia, fl_Ib, fl_Ic);
}

static inline float imbalance() {
  return fl_prot_phaseImbalancePct(fl_Ia, fl_Ib, fl_Ic);
}

/* ================= FAULT MANAGEMENT ================= */

void triggerFault(FaultType type) {
  if (motor.state != FAULT) {
    motor.state = FAULT;
    motor.faultType = type;
    motor.faultTimestamp = millis();
    motor.faultCurrent = avgCurrent();

    motor.startCommand = false;
    fl_setDO(motor.doContactor, false);
    fl_setDO(motor.doFaultAlarm, true);

    Serial.printf("!!! MOTOR FAULT: %s (Iavg=%.2fA I1=%.2f I2=%.2f I3=%.2f imb=%.1f%%) !!!\n",
                  faultTypeToString(type), motor.faultCurrent,
                  fl_Ia, fl_Ib, fl_Ic, imbalance());
    fl_sendFaultNotification(1, faultTypeToString(type), motor.faultCurrent);
  }
}

void resetFault() {
  if (motor.state == FAULT) {
    Serial.printf("Motor: Clearing fault: %s\n", faultTypeToString(motor.faultType));
    motor.state = STOPPED;
    motor.faultType = NO_FAULT;
    motor.pendingState = STOPPED;
    motor.stateDebounceCounter = 0;
    motor.startCommand = false;
    fl_prot_debounceInit(motor.ocDebounce);
    fl_prot_debounceInit(motor.imbDebounce);
    fl_prot_debounceInit(motor.plDebounce);
    fl_prot_debounceInit(motor.drDebounce);
    fl_setDO(motor.doFaultAlarm, false);
    Serial.println("Motor: Fault cleared. Ready to restart.");
  }
}

/* ================= STATE EVALUATION ================= */

// Returns the state the motor *should* be in based on current readings.
// Protection faults take priority. Run/stop is based on average current
// so transient single-phase dips don't toggle state.
MotorState evaluateMotorState() {
  uint32_t now = millis();
  float avg = avgCurrent();

  // --- Overcurrent (on average) ---
  bool ocCond = motor.overcurrentEnabled &&
                fl_prot_overcurrent(avg, motor.maxCurrentA);
  if (fl_prot_debounceTick(motor.ocDebounce, ocCond, now, motor.overcurrentDelayS * 1000UL)) {
    return FAULT;
  }

  // --- Phase imbalance ---
  // Only enforce while motor is commanded on and actually running.
  bool imbCond = false;
  if (motor.imbalanceEnabled && motor.startCommand && motor.state == RUNNING) {
    float imb = imbalance();
    imbCond = (imb > motor.imbalancePct);
  }
  if (fl_prot_debounceTick(motor.imbDebounce, imbCond, now, motor.imbalanceDelayS * 1000UL)) {
    return FAULT;
  }

  // --- Phase loss ---
  // Only meaningful while the motor is supposed to be drawing current.
  bool plCond = false;
  if (motor.phaseLossEnabled && motor.startCommand && motor.state == RUNNING) {
    plCond = fl_prot_phaseLoss(fl_Ia, fl_Ib, fl_Ic,
                               motor.phaseLossA, RUN_THRESHOLD);
  }
  if (fl_prot_debounceTick(motor.plDebounce, plCond, now, motor.phaseLossDelayS * 1000UL)) {
    return FAULT;
  }

  // --- Dry run (on average, only while running under command) ---
  bool drCond = false;
  if (motor.dryRunEnabled && motor.dryCurrentA > 0 &&
      motor.startCommand && motor.state == RUNNING) {
    drCond = fl_prot_dryRun(avg, motor.dryCurrentA);
  }
  if (fl_prot_debounceTick(motor.drDebounce, drCond, now, motor.dryrunDelayS * 1000UL)) {
    return FAULT;
  }

  // --- Start failure timeout ---
  if (motor.startCommand && motor.state != RUNNING) {
    if (now - motor.startCommandTime > START_TIMEOUT_MS) {
      Serial.println("Motor: Start failure timeout");
      return FAULT;
    }
  }

  // --- Run/stop via average current hysteresis ---
  if (motor.state == RUNNING) {
    if (avg < (RUN_THRESHOLD - HYSTERESIS_CURRENT)) return STOPPED;
    return RUNNING;
  }
  if (avg > RUN_THRESHOLD) return RUNNING;
  return STOPPED;
}

// Maps an evaluateMotorState() FAULT return to the specific fault type by
// re-checking conditions in priority order.
FaultType classifyFault() {
  float avg = avgCurrent();
  if (motor.overcurrentEnabled && fl_prot_overcurrent(avg, motor.maxCurrentA)) {
    return OVERCURRENT;
  }
  if (motor.imbalanceEnabled && imbalance() > motor.imbalancePct) {
    return PHASE_IMBALANCE;
  }
  if (motor.phaseLossEnabled &&
      fl_prot_phaseLoss(fl_Ia, fl_Ib, fl_Ic, motor.phaseLossA, RUN_THRESHOLD)) {
    return PHASE_LOSS;
  }
  if (motor.dryRunEnabled && fl_prot_dryRun(avg, motor.dryCurrentA)) {
    return DRY_RUN;
  }
  if (motor.startCommand && motor.state != RUNNING &&
      millis() - motor.startCommandTime > START_TIMEOUT_MS) {
    return START_FAILURE;
  }
  return OVERCURRENT;  // Fallback (shouldn't happen)
}

void updateMotorState() {
  if (motor.state == FAULT) {
    if (FAULT_AUTO_RESET_MS > 0 && (millis() - motor.faultTimestamp) > FAULT_AUTO_RESET_MS) {
      Serial.println("Motor: Auto-resetting fault");
      resetFault();
    }
    return;
  }

  // Sensor offline = sensor fault
  if (!fl_sensorOnline && fl_modbusFailCount >= FL_MAX_MODBUS_FAILURES) {
    triggerFault(SENSOR_FAULT);
    return;
  }

  MotorState target = evaluateMotorState();

  if (target == FAULT) {
    triggerFault(classifyFault());
    return;
  }

  if (target != motor.state) {
    if (target == motor.pendingState) {
      motor.stateDebounceCounter++;
      if (motor.stateDebounceCounter >= STATE_DEBOUNCE_COUNT) {
        motor.state = target;
        motor.stateDebounceCounter = 0;
        Serial.printf("Motor: State changed to %s\n", stateToString(motor.state));
      }
    } else {
      motor.pendingState = target;
      motor.stateDebounceCounter = 1;
    }
  } else {
    motor.stateDebounceCounter = 0;
    motor.pendingState = motor.state;
  }
}

/* ================= PROTECTION CONFIG (NVS) ================= */

void loadProtection() {
  fl_preferences.begin("prot_adam", true);
  motor.overcurrentEnabled = fl_preferences.getBool("oc_en", true);
  motor.imbalanceEnabled   = fl_preferences.getBool("imb_en", true);
  motor.phaseLossEnabled   = fl_preferences.getBool("pl_en", true);
  motor.dryRunEnabled      = fl_preferences.getBool("dr_en", true);

  motor.maxCurrentA   = fl_preferences.getFloat("max_i", 120.0f);
  motor.dryCurrentA   = fl_preferences.getFloat("dry_i", 2.0f);
  motor.imbalancePct  = fl_preferences.getFloat("imb_pct", 20.0f);
  motor.phaseLossA    = fl_preferences.getFloat("pl_i", 1.0f);

  motor.overcurrentDelayS = fl_preferences.getULong("oc_delay", 2);
  motor.imbalanceDelayS   = fl_preferences.getULong("imb_delay", 5);
  motor.phaseLossDelayS   = fl_preferences.getULong("pl_delay", 2);
  motor.dryrunDelayS      = fl_preferences.getULong("dr_delay", 5);
  fl_preferences.end();

  Serial.printf("Protection: max=%.1fA dry=%.1fA imb=%.1f%% pl=%.1fA\n",
                motor.maxCurrentA, motor.dryCurrentA,
                motor.imbalancePct, motor.phaseLossA);
  Serial.printf("Delays: oc=%lus imb=%lus pl=%lus dr=%lus\n",
                motor.overcurrentDelayS, motor.imbalanceDelayS,
                motor.phaseLossDelayS, motor.dryrunDelayS);
}

void saveProtection() {
  fl_preferences.begin("prot_adam", false);
  fl_preferences.putBool("oc_en", motor.overcurrentEnabled);
  fl_preferences.putBool("imb_en", motor.imbalanceEnabled);
  fl_preferences.putBool("pl_en", motor.phaseLossEnabled);
  fl_preferences.putBool("dr_en", motor.dryRunEnabled);
  fl_preferences.putFloat("max_i", motor.maxCurrentA);
  fl_preferences.putFloat("dry_i", motor.dryCurrentA);
  fl_preferences.putFloat("imb_pct", motor.imbalancePct);
  fl_preferences.putFloat("pl_i", motor.phaseLossA);
  fl_preferences.putULong("oc_delay", motor.overcurrentDelayS);
  fl_preferences.putULong("imb_delay", motor.imbalanceDelayS);
  fl_preferences.putULong("pl_delay", motor.phaseLossDelayS);
  fl_preferences.putULong("dr_delay", motor.dryrunDelayS);
  fl_preferences.end();
  Serial.println("Protection saved");
}

/* ================= SCHEDULE CONFIG ================= */

void loadSchedule() {
  fl_preferences.begin("sched_adam", true);
  motor.scheduleEnabled    = fl_preferences.getBool("en", false);
  motor.scheduleStartHour  = fl_preferences.getUChar("sH", 6);
  motor.scheduleStartMinute= fl_preferences.getUChar("sM", 0);
  motor.scheduleEndHour    = fl_preferences.getUChar("eH", 18);
  motor.scheduleEndMinute  = fl_preferences.getUChar("eM", 0);
  motor.scheduleDays       = fl_preferences.getUChar("days", 0x7F);
  fl_preferences.end();
  Serial.println("Schedule loaded");
}

void saveSchedule() {
  fl_preferences.begin("sched_adam", false);
  fl_preferences.putBool("en", motor.scheduleEnabled);
  fl_preferences.putUChar("sH", motor.scheduleStartHour);
  fl_preferences.putUChar("sM", motor.scheduleStartMinute);
  fl_preferences.putUChar("eH", motor.scheduleEndHour);
  fl_preferences.putUChar("eM", motor.scheduleEndMinute);
  fl_preferences.putUChar("days", motor.scheduleDays);
  fl_preferences.end();
  Serial.println("Schedule saved");
}

/* ================= RURAFLEX CONFIG ================= */

void loadRuraflexConfig() {
  fl_preferences.begin("ruraflex", true);
  ruraflexEnabled = fl_preferences.getBool("enabled", false);
  fl_preferences.end();
}

void saveRuraflexConfig() {
  fl_preferences.begin("ruraflex", false);
  fl_preferences.putBool("enabled", ruraflexEnabled);
  fl_preferences.end();
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

  return !isPeak && !isStandard;  // off-peak only
}

bool isWithinSchedule() {
  if (ruraflexEnabled) return isWithinRuraflex();
  if (!motor.scheduleEnabled) return true;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10)) return true;

  if (!(motor.scheduleDays & (1 << timeinfo.tm_wday))) return false;

  int nowMins = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int startMins = motor.scheduleStartHour * 60 + motor.scheduleStartMinute;
  int endMins = motor.scheduleEndHour * 60 + motor.scheduleEndMinute;

  if (startMins <= endMins) {
    return nowMins >= startMins && nowMins < endMins;
  }
  return nowMins >= startMins || nowMins < endMins;
}

/* ================= INIT ================= */

void initMotor() {
  motor.doContactor = 0;     // DO0
  motor.doFaultAlarm = 4;    // DO4
  motor.diFeedbackBit = 0;   // DI1 = bit 0

  motor.state = STOPPED;
  motor.pendingState = STOPPED;
  motor.faultType = NO_FAULT;
  motor.startCommand = false;
  motor.startCommandTime = 0;
  motor.stateDebounceCounter = 0;
  motor.contactorConfirmed = false;
  motor.lastDOState = false;
  motor.faultTimestamp = 0;
  motor.faultCurrent = 0;

  fl_prot_debounceInit(motor.ocDebounce);
  fl_prot_debounceInit(motor.imbDebounce);
  fl_prot_debounceInit(motor.plDebounce);
  fl_prot_debounceInit(motor.drDebounce);

  motor.wasWithinSchedule = false;
}

/* ================= MQTT CALLBACK ================= */

void adamMqttCallback(const char* cmd, unsigned int length) {
  static StaticJsonDocument<512> doc;
  doc.clear();
  DeserializationError error = deserializeJson(doc, cmd);
  if (error) {
    Serial.printf("MQTT: JSON parse error: %s\n", error.c_str());
    return;
  }

  const char* command = doc["command"];
  if (!command) return;

  if (strcmp(command, "UPDATE_FIRMWARE") == 0) {
    // Stop motor for safety during update (library does the actual flash)
    motor.startCommand = false;
    fl_setDO(motor.doContactor, false);
    return;
  }

  if (strcmp(command, "START") == 0) {
    if (motor.state == FAULT) {
      Serial.println("Motor: Cannot START while in FAULT");
    } else {
      motor.startCommand = true;
      motor.startCommandTime = millis();
      Serial.println("Motor: Start command accepted");
    }
    return;
  }

  if (strcmp(command, "STOP") == 0) {
    motor.startCommand = false;
    fl_setDO(motor.doContactor, false);
    if (motor.state != FAULT) motor.state = STOPPED;
    Serial.println("Motor: Stop command accepted");
    return;
  }

  if (strcmp(command, "RESET") == 0) {
    resetFault();
    return;
  }

  if (strcmp(command, "SET_THRESHOLDS") == 0) {
    if (doc.containsKey("max_current")) {
      float v = doc["max_current"];
      if (v >= 1.0f && v <= 500.0f) motor.maxCurrentA = v;
    }
    if (doc.containsKey("dry_current")) {
      float v = doc["dry_current"];
      if (v >= 0.0f && v <= 50.0f) motor.dryCurrentA = v;
    }
    if (doc.containsKey("imbalance_pct")) {
      float v = doc["imbalance_pct"];
      if (v >= 1.0f && v <= 100.0f) motor.imbalancePct = v;
    }
    if (doc.containsKey("phase_loss_current")) {
      float v = doc["phase_loss_current"];
      if (v >= 0.0f && v <= 50.0f) motor.phaseLossA = v;
    }
    saveProtection();
    Serial.println("Thresholds updated");
    return;
  }

  if (strcmp(command, "SET_PROTECTION") == 0) {
    if (doc.containsKey("overcurrent_enabled"))
      motor.overcurrentEnabled = doc["overcurrent_enabled"];
    if (doc.containsKey("imbalance_enabled"))
      motor.imbalanceEnabled = doc["imbalance_enabled"];
    if (doc.containsKey("phase_loss_enabled"))
      motor.phaseLossEnabled = doc["phase_loss_enabled"];
    if (doc.containsKey("dryrun_enabled"))
      motor.dryRunEnabled = doc["dryrun_enabled"];
    saveProtection();
    Serial.println("Protection updated");
    return;
  }

  if (strcmp(command, "SET_DELAYS") == 0) {
    if (doc.containsKey("overcurrent_delay_s")) {
      uint32_t v = doc["overcurrent_delay_s"];
      if (v <= 30) motor.overcurrentDelayS = v;
    }
    if (doc.containsKey("imbalance_delay_s")) {
      uint32_t v = doc["imbalance_delay_s"];
      if (v <= 30) motor.imbalanceDelayS = v;
    }
    if (doc.containsKey("phase_loss_delay_s")) {
      uint32_t v = doc["phase_loss_delay_s"];
      if (v <= 30) motor.phaseLossDelayS = v;
    }
    if (doc.containsKey("dryrun_delay_s")) {
      uint32_t v = doc["dryrun_delay_s"];
      if (v <= 30) motor.dryrunDelayS = v;
    }
    saveProtection();
    Serial.println("Delays updated");
    return;
  }

  if (strcmp(command, "SET_SCHEDULE") == 0) {
    if (doc.containsKey("enabled"))      motor.scheduleEnabled    = doc["enabled"];
    if (doc.containsKey("start_hour"))   motor.scheduleStartHour  = doc["start_hour"];
    if (doc.containsKey("start_minute")) motor.scheduleStartMinute= doc["start_minute"];
    if (doc.containsKey("end_hour"))     motor.scheduleEndHour    = doc["end_hour"];
    if (doc.containsKey("end_minute"))   motor.scheduleEndMinute  = doc["end_minute"];
    if (doc.containsKey("days"))         motor.scheduleDays       = doc["days"];
    saveSchedule();
    Serial.println("Schedule updated");
    return;
  }

  if (strcmp(command, "SET_RURAFLEX") == 0) {
    if (doc.containsKey("enabled")) ruraflexEnabled = doc["enabled"];
    saveRuraflexConfig();
    Serial.println("Ruraflex updated");
    return;
  }

  if (strcmp(command, "GET_SETTINGS") == 0) {
    StaticJsonDocument<768> resp;
    resp["type"] = "settings";
    resp["overcurrent_enabled"] = motor.overcurrentEnabled;
    resp["imbalance_enabled"]   = motor.imbalanceEnabled;
    resp["phase_loss_enabled"]  = motor.phaseLossEnabled;
    resp["dryrun_enabled"]      = motor.dryRunEnabled;
    resp["max_current"]         = motor.maxCurrentA;
    resp["dry_current"]         = motor.dryCurrentA;
    resp["imbalance_pct"]       = motor.imbalancePct;
    resp["phase_loss_current"]  = motor.phaseLossA;
    resp["overcurrent_delay_s"] = motor.overcurrentDelayS;
    resp["imbalance_delay_s"]   = motor.imbalanceDelayS;
    resp["phase_loss_delay_s"]  = motor.phaseLossDelayS;
    resp["dryrun_delay_s"]      = motor.dryrunDelayS;
    resp["sch_en"]   = motor.scheduleEnabled;
    resp["sch_sH"]   = motor.scheduleStartHour;
    resp["sch_sM"]   = motor.scheduleStartMinute;
    resp["sch_eH"]   = motor.scheduleEndHour;
    resp["sch_eM"]   = motor.scheduleEndMinute;
    resp["sch_days"] = motor.scheduleDays;
    resp["ruraflex_enabled"] = ruraflexEnabled;

    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 10)) {
      char timeStr[9];
      strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
      resp["current_time"] = timeStr;
    }

    char buf[768];
    serializeJson(resp, buf);
    fl_mqtt.publish(fl_TOPIC_TELEMETRY, buf);
    Serial.println("Settings sent via MQTT");
    return;
  }

  if (strcmp(command, "STATUS") == 0) {
    lastTelemetryTime = 0;
    return;
  }

  Serial.printf("MQTT: Unrecognized command: %s\n", command);
}

/* ================= SERIAL CALLBACK ================= */

void adamSerialCallback(const String& input) {
  if (input == "STATUS") {
    Serial.println("\n--- Adam Motor Status ---");
    Serial.printf("State: %s | Cmd: %s | CF: %s\n",
                  stateToString(motor.state),
                  motor.startCommand ? "ON" : "OFF",
                  motor.contactorConfirmed ? "YES" : "NO");
    Serial.printf("V1=%.1f V2=%.1f V3=%.1f\n", fl_Va, fl_Vb, fl_Vc);
    Serial.printf("I1=%.2f I2=%.2f I3=%.2f avg=%.2f imb=%.1f%%\n",
                  fl_Ia, fl_Ib, fl_Ic, avgCurrent(), imbalance());
    if (motor.state == FAULT) {
      Serial.printf("Fault: %s @ %.2fA\n",
                    faultTypeToString(motor.faultType), motor.faultCurrent);
    }
    Serial.printf("Sensor: %s | Sim: %s | Ruraflex: %s | Schedule: %s\n",
                  fl_sensorOnline ? "ONLINE" : "OFFLINE",
                  fl_simMode ? "ON" : "OFF",
                  ruraflexEnabled ? "ON" : "OFF",
                  motor.scheduleEnabled ? "ON" : "OFF");
  }
  else if (input == "HELP") {
    Serial.println("START       - Start motor");
    Serial.println("STOP        - Stop motor");
    Serial.println("FAULT_RESET - Clear motor fault");
    Serial.println("STATUS      - Show motor state");
  }
  else if (input == "START") {
    if (motor.state == FAULT) {
      Serial.println("Motor: Cannot start while in FAULT");
    } else {
      motor.startCommand = true;
      motor.startCommandTime = millis();
      Serial.println("Motor: Start command issued");
    }
  }
  else if (input == "STOP") {
    motor.startCommand = false;
    fl_setDO(motor.doContactor, false);
    Serial.println("Motor: Stop command issued");
  }
  else if (input == "FAULT_RESET") {
    resetFault();
  }
}

/* ================= ADAM WEB ROUTES ================= */

void setupAdamWebRoutes() {
  fl_server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    StaticJsonDocument<512> doc;
    doc["Va"] = round(fl_Va * 10) / 10.0;
    doc["Vb"] = round(fl_Vb * 10) / 10.0;
    doc["Vc"] = round(fl_Vc * 10) / 10.0;
    doc["Ia"] = round(fl_Ia * 100) / 100.0;
    doc["Ib"] = round(fl_Ib * 100) / 100.0;
    doc["Ic"] = round(fl_Ic * 100) / 100.0;
    doc["avgI"] = round(avgCurrent() * 100) / 100.0;
    doc["imb"] = round(imbalance() * 10) / 10.0;
    doc["state"] = stateToString(motor.state);
    doc["fault"] = faultTypeToString(motor.faultType);
    doc["faultI"] = motor.faultCurrent;
    doc["cmd"] = motor.startCommand;
    doc["cf"] = motor.contactorConfirmed;
    doc["sensor"] = fl_sensorOnline;
    doc["sim"] = fl_simMode;
    doc["uptime"] = millis() / 1000;
    doc["network"] = fl_useEthernet ? "ETH" : "WiFi";
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  fl_server.on("/api/command", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    if (request->hasParam("cmd", true)) {
      String cmd = request->getParam("cmd", true)->value();
      adamMqttCallback(cmd.c_str(), cmd.length());
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Missing cmd parameter");
    }
  });

  fl_server.on("/api/protection", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    StaticJsonDocument<512> doc;
    doc["overcurrent_enabled"] = motor.overcurrentEnabled;
    doc["imbalance_enabled"]   = motor.imbalanceEnabled;
    doc["phase_loss_enabled"]  = motor.phaseLossEnabled;
    doc["dryrun_enabled"]      = motor.dryRunEnabled;
    doc["max_current"]         = motor.maxCurrentA;
    doc["dry_current"]         = motor.dryCurrentA;
    doc["imbalance_pct"]       = motor.imbalancePct;
    doc["phase_loss_current"]  = motor.phaseLossA;
    doc["overcurrent_delay_s"] = motor.overcurrentDelayS;
    doc["imbalance_delay_s"]   = motor.imbalanceDelayS;
    doc["phase_loss_delay_s"]  = motor.phaseLossDelayS;
    doc["dryrun_delay_s"]      = motor.dryrunDelayS;
    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
  });

  fl_server.on("/api/schedule", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!fl_checkAuth(request)) return;
    StaticJsonDocument<384> doc;
    doc["enabled"]      = motor.scheduleEnabled;
    doc["start_hour"]   = motor.scheduleStartHour;
    doc["start_minute"] = motor.scheduleStartMinute;
    doc["end_hour"]     = motor.scheduleEndHour;
    doc["end_minute"]   = motor.scheduleEndMinute;
    doc["days"]         = motor.scheduleDays;
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
  fl_begin();

  Serial.println("\n\n*** ESP32 BOOT ***");
  Serial.println(FW_NAME);
  Serial.printf("Version: %s\n", FW_VERSION);
  Serial.flush();

  initMotor();

  fl_setMqttDefaults(DEFAULT_MQTT_HOST, DEFAULT_MQTT_PORT, DEFAULT_MQTT_USER, DEFAULT_MQTT_PASS);
  fl_setWebAuth(WEB_AUTH_USER, WEB_AUTH_PASS);
  fl_setTelegram(TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID);
  fl_setFirmwareInfo(FW_NAME, FW_VERSION, HW_TYPE);
  fl_setOtaPassword(OTA_PASSWORD);

  WiFi.mode(WIFI_STA);
  fl_checkWifiRestore();
  fl_generateDeviceId();
  fl_printDeviceInfo();

  fl_initNetwork();
  fl_initNTP(2 * 3600);

  fl_loadMqttConfig();
  loadProtection();
  loadSchedule();
  loadRuraflexConfig();

  motor.wasWithinSchedule = isWithinSchedule();
  Serial.printf("Schedule init: %s window\n", motor.wasWithinSchedule ? "within" : "outside");
  if ((motor.scheduleEnabled || ruraflexEnabled) && motor.wasWithinSchedule) {
    motor.startCommand = true;
    Serial.println("Boot within allowed hours, starting");
  }

  fl_setMqttCallback(adamMqttCallback);
  fl_setSerialCallback(adamSerialCallback);

  fl_setDashboardHtml(DASHBOARD_HTML);
  fl_setupWebRoutes();
  setupAdamWebRoutes();
  fl_server.begin();
  Serial.println("Web server started on port 80");

  fl_connectMQTT();
  fl_setupArduinoOTA();

  Serial.println("Setup complete. Entering main loop...");
}

/* ================= LOOP ================= */

void loop() {
  unsigned long now = millis();

  fl_tick();

  // Contactor feedback (DI1)
  bool diFeedback = (fl_diStatus & (1 << motor.diFeedbackBit)) != 0;
  bool contactorOn = (fl_do_state & (1 << motor.doContactor)) == 0;  // active-low
  motor.contactorConfirmed = contactorOn && diFeedback;

  // Force unused DOs off (preserve DO0 contactor + DO4 fault alarm)
  fl_do_state |= ADAM_DO_FORCE_OFF_MASK;
  fl_writeDO();

  // Sensor read + state machine every 500ms
  if (now - lastSensorReadTime >= SENSOR_READ_INTERVAL_MS) {
    lastSensorReadTime = now;
    fl_readSensors();
    updateMotorState();

    // Schedule enforcement
    bool scheduleAllows = isWithinSchedule();
    if (motor.scheduleEnabled || ruraflexEnabled) {
      if (scheduleAllows && !motor.wasWithinSchedule) {
        if (motor.state != FAULT) motor.startCommand = true;
        Serial.println("Schedule: entering allowed hours");
      }
      if (!scheduleAllows && motor.wasWithinSchedule) {
        motor.startCommand = false;
        Serial.println("Schedule: outside allowed hours");
      }
      motor.wasWithinSchedule = scheduleAllows;
    }

    // Update contactor DO
    bool desiredDO = (motor.startCommand && motor.state != FAULT && isWithinSchedule());
    if (desiredDO != motor.lastDOState) {
      fl_setDO(motor.doContactor, desiredDO);
      Serial.printf("Motor contactor: %s\n", desiredDO ? "ON" : "OFF");
      motor.lastDOState = desiredDO;
    }
  }

  // Telemetry publish every 2000ms
  if (now - lastTelemetryTime >= TELEMETRY_INTERVAL_MS) {
    lastTelemetryTime = now;

    if (fl_mqttConnected && fl_mqtt.connected()) {
      StaticJsonDocument<768> doc;
      // Keys match the portal's single-pump (Adam) convention:
      //   Va/Vb/Vc + Ia/Ib/Ic describe the 3 phases of the single motor.
      // Extended metrics (avgI, imb, fault, faultI) are Adam v4+ only.
      doc["Va"]   = round(fl_Va * 10) / 10.0;
      doc["Vb"]   = round(fl_Vb * 10) / 10.0;
      doc["Vc"]   = round(fl_Vc * 10) / 10.0;
      doc["Ia"]   = round(fl_Ia * 100) / 100.0;
      doc["Ib"]   = round(fl_Ib * 100) / 100.0;
      doc["Ic"]   = round(fl_Ic * 100) / 100.0;
      doc["avgI"] = round(avgCurrent() * 100) / 100.0;
      doc["imb"]  = round(imbalance() * 10) / 10.0;
      doc["state"] = stateToString(motor.state);
      doc["cmd"]   = motor.startCommand;
      doc["fault"] = faultTypeToString(motor.faultType);
      doc["faultI"] = motor.faultCurrent;
      doc["cf"]    = motor.contactorConfirmed;
      doc["contactor_confirmed"] = motor.contactorConfirmed;  // portal alias
      doc["mode"]  = "REMOTE";  // Adam v4 is remote-only
      doc["sensor"] = fl_sensorOnline;
      doc["sim"]    = fl_simMode;
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
