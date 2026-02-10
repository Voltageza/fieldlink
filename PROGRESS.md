# FieldLink Upgrade Progress

## Completed

### Firmware (main.cpp) - v2.10.0
- [x] Configurable fault delay times (overcurrent/dryrun, 0-30 seconds)
- [x] DI4 contactor feedback (contactorConfirmed in telemetry)
- [x] SET_DELAYS MQTT command handler
- [x] Delays stored in NVS (persist after reboot)
- [x] GET_SETTINGS returns delay values

### Portal (index.html)
- [x] Config modal with all settings (cleaner device page)
- [x] Fault Delay Settings card in modal
- [x] Contactor status badge (CONFIRMED/PENDING/OFF)
- [x] Startup animation (4-step sequence)
- [x] saveDelays() function sends SET_DELAYS command
- [x] Admin password protection for Threshold & Fault Delay settings
- [x] Admin password protection for Protection toggle settings
- [x] Admin password change UI (Settings page, admin-only)
- [x] Contactor badge uses actual DO state instead of cmd flag
- [x] Startup animation uses DO state with 5s grace period for stale telemetry

### Infrastructure
- [x] GitHub Actions workflow created (.github/workflows/build-firmware.yml)
- [x] GitHub Secrets added (MQTT_HOST, MQTT_USER, MQTT_PASS, etc.)
- [x] Supabase `portal_settings` table for admin password (default: `changeme`)

---

## In Progress

### Supabase Storage for OTA - COMPLETE ✅
**Date:** 2026-02-03

GitHub Actions workflow uploads firmware to Supabase Storage after build. OTA tested and working!

**Bucket:** `firmware-releases` (PUBLIC)
**Path:** `firmware-releases/PUMP_ESP32S3/v{version}.bin`

**OTA URL format:**
```
https://einfhsixzxfnbppzydcn.supabase.co/storage/v1/object/public/firmware-releases/PUMP_ESP32S3/v{version}.bin
```

**GitHub Secrets required:**
- `SUPABASE_URL` = `https://einfhsixzxfnbppzydcn.supabase.co`
- `SUPABASE_SERVICE_KEY` = (service_role key from Supabase Dashboard → Settings → API)

**Status:**
- [x] Supabase Storage bucket exists (`firmware-releases`)
- [x] GitHub Actions workflow updated
- [x] GitHub Secrets added (SUPABASE_URL, SUPABASE_SERVICE_KEY)
- [x] CI/CD build uploads to Supabase successfully
- [x] OTA tested on device - WORKS!
- [x] Device linked to hardware type (FL-22F968 → PUMP_ESP32S3)

---

### Firmware v2.10.0 - FLASHED ✅
**Date:** 2026-01-30

**OTA via GitHub URL failed** - ESP32 couldn't handle GitHub CDN redirects/SSL.
**Flashed via USB** using PlatformIO `pio run -t upload` - successful!

**Release URL:** https://github.com/Voltageza/fieldlink/releases/tag/v2.10.0

### Testing v2.10.0 Features
- [x] Verify device comes back online with v2.10.0 ✅
- [x] Test contactor feedback (DI4) works ✅ (`contactor_confirmed` in telemetry)
- [x] Test contactor badge shows in portal (CONFIRMED/PENDING/OFF) ✅ Fixed to use DO state
- [x] Test startup animation progression ✅ Fixed race condition with 5s grace period
- [ ] Test fault delays work correctly (set delay, trigger fault)
- [ ] Test delays persist after reboot
- [ ] Update local secrets.h with new MQTT password

---

## Documentation

### System Architecture (SYSTEM_ARCHITECTURE.txt)
**Created:** 2026-02-03

Complete documentation of the FieldLink hosting and infrastructure:
- Hosting breakdown (GitHub Pages, HiveMQ, Supabase, Deno Deploy)
- Data flows (telemetry, commands, notifications, auth)
- Service endpoints and credentials
- Database schema (Supabase tables)
- CI/CD pipeline details
- Security architecture
- Known issues and quick commands

---

## Not Started

