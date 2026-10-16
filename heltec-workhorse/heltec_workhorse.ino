/*
 * sensor-node :: Heltec WiFi LoRa 32 V4 (ESP32-S3 + SX1262)  —  WORKHORSE
 * (the "workhorse" is this sensor's main board; the "collector" is the
 *  SEPARATE piece of hardware this node uploads to over LoRa.)
 * ----------------------------------------------------------------------------
 * Roles on this one board:
 *   1. Wi-Fi scanner   : promiscuous 2.4 GHz sniffing of nearby management
 *                        frames -> unique client/AP MACs + RSSI.
 *   2. UART ingest     : receives "BT,..." lines from the XIAO ESP32-C3.
 *   3. Logger          : buffers observations in RTC memory (survives a crash
 *                        / reset and is replayed on boot) and writes them to
 *                        /sensor.log in BATCHES. Primary store is an EXTERNAL
 *                        microSD card (gigabytes); if no card is present it
 *                        falls back to on-board flash (LittleFS, ~16 MB).
 *                        Either way it survives power loss.
 *   4. Handoff         : hands the log to the COLLECTOR node over EITHER of two
 *                        transports, chosen by the collector:
 *                          - LoRa  : long range (100s m–km), slow. Used when the
 *                                    collector is far. Separate SX1262 radio.
 *                          - BLE   : short range (~10–30 ft), fast. Used when the
 *                                    collector is brought close. Shares the
 *                                    2.4 GHz front-end with Wi-Fi.
 *                        The sensor idles LISTENING ON BOTH (LoRa in receive +
 *                        BLE advertising) and answers whichever the collector
 *                        initiates. Transfer is framed + CRC32-verified; the log
 *                        is deleted ONLY after the collector confirms a good CRC.
 *   5. OLED + battery  : status display + 18650 voltage monitoring.
 *
 * Board target in Arduino IDE: "Heltec WiFi LoRa 32(V3)"  <-- V4 is pin/radio
 *   compatible with V3; the V3 board definition works. (As of now there is no
 *   separate V4 entry; the SX1262 pin map is identical.)
 *   Core: "Heltec ESP32 Series Dev-boards" by Heltec Automation.
 *
 * Libraries:
 *   - RadioLib            (SX1262 driver — robust, actively maintained)
 *   - U8g2                (OLED, SSD1306 128x64)
 *   - NimBLE-Arduino      (BLE server — lighter than stock BLE, important with
 *                          Wi-Fi also resident)
 *   - LittleFS            (bundled with esp32 core)
 *   - SD + SPI            (bundled with esp32 core; microSD over a 2nd SPI bus)
 *
 * ------------------------------- WIRING -------------------------------------
 * SX1262 LoRa (on-board, fixed by Heltec V3/V4 layout):
 *   NSS=8  SCK=9  MOSI=10  MISO=11  RST=12  BUSY=13  DIO1=14
 * microSD module (EXTERNAL, on a SECOND SPI bus — do NOT reuse LoRa's pins):
 *   SD_SCK=4  SD_MISO=5  SD_MOSI=6  SD_CS=7
 *   Power the module from 3V3 (the Vext 'Ve' rail is fine). Add a ~10uF cap
 *   across the module's 3V3/GND close to the socket: SD write bursts can pull
 *   100mA+ and brown out a battery node without local decoupling.
 * OLED SSD1306 (on-board, I2C):
 *   SDA=17  SCL=18  RST=21     (power gated by Vext_Ctrl = GPIO36, LOW = ON)
 * Battery sense (on-board divider):
 *   ADC on GPIO1, enabled by ADC_Ctrl = GPIO37 (drive LOW to read)
 * UART link to XIAO:
 *   Heltec RX  <--- XIAO TX (GPIO21/D6)
 *   Heltec TX  ---> XIAO RX (GPIO20/D7)   (optional)
 *   Common GND <--> XIAO GND              (REQUIRED)
 *   -> We use UART1 on GPIO45 (RX) / GPIO46 (TX); both are free header pins.
 *      Adjust LINK_RX_PIN / LINK_TX_PIN if you route to different pins.
 *
 * ----------------------------- LoRa / REGION --------------------------------
 * Set LORA_FREQ to your legal ISM band:  US=915.0, EU=868.0, Asia=433.0 MHz.
 * The collector node MUST use the same freq / SF / BW / sync word.
 *
 * =================== SHARED HANDOFF CONTRACT (collector impl spec) ===========
 * Both transports move the SAME log file with the SAME verified handshake. The
 * future collector (a Waveshare ESP32-C3 + LoRa module) implements the OTHER
 * side of exactly this. Keep both ends in sync with this block.
 *
 * Transfer phases (collector drives, sensor answers):
 *   1. REQUEST  collector -> sensor : "GET"           (begin a collection)
 *   2. HELLO    sensor -> collector : "HELLO,<id>,<totalBytes>,<crc32hex>"
 *   3. DATA     sensor -> collector : framed chunks of the raw log bytes
 *   4. END      sensor -> collector : "END"
 *   5. CONFIRM  collector -> sensor : "DONE,<crc32hex>" if its computed CRC of
 *                                     the received bytes matches HELLO's CRC
 *   6. sensor deletes the log ONLY on a matching DONE. Any mismatch, timeout or
 *      dropped link => sensor keeps the log for the next attempt.
 *
 * LoRa framing : each DATA packet is "D,<seq>,<base64-or-raw-bytes>"; small
 *   packets (<=200 B payload), sender waits for a per-chunk "A,<seq>" ack and
 *   resends on timeout (handles LoRa packet loss). seq lets the collector
 *   detect gaps and reassemble in order.
 * BLE framing  : BLE's connection layer already gives ordering + reliability +
 *   MTU-sized chunking, so DATA is just raw bytes streamed over a NOTIFY
 *   characteristic; no per-chunk ack/seq needed. Control messages (GET / DONE)
 *   arrive on a separate WRITE characteristic.
 *
 * BLE identifiers (collector scans for SERVICE_UUID):
 *   SERVICE_UUID  = see BLE_SVC_UUID below
 *   DATA  char    = BLE_DATA_UUID  (NOTIFY: sensor -> collector log stream)
 *   CTRL  char    = BLE_CTRL_UUID  (WRITE : collector -> sensor commands)
 * BLE link is bonded/encrypted (payload is MAC addresses); pair on first use.
 * ============================================================================
 */

