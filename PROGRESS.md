# FieldLink Upgrade Progress

## Current Status (2026-04-09)

Both devices flashed to **Eve v1.2.1** and confirmed online at home. Portal fleet dashboard fixed — devices stay online in admin fleet view.

| Device | ID | FW | Telegram Group | Notes |
|--------|----|-----|----------------|-------|
| EVE #1 | FL-22F968 | **v1.2.1** | Agrico 1 (`-5222862641`) | Online, tested at home |
| EVE #2 | FL-CC8CA0 | **v1.2.1** | Agrico 2 (`-5229237038`) | Online, tested at home |

**Blocking:** Site network at Agrico must allow outbound TCP 8883 + UDP 123 before devices can be reinstalled.

---

## Next Session (Prioritized)

1. [ ] **Get site IT to open outbound ports** at Agrico site: **TCP 8883** (HiveMQ MQTT TLS) and **UDP 123** (NTP). Without this, devices will keep dropping offline.
2. [ ] **Reinstall both devices at Agrico site** — once port 8883 is confirmed open
3. [ ] **Commit and tag Eve v1.2.1 firmware** — version bump + periodic status publish + auto-reconnect fix. Tag `eve-v1.2.1`, upload to Supabase Storage for future OTA.
4. [ ] **Merge `feature/frontend-fixes` to main** in fieldlogic-portal (per-pump schedule UI goes live now that devices are on v1.2.1)
5. [ ] Rename FL-22F968 in portal from "ADAM" to "EVE #1"
6. [ ] Reset Pump 3 protection on FL-CC8CA0 (garbage defaults: max=33A, dry=33A, oc_delay=23s)
7. [ ] Connect both devices to 3-phase energy meters and verify pump control
8. [ ] Test per-pump START/STOP/RESET via portal
9. [ ] Add customers to their respective Telegram groups

---

## Pending Changes (Not Yet Pushed — Firmware Repo)

- **Eve v1.2.1 firmware changes** (all local, not committed/tagged/pushed):
  - `FW_VERSION` bumped to `1.2.1` in `eve-controller/src/main.cpp`
  - `WiFi.setAutoReconnect(true)` in `fl_comms.cpp`
  - Periodic "online" status publish every 60s in `fl_comms.cpp` (clears stale LWT)
  - `FL_MQTT_STATUS_INTERVAL_MS` constant added to `fl_comms.h`
- **Portal `feature/frontend-fixes` branch** — per-pump schedule UI (pump tabs above schedule card, Save All Pumps button). Merge to main after confirming devices work at site.

---

## Completed

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
