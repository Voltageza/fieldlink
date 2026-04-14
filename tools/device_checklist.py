#!/usr/bin/env python3
"""
FieldLink Device Debug Checklist
=================================
Connects to HiveMQ MQTT broker, sends GET_SETTINGS to a device,
and prints a full status report.

Usage:
    python device_checklist.py                  # defaults to FL-22F968
    python device_checklist.py FL-CC8CA0        # specify device ID
    python device_checklist.py --all            # check all known devices
"""

import sys
import ssl
import time
import json
import paho.mqtt.client as mqtt

# --- Configuration ---
MQTT_HOST = 'a5c598acdbdc4abba053799bcefb73d0.s1.eu.hivemq.cloud'
MQTT_PORT = 8883
MQTT_USER = 'fieldlogicuser1'
MQTT_PASS = 'FL_mqTT#2026!xKp9'

KNOWN_DEVICES = ['FL-22F968', 'FL-CC8CA0']
TIMEOUT_S = 12

OK = 'PASS'
FAIL = 'FAIL'
WARN = 'WARN'


def check_device(device_id):
    """Run full checklist for a single device."""
    results = {}
    start_time = None
    done = False

    def on_connect(client, userdata, flags, rc):
        nonlocal start_time
        if rc != 0:
            print(f'  MQTT connect failed: rc={rc}')
            return
        client.subscribe(f'fieldlink/{device_id}/#')

    def on_subscribe(client, userdata, mid, granted_qos):
        nonlocal start_time
        start_time = time.time()
        client.publish(
            f'fieldlink/{device_id}/command',
            json.dumps({'command': 'GET_SETTINGS'})
        )

    def on_message(client, userdata, msg):
        nonlocal done
        try:
            data = json.loads(msg.payload)
            if data.get('type') == 'settings' and 'settings' not in results:
                results['settings'] = data
                results['settings_time'] = time.time() - start_time
            elif 'V1' in data and 'telemetry' not in results:
                results['telemetry'] = data
            # Once we have both, we're done
            if 'settings' in results and 'telemetry' in results:
                done = True
                client.disconnect()
        except (json.JSONDecodeError, ValueError):
            if 'status' not in results:
                results['status'] = msg.payload.decode().strip()

    c = mqtt.Client()
    c.username_pw_set(MQTT_USER, MQTT_PASS)
    c.tls_set(cert_reqs=ssl.CERT_REQUIRED)
    c.on_connect = on_connect
    c.on_subscribe = on_subscribe
    c.on_message = on_message

    try:
        c.connect(MQTT_HOST, MQTT_PORT)
    except Exception as e:
        print(f'  Connection failed: {e}')
        return False

    c.loop_start()
    deadline = time.time() + TIMEOUT_S
    while not done and time.time() < deadline:
        time.sleep(0.1)
    c.loop_stop()
    c.disconnect()

    # --- Print Report ---
    t = results.get('telemetry', {})
    s = results.get('settings', {})
    st = results.get('settings_time')

    print('============================================')
    print(f'  {device_id} DEBUG CHECKLIST')
    print('============================================')
    print()

    # MQTT & Connectivity
    print('--- MQTT & CONNECTIVITY ---')
    lwt = results.get('status', 'none')
    print(f'  LWT status online:          {OK if lwt == "online" else FAIL} ({lwt})')
    print(f'  Telemetry flowing:          {OK if t else FAIL}')
    if st is not None:
        status = OK if st < 3 else WARN
        print(f'  GET_SETTINGS response:      {status} ({st:.1f}s)')
    else:
        print(f'  GET_SETTINGS response:      {FAIL} (no response)')
    print()

    if not t and not s:
        print('  >> Device appears OFFLINE - no data received')
        print('============================================')
        print()
        return False

    # Sensor Data
    print('--- SENSOR DATA ---')
    sensor = t.get('sensor')
    if sensor:
        print(f'  Sensor online:              {OK} (True)')
    else:
        print(f'  Sensor online:              EXPECTED OFFLINE ({sensor})')
    print(f'  Voltages (V1/V2/V3):        {t.get("V1","?")} / {t.get("V2","?")} / {t.get("V3","?")}')
    print(f'  Currents (I1/I2/I3):        {t.get("I1","?")} / {t.get("I2","?")} / {t.get("I3","?")}')
    # Stale data check: if sensor offline, voltages should be 0
    if not sensor and (t.get('V1', 0) != 0 or t.get('V2', 0) != 0 or t.get('V3', 0) != 0):
        print(f'  Stale data check:           {FAIL} - sensor offline but V != 0')
    else:
        print(f'  Stale data check:           {OK}')
    print()

    # Pump States
    print('--- PUMP STATES ---')
    num_pumps = 3 if t.get('hardware_type') == 'EVE_ESP32S3' else 1
    for i in range(1, num_pumps + 1):
        state = t.get(f's{i}', '?')
        fault = t.get(f'f{i}', 'NONE')
        cmd = t.get(f'c{i}', '?')
        cf = t.get(f'cf{i}', '?')
        print(f'  Pump {i}:  state={state:<10s} fault={fault:<15s} cmd={cmd}  contactor={cf}')
    print()

    # Settings (per-pump)
    if s:
        print('--- SETTINGS (per-pump) ---')
        for i in range(1, num_pumps + 1):
            p = s.get(f'p{i}', {})
            if p:
                print(f'  Pump {i}:  oc_en={str(p.get("overcurrent_enabled","?")):<5s} '
                      f'dry_en={str(p.get("dryrun_enabled","?")):<5s} '
                      f'max_I={str(p.get("max_current","?")):<5s} '
                      f'dry_I={str(p.get("dry_current","?")):<5s} '
                      f'sch_en={str(p.get("sch_en","?")):<5s} '
                      f'{p.get("sch_sH",0):02d}:{p.get("sch_sM",0):02d}-'
                      f'{p.get("sch_eH",0):02d}:{p.get("sch_eM",0):02d}')
            else:
                print(f'  Pump {i}:  no settings data')
        print(f'  Ruraflex:                   {s.get("ruraflex_enabled", "?")}')
        print(f'  Device time:                {s.get("current_time", "?")}')
        print()

    # Firmware Info
    print('--- FIRMWARE ---')
    print(f'  Version:                    {t.get("firmware_version", "?")}')
    print(f'  Hardware type:              {t.get("hardware_type", "?")}')
    print(f'  Network:                    {t.get("network", "?")}')
    uptime = t.get('uptime', 0)
    h = uptime // 3600
    m = (uptime % 3600) // 60
    sec = uptime % 60
    print(f'  Uptime:                     {h:02d}:{m:02d}:{sec:02d}')
    print(f'  DI status:                  0x{t.get("di", 0):02X}')
    print(f'  DO status:                  0x{t.get("do", 0):02X}')
    print()

    # Warnings
    warnings = []
    if st and st > 3:
        warnings.append(f'GET_SETTINGS slow ({st:.1f}s) - check Modbus timeout or loop blocking')
    if not sensor and t.get('V1', 0) != 0:
        warnings.append('Stale voltage data - sensor offline but voltages non-zero')
    for i in range(1, num_pumps + 1):
        state = t.get(f's{i}', '')
        fault = t.get(f'f{i}', '')
        if state == 'FAULT' and fault == 'DRY_RUN':
            warnings.append(f'Pump {i} in DRY_RUN fault - was it started with no load?')
        if state == 'FAULT' and fault == 'OVERCURRENT':
            warnings.append(f'Pump {i} OVERCURRENT fault - check wiring/load')

    if warnings:
        print('--- WARNINGS ---')
        for w in warnings:
            print(f'  ! {w}')
        print()

    print('============================================')
    print()
    return True


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--all':
        devices = KNOWN_DEVICES
    elif len(sys.argv) > 1:
        devices = [sys.argv[1]]
    else:
        devices = ['FL-22F968']

    print()
    print(f'FieldLink Device Checklist - {time.strftime("%Y-%m-%d %H:%M:%S")}')
    print(f'Broker: {MQTT_HOST}:{MQTT_PORT}')
    print(f'Devices: {", ".join(devices)}')
    print()

    for device_id in devices:
        check_device(device_id)


if __name__ == '__main__':
    main()
