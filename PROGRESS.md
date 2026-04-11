# FieldLink Upgrade Progress

## Current Status (2026-04-11)

Both devices still on **Eve v1.2.1** at home, unchanged. Session focus was the Adam v4 rewrite (BUG-001): full pump-controller single-motor 3-phase rewrite cut as `pump-v4.0.0-rc1`, parked on feature branches in both firmware and portal repos awaiting spare hardware + Agrico site validation.

| Device | ID | FW | Telegram Group | Notes |
|--------|----|-----|----------------|-------|
| EVE #1 | FL-22F968 | **Eve v1.2.1** | Agrico 1 (`-5222862641`) | Online, tested at home |
| EVE #2 | FL-CC8CA0 | **Eve v1.2.1** | Agrico 2 (`-5229237038`) | Online, tested at home |

**Blocking — Eve deployment:** Site network at Agrico must allow outbound TCP 8883 + UDP 123 before devices can be reinstalled.

**Blocking — Adam v4 release:** (1) no spare ESP32-S3 at base for SIM-mode bench testing, (2) no site visit scheduled yet to run the `SITE_VALIDATION.md` checklist against a real SDM630 + real motor. Production Eve units explicitly **not** being reflashed for bench testing (see memory/feedback_production_hardware.md).

---

## Next Session (Prioritized)

**Adam v4 track (blocked on hardware):**
1. [ ] **When a spare ESP32-S3 arrives** — flash `feature/adam-v4-rc1` over USB, join MQTT under a throwaway device ID, drive SIM mode via `mqtt_test_protection.py`, exercise every fault path in `SITE_VALIDATION.md`. Must blank or redirect `TELEGRAM_CHAT_ID` first so fault tests don't spam customers.
2. [ ] **On next Agrico site visit** — run full `SITE_VALIDATION.md` checklist against real SDM630 + real motor. Capture results back into PROGRESS.md.
3. [ ] **If site validation passes** — bump `FW_VERSION` to `4.0.0` (drop `-rc1`), merge both feature branches to main (firmware + portal together), let `build-pump-firmware` cut `pump-v4.0.0` and upload to Supabase Storage.
4. [ ] **Close BUG-001** in BUGS.md once `pump-v4.0.0` is tagged.

**Eve deployment track (blocked on site network):**
5. [ ] **Get site IT to open outbound ports** at Agrico site: **TCP 8883** (HiveMQ MQTT TLS) and **UDP 123** (NTP).
6. [ ] **Reinstall both Eve devices at Agrico site** — once port 8883 is confirmed open.
7. [ ] **Push local main** — firmware repo `main` is 5 commits ahead of origin (includes the committed Eve v1.2.1 work). Nothing blocks this; just needs a `git push`.
8. [ ] **Merge `feature/frontend-fixes` to main** in fieldlogic-portal (per-pump schedule UI goes live once Eve devices are on v1.2.1 at site).

**Housekeeping:**
9. [ ] Rename FL-22F968 in portal from "ADAM" to "EVE #1"
10. [ ] Reset Pump 3 protection on FL-CC8CA0 (garbage defaults: max=33A, dry=33A, oc_delay=23s)
11. [ ] Add customers to their respective Telegram groups

---

## Pending Changes (Not Yet on Origin Main)

