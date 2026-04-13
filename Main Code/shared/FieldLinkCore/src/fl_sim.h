#ifndef FL_SIM_H
#define FL_SIM_H

#include <Arduino.h>

// -----------------------------------------------------------------------------
// Runtime energy-meter simulation.
//
// When fl_simMode is true, fl_readSensors() skips the real Modbus transaction
// and returns the last values set via fl_simSetPhases(). This lets us exercise
// every protection path (overcurrent, phase imbalance, phase loss, dry run)
// from the bench without a physical meter.
//
// SIM mode is a RUNTIME flag:
//   - Defaults to OFF on boot
//   - Never persisted to NVS — a reboot always returns to reality
//   - Toggled via MQTT `{"command":"SIM", ...}` or directly in code
//
// This prevents the "forgot to turn sim off" failure mode that compile-time
// flags would introduce, and lets us iterate at base on the same firmware
// binary that will ship to the field.
// -----------------------------------------------------------------------------

// Global sim-mode flag. Read-only for consumers; use fl_setSimMode() to change.
extern bool fl_simMode;

// Enable or disable sim mode. When enabling, fl_sensorOnline is forced true
// so project-level SENSOR_FAULT logic treats sim data as valid immediately.
void fl_setSimMode(bool enable);

// Push synthetic phase readings into the shared fl_Va/Vb/Vc + fl_Ia/Ib/Ic
// globals. No-op if sim mode is disabled.
void fl_simSetPhases(float V1, float V2, float V3,
                     float I1, float I2, float I3);

#endif // FL_SIM_H