// ===================== BOARD SELECT (required) ==============================
// Pick your board by UNCOMMENTING exactly ONE line below. Both are the same
// ESP32-S3 + SX1262 design; the only difference is the OLED:
//   BOARD_WIFI_LORA_32_V4     — has the on-board 0.96" OLED (status on screen)
//   BOARD_WIRELESS_STICK_LITE — no OLED (status goes to the serial console)
// Both are left commented on purpose so a build FAILS until you choose, rather
// than silently flashing the wrong variant.
//
// #define BOARD_WIFI_LORA_32_V4
// #define BOARD_WIRELESS_STICK_LITE

#if defined(BOARD_WIFI_LORA_32_V4) && defined(BOARD_WIRELESS_STICK_LITE)
#error "Select ONLY ONE board above (both are defined)."
#elif defined(BOARD_WIFI_LORA_32_V4)
#define HAS_OLED 1
#define BOARD_NAME "V4"
#elif defined(BOARD_WIRELESS_STICK_LITE)
#define HAS_OLED 0
#define BOARD_NAME "StickLite"
#else
#error "No board selected: uncomment BOARD_WIFI_LORA_32_V4 or BOARD_WIRELESS_STICK_LITE at the top of this file."
#endif
// ============================================================================

#include <Arduino.h>
#include <RadioLib.h>
#if HAS_OLED
#include <U8g2lib.h>
#endif
#include <Wire.h>
#include "FS.h"
#include <LittleFS.h>
#include <SPI.h>
#include <SD.h>
#include <NimBLEDevice.h>
#include "WiFi.h"
#include "esp_wifi.h"
#include "esp_rom_crc.h"   // esp_rom_crc32_le() for the file checksum

// ============================ CONFIG =======================================
static const float   LORA_FREQ      = 915.0;   // MHz — CHANGE for your region
static const float   LORA_BW        = 125.0;   // kHz
static const uint8_t LORA_SF        = 9;       // spreading factor 7..12
static const uint8_t LORA_CR        = 7;       // coding rate 4/5..4/8 -> 5..8
static const uint8_t LORA_SYNC      = 0x34;    // private network sync word
static const int8_t  LORA_POWER     = 20;      // dBm (SX1262 up to ~22)

static const uint32_t UART_BAUD     = 115200;
static const int LINK_RX_PIN        = 45;      // from XIAO TX
static const int LINK_TX_PIN        = 46;      // to   XIAO RX

static const char* LOG_PATH         = "/sensor.log";
// Size guards differ by backend: flash is tiny so keep a tight cap; SD has
// gigabytes so the cap is huge (effectively a sanity limit, not a real one).
static const size_t LOG_MAX_BYTES_FLASH = 512 * 1024;         // ~0.5 MB
static const size_t LOG_MAX_BYTES_SD    = 256UL * 1024 * 1024; // 256 MB