### OTA Improvements - COMPLETE ✅
- [x] Investigate why GitHub URLs fail (SSL/redirect issue) - GitHub CDN redirects + complex SSL chain
- [x] Configure Supabase Storage for firmware hosting
- [x] Test OTA with Supabase-hosted firmware URL - WORKS!
- [x] Fix device hardware type in portal for push updates (linked FL-22F968 to PUMP_ESP32S3)

---

## Files Modified

| File | Status |
|------|--------|
| `Main Code/FieldLink_Main_Code/src/main.cpp` | Modified, committed, v2.10.0 built |
| `fieldlogic-portal/index.html` | Modified, committed, deployed |
| `.github/workflows/build-firmware.yml` | Updated 2026-02-03, added Supabase upload |
| `SYSTEM_ARCHITECTURE.txt` | Created 2026-02-03, full system documentation |

---

## Deployment Info

### Portal (fieldlogic-portal)
- **Repo:** https://github.com/Voltageza/fieldlogic-portal
- **Separate git repo** (gitignored in main FieldLink repo)
- **Deploy:** Push to main branch triggers deployment

### Firmware (FieldLink)
- **Repo:** https://github.com/Voltageza/fieldlink
- **CI/CD:** GitHub Actions builds firmware on push/tag

---

## How to Resume

**Reference:** See `SYSTEM_ARCHITECTURE.txt` for full system documentation.

Firmware v2.10.0 is now running. Continue testing:

1. **Test fault delays:**
   - Set delay via portal Config modal (e.g., 5 seconds)
   - Trigger overcurrent or dryrun condition
   - Verify fault doesn't trip until delay expires

2. **Test delay persistence:**
   - Set delays, reboot device, verify delays retained

3. **Test on live pump:**
   - Contactor badge: START → PENDING (cyan) → CONFIRMED (green, if DI4 wired) → OFF on STOP
   - Startup animation: should progress through all 4 steps on real pump start
   - Note: Bench test triggers start failure timeout after 10s (no current detected). Set `BENCH_TEST_MODE = true` in main.cpp to disable for bench testing.

4. **MQTT check script:**
   ```bash
   python "Fieldlink/mqtt_check.py"
   ```

---

## Quick Commands

**Trigger manual build:**
GitHub Actions > Build Firmware > Run workflow

**Check device firmware version:**
Look at telemetry in portal - shows `firmware_version` field

**Flash via MQTT (using Supabase URL):**
Topic: `fieldlink/<DEVICE_ID>/command`
Payload:
```json
{"command": "UPDATE_FIRMWARE", "url": "https://einfhsixzxfnbppzydcn.supabase.co/storage/v1/object/public/firmware-releases/PUMP_ESP32S3/v2.10.0.bin"}
```

**Change admin password via SQL:**
```sql
UPDATE portal_settings SET admin_password = 'newpassword' WHERE id = 1;
```

---

## Session Log: 2026-02-09

### Portal Bug Fixes & Improvements (fieldlogic-portal)

**Commits pushed to `Voltageza/fieldlogic-portal`:**

1. `9f516e6` - **fix: Contactor badge uses actual DO state**
   - Badge was tracking `data.cmd` (software flag) which goes false on fault timeout
   - Now reads DO1 bit from telemetry `do_state` field (bit 0, active-low)

2. `6ff68b8` - **fix: Startup animation uses DO state**
   - Same issue as contactor badge — animation steps used `data.cmd`
   - Updated to use actual DO output state

3. `b46cd08` - **fix: 5s grace period on startup animation**
   - Race condition: stale telemetry (sent before device processed START) was hiding animation
   - Added `startupAnimationTime` with 5-second grace period before allowing hide

4. `6b1688f` - **feat: Admin password on protection toggles** (superseded)

5. `353671d` - **fix: Protection settings use Save button pattern**
   - Overcurrent/Dry Run protection toggles now have a "Save Protection" button
   - Requires admin password, matching the threshold and delay settings pattern
   - Shows success/error status message after save

**Key lesson:** Portal UI elements tracking `data.cmd` are unreliable because `startCommand` gets cleared by firmware fault timeouts. Use `data.do` (actual hardware DO state) for accurate representation.

---

## Future Consideration: Azure Hosting (100+ Devices)

