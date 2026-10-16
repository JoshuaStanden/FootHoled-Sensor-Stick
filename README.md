# Sensor Node — Heltec V4 + XIAO ESP32-C3

A dual-ESP32 RF sensor that collects **Wi-Fi** and **Bluetooth (BLE)** device
observations, logs them to a **microSD card** (with on-board flash as fallback),
and hands them off to a separate **collector node** over **LoRa (long range) or
Bluetooth (close range)** — the collector picks the transport. Powered by a
single 18650, fully enclosed in a 3D-printed case.

## Role split

| Board | Chip | Jobs |
|-------|------|------|
| **Heltec WiFi LoRa 32 V4** | ESP32-S3 + SX1262 | Wi-Fi promiscuous sniffer · UART ingest from XIAO · log file (microSD primary, flash fallback) · CRC-verified handoff over LoRa **or** BLE · OLED · 18650 voltage monitor |
| **Seeed XIAO ESP32-C3** | ESP32-C3 | BLE scanner → captures MAC, address type, RSSI, name, advertised service UUIDs, service data, and manufacturer data; streams to Heltec over UART |

**Why two chips:** Wi-Fi promiscuous mode and BLE scanning fight over the same
2.4 GHz radio on a single ESP32 and starve each other. Splitting them is the
whole reason for the dual-board design. The Heltec is the workhorse and keeps the
single merged log.

## Files

- `heltec-workhorse/heltec_workhorse.ino` — the Heltec "workhorse" firmware
- `xiao-bt/xiao_bt_scanner.ino` — the XIAO BLE scanner firmware

## Wiring

### UART link (XIAO → Heltec)
| XIAO ESP32-C3 | Heltec V4 | Purpose |
|---------------|-----------|---------|
| TX  = GPIO21 (D6) | RX = GPIO45 | BLE data up to the workhorse |
| RX  = GPIO20 (D7) | TX = GPIO46 | optional (future commands) |
| GND | GND | **required common ground** |
| 3V3 (from Heltec `Ve` rail) | `Ve` (GPIO36-gated) | power the XIAO |

> You can power the XIAO from the Heltec's switched **Ve** 3.3 V rail (max
> 500 mA, plenty for the C3). That keeps a single battery and a single power
> switch for the whole node.

### microSD module (EXTERNAL — add this)
Wire a microSD breakout to a **second SPI bus** (the Heltec's main SPI bus is
taken by the on-board LoRa radio, pins 8–14, so the card must not share it):

| microSD module | Heltec V4 | Notes |
|----------------|-----------|-------|
| SCK  | GPIO4 | second SPI clock |
| MISO | GPIO5 | |
| MOSI | GPIO6 | |
| CS   | GPIO7 | chip select |
| VCC  | 3V3 (or the `Ve` rail) | 3.3 V — most modules accept it directly |
| GND  | GND | |

> **Add a ~10 µF capacitor** across the module's VCC/GND close to the socket.
> SD write bursts can pull 100 mA+ and brown out a battery-powered node without
> local decoupling — it's the single most common cause of random SD failures.
> If the card doesn't mount, the firmware automatically falls back to internal
> flash and shows `Log(FL)` on the OLED instead of `Log(SD)`.

### On-board, already wired on the Heltec V4 (no action needed)
- **SX1262 LoRa:** NSS=8 SCK=9 MOSI=10 MISO=11 RST=12 BUSY=13 DIO1=14
- **OLED SSD1306:** SDA=17 SCL=18 RST=21 (rail gated by Vext_Ctrl=GPIO36)
- **Battery ADC:** GPIO1, enabled by ADC_Ctrl=GPIO37

### 18650 power
- Connect the 18650 holder to the Heltec's dedicated **LiPo/battery connector**
  (JST, on the back of the board). The Heltec V4 has an on-board charger +
  3.3 V regulator, so charging over USB-C and running from the cell both work.
- **Important:** a raw 18650 holder gives you an *unprotected* cell. Use either
  a protected 18650 **or** add a small protection board, since the Heltec's
  charger does basic management but a protected cell guards against
  over-discharge in a sealed case.

## Build & flash (Arduino IDE)

1. **Install the ESP32 core:** Boards Manager → "esp32 by Espressif" **and**
   "Heltec ESP32 Series Dev-boards" by Heltec Automation.