// microSD on a SECOND SPI bus (NOT the LoRa bus 8..14).
static const int SD_SCK_PIN  = 4;
static const int SD_MISO_PIN = 5;
static const int SD_MOSI_PIN = 6;
static const int SD_CS_PIN   = 7;

static const uint32_t WIFI_CHANNEL_DWELL_MS = 300;  // hop dwell per channel
static const int  WIFI_RSSI_FLOOR   = -95;

// ---- Batched logging --------------------------------------------------------
// Observations accumulate in a buffer and are written to storage in one
// open/write/close per batch, instead of once per observation. This cuts SD
// wear and avoids a storage stall on every single packet.
//
// Crash resilience: the live buffer lives in RTC SLOW MEMORY, which survives a
// software reset / watchdog / crash reboot (it is NOT cleared on a warm boot).
// On boot we replay any buffer left behind by the previous run before logging
// anew, so a crash loses nothing that was already buffered. (A true power cut
// still loses the current buffer — RTC memory needs the rail to hold up — but
// the short flush interval below keeps that window tiny.)
static const size_t LOG_FLUSH_BYTES   = 1024;   // flush once buffer >= this
static const uint32_t LOG_FLUSH_MS     = 250;   // ...or at least this often
                                                // (short: caps power-cut loss)
// RTC slow memory is only 8 KB total and is SHARED with the ULP coprocessor
// and some system use, so keep our slice small. 1.5 KB leaves ample headroom.
// This bounds a single batch, so LOG_FLUSH_BYTES must be < this.
static const size_t RTC_BUF_CAP        = 1536;
static const uint32_t RTC_MAGIC        = 0x5E501061;  // "sensor log" marker

// ---- Wi-Fi sniffer -> loop handoff ------------------------------------------
// The promiscuous callback runs in Wi-Fi driver context and must stay tiny and
// non-blocking. It pushes formatted lines into a FreeRTOS queue; the main loop
// drains the queue into the log buffer. A full queue drops the newest line
// (counted) rather than blocking the radio.
static const int WIFI_QUEUE_DEPTH     = 64;
static const int WIFI_LINE_MAX        = 48;     // "WIFI,<mac>,<rssi>,ch<n>\0"

// Transfer protocol (see SHARED HANDOFF CONTRACT in the banner).
static const char* NODE_ID          = "sensor-01";

// LoRa transfer tuning
static const int      LORA_CHUNK_BYTES = 180;   // raw bytes per DATA packet
static const uint32_t LORA_ACK_TIMEOUT = 1200;  // ms to wait for per-chunk ack
static const int      LORA_MAX_RETRY   = 5;     // resends before giving up
static const uint32_t LORA_SESS_TIMEOUT = 8000; // ms overall idle guard

// BLE identifiers — the collector scans for BLE_SVC_UUID. These are random
// 128-bit UUIDs for this project; the collector must use the identical values.
#define BLE_SVC_UUID   "6b1a0001-9d4e-4c2a-8f33-7c2e5a9b1d10"
#define BLE_DATA_UUID  "6b1a0002-9d4e-4c2a-8f33-7c2e5a9b1d10"  // NOTIFY
#define BLE_CTRL_UUID  "6b1a0003-9d4e-4c2a-8f33-7c2e5a9b1d10"  // WRITE
static const char* BLE_ADV_NAME     = "sensor-01";             // advertised name
static const int   BLE_CHUNK_BYTES  = 180;      // under default MTU; MTU raised
                                                // at connect so this is safe

// Battery (LiPo/18650 single cell) voltage thresholds for display.
static const int BATT_ADC_PIN       = 1;
static const int BATT_ADC_CTRL_PIN  = 37;   // drive LOW to enable divider read
static const int VEXT_CTRL_PIN      = 36;   // LOW = enable OLED/peripheral rail

// ============================ GLOBALS ======================================
// SX1262 pin map for Heltec V3/V4:  (NSS, DIO1, RST, BUSY)
SX1262 radio = new Module(8, 14, 12, 13);

// OLED 128x64 SSD1306 over hardware I2C (SDA=17, SCL=18, RST=21) — V4 only.
#if HAS_OLED
U8G2_SSD1306_128X64_NONAME_F_HW_I2C oled(U8G2_R0, /*reset=*/21, /*scl=*/18, /*sda=*/17);
#endif

HardwareSerial LinkSerial(1);  // UART1 for the XIAO link