**Date:** 2026-02-03

For scaling to 100+ devices, Azure VM + PostgreSQL is the recommended approach.

### Recommended Azure Architecture

```
100 ESP32 Devices
       │
       ▼ (MQTT - unchanged)
┌──────────────────┐
│ Azure VM (B1s)   │  $8/month
│ - EMQX Broker    │
│ - Nginx (portal) │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│ PostgreSQL       │  $15/month
│ Flexible Server  │
└──────────────────┘
┌──────────────────┐
│ Blob Storage     │  $1/month
│ (Firmware OTA)   │
└──────────────────┘

Total: ~$25/month for 100 devices
```

### Why Not Azure IoT Hub?

| Telemetry Rate | Messages/Day (100 devices) | IoT Hub Tier | Cost |
|----------------|---------------------------|--------------|------|
| 1 msg / 2 sec (current) | 4,320,000 | S2 | $250/mo |
| 1 msg / 30 sec | 288,000 | S1 | $25/mo |

Current firmware sends every 2 seconds - too expensive for IoT Hub pricing.
Self-hosted EMQX on Azure VM avoids per-message costs.

### Migration Path

1. Spin up Azure VM (B1s - $8/mo)
2. Install EMQX (open source MQTT broker)
3. Install Nginx for portal hosting
4. Create PostgreSQL Flexible Server ($15/mo)
5. Create Blob Storage container for firmware
6. Update firmware `secrets.h` with new MQTT host
7. Migrate Supabase data to PostgreSQL
8. Update portal config to new endpoints

**Estimated migration effort:** 4-8 hours
**No firmware logic changes needed** - same MQTT protocol

### Cost Comparison at Scale

| Devices | Current Stack | Azure (VM + PostgreSQL) |
|---------|---------------|------------------------|
| 50 | $0-25 | $25 |
| 100 | $25-40 | $25 |
| 200 | $40+ | $25-35 |
| 500 | Custom pricing | $35-50 |

Azure becomes more cost-effective at higher device counts.

---

## Future Consideration: Arduino Opta — NOT RECOMMENDED (for now)

**Initial evaluation:** 2026-01-30
**Full assessment:** 2026-02-10
**Verdict:** Not recommended for FieldLink pump control. Revisit late 2026.

### Assessment Summary

Explored Arduino Opta as an alternative industrial platform. After thorough research into stock availability, durability, and software maturity, the conclusion is to **stick with ESP32-S3** for now.

### Stock Availability — Good
- All 3 variants **in stock at RS Components South Africa**:
  - Opta Lite: R2,742.71 (excl. VAT)
  - Opta RS485: R3,003.79
  - Opta WiFi: R3,609.17
- Also available at Mouser, DigiKey, Arduino Store internationally
- No supply chain or lead time issues

### Durability — Adequate but concerns
- IP20 (indoor panel-mount only, same as most DIN-rail PLCs)
- CE and cULus certified, -20°C to +50°C, DIN rail mount
- 87-year MTBF claimed, **BUT:**
- **Documented crashes after months of continuous operation** — users running Modbus TCP polling report complete lockups every ~4 months. Suspected memory leak, no fix from Arduino
- Network issues can block local process execution — dangerous for pump control

### Software Development — Dealbreaker issues

1. **PLC IDE is not production-ready** — constant crashes, connection failures, upload anomalies confirmed across 5-7 devices. Systemic issue, not isolated hardware defects
2. **Mbed OS end-of-life July 2026** — Arduino transitioning to Zephyr RTOS, but replacement core still in beta (v0.3.2 as of Aug 2025). Deploying mid-RTOS transition is risky
3. **Arduino C++ is the only viable programming option** — which we can already do on ESP32-S3 with a more mature ecosystem

### Why not for FieldLink
- Gives up a working ESP32-S3 platform for: a nicer enclosure and 4 built-in relays
- Takes on: Mbed EOL risk, documented crash issues, fewer I/O points (4 DO vs our 8)
- ~40% code migration effort for arguably a worse outcome

### Better Alternatives

