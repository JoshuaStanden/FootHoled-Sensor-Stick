# FootHoled Sensor Stick — XIAO build

A battery-powered, 3D-printed **RF sensor node** built from two Seeed XIAO ESP32
boards. It passively observes nearby **Wi-Fi** (2.4 GHz **and** 5 GHz) and
**Bluetooth (BLE)** devices, logs them to a **microSD card** (with the C5's
on-board flash as an automatic fallback), and hands the log off to a separate
**collector node** over **LoRa** (long range) or **BLE** (close range) —
whichever the collector chooses. One 18650 cell powers the whole thing.

> **This is the `xiao` branch.** It replaces the Heltec workhorse from `main`
> with a XIAO ESP32-C5 + a stacked Wio-SX1262 LoRa hat. Design rationale, verified
> wiring, BOM and firmware notes live in [`docs/xiao/`](docs/xiao/). The firmware
> here is a **first bringup draft — not yet compiled or flashed on hardware.**

> **Scope note:** this repo is the **sensor** (the thing that listens). The
> **collector** (the thing that comes to pick up the logs) is a separate device
> and is **not written yet** — the [handoff contract](#handoff-to-the-collector)
> below is the spec it will implement. The handoff contract, LoRa parameters and
> BLE UUIDs are kept identical to the Heltec build so a collector works with either.

---

## Contents

- [How it works](#how-it-works)
- [Why two boards](#why-two-boards)
- [Bill of materials](#bill-of-materials)
- [Quick start](#quick-start)
- [Wiring](#wiring)
- [Configure before flashing](#configure-before-flashing)
- [Storage: SD-primary with flash fallback](#storage-sd-primary-with-flash-fallback)
- [Log format](#log-format)
- [Handoff to the collector](#handoff-to-the-collector)
- [Status output](#status-output)
- [Power & runtime](#power--runtime)
- [3D-printed case notes](#3d-printed-case-notes)
- [What's not built yet](#whats-not-built-yet)
- [Legal / ethical note](#legal--ethical-note)
- [Repo layout](#repo-layout)

---

## How it works

```
   Wi-Fi frames    ─┐                                   ┌─ LoRa  (far, slow)
   (2.4 + 5 GHz)    │   ┌─────────────────────────┐    │
                    ├──▶│  XIAO C5  "workhorse"    │────┤  handoff to COLLECTOR
   BLE ads ──▶ XIAO │   │  Wi-Fi sniff + logging   │    │   (collector picks
         (UART) C3 ─┘   └─────────────────────────┘    └─ BLE   (near, fast)  which)
                              │
                         microSD  (onboard flash fallback)
```

1. The **XIAO ESP32-C3** scans BLE advertisements and streams each one to the
   C5 over a one-wire **UART** link.
2. The **XIAO ESP32-C5** sniffs Wi-Fi in promiscuous mode across **2.4 GHz and
   5 GHz**, ingests the C3's BLE lines, and writes everything to one merged log
   in **batches**.
3. When a collector shows up, the C5 hands over the log — **CRC-verified** — and
   only deletes it once the collector confirms a matching checksum.

## Why two boards

Wi-Fi promiscuous sniffing and BLE scanning both want the single 2.4 GHz radio
on one ESP32, and they starve each other when they share it. Splitting the jobs
across two chips is the entire reason for the dual-board design:

| Board | Chip | Jobs |
|-------|------|------|
| **Seeed XIAO ESP32-C5** | ESP32-C5 (RISC-V, dual-band Wi-Fi 6) + Wio-SX1262 hat | Wi-Fi sniffer (2.4 **+** 5 GHz) · UART ingest from the C3 · batched log (microSD primary, onboard flash fallback) · CRC-verified handoff over LoRa **or** BLE · 18650 powered |
| **Seeed XIAO ESP32-C3** | ESP32-C3 | BLE scanner → MAC, address type, RSSI, name, advertised service UUIDs, service data, manufacturer data; streams to the C5 over UART |

The C5 is the **workhorse** and keeps the single merged log; the C3 does nothing
but Bluetooth. The 5 GHz capability is the main upgrade over the Heltec build —
the C5 is the first XIAO with a 5 GHz radio, closing the blind spot where modern
devices connect on 5 GHz and are invisible to a 2.4 GHz-only sniffer.

## Bill of materials

Full parts list with prices and alternatives: [`docs/xiao/BOM.md`](docs/xiao/BOM.md).
The essentials:

| Qty | Part | Notes |
|-----|------|-------|
| 1 | **Seeed XIAO ESP32-C5** | the Wi-Fi/SD/LoRa workhorse (dual-band) |
| 1 | **Seeed XIAO ESP32-C3** | the BLE scanner |
| 1 | **Wio-SX1262 for XIAO** | plug-and-play LoRa hat (SX1262, 868/915 MHz), stacks on the C5 |
| 1 | **microSD breakout + card** | SPI, **3.3 V**, shares the C5 SPI bus with LoRa; card ≤ 32 GB, FAT32 |
| 1 | **~10 µF capacitor** | across the SD module's VCC/GND, close to the socket (prevents brown-out on write bursts) |
| 1 | **dual-band 2.4/5 GHz U.FL antenna** | the C5's stock antenna is 2.4 GHz-only; **required** to get 5 GHz range |
| 1 | **protected 18650 cell** | protected cell (or add a protection board) for an unattended device |
| 1 | 18650 holder | connects to the C5's battery input |
| — | hookup wire, power switch, 3D-printed case | antennas near the case wall, non-conductive filament |

> **SD breakout caveat (important on this build):** the SD card shares the C5's
> single SPI bus with the LoRa radio. A cheap module with aggressive pull-ups on
> SCK/MISO/MOSI can fight the radio on the shared bus. Use a buffered breakout
> (Adafruit #4682) **or** remove the bus pull-ups on a cheap module. See
> [`docs/xiao/WIRING.md`](docs/xiao/WIRING.md).

## Quick start

1. **Install the ESP32 core** in Arduino IDE — Boards Manager → *"esp32 by
   Espressif"*. Use a **recent** version: the C5 is new, and 5 GHz promiscuous
   channel-set support depends on the ESP-IDF the core bundles.
2. **Install libraries** (Library Manager):
   - Both boards: **NimBLE-Arduino v2.x** — *the v2.x callback API is required;
     v1.x will not compile.*
   - C5 only: `RadioLib` (SX1262 driver).
   - `SD`, `SPI`, `LittleFS` ship with the esp32 core — no separate install.
3. **Set your LoRa region** — edit `LORA_FREQ` in the C5 sketch (US `915.0`, EU
   `868.0`, Asia `433.0`). See [Configure before flashing](#configure-before-flashing).
4. **Flash the C5** — select board *"XIAO_ESP32C5"*, then flash
   `xiao-c5-workhorse/xiao_c5_workhorse.ino`.
5. **Flash the C3** — select board *"XIAO_ESP32C3"*, then flash
   `xiao-c3-ble/xiao_c3_ble_scanner.ino`.
6. **Wire the two boards + SD + battery** per [Wiring](#wiring), power on, and
   confirm the serial `[STATUS]` line shows `Log(SD)` (or `Log(FL)` on fallback)
   and a climbing `WiFi:` count.

## Wiring

Verified GPIO numbers for both boards are in
[`docs/xiao/WIRING.md`](docs/xiao/WIRING.md). Summary:

### UART link (C3 → C5)

| XIAO C3 | XIAO C5 | Purpose |
|---------|---------|---------|
| TX = D6 / GPIO21 | RX = D7 / GPIO12 | BLE data up to the workhorse |
| RX = D7 / GPIO20 | TX = D6 / GPIO11 | optional (future commands) |
| GND | GND | **required common ground** |

> **TX↔RX crosses over.** Power the C3 from its own cell or share the C5's 3.3 V
> rail — either works, as long as the grounds are tied together.

### Wio-SX1262 LoRa hat (plug-and-play on the C5)

The hat stacks directly onto the C5 headers — you do **not** hand-wire it. It
consumes these C5 pins (documented so you know what's left for the SD card):

| Hat signal | C5 pin | | Hat signal | C5 pin |
|---|---|---|---|---|
| NSS (CS) | D4 / GPIO23 | | BUSY | D3 / GPIO7 |
| DIO1 | D1 / GPIO0 | | RF_SW | D5 / GPIO24 |
| NRST | D2 / GPIO25 | | SPI bus | D8/D9/D10 (GPIO8/9/10) |

### microSD module (hand-wired, SHARES the C5 SPI bus with LoRa)

The C5 has **one** SPI bus, so the SD card joins the LoRa radio's bus with its
**own chip-select**. Only the CS is unique; the three bus lines are shared:

| microSD module | C5 pin | Notes |
|----------------|--------|-------|
| SCK | D8 / GPIO8 | **shared** with LoRa |
| MISO | D9 / GPIO9 | **shared** with LoRa |
| MOSI | D10 / GPIO10 | **shared** with LoRa |
| CS | D0 / GPIO1 | SD's own (verified safe, non-strapping) |
| VCC | 3V3 | **3.3 V only** |
| GND | GND | |

> **Add a ~10 µF cap** across the module's VCC/GND close to the socket — SD write
> bursts can brown out a battery node. And **disable the module's bus pull-ups**
> (or use a buffered breakout) so the SD doesn't fight the LoRa radio on the
> shared bus. If the card won't mount, the firmware falls back to the C5's
> onboard flash and the status shows `Log(FL)` instead of `Log(SD)`.

### 18650 power

- Connect the 18650 to the C5's battery input (both XIAOs have onboard LiPo
  charge management, so USB-C charging and running from the cell both work).
- **Use a protected 18650 or add a protection board** — a raw cell in a sealed,
  unattended logger risks over-discharge.
- To run both XIAOs from one cell, feed the cell to both boards' battery inputs
  with a **common ground**, or share a 3.3 V rail. Decide the scheme before wiring.

## Configure before flashing

In `xiao-c5-workhorse/xiao_c5_workhorse.ino`:

| Setting | What to set | Why |
|---------|-------------|-----|
| `LORA_FREQ` | your legal ISM band: US `915.0`, EU `868.0`, Asia `433.0` | the collector must match this exactly |
| `ENABLE_5GHZ` | `1` (default) to hop 5 GHz channels too; `0` to force 2.4 GHz-only | set `0` if your esp32 core is too old to set a 5 GHz channel in promiscuous mode |
| `LORA_SF` / `LORA_BW` / `LORA_CR` / `LORA_SYNC` | leave as-is unless you change the collector too | both ends must match |
| `NODE_ID` | a unique string per sensor if you build several | identifies the node in the log/handoff |

The C3 (`xiao-c3-ble/xiao_c3_ble_scanner.ino`) needs no edits for a default build;
`REPORT_RSSI_FLOOR` and `SCAN_DURATION_S` are tunable if you want to filter weak
hits or change the scan cadence.

## Storage: SD-primary with flash fallback

On boot the C5 tries the microSD first (on the shared SPI bus). If a card mounts,
all logging goes to it (`/sensor.log`, status shows `Log(SD)`). If no card is
present or it fails, logging falls back to the C5's onboard flash (LittleFS,
status shows `Log(FL)`) — so logging never silently stops. File name, format, and
the upload path are identical either way.

| Backend | Size cap | Role |
|---------|----------|------|
| microSD | 256 MB sanity cap | primary — effectively unlimited (days–weeks between pickups) |
| C5 onboard flash | 0.5 MB | safety net — oldest log cleared if it fills before pickup |

> **Alternative worth considering:** for a sealed logger that always offloads over
> LoRa/BLE, logging straight to the C5's onboard flash (no SD card at all) removes
> the shared-bus pull-up problem entirely. See the storage alternatives in
> [`docs/xiao/BOM.md`](docs/xiao/BOM.md).

**Batched, crash-resilient writes.** Observations are buffered and written in
batches (flushed at ~1 KB **or** every 250 ms), not one file write per packet —
this cuts SD wear and avoids a storage stall on every packet. The staging buffer
lives in **RTC memory**, which survives a software reset / watchdog / crash
reboot, so on boot any left-over buffer is replayed before new logging begins. A
magic marker tells a genuine buffer from uninitialised RTC garbage.

> **What's still at risk:** a *true power cut* (battery dies/disconnects)
> collapses the rail, and RTC RAM needs the rail to retain data — so you lose at
> most the current unflushed buffer (~250 ms of the newest observations). Crashes,
> watchdog resets and reboots are fully covered.

## Log format

Lines written to `/sensor.log` (seconds-since-boot timestamp; the collector can
re-stamp with real time if it has a clock):

```
<secs>,WIFI,<mac>,<rssi>,ch<n>
<secs>,BT,<mac>,<addrType>,<rssi>,<name>,<svcUUIDs>,<svcData>,<mfgData>
```

Bluetooth fields (all read **passively** from the advertisement — no connection):

| Field | Meaning |
|-------|---------|
| `addrType` | `pub` / `rpa` / `nrpa` / `rnd` — public, or a privacy-rotating random address of the kind modern phones use |
| `svcUUIDs` | advertised service UUIDs, `\|`-separated (e.g. `180d` = Heart Rate, `181a` = Environmental Sensing). Capped at 4. |
| `svcData` | `<uuid>:<hexbytes>` service data if broadcast (often a live sensor reading, e.g. a Govee thermometer). `\|`-separated. |
| `mfgData` | manufacturer-specific data as hex; first 2 bytes are the vendor ID (e.g. `4c00` = Apple). Capped at 16 bytes. |

> This is the *advertised* subset of services, not the device's full GATT list —
> that is only visible after connecting, which the scanner never does.

## Handoff to the collector

The sensor hands its log to the collector over **either LoRa or BLE**, and the
**collector chooses** which — to play each radio's strength:

| Transport | Range | Speed | When |
|-----------|-------|-------|------|
| **LoRa** | 100s m – km | slow (minutes for a big log) | collector is far; pull from a distance |
| **BLE** | ~10–30 ft | fast (seconds) | you brought the collector close |

The sensor **idles listening on both at once** (LoRa in receive + BLE
advertising) and answers whichever the collector initiates — no mode switch on
the sensor. Wi-Fi sniffing is paused only during the transfer.

**Verified, safe handoff (same for both transports):** the transfer is framed and
the whole log is covered by a **CRC32**. The sensor sends the CRC up front, the
collector recomputes it over what it received, and the sensor **deletes its log
only when the collector confirms a matching CRC**. A dropped link, mismatch, or
timeout leaves the log intact for the next attempt.

### Shared contract (what the future collector implements)

```
1. collector → sensor : "GET"                               (begin collection)
2. sensor → collector : "HELLO,<id>,<totalBytes>,<crc32hex>"
3. sensor → collector : log bytes in chunks
4. sensor → collector : "END"
5. collector → sensor : "DONE,<crc32hex>"   (only if its CRC matches)
6. sensor deletes the log iff the DONE CRC matches.
```

- **LoRa framing:** each chunk is `D,<seq>,<rawbytes>` (≤180 B), acked per-chunk
  with `A,<seq>`; the sender resends on timeout. `seq` lets the collector detect
  gaps and reassemble in order.
- **BLE framing:** the connection layer handles ordering/reliability/chunking, so
  log bytes stream raw over a NOTIFY characteristic. The link is bonded +
  encrypted (payload is MAC addresses) — pair on first contact.
- **BLE UUIDs** (collector scans for the service UUID; must match exactly):
  - service `6b1a0001-9d4e-4c2a-8f33-7c2e5a9b1d10`
  - data (NOTIFY) `6b1a0002-…` · ctrl (WRITE) `6b1a0003-…`

> The CRC32 uses the standard convention (seed `0xFFFFFFFF`, reflected, final
> XOR), so any off-the-shelf CRC32 on the collector will match. These values are
> **identical to the Heltec build**, so an existing collector interoperates.

## Status output

Neither XIAO has an OLED, so the C5 prints live status to the **serial console**
(115200 baud) once per second:

```
[STATUS] WiFi:<n> drop:<n> BT:<n> Log(SD|FL):<n>B ch:<n> <state>
```

| Field | Meaning |
|-------|---------|
| `WiFi:<n> drop:<n>` | Wi-Fi observations seen / dropped to a full queue (normally 0; may tick up during a LoRa upload). If `drop` climbs, raise `WIFI_QUEUE_DEPTH`. |
| `BT:<n>` | BLE observations received from the C3 |
| `Log(SD):<n>B` / `Log(FL):<n>B` | current log size and which backend is active (SD or onboard flash) |
| `ch:<n>` | current sniff channel; a `5G-` prefix means it's dwelling on a 5 GHz channel |
| `<state>` | link state: `listen` / `BLE~` / `LoRa` / `BLE` |

> Want on-device status without a screen? Add a tiny I²C OLED on the C5's free
> I²C pins (listed as optional in [`docs/xiao/BOM.md`](docs/xiao/BOM.md)).

## Power & runtime

- Active Wi-Fi sniffing + BLE scanning ≈ **120–250 mA** combined across both XIAOs.
- A 3000 mAh 18650 → roughly **12–20 h** continuous.
- To extend: raise `WIFI_CHANNEL_DWELL_MS`, lengthen the C3 scan gap, or add
  deep-sleep duty cycling (scan N s, sleep M). The XIAO C5/C3 both have a 15 µA
  deep-sleep floor, so a duty-cycle mode extends runtime dramatically. Ask and I
  can add one.

## 3D-printed case notes

- **Keep antennas clear of ground planes.** The LoRa antenna and both 2.4/5 GHz
  antennas (C5 + C3) should sit near the case wall, not buried behind the battery.
  Don't use conductive filament. The C5 needs its **dual-band U.FL antenna** at
  the case wall to get real 5 GHz range.
- **Battery access:** design the 18650 holder for tool-free swap if you don't
  want to rely only on USB-C charging. Add cutouts for both XIAOs' USB-C ports and
  reset buttons so you can reflash without opening the case.
- **Heat:** two radios + a charger in a sealed box run warm. Add a couple of vent
  slots on the top face (labyrinth style for weather resistance).
- **microSD access:** put the card slot near a case edge with a slot cutout so you
  can pull the card without opening the enclosure. Keep the decoupling cap right
  at the module. (If you log to onboard flash instead, this is moot.)
- **Stack height:** the Wio-SX1262 hat sits on top of the C5 — leave headroom for
  the stacked boards plus the hand-soldered SD wires on the pass-through pads.

## What's not built yet

- **The collector node.** It's a separate device (planned: a Waveshare
  ESP32-C3 + LoRa module). This repo only defines the
  [handoff contract](#handoff-to-the-collector) it must implement.
- **A duty-cycle / deep-sleep power mode** (listed under
  [Power & runtime](#power--runtime)).
- **Hardware brownout last-gasp flush** to close the true-power-cut data gap.

## Bench / software open items (XIAO build)

These are datasheet-confirmed in hardware but not yet proven on a built unit —
see [`docs/xiao/ARCHITECTURE.md`](docs/xiao/ARCHITECTURE.md):

- **5 GHz channel-set in promiscuous mode** — the C5 radio + MAC support it per
  Espressif's datasheet; whether your ESP-IDF/core version exposes the 5 GHz
  channel set is a software-maturity check. `ENABLE_5GHZ 0` falls back to 2.4 GHz.
- **Mechanical pad access** — confirm the SD solder pads (D0 + the SPI bus) are
  reachable once the Wio-SX1262 hat is seated.
- **Firmware is a first bringup draft** — not yet compiled or flashed.

## Legal / ethical note

Passive RF scanning records device identifiers (MAC addresses) that are personal
data in many jurisdictions. Modern phones randomize Wi-Fi/BLE MACs, which limits
tracking, but you should still only deploy this where you have the right to,
disclose collection where required, and avoid building persistent profiles of
identifiable people. This firmware logs raw MACs for *your* use; consider hashing
them on the collector if you don't need the raw value.

## Repo layout

```
.
├── README.md                              # this file (XIAO build)
├── LICENSE                                # MIT
├── .gitignore                             # build artifacts, IDE/OS files, ESP-IDF/PlatformIO output
├── docs/
│   └── xiao/                              # design docs for this build
│       ├── README.md                      #   index
│       ├── ARCHITECTURE.md                #   why C5 + C3, decisions, open items
│       ├── WIRING.md                      #   verified pin map + GPIO numbers
│       ├── BOM.md                         #   full parts list + alternatives
│       └── FIRMWARE.md                    #   board targets, libraries, flashing
├── xiao-c5-workhorse/
│   └── xiao_c5_workhorse.ino              # XIAO ESP32-C5 workhorse (Wi-Fi/SD/LoRa/BLE)
└── xiao-c3-ble/
    └── xiao_c3_ble_scanner.ino            # XIAO ESP32-C3 BLE scanner
```

> The Heltec workhorse firmware lives on the `main` branch, not here.

## License

MIT — see [LICENSE](LICENSE).
