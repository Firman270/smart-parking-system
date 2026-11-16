# Pin Mapping Reference

## ESP8266 NodeMCU — Parking Block Node

| Component | NodeMCU Pin | GPIO |
|---|---|---|
| IR Sensor Slot 1 | D1 | GPIO5 |
| IR Sensor Slot 2 | D2 | GPIO4 |
| IR Sensor Slot 3 | D5 | GPIO14 |

> **Note:** D3 (GPIO0) is avoided — it is a boot-strapping pin and causes issues if pulled LOW during power-on.

---

## ESP32 (30-pin) — Central Controller

### TFT IPS ST7735 (SPI via VSPI)

| TFT Pin | Actual Function | ESP32 GPIO |
|---|---|---|
| GND | Ground | GND |
| VCC | Power | 3.3V |
| SCL | SPI Clock (SCK) | GPIO18 |
| SDA | SPI Data (MOSI) | GPIO23 |
| RES | Reset | GPIO4 |
| DC | Data/Command | GPIO2 |
| CS | Chip Select | GPIO5 |
| BLK | Backlight | 3.3V |

> The TFT labels `SCL` and `SDA` refer to SPI signals, not I2C.

### LCD 16x2 I2C

| LCD Pin | ESP32 GPIO |
|---|---|
| GND | GND |
| VCC | 5V |
| SDA | GPIO21 |
| SCL | GPIO22 |

### Actuators and Sensors

| Component | ESP32 GPIO | Notes |
|---|---|---|
| Servo masuk (entry) | GPIO25 | Signal wire only; power from 5V external |
| Servo keluar (exit) | GPIO26 | Signal wire only; power from 5V external |
| IR gate sensor | GPIO34 | Input-only pin; no internal pull-up |
| Push button keluar | GPIO33 | INPUT_PULLUP; button to GND |

> GPIO34 and GPIO35 on ESP32 are input-only — no internal pull-up available.
> GPIO33 supports INPUT_PULLUP and is preferred for the exit button.

### Power Notes

- Servo VCC must come from an **external 5V supply**, not from the ESP32 3.3V or 5V pin (insufficient current)
- **All GNDs must be connected together** (ESP32 GND + servo GND + external supply GND)
- TFT runs on 3.3V — do not connect VCC to 5V

---

## Wiring Diagram Notes

For the entry gate IR sensor connected to GPIO34:
```
IR Sensor OUT → GPIO34
IR Sensor VCC → 3.3V
IR Sensor GND → GND
```

For the exit push button on GPIO33:
```
One leg  → GPIO33
Other leg → GND
(INPUT_PULLUP active — no external resistor needed)
```

For servo power (external supply):
```
External 5V (+) → Servo VCC (red)
External 5V (−) → Servo GND (brown/black)
External 5V (−) → ESP32 GND  ← shared ground, required
ESP32 GPIO25/26 → Servo Signal (orange/yellow)
```
