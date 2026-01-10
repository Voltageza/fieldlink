# FieldLink ESP32-S3 Pump Controller

Industrial-grade pump controller with local/remote control, cloud connectivity, and fault protection.

## Features

- **Local/Remote Mode Selector** - Physical switch to enable/disable remote control
- **Manual Controls** - Industrial standard START (NO) and STOP (NC) buttons
- **Cloud MQTT** - HiveMQ cloud with TLS encryption
- **Real-time Dashboard** - Web-based monitoring at [fieldlink-dashboard](https://voltageza.github.io/fieldlink-dashboard/)
- **Fault Protection** - DRY_RUN detection with automatic shutdown
- **Indicator Outputs** - RUN and FAULT status LEDs
- **Modbus RS485** - Current sensing (Ia, Ib, Ic)

## Hardware

**Board:** [Waveshare ESP32-S3-POE-ETH-8DI-8DO](https://www.waveshare.com/wiki/ESP32-S3-POE-ETH-8DI-8DO)

- ESP32-S3 with WiFi/BLE
- 8 optocoupler-isolated digital inputs
- 8 Darlington sink digital outputs (via TCA9554 I2C)
- RS485 interface for Modbus
- POE or USB powered

## Wiring

### Digital Inputs (active when connected to GND)

| Terminal | Function | Type | Description |
|----------|----------|------|-------------|
| DI1 | START | NO | Momentary push button |
| DI2 | STOP | NC | Emergency stop (fail-safe) |
| DI3 | MODE | Switch | GND = LOCAL, Open = REMOTE |
| DI4-DI8 | Reserved | - | Future use |

### Digital Outputs (active-low, sink to GND)

| Terminal | Function | Description |
|----------|----------|-------------|
| DO1 | Contactor | Main pump contactor relay |
| DO2 | RUN LED | ON when pump is running (current detected) |
| DO3 | FAULT LED | ON when in fault state |
| DO4-DO8 | Reserved | Future use |

### Output Wiring (External Relay)

```
External 5-24V ──► COM terminal
DO1 ──────────────► Relay IN
GND ──────────────► Relay GND
```

## Operating Modes

### LOCAL Mode (DI3 → GND)
- MQTT commands blocked
- Manual START/STOP buttons active
- Dashboard shows mode as "LOCAL"

### REMOTE Mode (DI3 open)
- MQTT commands accepted (START, STOP, RESET)
- Manual buttons still work
- Dashboard shows mode as "REMOTE"

**Note:** STOP button always works in both modes (safety feature).

## Building & Flashing

### Prerequisites
- [PlatformIO](https://platformio.org/)
- USB cable connected to ESP32-S3

### Build & Upload

```bash
cd "Main Code/FieldLink_Main_Code"
pio run -t upload
```

### Serial Monitor

```bash
pio device monitor
```

## WiFi Setup

On first boot (or after reset):
1. Connect to WiFi network: `FieldLink-XXXXXX`
2. Open http://192.168.4.1
3. Select your WiFi network and enter password
4. Device will reboot and connect

## Cloud Dashboard

Access the real-time dashboard at:
**https://voltageza.github.io/fieldlink-dashboard/**

Enter your Device ID (shown on serial monitor at boot, e.g., `FL-22F968`)

### Dashboard Features
- Live current readings (Ia, Ib, Ic)
- Pump state indicator (RUNNING/STOPPED/FAULT)
- Mode indicator (LOCAL/REMOTE)
- START/STOP/RESET controls
- Connection status

## MQTT Topics

| Topic | Direction | Payload |
|-------|-----------|---------|
| `fieldlink/{device_id}/telemetry` | Subscribe | JSON telemetry data |
| `fieldlink/{device_id}/command` | Publish | `START`, `STOP`, `RESET` |

### Telemetry JSON

```json
{
  "Ia": 0.00,
  "Ib": 0.00,
  "Ic": 0.00,
  "state": "STOPPED",
  "mode": "REMOTE",
  "cmd": false,
  "sensor": true,
  "uptime": 12345,
  "di": 0,
  "do": 255
}
```

## Fault Protection

### DRY_RUN Fault
- Triggers if contactor ON but no current detected for 10 seconds
- Automatically shuts off contactor
- Requires manual RESET command to clear

## Serial Commands

Type these in serial monitor:
- `HELP` - Show available commands
- `STATUS` - Show current state
- `START` - Start pump (if in REMOTE mode)
- `STOP` - Stop pump
- `RESET` - Clear fault

## License

MIT License - See LICENSE file for details.