// ---- Storage backend ------------------------------------------------------
// Dedicated SPI bus for the SD card so it never collides with the LoRa radio
// (which owns the board's other SPI bus on pins 8..14).
SPIClass sdSPI(HSPI);
// logFS points at whichever filesystem is active. All logging goes through it,
// so the rest of the code does not care whether it is SD or internal flash.
fs::FS*  logFS      = nullptr;   // &SD or &LittleFS
bool     usingSD    = false;     // true if the microSD mounted successfully
size_t   logCapBytes = LOG_MAX_BYTES_FLASH;

volatile uint32_t wifiSeen = 0;
volatile uint32_t btSeen   = 0;
volatile uint32_t wifiDropped = 0;   // observations lost to a full queue
uint32_t          lastDisplay = 0;
uint32_t          lastChannelHop = 0;
uint8_t           wifiChannel = 1;
bool              uploading   = false;         // true during ANY handoff

// ---- BLE handoff state ------------------------------------------------------
NimBLEServer*         bleServer   = nullptr;
NimBLECharacteristic* bleDataChar = nullptr;   // NOTIFY: log stream out
NimBLECharacteristic* bleCtrlChar = nullptr;   // WRITE : collector commands
volatile bool         bleConnected    = false;
volatile bool         bleGetRequested = false; // collector wrote "GET"
String                bleDoneCrc;              // CRC the collector reported
volatile bool         bleDoneReceived = false; // collector wrote "DONE,<crc>"
const char*           activeXport = "idle";    // "idle" | "LoRa" | "BLE"

// ---- Batched-write buffer in RTC SLOW MEMORY -------------------------------
// RTC_NOINIT_ATTR survives software reset / watchdog / crash (not a real power
// loss). We keep a tiny header (magic + used length) so boot code can tell a
// valid left-over buffer from uninitialised RTC garbage.
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t rtcUsed;              // bytes currently staged
RTC_NOINIT_ATTR char     rtcBuf[RTC_BUF_CAP];  // the staged log text

uint32_t lastFlush   = 0;        // millis() of last flush

// ---- Wi-Fi ISR -> loop queue ------------------------------------------------
QueueHandle_t wifiQueue = nullptr;

// ---- small helper to timestamp lines (seconds since boot; collector can
//      re-stamp with real time if it has an RTC/network). -------------------
static uint32_t nowSecs() { return millis() / 1000UL; }

// ============================ STORAGE ======================================
// Try microSD first (primary). If it is missing or fails, fall back to the
// on-board flash filesystem so logging never silently stops.
void storageBegin() {
  usingSD = false;

  // Bring up the second SPI bus on the SD pins, then try to mount the card.
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
  if (SD.begin(SD_CS_PIN, sdSPI)) {
    uint8_t type = SD.cardType();
    if (type != CARD_NONE) {
      usingSD     = true;
      logFS       = &SD;
      logCapBytes = LOG_MAX_BYTES_SD;
      uint64_t mb = SD.cardSize() / (1024ULL * 1024ULL);
      Serial.printf("[STORAGE] microSD mounted (%llu MB) — primary\n", mb);
      return;
    }
  }

  // No usable card: fall back to internal flash.
  Serial.println("[STORAGE] no microSD — falling back to internal flash");
  if (!LittleFS.begin(true)) {
    Serial.println("[STORAGE] LittleFS mount ALSO failed — logging disabled");
    logFS = nullptr;
    return;
  }
  logFS       = &LittleFS;
  logCapBytes = LOG_MAX_BYTES_FLASH;
  Serial.println("[STORAGE] internal flash mounted — fallback");
}

// ============================ LOGGING ======================================
// Lines are appended to a fixed byte buffer in RTC memory (rtcBuf/rtcUsed) and
// written to storage in batches by flushLog(). The RTC buffer survives a crash
// reboot, so rtcBufInit() can replay a buffer left behind by the previous run.

static size_t fileBytes = 0;   // cached size of the on-disk log

// Validate the RTC buffer at boot. If the magic is intact, a previous run left
// staged data here (crash/reset) and we keep it to flush on the next write.
// Otherwise this is a cold/garbage RTC state, so start empty.
void rtcBufInit(bool& recovered) {
  recovered = false;
  if (rtcMagic == RTC_MAGIC && rtcUsed <= RTC_BUF_CAP) {
    if (rtcUsed > 0) recovered = true;         // genuine staged data survived
  } else {
    rtcMagic = RTC_MAGIC;                       // claim/clear the region
    rtcUsed  = 0;
  }
}