**Firmware repo (`Voltageza/fieldlink`):**
- `main` is **5 commits ahead of `origin/main`** — all Eve v1.2.1 + BUGS.md work (tag `eve-v1.2.1` already cut locally). Just needs `git push origin main` when ready.
- `feature/adam-v4-rc1` — pushed to origin, draft PR [#1](https://github.com/Voltageza/fieldlink/pull/1) open. Contains the full Adam v4.0.0-rc1 rewrite (Phases 1/2/3/5/6). fl-tests CI green on PR. Does **not** trigger `build-pump-firmware` (workflow filters on `main`), so no `pump-v4.0.0-rc1` tag or Supabase upload has been cut yet — that's intentional, it'll fire when the branch merges to main after site validation.

**Portal repo (`Voltageza/fieldlogic-portal`):**
- `feature/adam-v4-rc1` — pushed to origin. Contains the Adam v4 single-pump UI variant (fault banner, avg current + imbalance metrics, imbalance/phase-loss protection + threshold + delay controls, all `.adam-v4-only`-gated). No PR open yet; opens in lockstep with the firmware PR at merge time.
- `feature/frontend-fixes` — per-pump schedule UI (pump tabs above schedule card, Save All Pumps button). Unchanged from previous session. Merge to main after confirming Eve devices work at site.

---

## Completed

### Session 2026-04-11 (Adam v4.0.0-rc1 rewrite — BUG-001)
Rewrote `pump-controller` from the 2026-03-04 3-pump Eve clone back to its original single-motor 3-phase design. Delivered as a release candidate, parked on feature branches, awaiting spare hardware for bench SIM testing and a site visit for real-world validation. Production Eve devices were deliberately not touched.

- [x] **Phase 1 — Protection math extracted to `fl_protection.{h,cpp}`**
  - Pure functions: `fl_prot_average3`, `fl_prot_phaseImbalancePct`, `fl_prot_phaseLoss`, `fl_prot_overcurrent`, `fl_prot_dryRun`
  - `fl_DebounceState` POD + `fl_prot_debounceInit` / `fl_prot_debounceTick` for fault debouncing
  - New `projects/fl-tests` PlatformIO project with Unity native tests covering every primitive
  - New `.github/workflows/fl-tests.yml` runs `pio test -e native` on push + pull_request
  - Tests pass in CI (19s on PR #1)
- [x] **Phase 2 — Runtime SIM mode** (`fl_sim.{h,cpp}`)
  - `fl_simMode` flag, `fl_setSimMode()`, `fl_simSetPhases()` for synthetic phase readings
  - `fl_modbus.cpp` short-circuits reads when sim active, leaving `fl_Va…fl_Ic` driven by the setter
  - `fl_comms.cpp` internal MQTT callback handles `{"command":"SIM","enable":true}` and `{"command":"SIM","V1":...,"I1":...}` for bench injection without a meter
- [x] **Phase 3 — Adam single-motor rewrite** (`pump-controller/src/main.cpp`)
  - One `Motor` struct, DO0 (contactor) + DO4 (fault alarm) + DI1 (feedback), mask `0xEE` forces unused DOs off
  - STOPPED / RUNNING / FAULT state machine with debounced transitions
  - Six fault paths (OVERCURRENT / PHASE_IMBALANCE / PHASE_LOSS / DRY_RUN / SENSOR_FAULT / START_FAILURE), all delegating to `fl_protection` primitives
  - NVS namespaces `prot_adam` (thresholds + enables + delays) and `sched_adam` (schedule + Ruraflex)
  - Telemetry contract: `Va/Vb/Vc + Ia/Ib/Ic + avgI + imb + state/cmd/fault/faultI/cf/mode` + `contactor_confirmed` alias — aligned with the portal's existing single-pump UI contract
  - MQTT commands: START / STOP / RESET / SET_THRESHOLDS / SET_PROTECTION / SET_DELAYS / SET_SCHEDULE / SET_RURAFLEX / GET_SETTINGS / STATUS (no pump index)
  - `FW_VERSION 4.0.0-rc1`, `FW_NAME "ESP32 Adam Single-Motor Controller"`, `HW_TYPE PUMP_ESP32S3` (unchanged so the portal still routes it)
  - Local target build: 90.3% flash (1,184,201 / 1,310,720 bytes), 17.0% RAM, clean link
- [x] **Phase 4 — Portal Adam UI variant** (`fieldlogic-portal/index.html`)
  - New `#device-fault-banner` under state readout: `FAULT: <type> @ <I>A` when in fault, hidden otherwise
  - New `#device-metrics-row` showing avg current + phase imbalance %
  - Protection Settings card: two new `adam-v4-only` toggles (phase imbalance, phase loss)
  - Threshold Settings card: two new `adam-v4-only` inputs (imbalance %, phase-loss current A)
  - Fault Delay Settings card: two new `adam-v4-only` inputs (imbalance delay s, phase-loss delay s)
  - JS: `updateDeviceStatus` hydrates new telemetry, `updateDeviceSettings` toggles `.adam-v4-only` based on `hardware_type !== 'EVE_ESP32S3'` and hydrates new fields from `GET_SETTINGS`, `doSaveProtection`/`doUpdateThresholds`/`doSaveDelays` include new keys only when `!isEve` (Eve payloads stay byte-for-byte identical), back-compat reads both `overcurrent_enabled` and legacy `overcurrent_protection`
- [x] **Phase 5 — rc-aware CI/CD** (`.github/workflows/build-pump-firmware.yml`)
  - Version step now detects `-rc`/`-beta`/`-alpha`/`-dev` suffixes and sets `prerelease=true` accordingly
  - Release body surfaces the prerelease state
  - Supabase upload path handles suffixes transparently (`PUMP_ESP32S3/v4.0.0-rc1.bin` when the branch merges)
- [x] **Phase 6 — Site validation checklist** (`Main Code/projects/pump-controller/SITE_VALIDATION.md`)
  - Pre-visit prep, wiring verification, power-on baseline, per-fault-path exercises (OC / IMB / PL / DR / SENSOR / START), schedule + Ruraflex smoke test, network resilience, OTA round-trip, sign-off
  - The explicit gate between `pump-v4.0.0-rc1` and `pump-v4.0.0`
- [x] **Rollout branches pushed (no merge)**
  - Firmware: `Voltageza/fieldlink:feature/adam-v4-rc1` — draft PR [#1](https://github.com/Voltageza/fieldlink/pull/1), `fl-tests` CI green, `build-pump-firmware` intentionally not triggered (workflow is `main`-only, so no `pump-v4.0.0-rc1` tag or Supabase upload yet)
  - Portal: `Voltageza/fieldlogic-portal:feature/adam-v4-rc1` — pushed, no PR opened yet (non-interactive without live Adam device)
- [x] **Production hardware untouched** — FL-22F968 detected on COM6 during session; flagged and refused to flash. Saved as feedback memory (`feedback_production_hardware.md`): never reflash a deployed FieldLink device without explicit "pulled from site" confirmation.
- [x] **BUGS.md BUG-002 rewritten** — site-only meter access makes SIM the primary dev approach; releases must use `-rc` until site validation has been run
- [x] **BUGS.md BUG-001 status** → IN PROGRESS (rc1 cut, awaiting site validation)

### Session 2026-04-09 (firmware flash + portal fleet fix)
- [x] **Both devices flashed to Eve v1.2.1** via web UI (`/update`). FL-CC8CA0 flashed first (secrets.h already set), then rebuilt with FL-22F968's chat ID and flashed second. Both confirmed online.
- [x] **Periodic "online" status publish added** — `fl_comms.cpp` now publishes retained "online" to status topic every 60s (`FL_MQTT_STATUS_INTERVAL_MS`), clearing stale LWT messages automatically.
- [x] **Portal fleet dashboard fixed** — three bugs found and fixed:
  1. Fleet page was disconnecting MQTT on navigation (`showPage` hit `disconnectMQTT()` for non-dashboard/device pages). Fixed: added `fleet` to the keep-connected list.
  2. Fleet page only subscribed to admin's own devices, not all fleet devices. Fixed: added `subscribeFleetDevices()` to subscribe to all `device_registry` entries.
  3. `lastSeen` was only updated from telemetry, not from status messages. Fixed: "online" status messages now also update `lastSeen`, so the 15-second timeout doesn't falsely show devices as offline.
  - Portal commits: `ce59d48` (MQTT keep-alive + lastSeen), `55705ca` (fleet subscriptions). Both pushed and deployed via GitHub Pages.
- [x] **RLS enabled on Supabase** — verified no impact on portal; all queries returning 200 with data.

### Session 2026-04-07/08 (on-site at Agrico)
- [x] **Root cause identified: site network blocks outbound port 8883** — devices couldn't reach HiveMQ Cloud for MQTT TLS. Confirmed by connecting device to a different WiFi — MQTT connected immediately.
- [x] **On-site web UI verified working** — both `/config` and main dashboard reachable via device LAN IP. FL-22F968 MQTT status badge confirmed *Connected* after WiFi flap recovered.
- [x] **Captive portal behavior documented** — WiFiManager captive portal ONLY opens at boot if `autoConnect()` fails (180s timeout). Does NOT open during runtime. To force: wipe NVS (`pio run -t erase`) or power-cycle while saved AP is unreachable.
- [x] **Required outbound ports documented** — TCP 8883, UDP 123, TCP 443, UDP 53.
- [x] **Laptop prepared for site work** — VS Code + PlatformIO; ESP32-S3 uses native USB-CDC.

### Session 2026-03-26
- [x] **User Jan-Hendrik verified** — email confirmed via SQL (`UPDATE auth.users SET email_confirmed_at = now()`), password set via `crypt()` in Supabase SQL Editor
- [x] **Portal: Pump rename feature** — pencil icon on each pump card, names stored in Supabase `pump_names` JSONB column, updates pump cards + protection tabs. Live on fieldlogic.co.za (commit `a6190d3` cherry-picked to main)
- [x] **Supabase: `pump_names` column added** — `ALTER TABLE devices ADD COLUMN pump_names JSONB DEFAULT '{}'`
- [x] **Firmware: Per-pump schedule control** — each pump gets independent schedule (enable, start/end time, days). NVS namespaces `sched_p1/2/3`. `SET_SCHEDULE` accepts optional `"pump"` field. Ruraflex stays global, overrides at runtime but keeps schedules stored.
  - Eve v1.2.0 (`eve-v1.2.0` tag) — built and uploaded to Supabase Storage
  - Pump v3.1.0 (`pump-v3.1.0` tag) — built and uploaded to Supabase Storage
- [x] **CI workflows fixed** — added `TELEGRAM_BOT_TOKEN` and `TELEGRAM_CHAT_ID` GitHub secrets + updated both workflow YAML files
- [x] **Telegram library committed** — `fl_telegram.h/cpp` direct Bot API changes (from March 17) finally committed to repo
- [x] **WiFi auto-reconnect** — `WiFi.setAutoReconnect(true)` added to `fl_comms.cpp` (committed locally)
- [x] **Portal: Per-pump schedule UI** — pump tab selector moved above schedule card, schedule pump label, "Save All Pumps" button, backward compat with old firmware. On `feature/frontend-fixes` branch (commit `7b5d3cd`)

### Session 2026-03-24
- [x] **Deno Deploy dropped** — confirmed not in use; admin functions (create-user, reset-password) handled via Supabase Dashboard directly
- [x] **Portal: Fixed pump tab visibility in light mode** — `.pump-protection-tab.active` used undefined `--primary` CSS var, replaced with `--accent` (commit `3b4eb5b` in fieldlogic-portal)
- [x] **Portal: Fixed Eve UI flicker on load** — MQTT telemetry overwrote Supabase-seeded `hardware_type`, causing 3-pump UI to briefly revert to single-pump. Now preserves `hardware_type` across MQTT updates (commit `f2c0494` in fieldlogic-portal)

### Session 2026-03-17
- [x] **Telegram fault notifications — direct to Telegram API**
  - Removed Deno Deploy dependency for fault notifications
  - ESP32 calls Telegram Bot API directly (`fl_telegram.cpp`)
  - Each device has its own Telegram group chat ID in `secrets.h`
  - Bot: @FieldLogicAlertsBot (token in secrets.h)
- [x] **Per-device Telegram groups created**
  - "EPRO - FieldLogic - Agrico 1" → FL-22F968
  - "EPRO - FieldLogic - Agrico 2" → FL-CC8CA0
- [x] **Both devices flashed to Eve v1.1.0**
  - FL-22F968 via USB
  - FL-CC8CA0 via OTA (Supabase Storage: `EVE_ESP32S3/v1.1.0.bin`)
- [x] **Fault notification tested** — FL-22F968 SENSOR_FAULT appeared in Agrico 1 group
- [x] **Supabase schema updated** — `notification_chat_id` and `last_fault_notification` columns added to `devices` table
- [x] **Deno installed** on dev machine (`winget install DenoLand.Deno`)

### Session 2026-03-16
- [x] **FL-CC8CA0 powered on and connected** — WiFi + MQTT connected, Eve v1.0.1 running
- [x] **FL-CC8CA0 hardware_type_id fixed in Supabase**
- [x] **Portal flicker fix** — Eve devices briefly showed single-pump UI before switching to 3-pump
- [x] **Ethernet / MQTT clarified** — W5500 Ethernet gets IP but can't do TLS; MQTT always via WiFi

### Session 2026-03-04
- [x] **Both firmware projects upgraded to 3-pump controllers**
  - `eve-controller` v1.0.1 — static buffer fix in MQTT callback chain
  - `pump-controller` v3.0.0 — full rewrite to 3-pump (same architecture as Eve)
  - `FieldLinkCore/fl_comms.cpp` — static buffers to prevent stack overflow in TLS callbacks
- [x] **New CI/CD workflow** — `build-pump-firmware.yml` created, builds and uploads to Supabase `PUMP_ESP32S3/v3.0.0.bin`
- [x] **FL-22F968 flashed with Eve v1.0.1** via USB (OTA failed — device was on Ethernet)
- [x] **Portal device list fixed for Eve devices**

### Session 2026-02-16
- [x] Second device FL-CC8CA0 registered in Supabase
- [x] Fixed device_id typo in registry (FLCC8CA0 → FL-CC8CA0)
- [x] Fixed missing RLS SELECT policy on device_registry table
- [x] Device successfully added on portal

### Firmware (pump-controller / eve-controller)
- [x] Both projects use FieldLinkCore shared library
- [x] Both projects build via GitHub Actions and upload to Supabase Storage
- [x] Per-pump overcurrent/dry-run protection with NVS storage
- [x] Per-pump schedule control (v1.2.0+) with independent start/end/days per pump
- [x] Per-pump and aggregate MQTT commands (START/STOP/RESET with pump number)
- [x] 3-pump web dashboard embedded in firmware
- [x] OTA via Supabase Storage URLs
- [x] Direct Telegram fault notifications (per-device group)
- [x] WiFi auto-reconnect (v1.2.1+, not yet deployed)

### Portal (fieldlogic.co.za)
- [x] Config modal with all settings
- [x] Admin password protection
- [x] Eve 3-pump UI (auto-detects EVE_ESP32S3 from telemetry)
- [x] Device list correctly shows Eve state and readings
- [x] Startup animation, contactor badge
- [x] Firmware management page (push OTA to devices)
- [x] Per-pump rename (pump_names JSONB in Supabase)

### Infrastructure
- [x] GitHub Actions CI/CD for Eve and Pump firmwares (with Telegram secrets)
- [x] Supabase Storage for firmware OTA (`firmware-releases/EVE_ESP32S3/`, `PUMP_ESP32S3/`)
- [x] HiveMQ Cloud MQTT broker (TLS 8883)
- [x] Telegram Bot (@FieldLogicAlertsBot) — direct API from ESP32

---

## Required Outbound Network Ports (for site firewalls)

| Port | Proto | Purpose | Destination |
|------|-------|---------|-------------|
| 8883 | TCP | MQTT TLS to HiveMQ Cloud | `*.s1.eu.hivemq.cloud` |
| 443  | TCP | Telegram Bot API (fault alerts) | `api.telegram.org` |
| 443  | TCP | OTA firmware downloads | `*.supabase.co` |
| 53   | UDP | DNS | router / 8.8.8.8 / 1.1.1.1 |
| 123  | UDP | NTP (required for TLS cert validation!) | `pool.ntp.org` |

**Port 8883 and UDP 123 are the most commonly blocked** on locked-down site networks. If either is blocked, MQTT will silently fail (TLS handshake failure from clock drift looks identical to port block).

---

## Architecture Summary

```
2x Waveshare ESP32-S3-POE-ETH-8DI-8DO
  ├─ MQTT TLS (WiFi) → HiveMQ Cloud → fieldlogic.co.za portal
  └─ HTTPS (WiFi) → Telegram Bot API (fault alerts to customer groups)

fieldlogic.co.za (GitHub Pages portal) → Supabase (PostgreSQL + Auth + Storage)
```

**Telegram Groups:**
- EPRO - FieldLogic - Agrico 1 → FL-22F968 (chat_id: -5222862641)
- EPRO - FieldLogic - Agrico 2 → FL-CC8CA0 (chat_id: -5229237038)

**Firmware storage:**
- `firmware-releases/EVE_ESP32S3/v1.2.0.bin` (ready for OTA)
- `firmware-releases/PUMP_ESP32S3/v3.1.0.bin` (ready for OTA)

**OTA command (Eve v1.2.0):**
```json
{"command": "UPDATE_FIRMWARE", "url": "https://einfhsixzxfnbppzydcn.supabase.co/storage/v1/object/public/firmware-releases/EVE_ESP32S3/v1.2.0.bin"}
```
Note: OTA requires WiFi (W5500 Ethernet can't do TLS downloads)

---

## How to Resume

**Start here:** Check **Next Session** section above.

**OTA update (recommended order):**
1. Update FL-22F968 first — send OTA command via portal or MQTT
2. Wait 2-3 minutes, verify device reconnects with v1.2.0
3. If stable, update FL-CC8CA0
4. Merge `feature/frontend-fixes` to main in fieldlogic-portal

**Flash via USB if OTA fails:**
```
cd "Main Code/projects/eve-controller"
C:\Users\jacok\.platformio\penv\Scripts\pio.exe run -t upload
```

**Important:** Before flashing via USB, check `secrets.h` has the correct `TELEGRAM_CHAT_ID` for the target device:
- FL-22F968: `-5222862641` (Agrico 1)
- FL-CC8CA0: `-5229237038` (Agrico 2)

**Portal branches:**
- `main` — live on fieldlogic.co.za (has pump rename)
- `feature/frontend-fixes` — has per-pump schedule UI (merge after OTA)

**Check MQTT telemetry:**
```bash
python mqtt_check.py
```
