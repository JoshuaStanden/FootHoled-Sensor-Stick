# XIAO Build — Firmware

Firmware for the XIAO C5 + C3 sensor stick ("new thinking" build). Two sketches,
one per board, mirroring the proven two-MCU split from the Heltec design.

> **Status:** Written against verified datasheet pinouts; **not yet compiled or
> flashed on physical hardware.** Treat as a first bringup draft. The bench/SDK
> open items in `docs/xiao/ARCHITECTURE.md` still apply (5 GHz channel-set in
> promiscuous mode; mechanical pad access under the LoRa hat).

## Sketches

| Folder | Board | Role |
|---|---|---|
| [`xiao-c5-workhorse/`](../../xiao-c5-workhorse/) | XIAO ESP32-C5 | Wi-Fi sniff (2.4 + 5 GHz) + SD logging + LoRa handoff + BLE handoff |
| [`xiao-c3-ble/`](../../xiao-c3-ble/) | XIAO ESP32-C3 | BLE scanner → UART to the C5 |

## Arduino IDE setup

- **Core:** esp32 by Espressif (use a recent version — the C5 is new; 5 GHz
  promiscuous channel-set support depends on the ESP-IDF the core bundles).
- **Board targets:** `XIAO_ESP32C5` for the workhorse, `XIAO_ESP32C3` for the scanner.
- **Libraries (Library Manager):**
  - RadioLib (SX1262 driver) — C5 only
  - NimBLE-Arduino v2.x — both boards
  - LittleFS, SD, SPI — bundled with the esp32 core

## Wiring (summary — full table in `docs/xiao/WIRING.md`)

```
C5 (workhorse)                         C3 (BLE)
  Wio-SX1262 hat: plug-and-play          D6/GPIO21 TX ──► C5 D7/GPIO12 RX
  SD: SCK=D8 MISO=D9 MOSI=D10 CS=D0      D7/GPIO20 RX ◄── C5 D6/GPIO11 TX
      (shared SPI bus w/ LoRa)           GND ◄──────────► C5 GND (required)
  UART to C3: TX=D6/GPIO11 RX=D7/GPIO12
```

## Key differences from the Heltec firmware

1. **One shared SPI bus.** The C5 runs the LoRa hat and the SD card on the *same*
   SPI bus (LoRa CS = GPIO23/D4, SD CS = GPIO1/D0). The Heltec used two separate
   buses. Use a buffered/pull-up-free SD breakout so it doesn't fight the radio.
2. **Dual-band sniffing.** The C5 hops 2.4 GHz channels and, when the core
   supports it, a 5 GHz UNII set too (`ENABLE_5GHZ`). Set `ENABLE_5GHZ 0` to force
   2.4 GHz-only on an older core.
3. **No OLED.** Status prints to the serial console instead of a screen.
4. **Same handoff contract + BLE UUIDs + LoRa params** as the Heltec build, so an
   existing collector interoperates unchanged.

## Flashing

1. Open the `.ino`, select the matching board target, plug in over USB-C.
2. Upload. Open Serial Monitor at 115200 to watch `[STATUS]` lines.
3. Flash the C5 and C3 separately; then wire the UART link (TX↔RX crossed + GND).

## First-light checklist

- [ ] C5 boots, `[LoRa] init OK`, `[STORAGE] active=...`
- [ ] SD mounts on the shared bus (or clean fallback to onboard flash)
- [ ] `[STATUS]` shows WiFi count climbing; channel field cycles incl. `5G-` if 5 GHz works
- [ ] C3 prints `scanning...` and `BT,...` lines reach the C5 (`BT:` count rises)
- [ ] Set `LORA_FREQ` to your region before transmitting