// Write the buffered bytes to storage in a single open/append/close, then
// clear the buffer. Safe to call when the buffer is empty (no-op).
void flushLog() {
  if (!logFS || rtcUsed == 0) { lastFlush = millis(); return; }
  File f = logFS->open(LOG_PATH, FILE_APPEND);
  if (!f) {
    Serial.println("[LOG] flush open failed; keeping buffer for retry");
    return;                       // keep rtcBuf; try again next cycle
  }
  size_t wrote = f.write((const uint8_t*)rtcBuf, rtcUsed);
  f.close();
  fileBytes += wrote;
  rtcUsed = 0;
  lastFlush = millis();
}

// Append one observation to the RTC buffer as "<secs>,<line>\n". The record is
// written straight into rtcBuf (no fixed scratch), so long BT lines (enriched
// service data) are never clipped. If a single record is larger than the whole
// RTC buffer it is dropped rather than corrupting the buffer.
void logLine(const String& line) {
  if (!logFS) return;                         // storage unavailable

  char prefix[16];
  int pn = snprintf(prefix, sizeof(prefix), "%lu,", (unsigned long)nowSecs());
  if (pn <= 0) return;
  size_t need = (size_t)pn + line.length() + 1;   // prefix + line + '\n'
  if (need > RTC_BUF_CAP) return;                 // pathological; drop

  if (rtcUsed + need > RTC_BUF_CAP) flushLog();   // make room
  if (rtcUsed + need > RTC_BUF_CAP) return;       // flush failed; drop safely

  memcpy(rtcBuf + rtcUsed, prefix, pn);                  rtcUsed += pn;
  memcpy(rtcBuf + rtcUsed, line.c_str(), line.length()); rtcUsed += line.length();
  rtcBuf[rtcUsed++] = '\n';
}

// Decide whether it is time to flush (size- or time-triggered).
void maybeFlush() {
  if (rtcUsed >= LOG_FLUSH_BYTES ||
      (rtcUsed > 0 && millis() - lastFlush >= LOG_FLUSH_MS)) {
    flushLog();
  }
}

// Effective log size = bytes already on disk + bytes waiting in the buffer.
size_t logSize() {
  return fileBytes + rtcUsed;
}

// Read the true on-disk size once (used at boot and after a remove).
size_t diskLogSize() {
  if (!logFS) return 0;
  File f = logFS->open(LOG_PATH, FILE_READ);
  if (!f) return 0;
  size_t s = f.size();
  f.close();
  return s;
}

// ============================ TRANSFER CORE ================================
// Shared by both transports: compute the file CRC, and clear the log once the
// collector has confirmed a matching CRC.

// Streaming CRC32 over the whole on-disk log. Uses the standard convention
// (seed = ~0, result one's-complemented) so ANY conventional CRC32 on the
// collector side produces the same value — important since the collector is a
// separate device. esp_rom_crc32_le itself one's-complements in/out, so we feed
// ~crc through it and complement the final result.
uint32_t fileCrc32(size_t& outBytes) {
  outBytes = 0;
  uint32_t crc = 0xFFFFFFFF;
  if (!logFS) return crc ^ 0xFFFFFFFF;
  File f = logFS->open(LOG_PATH, FILE_READ);
  if (!f) return crc ^ 0xFFFFFFFF;
  uint8_t buf[256];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    // esp_rom_crc32_le expects/returns a complemented running value.
    crc = esp_rom_crc32_le(crc, buf, n);
    outBytes += n;
  }
  f.close();
  return crc;   // esp_rom_crc32_le already returns the finished CRC32
}

// Delete the log after a verified handoff and reset the size cache.
void clearLogAfterConfirm(const char* via) {
  if (logFS) logFS->remove(LOG_PATH);
  fileBytes = 0;
  Serial.printf("[XFER] %s confirmed; log cleared\n", via);
}

// ============================ WIFI SNIFFER =================================
// Promiscuous callback: pull the source MAC + RSSI from the 802.11 header,
// format a line, and push it to the queue. Non-blocking: if the queue is full
// the line is dropped (and counted) rather than stalling the Wi-Fi driver.

void IRAM_ATTR wifiSnifferCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  const int rssi = pkt->rx_ctrl.rssi;
  if (rssi < WIFI_RSSI_FLOOR) return;

  // 802.11 header: addr2 (transmitter) is bytes 10..15 of the frame body.
  const uint8_t* p = pkt->payload;
  const uint8_t* mac = p + 10;

  char line[WIFI_LINE_MAX];
  snprintf(line, sizeof(line),
           "WIFI,%02x:%02x:%02x:%02x:%02x:%02x,%d,ch%u",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           rssi, (unsigned)wifiChannel);

  wifiSeen++;
  BaseType_t hpw = pdFALSE;
  if (wifiQueue == nullptr ||
      xQueueSendFromISR(wifiQueue, line, &hpw) != pdTRUE) {
    wifiDropped++;                 // queue full: drop newest, keep radio alive
  }
  if (hpw) portYIELD_FROM_ISR();
}

