# FootHoled Sensor Stick — XIAO Build Wiring Sheet (C5 + C3)

> **Status:** Research / design reference. Nothing here is built or verified on
> physical hardware yet. All GPIO numbers are verified against datasheets (see
> sources); the mechanical/bench items are flagged as "confirm at the bench".

## Architecture

Two-MCU split that mirrors the proven Heltec + XIAO-C3 design, but collapses the
Wi-Fi workhorse onto a single modern chip and adds 5 GHz capture:

- **XIAO ESP32-C5** — Wi-Fi sniffer (2.4 **and** 5 GHz, promiscuous mode) + SD
  logging + LoRa handoff. The workhorse; replaces the Heltec.
- **XIAO ESP32-C3** — BLE scanner only; streams results to the C5 over UART.
  Unchanged in role from the current build.
- **LoRa:** Wio-SX1262 for XIAO — plug-and-play hat on the C5.
- **SD:** generic 3.3 V SPI microSD module on the C5, sharing the C5 SPI bus with
  the LoRa radio (separate chip-selects).

Why one chip can't do both radios: on a single ESP32, Wi-Fi promiscuous mode and
BLE scanning both contend for the one 2.4 GHz radio, so time-slicing drops
packets. Running Wi-Fi full-time on the C5 and BLE full-time on the C3 is the
reason for the two-MCU split.

---

## Board 1 — XIAO ESP32-C5 (workhorse)

*GPIO numbers verified against the XIAO ESP32-C5 / ESP32-C5 Series Datasheet v1.5.*

