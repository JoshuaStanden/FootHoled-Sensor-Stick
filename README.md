# FootHoled Sensor Stick

A battery-powered, 3D-printed **RF sensor node** built from two ESP32 boards. It
passively observes nearby **Wi-Fi** and **Bluetooth (BLE)** devices, logs them to
a **microSD card** (with on-board flash as an automatic fallback), and hands the
log off to a separate **collector node** over **LoRa** (long range) or **BLE**
(close range) — whichever the collector chooses. One 18650 cell powers the whole
thing.

> **Scope note:** this repo is the **sensor** (the thing that listens). The
> **collector** (the thing that comes to pick up the logs) is a separate device
> and is **not written yet** — the [handoff contract](#handoff-to-the-collector)
> below is the spec it will implement.

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
- [On-screen status](#on-screen-status)
- [Power & runtime](#power--runtime)
- [3D-printed case notes](#3d-printed-case-notes)
- [What's not built yet](#whats-not-built-yet)
- [Legal / ethical note](#legal--ethical-note)
- [Repo layout](#repo-layout)

---

## How it works

```
   2.4 GHz Wi-Fi  ─┐                                   ┌─ LoRa  (far, slow)
   frames          │   ┌─────────────────────────┐    │
                   ├──▶│  Heltec V4  "workhorse"  │────┤  handoff to COLLECTOR
   BLE ads ──▶ XIAO │   │  Wi-Fi sniff + logging  │    │   (collector picks
         (UART)─────┘   └─────────────────────────┘    └─ BLE   (near, fast)   which)
                              │
                         microSD  (flash fallback)
```

1. The **XIAO ESP32-C3** scans BLE advertisements and streams each one to the
   Heltec over a one-wire **UART** link.
2. The **Heltec V4** sniffs Wi-Fi in promiscuous mode, ingests the XIAO's BLE
   lines, and writes everything to one merged log in **batches**.
3. When a collector shows up, the Heltec hands over the log — **CRC-verified** —
   and only deletes it once the collector confirms a matching checksum.

## Why two boards

Wi-Fi promiscuous sniffing and BLE scanning both want the single 2.4 GHz radio
on one ESP32, and they starve each other when they share it. Splitting the jobs
across two chips is the entire reason for the dual-board design:

| Board | Chip | Jobs |
|-------|------|------|
| **Heltec WiFi LoRa 32 V4** | ESP32-S3 + SX1262 | Wi-Fi sniffer · UART ingest from XIAO · batched log (microSD primary, flash fallback) · CRC-verified handoff over LoRa **or** BLE · OLED · 18650 voltage monitor |
| **Seeed XIAO ESP32-C3** | ESP32-C3 | BLE scanner → MAC, address type, RSSI, name, advertised service UUIDs, service data, manufacturer data; streams to the Heltec over UART |

The Heltec is the **workhorse** and keeps the single merged log; the XIAO does
nothing but Bluetooth.

## Bill of materials

| Qty | Part | Notes |
|-----|------|-------|
| 1 | **Heltec WiFi LoRa 32 V4** (or Wireless Stick Lite V3) | ESP32-S3 + SX1262. Stick Lite is cheaper and has no OLED. |
| 1 | **Seeed XIAO ESP32-C3** | the BLE scanner |
| 1 | **microSD breakout + card** | wired to a 2nd SPI bus; card ≤ "normal" sizes, FAT32 |
| 1 | **~10 µF capacitor** | across the SD module's VCC/GND, close to the socket (prevents brown-out on write bursts) |
| 1 | **18650 cell** | use a **protected** cell or add a protection board |
| 1 | 18650 holder + JST lead | connects to the Heltec's on-board charger/battery port |
| — | hookup wire, 3D-printed case | antennas near the case wall, non-conductive filament |

## Quick start

1. **Install the ESP32 core** in Arduino IDE — Boards Manager → *"esp32 by
   Espressif"* **and** *"Heltec ESP32 Series Dev-boards"* by Heltec Automation.
2. **Install libraries** (Library Manager):
   - Both boards: **NimBLE-Arduino v2.x** — *the v2.x callback API is required;
     v1.x will not compile.*
   - Heltec only: `RadioLib`, and `U8g2` **only** for the OLED (V4) variant.
   - `SD`, `SPI`, `LittleFS` ship with the esp32 core — no separate install.
3. **Pick the Heltec board variant** — at the top of `heltec_workhorse.ino`,
   uncomment **exactly one** `#define` (the sketch will not compile until you do,
   on purpose, so you can't flash the wrong one):
   - `BOARD_WIFI_LORA_32_V4` — has the OLED; status shows on screen.
   - `BOARD_WIRELESS_STICK_LITE` — no OLED; status prints to serial.
4. **Set your LoRa region** — edit `LORA_FREQ` (US `915.0`, EU `868.0`, Asia
   `433.0`). See [Configure before flashing](#configure-before-flashing).
5. **Flash the Heltec** — select board *"Heltec WiFi LoRa 32(V3)"* in the IDE
   (the V4 and Stick Lite V3 are pin/radio compatible with the V3 definition),
   then flash `heltec-workhorse/heltec_workhorse.ino`.
6. **Flash the XIAO** — select board *"XIAO_ESP32C3"*, then flash
   `xiao-bt/xiao_bt_scanner.ino`.
7. **Wire the two boards + SD + battery** per [Wiring](#wiring), power on, and
   confirm the OLED shows `Log(SD)` (or `Log(FL)` on flash fallback).

## Wiring

### UART link (XIAO → Heltec)

| XIAO ESP32-C3 | Heltec V4 | Purpose |
|---------------|-----------|---------|
| TX = GPIO21 (D6) | RX = GPIO45 | BLE data up to the workhorse |
| RX = GPIO20 (D7) | TX = GPIO46 | optional (future commands) |
| GND | GND | **required common ground** |
| 3V3 (from Heltec `Ve` rail) | `Ve` (GPIO36-gated) | power the XIAO |

> You can power the XIAO from the Heltec's switched **Ve** 3.3 V rail (max
> 500 mA — plenty for the C3). That keeps one battery and one power switch for
> the whole node.

### microSD module (EXTERNAL — add this)

Wire a microSD breakout to a **second SPI bus**. The Heltec's main SPI bus is
taken by the on-board LoRa radio (pins 8–14), so the card must **not** share it:

| microSD module | Heltec V4 | Notes |
|----------------|-----------|-------|
| SCK | GPIO4 | second SPI clock |
| MISO | GPIO5 | |
| MOSI | GPIO6 | |
| CS | GPIO7 | chip select |
| VCC | 3V3 (or the `Ve` rail) | most modules accept 3.3 V directly |
| GND | GND | |

> **Add a ~10 µF cap** across the module's VCC/GND close to the socket. SD write
> bursts can pull 100 mA+ and brown out a battery node without local decoupling
> — it's the single most common cause of random SD failures. If the card won't
> mount, the firmware falls back to internal flash and shows `Log(FL)` instead
> of `Log(SD)`.

### On-board, already wired on the Heltec V4 (no action needed)

- **SX1262 LoRa:** NSS=8 SCK=9 MOSI=10 MISO=11 RST=12 BUSY=13 DIO1=14
- **OLED SSD1306:** SDA=17 SCL=18 RST=21 (rail gated by Vext_Ctrl=GPIO36)
- **Battery ADC:** GPIO1, enabled by ADC_Ctrl=GPIO37

### 18650 power

- Connect the 18650 holder to the Heltec's dedicated **LiPo/battery connector**
  (JST, on the back). The V4 has an on-board charger + 3.3 V regulator, so
  charging over USB-C and running from the cell both work.
- **Use a protected 18650 or add a protection board.** A raw holder gives you an
  *unprotected* cell; the Heltec's charger does basic management but a protected
  cell guards against over-discharge in a sealed case.

## Configure before flashing

In `heltec-workhorse/heltec_workhorse.ino`:

| Setting | What to set | Why |
|---------|-------------|-----|
| `LORA_FREQ` | your legal ISM band: US `915.0`, EU `868.0`, Asia `433.0` | the collector must match this exactly |
| `LORA_SF` / `LORA_BW` / `LORA_CR` / `LORA_SYNC` | leave as-is unless you change the collector too | both ends must match |
| `NODE_ID` | a unique string per sensor if you build several | identifies the node in the log/handoff |

The XIAO (`xiao-bt/xiao_bt_scanner.ino`) needs no edits for a default build;
`REPORT_RSSI_FLOOR` and `SCAN_DURATION_S` are tunable if you want to filter weak
hits or change the scan cadence.

## Storage: SD-primary with flash fallback

On boot the workhorse tries the microSD first. If a card mounts, all logging goes
to it (`/sensor.log`, OLED shows `Log(SD)`). If no card is present or it fails,
logging falls back to on-board flash (LittleFS, OLED shows `Log(FL)`) — so logging
never silently stops. File name, format, and the upload path are identical either
way.

| Backend | Size cap | Role |
|---------|----------|------|
| microSD | 256 MB sanity cap | primary — effectively unlimited (days–weeks between pickups) |
| internal flash | 0.5 MB | safety net — oldest log cleared if it fills before pickup |

**Batched, crash-resilient writes.** Observations are buffered and written in
batches (flushed at ~1 KB **or** every 250 ms), not one file write per packet —
this cuts SD wear and avoids a storage stall on every packet. The staging buffer
lives in **RTC memory**, which survives a software reset / watchdog / crash
reboot, so on boot any left-over buffer is replayed before new logging begins. A
magic marker tells a genuine buffer from uninitialised RTC garbage.

> **What's still at risk:** a *true power cut* (battery dies/disconnects)
> collapses the rail, and RTC RAM needs the rail to retain data — so you lose at
> most the current unflushed buffer (~250 ms of the newest observations). Crashes,
> watchdog resets and reboots are fully covered. Closing the power-cut gap would
> need a hardware brownout detector doing a last-gasp flush — not included.

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
> XOR), so any off-the-shelf CRC32 on the collector will match.

## On-screen status

On the V4 the OLED shows live status (the Stick Lite prints the same to serial):

| Display | Meaning |
|---------|---------|
| `WiFi:<n> drop:<n>` | Wi-Fi observations seen / dropped to a full queue (normally 0; may tick up during a LoRa upload). If `drop` climbs, raise `WIFI_QUEUE_DEPTH`. |
| `BT seen:<n>` | BLE observations received from the XIAO |
| `Log(SD):<n>B` / `Log(FL):<n>B` | current log size and which backend is active (SD or flash) |
| `Batt:<v>V <state>` | 18650 voltage and link state: `listen` / `BLE~` / `LoRa` / `BLE` |

## Power & runtime

- Active Wi-Fi sniffing + BLE scanning ≈ **150–300 mA** combined.
- A 3000 mAh 18650 → roughly **10–18 h** continuous.
- To extend: raise `WIFI_CHANNEL_DWELL_MS`, lengthen the XIAO scan gap, dim/kill
  the OLED, or add deep-sleep duty cycling (scan N s, sleep M). Ask and I can add
  a duty-cycle mode.

## 3D-printed case notes

- **Keep antennas clear of ground planes.** The LoRa antenna and both 2.4 GHz
  antennas (Heltec PCB + XIAO) should sit near the case wall, not buried behind
  the battery. Don't use conductive filament.
- **Battery access:** design the 18650 holder for tool-free swap if you don't
  want to rely only on USB-C charging. Add cutouts for the USB-C port and the
  RST/PRG buttons so you can reflash without opening the case.
- **Heat:** two radios + a charger in a sealed box run warm. Add a couple of vent
  slots on the top face (labyrinth style for weather resistance), or at minimum
  leave an air gap around the regulator.
- **OLED window:** cut a clear window, or omit it if you plan to disable the
  display to save power.
- **microSD access:** put the card slot near a case edge with a slot cutout so
  you can pull the card without opening the enclosure. Keep the decoupling cap
  right at the module.

## What's not built yet

- **The collector node.** It's a separate device (planned: a Waveshare
  ESP32-C3 + LoRa module). This repo only defines the
  [handoff contract](#handoff-to-the-collector) it must implement.
- **A duty-cycle / deep-sleep power mode** (listed under
  [Power & runtime](#power--runtime)).
- **Hardware brownout last-gasp flush** to close the true-power-cut data gap.

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
├── README.md                              # this file
├── LICENSE                                # MIT
├── .gitignore                             # build artifacts, IDE/OS files, ESP-IDF/PlatformIO output
├── heltec-workhorse/
│   └── heltec_workhorse.ino               # Heltec V4 workhorse firmware
└── xiao-bt/
    └── xiao_bt_scanner.ino                # XIAO ESP32-C3 BLE scanner firmware
```

## License

MIT — see [LICENSE](LICENSE).