void startWifiSniffer() {
  // Create the ISR -> loop queue before enabling the callback.
  if (wifiQueue == nullptr) {
    wifiQueue = xQueueCreate(WIFI_QUEUE_DEPTH, WIFI_LINE_MAX);
    if (wifiQueue == nullptr) Serial.println("[WiFi] queue alloc failed!");
  }
  WiFi.mode(WIFI_MODE_NULL);
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  esp_wifi_start();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&wifiSnifferCb);
  esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
}

void hopWifiChannel() {
  wifiChannel++;
  if (wifiChannel > 13) wifiChannel = 1;      // 1..13 (region dependent)
  esp_wifi_set_channel(wifiChannel, WIFI_SECOND_CHAN_NONE);
}

// ============================ BATTERY ======================================
float readBatteryVolts() {
  pinMode(BATT_ADC_CTRL_PIN, OUTPUT);
  digitalWrite(BATT_ADC_CTRL_PIN, LOW);       // enable divider
  delay(10);
  // Heltec V3/V4 divider factor ~ 4.9; analogReadMilliVolts handles ref cal.
  uint32_t mv = analogReadMilliVolts(BATT_ADC_PIN);
  digitalWrite(BATT_ADC_CTRL_PIN, HIGH);      // disable to save power
  return (mv * 4.9f) / 1000.0f;
}

// ============================ STATUS =======================================
// On the V4 this draws to the OLED; on the Wireless Stick Lite (no screen) it
// prints the same status to the serial console instead.
void drawStatus() {
  float vb = readBatteryVolts();
  const char* state = uploading ? activeXport
                    : (bleConnected ? "BLE~" : "listen");
#if HAS_OLED
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 10, "sensor-node  " BOARD_NAME);
  char l[24];
  snprintf(l, sizeof(l), "WiFi:%lu drop:%lu",
           (unsigned long)wifiSeen, (unsigned long)wifiDropped);
  oled.drawStr(0, 24, l);
  snprintf(l, sizeof(l), "BT   seen: %lu", (unsigned long)btSeen);
  oled.drawStr(0, 36, l);
  snprintf(l, sizeof(l), "Log(%s):%uB", usingSD ? "SD" : "FL", (unsigned)logSize());
  oled.drawStr(0, 48, l);
  snprintf(l, sizeof(l), "Batt:%.2fV %s", vb, state);
  oled.drawStr(0, 60, l);
  oled.sendBuffer();
#else
  // No screen: emit one status line to serial (visible when USB-tethered).
  Serial.printf("[STATUS] WiFi:%lu drop:%lu BT:%lu Log(%s):%uB Batt:%.2fV %s\n",
                (unsigned long)wifiSeen, (unsigned long)wifiDropped,
                (unsigned long)btSeen, usingSD ? "SD" : "FL",
                (unsigned)logSize(), vb, state);
#endif
}

// ============================ LoRa TRANSFER ================================
// Implements the shared contract over LoRa: HELLO -> framed+acked chunks ->
// END -> wait for DONE,<crc>. Deletes the log only on a matching DONE.
// Called when a "GET" is heard on LoRa. Returns true if the log was cleared.
bool loraTransfer() {
  if (!logFS) return false;
  activeXport = "LoRa";
  flushLog();                                   // include buffered lines

  size_t total = 0;
  uint32_t crc = fileCrc32(total);
  char crcHex[9]; snprintf(crcHex, sizeof(crcHex), "%08x", crc);

  // HELLO
  radio.transmit(String("HELLO,") + NODE_ID + "," + total + "," + crcHex);

  // DATA: fixed-size raw chunks, each acked by "A,<seq>" before advancing.
  File f = logFS->open(LOG_PATH, FILE_READ);
  if (!f) return false;
  uint8_t buf[LORA_CHUNK_BYTES];
  uint32_t seq = 0;
  bool ok = true;
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    String pkt = String("D,") + seq + ",";
    pkt.reserve(pkt.length() + n);
    for (size_t i = 0; i < n; i++) pkt += (char)buf[i];  // raw bytes

    int retry = 0;
    bool acked = false;
    while (retry <= LORA_MAX_RETRY && !acked) {
      radio.transmit(pkt);
      String resp;
      if (radio.receive(resp, LORA_ACK_TIMEOUT) == RADIOLIB_ERR_NONE &&
          resp == (String("A,") + seq)) {
        acked = true;
      } else {
        retry++;
      }
    }
    if (!acked) { ok = false; break; }          // link lost; keep the log
    seq++;
  }
  f.close();
  if (!ok) { radio.startReceive(); activeXport = "idle"; return false; }

  radio.transmit("END");

  // Wait for the collector's verified confirmation.
  String resp;
  bool cleared = false;
  if (radio.receive(resp, LORA_SESS_TIMEOUT) == RADIOLIB_ERR_NONE &&
      resp == (String("DONE,") + crcHex)) {
    clearLogAfterConfirm("LoRa");
    cleared = true;
  } else {
    Serial.println("[LoRa] no/!match DONE; keeping log for next pass");
  }
  radio.startReceive();
  activeXport = "idle";
  return cleared;
}

