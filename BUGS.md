# FieldLink Bug Tracker

Known bugs, issues, and their status across firmware, portal, and dashboard.

**Status legend:** `OPEN` · `IN PROGRESS` · `FIXED` · `WONTFIX` · `BLOCKED`

---

## Open

### BUG-002 No Modbus energy meter at base — blocks end-to-end validation for Adam and Eve
- **Status:** OPEN
- **Severity:** medium
- **Component:** infra / bench setup
- **Reported:** 2026-04-11
- **Symptom:** Neither Adam nor Eve has been validated against a real 3-phase Modbus energy meter. Current firmware reads registers but no one has ever confirmed the readings match a real load.
- **Impact:**
  - Eve: `SENSOR_FAULT` re-triggers every tick when no meter is attached (known gotcha)
  - Adam: entire protection suite (overcurrent, phase imbalance, phase loss, dry-run) is unverified
  - Field devices FL-22F968 and FL-CC8CA0 still need "Connect to 3-phase energy meter and verify pump control" per PROGRESS.md
- **Workaround / mitigation strategy while no meter is available:**
  - **I/O test mode built** (`feature/io-test` branch) — `TEST_PUMP_SIM` command injects fake voltage/current via MQTT, bypasses real Modbus reads. Verified all 6 relay outputs on both devices. Interactive CLI: `tools/io_test.py`.
  - Extract protection math into pure functions (`fl_protection.h`) and unit-test with PlatformIO `pio test -e native` on the host
  - Do Adam rewrite (BUG-001) in a feature branch; merge when unit tests pass; tag + release only after bench validation with a real meter
- **Target meters:** Eastron SDM630-Modbus (primary, already targeted in `fl_modbus.cpp`), Acrel ADL400 3P (secondary, needs driver abstraction). See `memory/modbus_meters.md`.
- **Fix:** Buy an Eastron SDM630 for the base bench rig. ~R2k in ZA. Unblocks both Adam and Eve validation in a single purchase.
- **Notes:** Until a meter is at base, don't tag any new firmware release as "validated" — only "bench tested in simulation". I/O (relay outputs, DI inputs) has been fully verified on both devices.

### BUG-001 Adam (`pump-controller`) is a 3-pump clone of Eve, not a single-motor 3-phase controller
- **Status:** OPEN
- **Severity:** high
- **Component:** firmware (pump-controller)
- **Reported:** 2026-04-11
- **Symptom:** `pump-controller/src/main.cpp` defines `NUM_PUMPS 3` and runs three independent pump state machines, identical to `eve-controller`. This contradicts the original product intent.
- **Design intent:** Adam controls **ONE** motor and uses all 3 CTs of the energy meter to measure L1/L2/L3 of that single motor. Eve controls **THREE** motors and uses one CT per motor. Today both projects are 3-pump clones — only `HW_TYPE`, branding and version differ.
- **Root cause:** Session 2026-03-04 rewrite — `pump-controller v3.0.0 — full rewrite to 3-pump (same architecture as Eve)` threw away the single-motor model.
- **Impact:**
  - No true Adam firmware exists for big single-pump sites
  - Portal has no single-motor/3-phase UI variant
  - Protection logic is wrong for a 3-phase motor (missing phase imbalance and phase-loss detection)
  - `HW_TYPE` misleadingly promises a different product than the code delivers
  - Confusion risk is high for any future contributor
- **Fix (planned — Option A):** Rewrite `pump-controller` as single-motor 3-phase
  - `NUM_PUMPS 1`, one state machine
  - Telemetry schema: `V1/V2/V3 + IL1/IL2/IL3 + avgI + imbalance%`
  - Protection: per-phase overcurrent, phase imbalance %, phase loss / single-phasing detection, dry-run on average current
  - MQTT commands: single-motor (no pump index)
  - Bump to `pump-v4.0.0` (breaking schema change)
  - Portal: add Adam UI variant (single pump card, 3-phase gauges)
- **Migration cost:** Zero — no Adam devices deployed in the field. Both FL-22F968 and FL-CC8CA0 run Eve.
- **Notes:** Do this before anyone installs an Adam device. Design intent saved to memory: `adam_eve_design.md`.

---

## Fixed

### BUG-003 TEST_PUMP_SIM data immediately overwritten by real Modbus read
- **Status:** FIXED
- **Severity:** low (test mode only)
- **Component:** firmware (eve-controller, feature/io-test branch)
- **Reported:** 2026-04-14
- **Symptom:** `sim on` command set fake voltage/current, but pumps stayed in SENSOR_FAULT. Fault alarm relay stayed on, contactor never engaged.
- **Root cause:** Sim override ran before `fl_readSensors()` in the loop. Every 500ms the real Modbus read failed, set `fl_sensorOnline = false`, and zeroed all values — overwriting the simulation.
- **Fix:** Skip `fl_readSensors()` entirely when `testSimEnabled = true`. Sim values injected directly in the sensor read block. Commit `54eb278` on `feature/io-test`.

### BUG-004 Pump START blocked by schedule outside allowed hours
- **Status:** FIXED (workaround)
- **Severity:** low (testing only)
- **Component:** firmware (eve-controller)
- **Reported:** 2026-04-14
- **Symptom:** `start 1` command accepted but relay didn't click. Contactor logic requires `wasWithinSchedule = true`.
- **Root cause:** Testing after 18:00 with schedule set to 06:00-18:00. Pump start is gated by schedule check.
- **Fix:** Widened schedule to 00:00-23:59 via MQTT `SET_SCHEDULE` for testing. Reset to 06:00-18:00 after testing complete. Not a firmware bug — working as designed.

---

## Template

```markdown
### [ID] Short title
- **Status:** OPEN
- **Severity:** low / medium / high / critical
- **Component:** firmware (eve / pump / core) / portal / dashboard / supabase / infra
- **Reported:** YYYY-MM-DD
- **Symptom:** What the user sees / what goes wrong.
- **Repro:** Steps to reproduce (if known).
- **Root cause:** (once known)
- **Fix:** (once fixed — commit hash, version, PR)
- **Notes:** Related bugs, workarounds, links.
```
