# FieldLink Upgrade Progress

## Current Status (2026-04-17)

Both Eve devices running **v1.2.3** production firmware. All safety fixes applied. Hardware I/O verified on both devices via test mode. Portal debug logs cleaned. Pump-controller project updated to v3.2.0 with same fixes. Portal admin rename bugs fixed (pump + device).

| Device | ID | FW | Telegram Group | Notes |
|--------|----|-----|----------------|-------|
| EVE #1 | FL-22F968 | **v1.2.3** (production) | Agrico 1 (`-5222862641`) | **At base.** I/O tested — all 6 relays verified (3 contactors + 3 fault alarms). Renamed to "EVE #1" in Supabase. |
| EVE #2 | FL-CC8CA0 | **v1.2.3** (production) | Agrico 2 (`-5229237038`) | **At base.** I/O tested — all 6 relays verified. Protection reset to sensible defaults. |

**Raspberry Pi 3:** SSH configured (user: `jaco`, hostname: `fieldlink-pi`, IP: `192.168.101.185`). Setup script partially run — errored on PlatformIO install due to PEP 668. Script fixed and pushed (`7331ae8`). Manual PlatformIO install needed to complete setup.

**No blockers.** Both devices ready for field deployment.

---

## Next Steps (Prioritized)

1. [x] ~~**USB flash FL-CC8CA0**~~ — Done. Flashed via COM3, verified with checklist.
2. [x] ~~**Commit + push OTA fix**~~ — Done. Commit `9a757e3`.
3. [x] ~~**Author + ship Eve v1.2.3**~~ — Done. JSON buffer 256→512, removed debug logs. Commit `0094958`.
4. [x] ~~**Reset Pump 2/3 protection on FL-CC8CA0**~~ — All pumps: oc_en=True, dry_en=True, max_I=10, dry_I=1, delays=5s.
5. [x] ~~**Rename FL-22F968**~~ — Renamed to "EVE #1" in Supabase.
6. [x] ~~**Apply fixes to pump-controller**~~ — v3.2.0. Commit `08b77db`.
7. [x] ~~**Remove portal debug logs**~~ — `[MQTT-IN]` removed. Commit `a0fbd2d` (portal repo).
8. [x] ~~**Hardware I/O test mode**~~ — `feature/io-test` branch. TEST_DO, TEST_DI, TEST_PUMP_SIM commands. Interactive CLI `tools/io_test.py`. All 6 relays verified on both devices.
9. [x] ~~**Portal: Pump rename bug fixed**~~ — admin couldn't save pump names. Root cause: `devices.find()` returned null + `user_id` filter blocked cross-user updates. Commit `48845b5`.
10. [x] ~~**Portal: Device rename bug fixed**~~ — same pattern as pump rename. Commit `4446a35`.
11. [ ] **Setup Raspberry Pi 3 for remote site management** — SSH working (user: `jaco`, IP: `192.168.101.185`). Setup script partially run — PlatformIO install needs manual fix (PEP 668). Tailscale not yet configured.
12. [ ] Connect both devices to 3-phase energy meters and verify pump control
13. [ ] Test per-pump START/STOP/RESET via portal (with real energy meter)
14. [ ] Add customers to their respective Telegram groups
15. [ ] Consider local Pi architecture (Mosquitto + Nginx + SQLite) vs cloud — see session notes 2026-04-16

---

## Pending Changes (Not Yet Pushed)

### All firmware committed and pushed (2026-04-14)
- **Eve v1.2.3** (`0094958`): Safety fixes, Modbus skip, deferred publish, notification filter, JSON buffer 512, clean logs
- **OTA fix** (`9a757e3`): WiFiClientSecure for HTTPS firmware downloads
- **Pump-controller v3.2.0** (`08b77db`): Same safety fixes ported from Eve
- **Portal** (`a0fbd2d`): MQTT reconnect, live device time, notification filter, debug logs removed

### Feature branches (not merged to main)
- **`feature/io-test`** — I/O test mode for hardware verification. TEST_DO, TEST_DI, TEST_PUMP_SIM MQTT commands + `tools/io_test.py` CLI. Safe to merge later or keep separate.

### Diagnostic scripts (keep local)
- `mqtt_cmd_diagnose.py`, `mqtt_get_settings_probe.py`, `mqtt_ota_watch.py`, `mqtt_reset_probe.py`, `mqtt_ota_eve.py`

