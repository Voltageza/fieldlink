#include "fl_sim.h"
#include "fl_modbus.h"

bool fl_simMode = false;

void fl_setSimMode(bool enable) {
  if (enable == fl_simMode) return;
  fl_simMode = enable;
  Serial.printf("[SIM] Sim mode %s\n", enable ? "ENABLED" : "DISABLED");
  if (enable) {
    // Force sensor-online so projects don't fire SENSOR_FAULT against
    // the stale "meter offline" state from before sim was enabled.
    fl_sensorOnline = true;
    fl_modbusFailCount = 0;
  }
}

void fl_simSetPhases(float V1, float V2, float V3,
                     float I1, float I2, float I3) {
  if (!fl_simMode) return;
  fl_Va = V1; fl_Vb = V2; fl_Vc = V3;
  fl_Ia = I1; fl_Ib = I2; fl_Ic = I3;
}