2. **Libraries** (Library Manager):
   - Heltec: `RadioLib`, `NimBLE-Arduino` (**v2.x** — the handoff code uses the
     2.x callback API; v1.x will not compile), plus `U8g2` **only if** you build
     the V4 (OLED) variant.
   - XIAO: `NimBLE-Arduino` (**v2.x**)
   - (`SD` and `SPI` for the microSD are bundled with the esp32 core — no
     separate install.)
3. **Select the board variant in the sketch.** At the top of
   `heltec_workhorse.ino` are two commented `#define`s — **uncomment exactly
   one**:
   - `BOARD_WIFI_LORA_32_V4` — the board *with* the OLED; status shows on screen.
   - `BOARD_WIRELESS_STICK_LITE` — the OLED-less board (cheaper, no fragile
     screen); status prints to the serial console instead.

   Both are left commented on purpose, so the sketch **will not compile until
   you choose** — this prevents accidentally flashing the wrong variant.
4. **Heltec:** select board **"Heltec WiFi LoRa 32(V3)"** in the IDE (the V4 and
   the Wireless Stick Lite V3 are both pin/radio compatible with the V3
   definition — same ESP32-S3 + SX1262 map). Flash `heltec_workhorse.ino`.
5. **XIAO:** select board **"XIAO_ESP32C3"**. Flash `xiao_bt_scanner.ino`.

## Configure before flashing

In `heltec_workhorse.ino`:
- `LORA_FREQ` — **set your legal band**: US `915.0`, EU `868.0`, Asia `433.0`.
  The collector node must match this and `LORA_SF/BW/CR/SYNC` exactly.
- `NODE_ID` — unique per sensor if you build several.

## Handoff to the collector (two transports)

The sensor hands its log to the collector over **either LoRa or Bluetooth**, and
the **collector chooses** which — the idea is to use each radio's strength:

| Transport | Range | Speed | When you'd use it |
|-----------|-------|-------|-------------------|
| **LoRa** | 100s m – km | slow (minutes for a big log) | collector is far; pull from a distance |
| **Bluetooth (BLE)** | ~10–30 ft | fast (seconds) | you've brought the collector close |

The sensor **idles listening on both at once** — LoRa radio in receive mode *and*
BLE advertising — and answers whichever the collector initiates. No mode switch
on the sensor. Wi-Fi sniffing is paused only during the transfer (both radios
keep the main loop busy; BLE additionally shares the 2.4 GHz front-end with
Wi-Fi).

**Verified, safe handoff (same for both transports):** the transfer is framed and
the whole log is covered by a **CRC32**. The sensor sends the CRC up front, the
collector recomputes it over what it received, and the sensor **deletes its log
only when the collector confirms a matching CRC**. A dropped link, a mismatch, or
a timeout leaves the log intact for the next attempt.

### Shared contract (what the future collector implements)