---

## Completed

### Session 2026-04-15–17 (Portal admin fixes, Pi 3 setup, local architecture discussion)
- [x] **Portal: Pump rename bug fixed** — admin couldn't save pump names. `devices.find()` returned null for admin viewing other users' devices (when `showAllDevices=false`, `devices` array is empty). Also `.eq('user_id', currentUser.id)` blocked admin updates to other users' devices. Fixed by fetching `pump_names` directly from Supabase if not in array, and removing `user_id` filter from update. Commit `48845b5`.
- [x] **Portal: Device rename bug fixed** — same root cause as pump rename. Admin couldn't rename devices belonging to other users. Same fix pattern applied. Commit `4446a35`.
- [x] **Pi setup script created** — `tools/pi-setup.sh` automates full Raspberry Pi setup: system update, Python, PlatformIO, Tailscale, USB serial permissions (udev rules for ESP32-S3), git clone, helper scripts (fl-check, fl-monitor, fl-flash, fl-build, fl-test, fl-ota, fl-pull, fl-ports). Run via `curl -sL .../pi-setup.sh | bash`. Commit `42875eb`.
- [x] **Pi setup script PEP 668 fix** — newer Raspberry Pi OS (Bookworm) blocks `pip3 install` system-wide. Changed to `apt-get install python3-paho-mqtt python3-requests`. Commit `7331ae8`.
- [x] **Raspberry Pi 3 SSH configured** — Pi Imager v2.0.3 used. User: `jaco`, hostname: `fieldlink-pi`, IP: `192.168.101.185`. SSH connection confirmed working.
- [ ] **Pi setup incomplete** — script errored on PlatformIO install (PEP 668 in get-platformio.py). Manual install needed: `python3 -m venv ~/.platformio/penv && ~/.platformio/penv/bin/pip install -U platformio`. Then re-run setup script for remaining steps (Tailscale, git clone, helper scripts).
- [x] **Local Pi architecture discussed** — user considering replacing cloud stack (HiveMQ + Supabase + GitHub Pages) with local Pi running Mosquitto + Nginx + SQLite. Hybrid approach recommended: local MQTT broker + portal on Pi, Tailscale for remote access, keep Telegram for alerts and GitHub for code. Biggest effort: replacing Supabase REST calls in portal (~1-2 days). Decision pending.

### Session 2026-04-14 (I/O test mode, production cleanup, Pi planning)
- [x] **I/O test mode created** — `feature/io-test` branch with MQTT commands: `TEST_DO` (cycle/individual relay control), `TEST_DI` (read inputs), `TEST_DI_MONITOR` (continuous change reporting), `TEST_PUMP_SIM` (fake sensor data to bypass SENSOR_FAULT), `TEST_ALL_OFF` (safety kill switch)
- [x] **Interactive test CLI** — `tools/io_test.py` with telemetry suppression, pump start/stop/reset, and sim control
- [x] **Bug fix: sim data overwritten by Modbus** — `fl_readSensors()` was called after sim override, zeroing values. Fixed by skipping Modbus reads when `testSimEnabled=true`
- [x] **Hardware I/O verified on both devices** — all 6 relays confirmed working (3 contactors DO0-DO2, 3 fault alarms DO4-DO6) on FL-22F968 and FL-CC8CA0
- [x] **Both devices flashed back to production v1.2.3** — `main` branch, no test code
- [x] **Schedules reset** to 06:00-18:00 on both devices after testing
- [x] **Protection settings cleaned up** — FL-CC8CA0 all pumps reset to sensible defaults via MQTT
- [x] **FL-22F968 renamed** to "EVE #1" in Supabase
- [x] **Portal debug logs removed** — `[MQTT-IN]` console.log removed, commit `a0fbd2d`
- [x] **Pump-controller v3.2.0** — all Eve safety fixes ported (deferred publish, notification filter, contactor enforcement, schedule tracking), commit `08b77db`
- [x] **Raspberry Pi 3 planned** — remote site management via SSH + Tailscale, USB serial to one Eve device for remote flashing/monitoring