// ============================ BLE SERVER ===================================
class CtrlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    std::string v = c->getValue();
    if (v.rfind("GET", 0) == 0) {
      bleGetRequested = true;
    } else if (v.rfind("DONE,", 0) == 0) {
      bleDoneCrc = String(v.substr(5).c_str());
      bleDoneReceived = true;
    }
  }
};

class SrvCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    bleConnected = true;
    s->updateConnParams(info.getConnHandle(), 6, 12, 0, 200);
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    bleConnected = false;
    NimBLEDevice::startAdvertising();           // re-advertise for next time
  }
};

void startBleServer() {
  NimBLEDevice::init(BLE_ADV_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(247);                    // large MTU for fast streaming
  // Bonded + encrypted link (payload is MAC addresses).
  NimBLEDevice::setSecurityAuth(true, false, true);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new SrvCallbacks());

  NimBLEService* svc = bleServer->createService(BLE_SVC_UUID);
  bleDataChar = svc->createCharacteristic(
      BLE_DATA_UUID, NIMBLE_PROPERTY::NOTIFY);
  bleCtrlChar = svc->createCharacteristic(
      BLE_CTRL_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC);
  bleCtrlChar->setCallbacks(new CtrlCallbacks());
  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SVC_UUID);
  adv->enableScanResponse(true);
  NimBLEDevice::startAdvertising();
}

// ============================ BLE TRANSFER =================================
// BLE gives ordering/reliability/chunking, so DATA is raw bytes over NOTIFY.
// Called when the connected collector writes "GET". Returns true if cleared.
bool bleTransfer() {
  if (!logFS || !bleConnected) return false;
  activeXport = "BLE";
  flushLog();

  size_t total = 0;
  uint32_t crc = fileCrc32(total);
  char crcHex[9]; snprintf(crcHex, sizeof(crcHex), "%08x", crc);

  // HELLO over the control channel's notify-less path: send as the first
  // DATA notification so the collector sees it in-stream.
  String hello = String("HELLO,") + NODE_ID + "," + total + "," + crcHex;
  bleDataChar->setValue((uint8_t*)hello.c_str(), hello.length());
  bleDataChar->notify();

  File f = logFS->open(LOG_PATH, FILE_READ);
  if (!f) { activeXport = "idle"; return false; }
  uint8_t buf[BLE_CHUNK_BYTES];
  while (f.available() && bleConnected) {
    size_t n = f.read(buf, sizeof(buf));
    bleDataChar->setValue(buf, n);
    bleDataChar->notify();
    delay(8);                                   // pace to the connection interval
  }
  f.close();
  if (!bleConnected) { activeXport = "idle"; return false; }  // dropped; keep log

  // END marker, then wait for the collector's verified DONE write.
  const char* endMsg = "END";
  bleDataChar->setValue((uint8_t*)endMsg, 3);
  bleDataChar->notify();

  bleDoneReceived = false;
  uint32_t t0 = millis();
  bool cleared = false;
  while (millis() - t0 < LORA_SESS_TIMEOUT && bleConnected) {
    if (bleDoneReceived) {
      if (bleDoneCrc.equalsIgnoreCase(crcHex)) {
        clearLogAfterConfirm("BLE");
        cleared = true;
      } else {
        Serial.println("[BLE] DONE CRC mismatch; keeping log");
      }
      break;
    }
    delay(10);
  }
  activeXport = "idle";
  return cleared;
}

// ============================ SETUP ========================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("[Heltec] workhorse booting (" BOARD_NAME ")");

  // Enable the switched peripheral rail (Vext). On both boards this powers
  // external 3V3 peripherals (and the OLED on the V4). LOW = ON on V3/V4 (R2).
  pinMode(VEXT_CTRL_PIN, OUTPUT);
  digitalWrite(VEXT_CTRL_PIN, LOW);
  delay(50);