| Platform | Why |
|----------|-----|
| **Industrial Shields ESP32 PLC** | Same ESP32 chip family, DIN-rail, industrial I/O protection, PlatformIO, code ports almost directly. ~$150-500 |
| **Current Waveshare + proper enclosure** | Cheapest path. Buy DIN-rail enclosure + terminal blocks for a proven working system |
| **Siemens S7-1200** | If a client demands "real PLC" credentials. Overkill but bulletproof |

### Recommendation
**Stick with ESP32-S3.** If industrial credibility is needed, look at Industrial Shields ESP32 PLC — same chip, minimal code changes, proper industrial housing without Opta's software risks. Revisit Opta in late 2026 after Zephyr transition is complete and stability is proven.

### Links
- Product page: https://www.arduino.cc/pro/hardware-arduino-opta/
- Arduino Forum - crash reports: https://forum.arduino.cc/t/arduino-opta-crashes-after-a-few-months/1413454
- Mbed EOL announcement: https://os.mbed.com/blog/entry/Important-Update-on-Mbed/
- Zephyr transition: https://blog.arduino.cc/2024/07/24/the-end-of-mbed-marks-a-new-beginning-for-arduino/
- Industrial Shields ESP32 PLC: https://www.industrialshields.com/industrial-esp32-plc-products-family-ideal-for-iot-solutions

---

## Hardware Sourcing — South Africa

**Date:** 2026-02-10
**Problem:** Waveshare ESP32-S3-POE-ETH-8DI-8DO is hard to source locally in SA.

### Locally in stock (expensive)

| Board | Supplier | Price (inc VAT) | Notes |
|-------|----------|-----------------|-------|
| Industrial Shields ESP32 PLC 21 | RS Components SA | ~R8,800 | ESP32, 13 DI / 8 DO, RS485, Ethernet, DIN rail. 5 in stock |
| Controllino MEGA | Communica (Centurion) | R9,456 | ATmega2560 — NOT ESP32, firmware rewrite needed. Not recommended |

### Import from AliExpress / Waveshare direct (best value)

| Board | Price (USD) | Shipping | Total (ZAR est.) | Firmware impact |
|-------|-------------|----------|-------------------|-----------------|
| **ESP32-S3-POE-ETH-8DI-8DO** (current board) | ~$32 | $8-12, 15-40 days | ~R750-R900 | None — exact same board |
| **ESP32-S3-ETH-8DI-8RO** (relay version) | ~$45 | $8-12, 15-40 days | ~R850-R1,050 | Minor pin remap only |

The **8DI-8RO relay variant** is worth considering — same ESP32-S3, same W5500 Ethernet, RS485, but with built-in 10A/250VAC relays instead of Darlington transistor outputs. Eliminates external relay boards for contactor switching.

### Local Waveshare reseller
- **Micro Robotics (robotics.org.za)** in Pretoria stocks some Waveshare ESP32 boards
  - Confirmed: ESP32-S3 8-Channel I/O Controller (product code W32108)
  - Phone: 012 665 5713 — worth calling to check 8DI-8DO/8RO stock or ordering
  - Website: https://www.robotics.org.za/W32108

### Other boards researched (not suitable)

| Board | Why not |
|-------|---------|
| M5Stack StamPLC ($43) | Only 4 relay outputs, no Ethernet |
| DFRobot Edge101 ($40) | Only 11 GPIOs, no dedicated industrial I/O |
| EBYTE ECM50-A ($35) | Only 2 DI + 2 DO — far too few |
| NORVI ESP32 ($54-134) | No SA distributor, import from Sri Lanka |
| LILYGO T-ETH-Elite | Dev board only, no industrial I/O or enclosure |

### Import duties to SA
- Electronic dev boards/modules: typically **0-10%** ad valorem
- VAT: **15%** on CIF value (cost + insurance + freight)
- Shipments under **R500** may avoid customs processing
- Amazon Global collects duties at checkout; AliExpress does not (pay on delivery)

### Recommendation
**Order 3-5 Waveshare boards from AliExpress** (~R3,000-R5,000 total). Per-unit cost is a fraction of "industrial" alternatives, firmware runs without changes, and having spares on hand solves the availability problem. For industrial presentation, buy a proper DIN-rail ABS enclosure with cable glands from RS Components SA locally.