### Session 2026-04-13/14 (Modbus root cause, safety fixes, code review, OTA attempt)
- [x] **Root cause: Modbus timeout blocking main loop** — `readInputRegisters()` 2000ms timeout with no meter connected. Loop stuck at 2s cycles. Fixed by skipping reads when sensor offline, retrying every 30s. Loop now ~10ms.
- [x] **GET_SETTINGS verified: 0.4 seconds** — down from 27+ seconds. Confirmed via `tools/device_checklist.py`.
- [x] **Safety fix: Contactor DO enforced every cycle** — `fl_setDO()` called unconditionally, not just on state change. Prevents pump being left on if tracking gets out of sync.
- [x] **Safety fix: Schedule tracking always updated** — `wasWithinSchedule` updated every cycle regardless of `scheduleEnabled`. Prevents stale state causing unexpected auto-start on schedule toggle.
- [x] **Safety fix: Stale sensor readings zeroed** — voltages and currents reset to 0 on validation failure and when sensor goes offline. Prevents system thinking power is present when it's not.
- [x] **Redundant `isWithinSchedule()` removed** — DO output uses `wasWithinSchedule` instead of recalling function.
- [x] **Telegram only for protection faults** — SENSOR_FAULT excluded from notifications (firmware + portal).
- [x] **3s HTTP timeout on Telegram** — `http.setTimeout(3000)` prevents indefinite blocking.
- [x] **Portal: MQTT reconnect re-subscribes** — fixes admin losing subscription.
- [x] **Portal: Eve device time updates live** — from every telemetry `time` field.
- [x] **Code review completed** — full firmware + portal review. 3 critical safety issues identified and fixed. Portal security/performance issues documented for future.
- [x] **Device checklist tool created** — `tools/device_checklist.py` with `--all` flag.
- [x] **All changes committed and pushed** — firmware `43a12f0`, portal `f970231`. CI builds passed.
- [x] **OTA FL-CC8CA0 attempted** — failed, device MQTT subscription is dead (publishes but doesn't receive commands). Same known issue.
- [x] **OTA HTTPS fix authored** (`fl_ota.cpp`) — WiFiClientSecure + MQTT disconnect before download. Not yet committed.
- [x] **FL-CC8CA0 USB flashed to v1.2.2** — flashed via COM3, verified with checklist. GET_SETTINGS 0.6s. MQTT online, telemetry flowing. Pump 2/3 protection settings need reset (garbage defaults from previous firmware).

### Session 2026-04-12 (schedule redesign + admin pump count + serial diagnosis)
- [x] **Portal: Schedule UI redesigned for Eve devices** — replaced single start/end with per-pump table view. All pumps visible at once with Enabled toggle, Start time, End time per row. Single "Save Schedules" button sends `SET_SCHEDULE` with `pump` parameter for each active pump. Adam (single-pump) devices keep the original simple layout.
- [x] **Portal: Day selection removed from schedule** — schedules now apply every day (`days: 127`), matching customer use case. Day checkboxes removed from both Adam and Eve schedule cards.
- [x] **Portal: Admin-only Active Pumps feature** — admin can set 1, 2, or 3 active pumps per Eve device from Device Config. Inactive pumps hidden from: pump cards, schedule table rows, and protection tabs. Non-admin users cannot see or change this setting.
- [x] **Supabase: `active_pumps` column added** — `ALTER TABLE devices ADD COLUMN active_pumps integer DEFAULT 3 CHECK (active_pumps >= 1 AND active_pumps <= 3)`. Admin UPDATE RLS policy added for cross-user device updates.
- [x] **Portal: `viewDevice()` fetches `active_pumps` directly** — avoids stale cache issues when navigating via Fleet (where `devices` array may not include the column). Direct Supabase query on each device view.
- [x] **Portal: Fleet query updated** — `loadFleetDashboard()` now fetches `active_pumps`, `pump_names`, and `hardware_types` for registered devices.
- [x] **Portal: Pump names applied to schedule table** — `applyPumpNames()` now also updates `sched-name-{n}` cells in the schedule table.
- [x] **FL-22F968 MQTT subscription restored** — USB serial boot log confirmed `Subscribed to: fieldlink/FL-22F968/#` after reset. GET_SETTINGS probe confirmed round-trip: command received on serial, settings response published via MQTT. Dead subscription was transient — resolved by power cycle.
- [x] **FL-22F968 reflashed with correct Telegram chat ID** — `secrets.h` had correct `-5222862641` but previous build had `0`. Rebuilt and flashed via USB. Telegram notifications now working (HTTP 200 confirmed on serial).
- [ ] **Configuration settings not persisting** — protection/threshold/delay settings not saving correctly. Needs investigation (portal commands vs firmware NVS).

### Session 2026-04-11/12 (settings-bleed bug — portal fixed, firmware blocked)
- [x] **Root-caused the "pump settings bleed across tabs" bug** — two stacked issues:
  1. **Portal-side (display):** `switchProtectionTab(n)` silently no-oped if `window.eveProtectionData[p+n]` was missing, leaving form fields with stale values from the previously-selected tab → looked like saves bled across pumps.
  2. **Firmware-side (response never arrives):** v1.2.1's `GET_SETTINGS` handler allocated a 2KB `StaticJsonDocument<1024>` + 1KB char buffer on the stack **inside the MQTT/TLS callback chain** — same pattern that caused commit `86ac3e2`. Stack overflow meant no settings response ever published, so the portal's cache was forever null.
- [x] **Portal fix deployed** (`fieldlogic-portal` commit `78d61b3`) — `openDeviceConfig()` wrapper re-requests `GET_SETTINGS` every time Configure opens, and `switchProtectionTab()` clears the form + re-requests if cache is missing instead of silently no-oping.
- [x] **Eve v1.2.2 firmware built and on Supabase Storage** (`fieldlink` commit `d6586cb`) — `GET_SETTINGS` handler's `StaticJsonDocument<1024> resp` and `char buf[1024]` both made `static` to match the pattern from `86ac3e2`. CI ran, binary uploaded to `firmware-releases/EVE_ESP32S3/v1.2.2.bin`, `curl -sI` confirms 1,192,336 bytes HTTP 200.
- [x] **Discovered latent deadlock: FL-22F968 MQTT subscription is dead** — can't OTA because commands aren't reaching `eveMqttCallback` at all. Proven via `mqtt_reset_probe.py`: RESET pump 1 produces no `s1` transition even though `resetFault()` is unconditional. Power cycle **did not fix it** (confirmed by user — `f1` state changed from `SENSOR_FAULT` to `DRY_RUN` between runs, proving boot happened). Device web UI shows MQTT: Connected, so TCP/TLS is fine — something is wrong between the CONNECT and receiving delivered messages.
- [x] **Identified latent bug in staleness detector** (`fl_comms.cpp:382-390`) — `fl_lastMqttActivity` is bumped on every outgoing telemetry publish (`main.cpp:1374`), so the 90s staleness timeout can never fire even when the subscription is dead. Device can TX-loop forever without self-healing. Fix queued for v1.2.3 (see Next Session #6).
- [x] **Diagnostic scripts added** to project root (uncommitted):
  - `mqtt_cmd_diagnose.py` — sequential STATUS → GET_SETTINGS → UPDATE_FIRMWARE, watches per-phase telemetry counts
  - `mqtt_get_settings_probe.py` — focused STATUS vs GET_SETTINGS round-trip test
  - `mqtt_ota_watch.py` — OTA trigger + 90s telemetry watcher, flags `{"status":"updating"}` + version transitions
  - `mqtt_reset_probe.py` — RESET probe, the one that found the dead subscription
  - `mqtt_ota_eve.py` — one-shot OTA trigger, updated to take `<device_id> <version>` args

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
- [x] Per-pump schedule table (Eve) — all pumps visible at once, daily schedule
- [x] Admin-only active pump count — hide unused pumps across entire UI

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
- `firmware-releases/EVE_ESP32S3/v1.2.3.bin` (current production, CI auto-built)
- `firmware-releases/PUMP_ESP32S3/v3.2.0.bin` (current, CI auto-built)

**OTA command (Eve v1.2.3):**
```json
{"command": "UPDATE_FIRMWARE", "url": "https://einfhsixzxfnbppzydcn.supabase.co/storage/v1/object/public/firmware-releases/EVE_ESP32S3/v1.2.3.bin"}
```
Note: OTA requires WiFi (W5500 Ethernet can't do TLS downloads). v1.2.3 uses WiFiClientSecure for HTTPS.

**Diagnostic tools:**
- `tools/device_checklist.py` — automated health check (`--all` for both devices)
- `tools/io_test.py` — interactive I/O test (requires `feature/io-test` firmware)
- `mqtt_ota_eve.py` — one-shot OTA trigger (`python mqtt_ota_eve.py FL-22F968 1.2.3`)

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