| XIAO label | GPIO | Connects to | Purpose |
|---|---|---|---|
| D8 / SCK | **GPIO8** | LoRa SCK **+** SD SCK | Shared SPI clock |
| D9 / MISO | **GPIO9** | LoRa MISO **+** SD MISO | Shared SPI data (in) |
| D10 / MOSI | **GPIO10** | LoRa MOSI **+** SD MOSI | Shared SPI data (out) |
| D4 / SDA | **GPIO23** | Wio-SX1262 NSS | LoRa chip-select *(hat's own)* |
| D1 | **GPIO0** | Wio-SX1262 DIO1 | LoRa interrupt *(hat's own)* |
| D2 | **GPIO25** ⚠️strap | Wio-SX1262 NRST | LoRa reset *(hat drives it)* |
| D3 | **GPIO7** ⚠️strap | Wio-SX1262 BUSY | LoRa busy *(hat drives it)* |
| D5 / SCL | **GPIO24** | Wio-SX1262 RF_SW | LoRa RF switch *(hat's own)* |
| **D0** | **GPIO1** ✅safe | generic SD CS | SD chip-select |
| **D6 / TX** | **GPIO11** | → C3 RX (GPIO20) | UART to C3 |
| **D7 / RX** | **GPIO12** | ← C3 TX (GPIO21) | UART from C3 |
| 3V3 | — | LoRa VCC, SD VCC | 3.3 V rail |
| GND | — | LoRa GND, SD GND, **C3 GND** | Common ground |

The **Wio-SX1262 hat** plugs straight onto the C5 headers — you do not hand-wire
its rows. The table documents which XIAO pins it consumes (D1–D5 + the SPI bus).
You only hand-wire the **SD module** and the **UART-to-C3** link.

### SD module → C5 (solder to pass-through pads)

```
SD VCC  -> 3V3        SD SCK  -> D8  (GPIO8)   [shared w/ LoRa]
SD GND  -> GND        SD MISO -> D9  (GPIO9)   [shared w/ LoRa]
SD CS   -> D0 (GPIO1) SD MOSI -> D10 (GPIO10)  [shared w/ LoRa]
```

---

## Board 2 — XIAO ESP32-C3 (BLE scanner)

*GPIO numbers verified against the XIAO ESP32-C3 Datasheet v2.4.*

| XIAO label | GPIO | Connects to | Purpose |
|---|---|---|---|
| **D6 / TX** | **GPIO21** | → C5 RX / D7 (GPIO12) | UART to C5 |
| **D7 / RX** | **GPIO20** | ← C5 TX / D6 (GPIO11) | UART from C5 |
| GND | — | **C5 GND** | Common ground |
| 3V3 or BAT | — | power source | own battery or shared rail |

The C3 does nothing but scan BLE and talk UART, so only **3 wires** leave it:
TX, RX, GND.

---

## Inter-board link (the only wires between the two XIAOs)

```
   C5 D6/TX (GPIO11) ───────────────►  C3 D7/RX (GPIO20)
   C5 D7/RX (GPIO12) ◄───────────────  C3 D6/TX (GPIO21)
   C5 GND            ─────────────────  C3 GND          (mandatory common ground)
```

**TX↔RX crosses over** — C5 TX to C3 RX, C5 RX to C3 TX. Three wires total.

---

## Build notes (the things that bite people)

1. **SD + LoRa share the C5 SPI bus (GPIO8/9/10).** In firmware, two separate SPI
   device objects, each with its own CS held idle-HIGH while the other transacts:
   **LoRa CS = GPIO23 (D4)**, **SD CS = GPIO1 (D0)**.
2. **Disable the SD breakout's bus pull-ups** on SCK/MISO/MOSI (cut the jumper, or
   pick a module without them). This is the documented Seeed conflict where SD-side
   bus pull-ups disturb the LoRa radio on the shared bus. The SD's CS pull-up is
   fine; it's the three *bus* lines that matter. A buffered breakout
   (e.g. Adafruit #4682) avoids this entirely.
3. **C5 strapping pins (D2=GPIO25, D3=GPIO7)** are used by the LoRa hat and *driven
   by the hat*, so they're not floating at boot — fine. Don't repurpose them.
4. **C3 side** — only using D6/D7 (native UART pins), which carry no boot duty for
   UART use. No issue.
5. **3.3 V only** on the SD module — never feed it 5 V from a XIAO.
6. **Common ground is mandatory** for the UART to work; power rails can be separate.

---

## Firmware pin `#define` summary

```c
// ---- XIAO ESP32-C5 (workhorse) ----
// Shared SPI bus (LoRa + SD)
#define PIN_SPI_SCK   8    // D8
#define PIN_SPI_MISO  9    // D9
#define PIN_SPI_MOSI  10   // D10
// LoRa (Wio-SX1262 hat) — hat's own mapping
#define PIN_LORA_NSS  23   // D4
#define PIN_LORA_DIO1 0    // D1
#define PIN_LORA_NRST 25   // D2  (strapping, driven by hat)
#define PIN_LORA_BUSY 7    // D3  (strapping, driven by hat)
#define PIN_LORA_RFSW 24   // D5
// SD card
#define PIN_SD_CS     1    // D0  (verified safe, non-strapping)
// UART to C3
#define PIN_C5_TX     11   // D6
#define PIN_C5_RX     12   // D7

// ---- XIAO ESP32-C3 (BLE scanner) ----
#define PIN_C3_TX     21   // D6  -> C5 RX
#define PIN_C3_RX     20   // D7  <- C5 TX
```

---

## Still to confirm at the bench (not resolvable from a datasheet)

- The Wio-SX1262's physical pass-through leaves the **D0 pad and the SPI pads
  reachable** for the SD solder joints once the hat is seated — a mechanical check.
- The current **ESP-IDF build exposes 5 GHz channel-selection in promiscuous mode**
  (hardware supports it; this is an SDK-maturity question on a newish chip).

## Sources

- XIAO ESP32-C5 / ESP32-C5 Series Datasheet v1.5 (promiscuous mode in Wi-Fi MAC
  feature list; dual-band 2.4 + 5 GHz radio fully specified).
- XIAO ESP32-C3 Datasheet v2.4.
- ESPboards.dev per-pin GPIO + strapping tables for both boards.
- Seeed Studio forum: documented SD/LoRa shared-SPI pull-up conflict and the J3
  solder-jumper fix on XIAO SD boards.
- Wio-SX1262 for XIAO product page (pin map: NSS=D4, DIO1=D1, NRST=D2, BUSY=D3,
  RF_SW=D5).