#if HAS_OLED
  // OLED (V4 only)
  oled.begin();
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  oled.drawStr(0, 12, "Booting...");
  oled.sendBuffer();
#endif

  // Storage: microSD primary, internal flash fallback.
  storageBegin();
  fileBytes = diskLogSize();       // count any log that survived a reboot

  // Recover any observations the previous run had staged in RTC memory before
  // it crashed/reset, and flush them to storage now so they are not lost.
  bool recovered = false;
  rtcBufInit(recovered);
  if (recovered) {
    Serial.printf("[STORAGE] recovered %u B staged in RTC from last run\n",
                  (unsigned)rtcUsed);
    flushLog();
  }
  lastFlush = millis();
  Serial.printf("[STORAGE] active=%s, log=%u B\n",
                usingSD ? "microSD" : "flash", (unsigned)logSize());

  // UART link to XIAO
  LinkSerial.begin(UART_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);

  // LoRa radio (SX1262)
  int st = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_POWER);
  if (st == RADIOLIB_ERR_NONE) {
    Serial.println("[LoRa] init OK");
  } else {
    Serial.printf("[LoRa] init failed, code %d\n", st);
  }
  radio.startReceive();   // listen for the collector's beacon

  // Wi-Fi sniffer
  startWifiSniffer();
  Serial.println("[WiFi] promiscuous sniffer started");

  // BLE handoff server: advertise so a nearby collector can pull via BLE.
  startBleServer();
  Serial.println("[BLE] advertising; ready for BLE handoff");
}

// ============================ MAIN LOOP ====================================
void loop() {
  // 1) Drain the Wi-Fi queue the sniffer ISR fills, into the log buffer.
  //    Bounded per iteration so one burst can't starve the rest of the loop.
  if (wifiQueue) {
    char line[WIFI_LINE_MAX];
    int drained = 0;
    while (drained < WIFI_QUEUE_DEPTH &&
           xQueueReceive(wifiQueue, line, 0) == pdTRUE) {
      logLine(String(line));
      drained++;
    }
  }

  // 2) Drain the UART from the XIAO (BLE observations + heartbeats).
  while (LinkSerial.available()) {
    String msg = LinkSerial.readStringUntil('\n');
    msg.trim();
    if (msg.length() == 0) continue;
    if (msg.startsWith("BT,")) {
      btSeen++;
      logLine(msg);                 // already in "BT,mac,rssi,name" form
    } else if (msg.startsWith("SYS,")) {
      Serial.print("[XIAO] "); Serial.println(msg);
    }
  }

  // 3) Hop Wi-Fi channel on a timer so we cover the band.
  if (millis() - lastChannelHop > WIFI_CHANNEL_DWELL_MS) {
    hopWifiChannel();
    lastChannelHop = millis();
  }

  // 3b) Flush the RAM log buffer to storage in a batch (size- or time-based).
  maybeFlush();

  // 4) HANDOFF: answer whichever transport the collector initiates.
  //    (a) BLE — the collector connected and wrote "GET".
  if (bleGetRequested) {
    bleGetRequested = false;
    uploading = true;
    esp_wifi_set_promiscuous(false);   // BLE shares the 2.4 GHz front-end
    bleTransfer();
    esp_wifi_set_promiscuous(true);
    uploading = false;
  }

  //    (b) LoRa — the collector sent "GET" over the air. Peek briefly.
  //        Skip the peek while a BLE collector is connected, to keep the loop
  //        responsive to the active BLE session.
  if (!bleConnected) {
    String rx;
    if (radio.receive(rx, 20) == RADIOLIB_ERR_NONE && rx.startsWith("GET")) {
      uploading = true;
      esp_wifi_set_promiscuous(false);  // keep the loop free during the xfer
      loraTransfer();
      esp_wifi_set_promiscuous(true);
      uploading = false;
    }
  }

  // 5) Refresh OLED ~1 Hz, rotate log if it grows too large.
  if (millis() - lastDisplay > 1000) {
    drawStatus();
    lastDisplay = millis();
    if (logSize() > logCapBytes) {
      Serial.println("[LOG] size cap hit; clearing (collector should drain)");
      if (logFS) logFS->remove(LOG_PATH);   // simplest guard; on SD this cap
      fileBytes = 0;                         // is huge so it rarely triggers
      rtcUsed   = 0;                          // drop staged bytes too
    }
  }
}
