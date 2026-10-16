# FootHoled Sensor Stick — XIAO Build: Architecture & Design Decisions

> **Status:** Research / design reference only. Nothing built. Captures the
> reasoning behind choosing the XIAO C5 + C3 stack over the Heltec, so the "why"
> isn't lost. See `WIRING.md` for the verified pin map and `BOM.md` for the parts.

## Summary

The sensor stick moves from a **Heltec ESP32 + XIAO ESP32-C3** design to a
**XIAO ESP32-C5 + XIAO ESP32-C3** design. The C5 replaces the Heltec as the
Wi-Fi/SD/LoRa workhorse and adds 5 GHz capture; the C3's BLE-scanner role is
unchanged. Net result: cheaper than a Heltec, smaller, lower power, and it gains
5 GHz Wi-Fi the Heltec never had.

Scope note: the **collector node is a separate project and out of scope here.**
Everything below is about the sensor role only.

## Board selection — why the C5

Evaluated three XIAO ESP32 options as a cheaper alternative to the Heltec:

| | C6 ($5.20) | **C5 ($6.90)** | S3 |
|---|---|---|---|
| Wi-Fi | 2.4 GHz only | **2.4 + 5 GHz** | 2.4 GHz only |
| PSRAM | none | **8 MB** | 8 MB |
| Flash | 4 MB | **8 MB** | 8 MB |
| CPU | 160 MHz | **240 MHz** | 240 MHz |
| Deep sleep | 15 µA | **15 µA** | 14 µA |
| BLE / 802.15.4 | ✅ / ✅ | ✅ / ✅ | ✅ / — |

- **"Wi-Fi 6" is a protocol generation, not a band.** The C6 implements Wi-Fi 6
  on **2.4 GHz only** — no ESP32 before the C5 has a 5 GHz radio.
- The **C5 is the first XIAO with dual-band 2.4 + 5 GHz Wi-Fi**, and it lands the
  5 GHz capability on the chip that benefits (the Wi-Fi sniffer). For a recon
  sensor, 5 GHz closes the blind spot where modern phones/laptops connect on 5 GHz
  and are nearly invisible to a 2.4 GHz-only sniffer.
- The C5 also carries S3-class memory/CPU (240 MHz, 8 MB PSRAM, 8 MB flash) for
  buffering sniffed packets, while keeping the C6's 15 µA deep sleep — so no power
  penalty for the upgrade.

## Radio split — why still two MCUs

On a single ESP32, Wi-Fi promiscuous mode and BLE scanning both want the one
2.4 GHz radio; time-slicing drops packets. So the design keeps the proven split:

- **C5:** Wi-Fi promiscuous sniffing full-time (+ SD + LoRa handoff).
- **C3:** BLE scanning full-time, streaming to the C5 over UART.

BLE is 2.4 GHz-only by nature, so the cheap C3 is the right chip for it — nothing
fancier helps BLE. Putting a second C5 on BLE would spend money for zero benefit.

## Promiscuous mode — confirmed

Confirmed from Espressif's official **ESP32-C5 datasheet**: the Wi-Fi MAC feature
list explicitly includes **promiscuous mode**, and the chip has a fully-specified
**5 GHz radio** (separate characterized 5 GHz TX/RX sections, 802.11ax/ac). Because
promiscuous mode is a property of the MAC (which drives both bands), capture works
on 5 GHz channels the same as 2.4 GHz — **at the hardware level**.

Residual (software, not a hardware risk): confirm the ESP-IDF version used exposes
5 GHz channel-selection while in promiscuous mode on this newish chip.

## LoRa — Wio-SX1262 for XIAO

- Same **Semtech SX1262** as the Heltec, so existing RadioLib code and LoRa params
  (SF9 / BW125 / CR7 / SYNC 0x34 / 915.0 MHz) carry over.
- 868/915 MHz, 22 dBm, 1.62 µA sleep, **plug-and-play** on the XIAO headers — kills
  the hand-mapping of ~6 LoRa pins into limited GPIO.
- Consumes D1–D5 (NSS=D4, DIO1=D1, NRST=D2, BUSY=D3, RF_SW=D5) + the shared SPI bus.

## Storage — SD on the C5 shared bus

Decision: **keep SD and LoRa co-located on the C5.** Moving SD to the C3 was
considered (it cleanly separates the two SPI buses) but rejected: it would invert
the data flow so that during a LoRa handoff the C3 has to pump the whole log back
to the C5 over UART, coupling the two MCUs tightly on every collection. Keeping SD
and LoRa on the same node keeps the handoff clean.

Consequence: SD shares the C5's single SPI bus with the LoRa radio (separate CS
pins — LoRa CS = D4/GPIO23, SD CS = D0/GPIO1). The one gotcha is the documented
Seeed shared-SPI pull-up conflict — mitigated by a buffered SD breakout
(Adafruit #4682) or by removing the bus pull-ups on a cheap module.

Alternatives documented in `BOM.md`: logging to the C5's onboard 8 MB flash via
LittleFS (no extra part, frees the SPI bus entirely) or a 16 MB SPI NOR flash chip —
both arguably better than SD for a sealed logger that offloads over LoRa.

## Cost

~$21–25 for the electronics core (C5 + C3 + Wio-SX1262 + SD + antenna) vs a Heltec
(~$25–40) that *still* needed a separate C3 for BLE. Full BOM with enclosure/battery
lands ~$45–60. The cost premise that drove this exploration holds up.

## Open items (carried forward)

1. **Bench:** Wio-SX1262 pass-through leaves the D0 + SPI pads reachable for the SD
   solder joints once the hat is seated (mechanical, needs the board in hand).
2. **Software:** current ESP-IDF exposes 5 GHz channel-set in promiscuous mode.
3. **Decision:** SD card vs C5 onboard flash (LittleFS) vs SPI NOR flash — pick the
   storage medium before finalizing the wiring.
