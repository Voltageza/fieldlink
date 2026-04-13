#include "fl_protection.h"

#include <math.h>

// ---------- Pure math ----------

float fl_prot_average3(float il1, float il2, float il3) {
  return (il1 + il2 + il3) / 3.0f;
}

float fl_prot_phaseImbalancePct(float il1, float il2, float il3) {
  float avg = fl_prot_average3(il1, il2, il3);
  if (avg < FL_PROT_IMBALANCE_MIN_AVG_A) {
    // Motor not running (or barely). Imbalance % is meaningless here.
    return 0.0f;
  }
  float d1 = fabsf(il1 - avg);
  float d2 = fabsf(il2 - avg);
  float d3 = fabsf(il3 - avg);
  float maxDev = d1;
  if (d2 > maxDev) maxDev = d2;
  if (d3 > maxDev) maxDev = d3;
  return (maxDev / avg) * 100.0f;
}

bool fl_prot_phaseLoss(float il1, float il2, float il3,
                       float phaseLossA, float runningA) {
  // Motor must actually be drawing load on at least one phase, otherwise
  // "all phases at 0A" is just "stopped", not phase loss.
  bool anyRunning = (il1 >= runningA) || (il2 >= runningA) || (il3 >= runningA);
  if (!anyRunning) return false;

  // Phase loss: any phase below the loss threshold while something else is running.
  return (il1 < phaseLossA) || (il2 < phaseLossA) || (il3 < phaseLossA);
}

bool fl_prot_overcurrent(float current, float maxA) {
  return current > maxA;
}

bool fl_prot_dryRun(float current, float dryA) {
  return current < dryA;
}


// ---------- Debounce ----------

void fl_prot_debounceInit(fl_DebounceState& s) {
  s.active = false;
  s.startTimeMs = 0;
}

bool fl_prot_debounceTick(fl_DebounceState& s,
                          bool conditionTrue,
                          uint32_t nowMs,
                          uint32_t delayMs) {
  if (!conditionTrue) {
    s.active = false;
    s.startTimeMs = 0;
    return false;
  }

  if (!s.active) {
    s.active = true;
    s.startTimeMs = nowMs;
  }

  if (delayMs == 0) {
    return true;
  }

  return (nowMs - s.startTimeMs) >= delayMs;
}
