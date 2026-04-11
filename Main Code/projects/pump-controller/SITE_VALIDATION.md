# Adam v4.0.0 — Site Validation Checklist

Gate between `pump-v4.0.0-rc1` and `pump-v4.0.0`. No Adam device gets
promoted to the stable tag until every item below passes on real
hardware with a real SDM630 meter wired to a real motor.

## Why this exists
Adam v4 is a ground-up rewrite of the pump-controller project from a
3-pump Eve clone back to its original single-motor 3-phase design
(BUG-001). Protection math is unit-tested in `projects/fl-tests` and
has been exercised via SIM mode, but nothing in this firmware has
touched a real CT or contactor since the rewrite. This checklist is
the bridge.

## Pre-visit prep
- [ ] Confirm device is flashed with `pump-v4.0.0-rc1` (OTA URL:
      `.../firmware-releases/PUMP_ESP32S3/v4.0.0-rc1.bin`)
- [ ] `secrets.h` points Telegram at the correct customer group
- [ ] Portal shows the device online and reports
      `hardware_type: PUMP_ESP32S3`
- [ ] Portal is rendering the Adam-only UI (fault banner,
      avg current, imbalance %, imbalance/phase-loss protection
      toggles and thresholds — anything with the `adam-v4-only`
      class should be visible)
- [ ] Serial console reachable on site (laptop + USB cable) as a
      fallback channel if MQTT drops

## Wiring verification (power OFF)
- [ ] SDM630 L1/L2/L3 CTs clamped on the correct motor phases,
      arrows pointing toward the load
- [ ] SDM630 RS485 A/B wired to FL_RS485 (no polarity swap)
- [ ] Modbus ID = 1, 9600 8N1 (matches `FL_MODBUS_ID` +
      `FL_RS485_BAUD`)
- [ ] DO0 drives the motor contactor coil (via interposing relay
      if needed — TCA9554 is not rated for coil current)
- [ ] DO4 wired to the external fault alarm / beacon
- [ ] DI1 sees an auxiliary contact from the motor contactor
      (used for `contactor_confirmed`)
- [ ] All other DO/DI channels left unconnected (mask `0xEE`
      forces them off anyway)

## Power-on baseline (motor still OFF)
- [ ] Device boots, joins network, portal goes online
- [ ] Telemetry shows `Va/Vb/Vc` near nominal (e.g. 230V / 400V),
      all three within ±5% of each other
- [ ] Telemetry shows `Ia/Ib/Ic ≈ 0` (no CT drift)
- [ ] `state = STOPPED`, `cmd = false`, `fault = NO_FAULT`
- [ ] `contactor_confirmed = false`
- [ ] `avgI` and `imb` fields populated in the portal metrics row
- [ ] No spurious `SENSOR_FAULT` — Modbus comms stable for ≥ 60s

## Thresholds — set BEFORE first start
From the portal Threshold Settings card, load conservative
values for this pump. Defaults in firmware are `max 120A, dry 2A,
imb 20%, PL 1A` which are placeholders. Always override for real
pumps:
- [ ] Overcurrent set to ~115-125% of the pump's nameplate FLA
- [ ] Dry-run set to ~30-50% of no-load running current
- [ ] Phase imbalance left at 20% (tighten later if site is clean)
- [ ] Phase-loss threshold at 1-2A (well below real running current)
- [ ] Delays: OC=2s, IMB=5s, PL=2s, DR=5s (defaults — only change
      if the motor has a known long inrush)
- [ ] `GET_SETTINGS` round-trips the new values and survives a
      power cycle (NVS namespace `prot_adam`)

## First start — no fault
- [ ] Issue START from the portal
- [ ] DO0 energises, contactor pulls in, `cmd = true`
- [ ] Within ~1s, `contactor_confirmed = true` (DI1 feedback)
- [ ] Within `STATE_DEBOUNCE_COUNT` telemetry ticks, `state`
      transitions STOPPED → RUNNING
- [ ] `avgI` climbs to expected running current, `imb` stays
      below threshold
- [ ] No faults raised for at least 60s of steady running
- [ ] Issue STOP from the portal
- [ ] DO0 de-energises, `state` returns to STOPPED, `avgI ≈ 0`

## Fault path exercises
Do each of these at least once. After each trip:
  - [ ] Portal fault banner displays the correct fault + current
  - [ ] Telegram notification arrives in the customer group with the
        right fault type and current
  - [ ] DO4 (fault alarm) energises
  - [ ] RESET from portal clears the fault and returns to STOPPED
  - [ ] Serial log matches portal/telegram

### OVERCURRENT
- [ ] Temporarily drop the OC threshold below the motor's running
      current, START, confirm trip after `oc_delay` seconds
- [ ] Restore the real threshold before leaving site

### PHASE_IMBALANCE
- [ ] Drop imbalance threshold to something below measured natural
      imbalance (e.g. 2%), START, confirm trip after `imb_delay`
- [ ] Restore original threshold

### PHASE_LOSS
- [ ] With motor running, pull one phase (disconnect at isolator
      or remove one contactor wire — **power off first**, resume,
      then open that phase via an external isolator while running)
- [ ] Confirm `PHASE_LOSS` trip after `pl_delay`
- [ ] Safer alternative: raise PL threshold above running current
      on one phase via thresholds card and observe

### DRY_RUN
- [ ] Raise dry-run threshold above the motor's loaded current,
      START, confirm `DRY_RUN` trip after `dr_delay`
- [ ] Restore real threshold

### SENSOR_FAULT
- [ ] While stopped, disconnect the RS485 link to the SDM630
- [ ] After `FL_MAX_MODBUS_FAILURES` consecutive failures the
      firmware raises `SENSOR_FAULT`
- [ ] Reconnect, RESET, verify recovery

### START_FAILURE
- [ ] With motor mechanically isolated (breaker off) issue START
- [ ] No current develops; after `START_TIMEOUT_MS` the firmware
      raises `START_FAILURE`
- [ ] Restore breaker, RESET, verify normal start works

## Schedule + Ruraflex smoke test
- [ ] Set a schedule window a few minutes ahead, confirm motor
      auto-starts at the window
- [ ] Set schedule end a few minutes later, confirm auto-stop
- [ ] Enable Ruraflex with a fake off-peak window covering "now",
      confirm start allowed
- [ ] Flip Ruraflex window to a peak period, confirm start is
      blocked and telemetry reflects it
- [ ] Disable Ruraflex before leaving

## Network resilience
- [ ] Unplug Ethernet (if present) — device should fall back to
      WiFi within ~30s without losing motor state
- [ ] Reboot device while motor is RUNNING — on recovery the
      motor should come back in a safe STOPPED state (no auto
      restart) and fault history should be intact in NVS

## OTA round-trip
- [ ] From the portal, push an OTA to the same `v4.0.0-rc1` binary
      (or a throwaway `v4.0.0-rc2`) and confirm the device
      re-appears online
- [ ] Motor is STOPPED across the OTA (firmware stops on
      `UPDATE_FIRMWARE` before flashing)
- [ ] No fault raised during or after OTA

## Sign-off
- [ ] All fault paths above exercised and recorded
- [ ] Customer thresholds persisted in NVS and verified via
      `GET_SETTINGS`
- [ ] Customer added to correct Telegram group
- [ ] This checklist filed with the commissioning pack

Only once every box above is ticked for at least one deployment
site, promote `pump-v4.0.0-rc1` → `pump-v4.0.0` by bumping
`FW_VERSION` in `main.cpp` and letting the workflow cut the
stable tag. Update **BUG-001** in `BUGS.md` with the validation
date and close it.