```
1. collector → sensor : "GET"                           (begin collection)
2. sensor → collector : "HELLO,<id>,<totalBytes>,<crc32hex>"
3. sensor → collector : log bytes in chunks
4. sensor → collector : "END"
5. collector → sensor : "DONE,<crc32hex>"  (only if its CRC matches)
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
  - data (NOTIFY) `6b1a0002-...` · ctrl (WRITE) `6b1a0003-...`

> The collector side (a Waveshare ESP32-C3 + LoRa module) is **not written yet** —
> this contract is the spec it will implement. The CRC32 uses the standard
> convention (seed `0xFFFFFFFF`, reflected, final XOR), so any off-the-shelf
> CRC32 on the collector will match.

Log line format written to `/sensor.log`:
```
<secondsSinceBoot>,WIFI,<mac>,<rssi>,ch<n>
<secondsSinceBoot>,BT,<mac>,<addrType>,<rssi>,<name>,<svcUUIDs>,<svcData>,<mfgData>
```

Bluetooth fields (all read passively from the advertisement — no connection):
- `addrType` — `pub` / `rpa` / `nrpa` / `rnd` (public, or a privacy-rotating
  random address of the kind modern phones use)
- `svcUUIDs` — advertised service UUIDs, `|`-separated, telling you what the
  device *is* without connecting (e.g. `180d` = Heart Rate, `181a` =
  Environmental Sensing). Capped at 4.
- `svcData` — `<uuid>:<hexbytes>` service data if broadcast (often a live sensor
  reading, e.g. a Govee thermometer's temperature), `|`-separated.
- `mfgData` — manufacturer-specific data as hex; first 2 bytes are the vendor ID
  (e.g. `4c00` = Apple). Capped at 16 bytes.

> This is the *advertised* subset of services, not the device's full GATT
> service list — that is only visible after connecting, which the scanner does
> not do.

## Storage (SD-primary with flash fallback)

On boot the workhorse tries the microSD first. If a card mounts, all logging
goes to it (`/sensor.log`) and the OLED shows `Log(SD)`. If no card is present
or it fails to mount, logging falls back to the on-board flash (LittleFS) and
the OLED shows `Log(FL)` — so logging never silently stops. The log file name,
format, and the LoRa upload path are identical either way.

Size caps differ by backend because capacity does:
- **microSD:** 256 MB sanity cap (effectively unlimited for this use — days to
  weeks of data between collector contacts).
- **flash fallback:** 0.5 MB, since the internal flash is small and shared with
  firmware. If it fills before the collector drains it, the oldest log is
  cleared. Treat flash as a safety net, not the primary store.

In both cases the log is only cleared after the collector returns an `ACK`, so
data stays on the node until delivery is confirmed.

### Batched writes (crash-resilient)

Observations are **buffered and written in batches**, not one file write per
packet. This matters because an SD write can stall for tens of milliseconds, and
doing that on every packet would both wear the card and risk dropping
observations during the write.

- The Wi-Fi sniffer callback (an interrupt) does the bare minimum — formats a
  line and pushes it into a 64-slot FreeRTOS queue — then returns immediately.
- The main loop drains that queue plus the Bluetooth UART into a staging buffer.
- The buffer is flushed to storage when it reaches ~1 KB **or** every 250 ms,
  whichever comes first (`LOG_FLUSH_BYTES` / `LOG_FLUSH_MS`), in a single
  open/append/close.

**The staging buffer lives in RTC memory, so it survives a crash.** The ESP32-S3
has a small block of RTC RAM whose contents are *not* wiped by a software reset,
watchdog, or crash reboot (`RTC_NOINIT_ATTR`). A magic marker lets the firmware
tell a genuine left-over buffer from uninitialised RTC garbage. On boot, if a
valid buffer is found it is replayed to storage before new logging begins — so a
crash/reset loses nothing that was already buffered. The buffer is capped at
1.5 KB because RTC RAM is only 8 KB total and shared with the system.

The OLED shows `WiFi:<n> drop:<n>` — the second number is observations lost to a
full queue (normally 0; it can tick up during a LoRa upload, when the loop is
busy transmitting). If `drop` climbs, raise `WIFI_QUEUE_DEPTH`.

> **What's still at risk:** a *true power cut* (battery dies or disconnects)
> collapses the rail, and RTC RAM needs that rail to retain data — so you lose at
> most the current unflushed buffer, i.e. up to ~250 ms of the newest
> observations (`LOG_FLUSH_MS`). Crashes, watchdog resets and reboots are fully
> covered. Closing the power-cut gap entirely would need a hardware brownout
> detector (a resistor divider to a GPIO) doing a last-gasp flush — not included.

## 3D-printed case notes

- **Keep antennas clear of ground planes.** The Heltec's LoRa antenna and both
  2.4 GHz antennas (Heltec PCB + XIAO) should sit near the case wall, not buried
  behind the battery. Don't use conductive filament.
- **Battery access:** design the 18650 holder for tool-free swap if you don't
  want to rely only on USB-C charging. Add a small cutout for the USB-C port and
  the RST/PRG buttons so you can reflash without opening the case.
- **Heat:** two radios + a charger in a sealed box run warm. Add a couple of
  vent slots on the top face (labyrinth style if you need weather resistance),
  or at minimum leave air gap around the regulator.
- **OLED window:** cut a clear window or omit if you plan to disable the display
  to save power.
- **microSD access:** put the card slot near a case edge with a slot cutout so
  you can pull the card to offload data without opening the enclosure. Keep the
  decoupling cap right at the module.

## Power & runtime (rough)

- Active Wi-Fi sniffing + BLE scanning ≈ 150–300 mA combined.
- A 3000 mAh 18650 → ~10–18 h continuous.
- To extend: raise `WIFI_CHANNEL_DWELL_MS`, lengthen the XIAO scan gap, dim/kill
  the OLED, or add deep-sleep duty cycling (scan N seconds, sleep M). Ask and I
  can add a duty-cycle mode.

## Legal / ethical note

Passive RF scanning records device identifiers (MAC addresses) that are
personal data in many jurisdictions. Modern phones randomize Wi-Fi/BLE MACs,
which limits tracking, but you should still only deploy this where you have the
right to, disclose collection where required, and avoid building persistent
profiles of identifiable people. This firmware logs raw MACs for *your* use;
consider hashing them on the collector if you don't need the raw value.
