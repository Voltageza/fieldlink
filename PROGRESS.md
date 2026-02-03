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
- [x] Admin password change UI (Settings page, admin-only)

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
- [ ] Test contactor badge shows in portal (CONFIRMED/PENDING/OFF)
- [ ] Test fault delays work correctly (set delay, trigger fault)
- [ ] Test startup animation progression
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

2. **Test contactor badge in portal:**
   - Start pump, verify badge shows CONFIRMED (green)
   - Stop pump, verify badge shows OFF

3. **Test startup animation:**
   - Refresh portal page, watch for 4-step animation

4. **Test delay persistence:**
   - Set delays, reboot device, verify delays retained

5. **MQTT check script:**
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

## Future Consideration: Arduino Opta

**Date:** 2026-01-30

Exploring Arduino Opta as an alternative platform for future projects. Benefits: industrial-grade, DIN-rail mount, IP20 rated, long-term Arduino support.

### Opta Models
| Model | Connectivity | Price |
|-------|--------------|-------|
| Opta Lite | Ethernet only | ~$90 |
| Opta RS485 | Ethernet + RS485 | ~$110 |
| Opta WiFi | Ethernet + WiFi + BLE + RS485 | ~$140 |

**Recommended:** Opta WiFi or Opta RS485 (Modbus support needed)

### Migration Compatibility
| Component | Effort |
|-----------|--------|
| Core logic (state machine, faults) | ✅ Direct transfer |
| ArduinoJson, PubSubClient, ModbusMaster | ✅ Compatible |
| Digital Inputs (8) | ✅ Built-in |
| Digital Outputs | ⚠️ Only 4 relays (need expansion for more) |
| ESPAsyncWebServer | ❌ Rewrite needed |
| WiFiManager | ❌ Rewrite needed |
| TLS/SSL | ⚠️ Different library |
| OTA Updates | ⚠️ Different mechanism |
| NVS/Preferences | ⚠️ Use EEPROM/SD instead |

**Estimated migration effort:** ~40% code changes

### Future-Proofing Suggestion
Consider adding hardware abstraction layer to current codebase:
```cpp
// hardware.h - abstract interface
class IHardware {
  virtual void setOutput(uint8_t channel, bool state) = 0;
  virtual bool getInput(uint8_t channel) = 0;
};
// Implement: hardware_esp32.cpp, hardware_opta.cpp
```

### Expansion Options
- **Opta Ext D1608E** - 16 DI + 8 DO (daisy-chain up to 5)

### Links
- Product page: https://www.arduino.cc/pro/hardware-arduino-opta/
- PlatformIO supports Opta (Arduino Mbed OS core)
