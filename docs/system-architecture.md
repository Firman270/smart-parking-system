# System Architecture

## Overview

The Smart Parking System uses a distributed multi-node IoT architecture where each parking block has its own monitoring node. All nodes report to a central controller that handles recommendation logic, display output, barrier control, and cloud telemetry.

---

## Component Roles

### ESP8266 Nodes (A, B, C)

Each node is responsible for one parking block.

**Responsibilities:**
- Read 3 IR obstacle sensors (one per slot)
- Determine slot status: `TERISI` (occupied) or `KOSONG` (empty)
- Calculate available slot count
- Determine block status: `TERSEDIA` (available) or `PENUH` (full)
- Publish status to local Mosquitto broker every 5 seconds
- Publish telemetry to ThingsBoard Cloud (MQTT or HTTP depending on experiment mode)

**Sensor logic:**
```
IR sensor output LOW  → slot OCCUPIED (vehicle present)
IR sensor output HIGH → slot EMPTY
```

**Block status logic:**
```
If (slot1 == EMPTY OR slot2 == EMPTY OR slot3 == EMPTY) → AVAILABLE
Else → FULL
```

---

### ESP32 Central Controller

The brain of the system. Aggregates all node data and drives outputs.

**Responsibilities:**
- Subscribe to all node topics via local MQTT
- Read entry gate IR sensor for vehicle detection
- Run block recommendation algorithm
- Control entry servo (automatic, based on slot availability)
- Control exit servo (manual, staff button press-and-hold)
- Drive LCD 16x2 with overall slot count
- Drive TFT IPS with animated block map and navigation
- Forward aggregated telemetry to ThingsBoard every 5 seconds
- Send Telegram notifications on status changes
- Detect stale nodes (no data for 15+ seconds → mark as UNKNOWN)

---

## Data Flow

```
IR Sensor (slot)
      ↓
ESP8266 Node
      ↓ JSON payload
Local Mosquitto MQTT Broker (laptop)
      ↓ subscribed topics
ESP32 Central Controller
      ├─→ Recommendation logic
      ├─→ Servo control (entry + exit)
      ├─→ LCD update
      ├─→ TFT update
      ├─→ ThingsBoard Cloud (MQTT/HTTPS)
      └─→ Telegram Bot (HTTPS)
```

---

## MQTT Topics

| Topic | Publisher | Subscriber | Payload |
|---|---|---|---|
| `parkir/nodeA/status` | Node A | Central | JSON |
| `parkir/nodeB/status` | Node B | Central | JSON |
| `parkir/nodeC/status` | Node C | Central | JSON |
| `v1/devices/me/telemetry` | All devices | ThingsBoard | JSON |

### Node Payload Example

```json
{
  "node_id": "A",
  "slot1": "KOSONG",
  "slot2": "TERISI",
  "slot3": "KOSONG",
  "slot_kosong": 2,
  "blok": "TERSEDIA",
  "local_seq": 47
}
```

### Central Telemetry Payload Example

```json
{
  "total_slot_kosong": 5,
  "status_blok_A": "TERSEDIA",
  "status_blok_B": "PENUH",
  "status_blok_C": "TERSEDIA",
  "slot_kosong_A": 2,
  "slot_kosong_B": 0,
  "slot_kosong_C": 3,
  "kendaraan": "TIDAK_ADA",
  "palang_masuk": "TERTUTUP",
  "palang_keluar": "TERTUTUP",
  "rekomendasi": "Blok A & Blok C",
  "run_id": "P0_MQTT_R1",
  "seq": 42,
  "send_timestamp": 1731234567890,
  "protokol": "MQTT",
  "phase": "MEASURE"
}
```

---

## Recommendation Algorithm

```
priority = [A, B, C, D]
recommended = []

for each block in priority:
    if block.status == AVAILABLE:
        recommended.append(block)
    if len(recommended) == 2:
        break

if len(recommended) == 0:
    display "PARKING FULL"
    keep barrier CLOSED
else:
    display recommended blocks (max 2)
    allow barrier to OPEN when vehicle detected
```

---

## Barrier Control

### Entry Barrier (automatic)

```
Vehicle detected (ADA) AND slots available → OPEN (servo 90°)
Vehicle gone (TIDAK_ADA) → CLOSE (servo 0°)
All slots full → CLOSE regardless of vehicle detection
```

### Exit Barrier (manual)

```
Staff presses button → OPEN (servo 90°) while held
Staff releases button → CLOSE (servo 0°)
```

---

## Stale Node Detection

If a node stops publishing for more than 15 seconds, the central controller marks it as `UNKNOWN` and displays it in yellow on the TFT. This prevents stale data from influencing recommendations.

---

## WiFi Configuration

All devices use WiFiManager (tzapu). On first boot or after reset, the device opens a configuration portal:

- **ESP8266 nodes:** portal name `QoS-Node-A/B/C`
- **ESP32 central:** portal name `QoS-SistemPusat`

Configuration is persisted:
- ESP8266 → EEPROM (stores Mosquitto broker IP)
- ESP32 → Preferences (stores Mosquitto broker IP)

Credentials are never hardcoded in firmware.
