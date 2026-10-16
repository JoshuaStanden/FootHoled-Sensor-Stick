# FootHoled Sensor Stick — XIAO Build Bill of Materials

> **Status:** Research / design reference. Prices are approximate and for planning
> only; none of this is purchased or built yet.

## Core (the chips and radios)

| Part | Role | ~Cost |
|---|---|---|
| XIAO ESP32-C5 | Wi-Fi sniffer (2.4 + 5 GHz) + SD + LoRa workhorse | $6.90 |
| XIAO ESP32-C3 | BLE scanner (over UART to C5) — may already own | $5.00 |
| Wio-SX1262 for XIAO | LoRa hat (SX1262, 868/915 MHz, 22 dBm), plug-and-play on C5 | $4.99 |

## Must-have — it won't work / won't charge without these

| Part | Why | ~Cost |
|---|---|---|
| SD card reader (SPI, 3.3 V) | Local log storage on the C5 shared SPI bus. Adafruit #4682 (buffered, no naked bus pull-ups) preferred; a clean 3.3 V blue module works with the pull-ups removed | $2–7 |
| microSD card (≤32 GB, FAT32) | The reader is empty without one; tiny card is plenty for MAC/RSSI text logs | $5 |
| **Protected 18650 cell(s)** | The battery. Protected cells (over-discharge/short protection) for an unattended device | $5–10 |
| 18650 holder/clip | Holds the cell | $1–3 |
| **Dual-band 2.4/5 GHz U.FL antenna** | C5's stock antenna is 2.4 GHz-only — required to actually get 5 GHz capture, the whole reason for the C5. U.FL / IPEX connector | $3–6 |
| Hookup wire (30 AWG solid / Dupont) | SD module + C5↔C3 UART link | $5 |

## Strongly recommended — makes it real hardware, not a breadboard

| Part | Why | ~Cost |
|---|---|---|
| Power switch (slide/toggle) | Turn the sensor off without unplugging the cell | $1 |
| Enclosure / project box | It's a "stick" you carry/stash; 3D-printed or off-the-shelf | $0–10 |
| Perfboard / small protoboard | Mount both XIAOs + SD module + wiring solidly | $3 |

## Optional — depends on goals

| Part | Why you might want it | ~Cost |
|---|---|---|
| TP4056 LiPo charger | Both XIAOs have onboard charging, so likely NOT needed; dedicated module gives better control | $1 |
| Status LED / tiny I²C OLED | The Heltec had an OLED; XIAOs don't. Add on the C5's free I²C pins for on-device status | $3 |
| C3 U.FL antenna variant | Better BLE range if needed (C3 onboard antenna usually fine) | $3 |
| Schottky diode / power mgmt | Safe run-while-charging | $1 |

## Shortest "it works" shopping list

C5 · C3 · Wio-SX1262 · SD reader (Adafruit #4682) · microSD card ·
**protected 18650 + holder** · **dual-band U.FL antenna** · power switch ·
hookup wire · perfboard · enclosure.

**All-in estimate: ~$45–60** depending on enclosure and antenna — comfortably
under a comparable Heltec-based build once the separate BLE board and charging the
Heltec needed are counted.

## Two power notes to settle before ordering

1. **Powering two boards from one cell.** Feed the raw 18650 to both boards'
   battery inputs (both have onboard charge/regulation) with a **common ground**, or
   split a shared 3.3 V rail. Decide the scheme before wiring.
2. **Protected cells + holder.** A plain holder + unprotected cell in an unattended
   logger is the one safety corner not worth cutting. Protected cells cost ~$1 more
   and remove the over-discharge risk.

## Easy-to-forget essentials

- **Dual-band U.FL antenna** — without it the C5's 5 GHz is dead weight.
- **Protected 18650 cells** — safety for an unattended device.

## Memory-storage alternatives to the SD card (optional)

For a sealed logger that offloads over LoRa rather than ejecting a card, two
options are arguably cleaner than an SD slot:

- **C5 onboard 8 MB flash via LittleFS** — zero extra parts/pins, frees the entire
  SPI bus for LoRa alone (removes the shared-bus pull-up problem). ~3–5 MB usable
  for logs; not removable.
- **SPI NOR flash chip (e.g. W25Q128, 16 MB)** — same shared SPI bus as the SD
  (SCK/MISO/MOSI + own CS), no aggressive bus pull-ups, cheap, rugged; not removable.

The SD card only earns its place if you specifically want to pull the card and read
it on a PC — which the collector-over-LoRa handoff design was built to avoid.
