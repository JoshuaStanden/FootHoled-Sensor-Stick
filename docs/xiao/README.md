# XIAO Build — Design Docs

Research/design notes for a XIAO-based sensor stick (an alternative to the Heltec
workhorse on `main`). **Nothing here is built or verified on hardware yet** — these
are datasheet-grounded design references capturing the C5 + C3 line of thinking.

| Doc | What it covers |
|---|---|
| [ARCHITECTURE.md](ARCHITECTURE.md) | Why C5 + C3, board comparison, radio split, design decisions and open items |
| [WIRING.md](WIRING.md) | Verified pin map and GPIO numbers for both boards, the SD + LoRa + UART wiring, firmware `#define`s |
| [BOM.md](BOM.md) | Complete bill of materials, cost estimate, storage alternatives |

## The build in one line

**XIAO ESP32-C5** (Wi-Fi 2.4+5 GHz sniff + SD + LoRa) **+ XIAO ESP32-C3** (BLE
scanner over UART) **+ Wio-SX1262** LoRa hat **+** 3.3 V SPI microSD — cheaper than
a Heltec and gains 5 GHz capture.
