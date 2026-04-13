#ifndef FL_PROTECTION_H
#define FL_PROTECTION_H

// FieldLink protection math.
//
// Pure functions and small POD state machines. No Arduino.h, no millis(),
// no hardware access — so this compiles on any host and can be unit-tested
// via PlatformIO `pio test -e native`.
//
// Time is always passed in as a parameter (nowMs). Callers pass millis()
// on a real device, or a test-controlled clock in unit tests.

#include <stdint.h>

// ---------- Pure math helpers ----------

// Arithmetic mean of three currents. Safe for any inputs.
float fl_prot_average3(float il1, float il2, float il3);

// Max phase deviation from the mean, expressed as a percent of the mean.
// Industry convention: maxDev / avg * 100. Returns 0.0 when avg is very small
// (below FL_PROT_IMBALANCE_MIN_AVG_A) because imbalance % is meaningless on
// a stopped motor and would otherwise divide-by-near-zero.
#define FL_PROT_IMBALANCE_MIN_AVG_A 0.5f
float fl_prot_phaseImbalancePct(float il1, float il2, float il3);

// Phase loss / single-phasing detection.
// Returns true when at least one phase is below `phaseLossA` while at
// least one other phase is above `runningA`. This pattern catches a
// blown fuse / dropped phase under load. Returns false if the motor
// is simply not running (all phases below runningA).
bool fl_prot_phaseLoss(float il1, float il2, float il3,
                       float phaseLossA, float runningA);

// Simple overcurrent: current > maxA. Used per-phase for Adam and
// per-pump for Eve.
bool fl_prot_overcurrent(float current, float maxA);

// Dry-run: current < dryA. Caller is responsible for only calling
// this while the motor is commanded to run.
bool fl_prot_dryRun(float current, float dryA);


// ---------- Debounce state machine ----------
//
// A condition is "latched" only if it persists for at least `delayMs`
// milliseconds. This prevents momentary spikes (e.g. inrush current,
// single bad Modbus sample) from tripping a fault.
//
// Usage:
//   fl_DebounceState oc;
//   fl_prot_debounceInit(oc);
//   ...each loop:
//   bool oc_now = fl_prot_overcurrent(current, maxA);
//   if (fl_prot_debounceTick(oc, oc_now, millis(), delayMs)) {
//       triggerFault(OVERCURRENT);
//   }

struct fl_DebounceState {
  bool active;           // is the condition currently being timed?
  uint32_t startTimeMs;  // when the condition first became true
};

void fl_prot_debounceInit(fl_DebounceState& s);

// Advance the debounce state machine.
// Returns true exactly when the condition has been continuously true
// for >= delayMs milliseconds. If delayMs == 0, returns true on the
// first tick where the condition is true (no debounce).
// When the condition goes false, the timer resets.
bool fl_prot_debounceTick(fl_DebounceState& s,
                          bool conditionTrue,
                          uint32_t nowMs,
                          uint32_t delayMs);

#endif // FL_PROTECTION_H
