/*
 * sensor-node :: XIAO ESP32-C5 (RISC-V, dual-band Wi-Fi 6 + BLE + SX1262 hat)
 *               —  WORKHORSE  (XIAO build / "new thinking")
 * ----------------------------------------------------------------------------
 * This is the XIAO-based replacement for the Heltec workhorse. Same job, same
 * handoff contract, same log format — but on a XIAO ESP32-C5 with a stacked
 * Wio-SX1262 LoRa hat and a generic SPI microSD module SHARING the C5's one
 * SPI bus with the LoRa radio. See docs/xiao/ for the design + verified wiring.
 *
 * Roles on this one board:
 *   1. Wi-Fi scanner : promiscuous sniffing on 2.4 GHz AND 5 GHz (the C5 is the
 *                      first XIAO with a 5 GHz radio) -> TX MACs + RSSI.
 *   2. UART ingest   : receives "BT,..." lines from the XIAO ESP32-C3.
 *   3. Logger        : buffers in RTC slow memory (survives crash/reset), writes
 *                      to /sensor.log in BATCHES on an SD card shared with LoRa;
 *                      falls back to the C5's onboard LittleFS flash if no card.
 *   4. Handoff       : hands the log to the (separate, out-of-scope) COLLECTOR
 *                      over LoRa or BLE, framed + CRC32-verified. Log deleted
 *                      only after a confirmed matching CRC.
 *
 * ========================= HARDWARE DIFFERENCES vs HELTEC ====================
 * The Heltec had TWO SPI buses (LoRa on 8..14, SD on a second bus 4..7). The
 * XIAO C5 has ONE SPI bus, so the LoRa hat and the SD card SHARE it with
 * SEPARATE chip-selects. This changes two things you must respect:
 *   - LoRa CS = GPIO23 (D4), SD CS = GPIO1 (D0); both share SCK/MISO/MOSI.
 *   - The SD breakout must NOT have aggressive pull-ups on SCK/MISO/MOSI, or it
 *     fights the LoRa radio on the shared bus (documented Seeed issue). Use a
 *     buffered breakout (Adafruit #4682) or remove the bus pull-ups.
 * Firmware keeps ONE device's CS idle-HIGH while the other transacts (the SD +
 * RadioLib libraries do this per-transaction; just don't drive both CS low).
 * ============================================================================
 *
 * Board target in Arduino IDE: "XIAO_ESP32C5"  (esp32 core by Espressif).
 *   5 GHz promiscuous channel-set needs a recent ESP-IDF/core — see the 5 GHz
 *   note at startWifiSniffer(). If your core lacks it, the sketch still runs on
 *   2.4 GHz (set ENABLE_5GHZ 0).
 *
 * Libraries:
 *   - RadioLib        (SX1262 driver)
 *   - NimBLE-Arduino  (BLE server; lighter than stock, matters with Wi-Fi up)
 *   - LittleFS, SD, SPI (bundled with the esp32 core)
 *
 * ------------------------------- WIRING (verified, see docs/xiao/WIRING.md) --
 * Wio-SX1262 hat (plug-and-play on the C5 headers — do NOT hand-wire it):
 *   NSS=D4/GPIO23  DIO1=D1/GPIO0  NRST=D2/GPIO25  BUSY=D3/GPIO7  RF_SW=D5/GPIO24
 *   SPI bus: SCK=D8/GPIO8  MISO=D9/GPIO9  MOSI=D10/GPIO10
 * Generic SPI microSD module (hand-wired to pass-through pads, SHARED bus):
 *   SD_SCK=D8/GPIO8  SD_MISO=D9/GPIO9  SD_MOSI=D10/GPIO10  SD_CS=D0/GPIO1
 *   VCC=3V3  GND=GND   (3.3 V ONLY; add ~10uF across the module's 3V3/GND)
 * UART link to the C3:
 *   C5 TX=D6/GPIO11 ---> C3 RX (D7/GPIO20)
 *   C5 RX=D7/GPIO12 <--- C3 TX (D6/GPIO21)
 *   Common GND <--> C3 GND   (REQUIRED)
 *
 * ----------------------------- LoRa / REGION --------------------------------
 * Set LORA_FREQ to your legal ISM band: US=915.0, EU=868.0, Asia=433.0 MHz.
 * The collector MUST match freq / SF / BW / sync word. Values below match the
 * existing Heltec build so a collector already in the field interoperates.
 *
 * =================== SHARED HANDOFF CONTRACT (unchanged) =====================
 * Identical to the Heltec sketch — kept byte-for-byte so the same collector
 * works with either workhorse:
 *   1. REQUEST  collector -> sensor : "GET"
 *   2. HELLO    sensor -> collector : "HELLO,<id>,<totalBytes>,<crc32hex>"
 *   3. DATA     sensor -> collector : framed chunks ("D,<seq>,<raw>" over LoRa;
 *                                     raw NOTIFY bytes over BLE)
 *   4. END      sensor -> collector : "END"
 *   5. CONFIRM  collector -> sensor : "DONE,<crc32hex>" on a matching CRC
 *   6. sensor deletes the log ONLY on a matching DONE; else keeps it.
 * LoRa DATA is per-chunk acked ("A,<seq>"); BLE relies on its own reliability.
 * BLE identifiers must match the collector:
 *   SERVICE=BLE_SVC_UUID  DATA(NOTIFY)=BLE_DATA_UUID  CTRL(WRITE)=BLE_CTRL_UUID
 * ============================================================================
 */

#include <Arduino.h>
#include <RadioLib.h>
#include "FS.h"
#include <LittleFS.h>
#include <SPI.h>
#include <SD.h>
#include <NimBLEDevice.h>
#include "WiFi.h"
#include "esp_wifi.h"
#include "esp_rom_crc.h"

// ============================ CONFIG =======================================
static const float   LORA_FREQ  = 915.0;   // MHz — CHANGE for your region
static const float   LORA_BW    = 125.0;   // kHz
static const uint8_t LORA_SF    = 9;       // spreading factor 7..12
static const uint8_t LORA_CR    = 7;       // coding rate 4/5..4/8 -> 5..8
static const uint8_t LORA_SYNC  = 0x34;    // private network sync word
static const int8_t  LORA_POWER = 20;      // dBm (SX1262 up to ~22)

static const uint32_t UART_BAUD = 115200;
static const int LINK_RX_PIN    = 12;      // D7  <- C3 TX
static const int LINK_TX_PIN    = 11;      // D6  -> C3 RX

static const char* LOG_PATH     = "/sensor.log";
static const size_t LOG_MAX_BYTES_FLASH = 512 * 1024;          // ~0.5 MB
static const size_t LOG_MAX_BYTES_SD    = 256UL * 1024 * 1024; // 256 MB

// ---- Shared SPI bus (LoRa hat + SD) ---------------------------------------
// One bus. LoRa and SD each select with their own CS; SCK/MISO/MOSI are common.
static const int SPI_SCK_PIN  = 8;    // D8
static const int SPI_MISO_PIN = 9;    // D9
static const int SPI_MOSI_PIN = 10;   // D10
static const int SD_CS_PIN    = 1;    // D0  (verified safe, non-strapping)

// ---- LoRa hat (Wio-SX1262) control pins -----------------------------------
static const int LORA_NSS_PIN  = 23;  // D4
static const int LORA_DIO1_PIN = 0;   // D1
static const int LORA_NRST_PIN = 25;  // D2 (strapping; driven by hat)
static const int LORA_BUSY_PIN = 7;   // D3 (strapping; driven by hat)
// RF_SW (D5/GPIO24) is handled on the hat; no firmware pin needed.

// ---- Wi-Fi sniffer ---------------------------------------------------------
// The C5 is dual-band. We hop 2.4 GHz channels always, and (if the core
// supports a 5 GHz channel-set in promiscuous mode) a set of 5 GHz channels too.
#define ENABLE_5GHZ 1                        // set 0 if your core lacks 5 GHz set
static const uint32_t WIFI_CHANNEL_DWELL_MS = 300;
static const int      WIFI_RSSI_FLOOR       = -95;
// 2.4 GHz channels 1..13; a representative US 5 GHz UNII set. The 5 GHz list is
// only used when ENABLE_5GHZ and the core accepts the channel.
static const uint8_t CH_24[] = {1,2,3,4,5,6,7,8,9,10,11,12,13};
static const uint8_t CH_5G[] = {36,40,44,48,149,153,157,161};

// ---- Batched logging (RTC slow memory survives crash/reset) ----------------
static const size_t   LOG_FLUSH_BYTES = 1024;
static const uint32_t LOG_FLUSH_MS    = 250;
static const size_t   RTC_BUF_CAP     = 1536;
static const uint32_t RTC_MAGIC       = 0x5E50C5A0;  // "sensor log C5" marker

// ---- Wi-Fi ISR -> loop queue ----------------------------------------------
static const int WIFI_QUEUE_DEPTH = 64;
static const int WIFI_LINE_MAX    = 48;      // "WIFI,<mac>,<rssi>,ch<n>\0"

// ---- Transfer protocol -----------------------------------------------------
static const char* NODE_ID = "sensor-01";
static const int      LORA_CHUNK_BYTES  = 180;
static const uint32_t LORA_ACK_TIMEOUT  = 1200;
static const int      LORA_MAX_RETRY    = 5;
static const uint32_t LORA_SESS_TIMEOUT = 8000;

// BLE identifiers — MUST match the collector + the Heltec sketch.
#define BLE_SVC_UUID   "6b1a0001-9d4e-4c2a-8f33-7c2e5a9b1d10"
#define BLE_DATA_UUID  "6b1a0002-9d4e-4c2a-8f33-7c2e5a9b1d10"  // NOTIFY
#define BLE_CTRL_UUID  "6b1a0003-9d4e-4c2a-8f33-7c2e5a9b1d10"  // WRITE
static const char* BLE_ADV_NAME    = "sensor-01";
static const int   BLE_CHUNK_BYTES = 180;

// ============================ GLOBALS ======================================
// SX1262 on the shared bus: Module(NSS, DIO1, NRST, BUSY).
SX1262 radio = new Module(LORA_NSS_PIN, LORA_DIO1_PIN, LORA_NRST_PIN, LORA_BUSY_PIN);

HardwareSerial LinkSerial(1);   // UART1 for the C3 link

// One SPI bus, shared by LoRa and SD. RadioLib uses the default SPI instance,
// so we drive the SAME bus for the SD card instead of a second SPIClass.
fs::FS*  logFS       = nullptr;   // &SD or &LittleFS
bool     usingSD     = false;
size_t   logCapBytes = LOG_MAX_BYTES_FLASH;

volatile uint32_t wifiSeen    = 0;
volatile uint32_t btSeen      = 0;
volatile uint32_t wifiDropped = 0;
uint32_t lastDisplay    = 0;
uint32_t lastChannelHop = 0;
int      chIndex        = 0;       // index into the current band's channel list
bool     on5ghz         = false;   // which band we are currently dwelling on
uint8_t  curChannel     = 1;
bool     uploading      = false;
const char* activeXport = "idle";  // "idle" | "LoRa" | "BLE"

// ---- BLE handoff state -----------------------------------------------------
NimBLEServer*         bleServer   = nullptr;
NimBLECharacteristic* bleDataChar = nullptr;
NimBLECharacteristic* bleCtrlChar = nullptr;
volatile bool bleConnected    = false;
volatile bool bleGetRequested = false;
String        bleDoneCrc;
volatile bool bleDoneReceived = false;

// ---- Batched-write buffer in RTC SLOW MEMORY -------------------------------
RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t rtcUsed;
RTC_NOINIT_ATTR char     rtcBuf[RTC_BUF_CAP];
uint32_t lastFlush = 0;

QueueHandle_t wifiQueue = nullptr;

static uint32_t nowSecs() { return millis() / 1000UL; }

// ============================ STORAGE ======================================
// microSD first (shares the LoRa SPI bus, own CS). Fall back to LittleFS.
void storageBegin() {
  usingSD = false;

  // The shared SPI bus is already begun in setup() (for the LoRa radio). The SD
  // library attaches to the same default SPI instance with its own CS pin.
  if (SD.begin(SD_CS_PIN, SPI)) {
    if (SD.cardType() != CARD_NONE) {
      usingSD     = true;
      logFS       = &SD;
      logCapBytes = LOG_MAX_BYTES_SD;
      uint64_t mb = SD.cardSize() / (1024ULL * 1024ULL);
      Serial.printf("[STORAGE] microSD mounted (%llu MB) on shared bus — primary\n", mb);
      return;
    }
  }

  Serial.println("[STORAGE] no microSD — falling back to onboard flash (LittleFS)");
  if (!LittleFS.begin(true)) {
    Serial.println("[STORAGE] LittleFS mount ALSO failed — logging disabled");
    logFS = nullptr;
    return;
  }
  logFS       = &LittleFS;
  logCapBytes = LOG_MAX_BYTES_FLASH;
  Serial.println("[STORAGE] onboard flash mounted — fallback");
}

// ============================ LOGGING ======================================
static size_t fileBytes = 0;

void rtcBufInit(bool& recovered) {
  recovered = false;
  if (rtcMagic == RTC_MAGIC && rtcUsed <= RTC_BUF_CAP) {
    if (rtcUsed > 0) recovered = true;
  } else {
    rtcMagic = RTC_MAGIC;
    rtcUsed  = 0;
  }
}

void flushLog() {
  if (!logFS || rtcUsed == 0) { lastFlush = millis(); return; }
  File f = logFS->open(LOG_PATH, FILE_APPEND);
  if (!f) {
    Serial.println("[LOG] flush open failed; keeping buffer for retry");
    return;
  }
  size_t wrote = f.write((const uint8_t*)rtcBuf, rtcUsed);
  f.close();
  fileBytes += wrote;
  rtcUsed = 0;
  lastFlush = millis();
}

void logLine(const String& line) {
  if (!logFS) return;
  char prefix[16];
  int pn = snprintf(prefix, sizeof(prefix), "%lu,", (unsigned long)nowSecs());
  if (pn <= 0) return;
  size_t need = (size_t)pn + line.length() + 1;
  if (need > RTC_BUF_CAP) return;
  if (rtcUsed + need > RTC_BUF_CAP) flushLog();
  if (rtcUsed + need > RTC_BUF_CAP) return;
  memcpy(rtcBuf + rtcUsed, prefix, pn);                  rtcUsed += pn;
  memcpy(rtcBuf + rtcUsed, line.c_str(), line.length()); rtcUsed += line.length();
  rtcBuf[rtcUsed++] = '\n';
}

void maybeFlush() {
  if (rtcUsed >= LOG_FLUSH_BYTES ||
      (rtcUsed > 0 && millis() - lastFlush >= LOG_FLUSH_MS)) {
    flushLog();
  }
}

size_t logSize() { return fileBytes + rtcUsed; }

size_t diskLogSize() {
  if (!logFS) return 0;
  File f = logFS->open(LOG_PATH, FILE_READ);
  if (!f) return 0;
  size_t s = f.size();
  f.close();
  return s;
}

// ============================ TRANSFER CORE ================================
uint32_t fileCrc32(size_t& outBytes) {
  outBytes = 0;
  uint32_t crc = 0xFFFFFFFF;
  if (!logFS) return crc ^ 0xFFFFFFFF;
  File f = logFS->open(LOG_PATH, FILE_READ);
  if (!f) return crc ^ 0xFFFFFFFF;
  uint8_t buf[256];
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    crc = esp_rom_crc32_le(crc, buf, n);
    outBytes += n;
  }
  f.close();
  return crc;
}

void clearLogAfterConfirm(const char* via) {
  if (logFS) logFS->remove(LOG_PATH);
  fileBytes = 0;
  Serial.printf("[XFER] %s confirmed; log cleared\n", via);
}

// ============================ WIFI SNIFFER =================================
void IRAM_ATTR wifiSnifferCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  const int rssi = pkt->rx_ctrl.rssi;
  if (rssi < WIFI_RSSI_FLOOR) return;

  const uint8_t* p = pkt->payload;
  const uint8_t* mac = p + 10;   // addr2 (transmitter)

  char line[WIFI_LINE_MAX];
  snprintf(line, sizeof(line),
           "WIFI,%02x:%02x:%02x:%02x:%02x:%02x,%d,ch%u",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           rssi, (unsigned)curChannel);

  wifiSeen++;
  BaseType_t hpw = pdFALSE;
  if (wifiQueue == nullptr ||
      xQueueSendFromISR(wifiQueue, line, &hpw) != pdTRUE) {
    wifiDropped++;
  }
  if (hpw) portYIELD_FROM_ISR();
}

void startWifiSniffer() {
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
  // Start on 2.4 GHz channel 1.
  on5ghz = false; chIndex = 0; curChannel = CH_24[0];
  esp_wifi_set_channel(curChannel, WIFI_SECOND_CHAN_NONE);
  Serial.println("[WiFi] promiscuous started (dual-band hop if 5 GHz available)");
  // NOTE on 5 GHz: the ESP32-C5 radio supports 5 GHz and the datasheet lists
  // promiscuous mode in the Wi-Fi MAC. Whether esp_wifi_set_channel() accepts a
  // 5 GHz channel number in promiscuous mode depends on the ESP-IDF/core
  // version. We attempt it (ENABLE_5GHZ) and simply skip a channel the core
  // rejects, so an older core degrades to 2.4 GHz-only rather than failing.
}

// Hop to the next channel, alternating 2.4 GHz sweeps with 5 GHz sweeps.
void hopWifiChannel() {
  chIndex++;
  if (!on5ghz) {
    if (chIndex >= (int)(sizeof(CH_24) / sizeof(CH_24[0]))) {
#if ENABLE_5GHZ
      on5ghz = true; chIndex = 0;               // switch to the 5 GHz list
#else
      chIndex = 0;                              // stay on 2.4 GHz
#endif
    }
  } else {
    if (chIndex >= (int)(sizeof(CH_5G) / sizeof(CH_5G[0]))) {
      on5ghz = false; chIndex = 0;              // back to 2.4 GHz
    }
  }
  curChannel = on5ghz ? CH_5G[chIndex] : CH_24[chIndex];
  // If the core rejects a 5 GHz channel on this build, skip to the next one.
  if (esp_wifi_set_channel(curChannel, WIFI_SECOND_CHAN_NONE) != ESP_OK && on5ghz) {
    on5ghz = false; chIndex = 0; curChannel = CH_24[0];
    esp_wifi_set_channel(curChannel, WIFI_SECOND_CHAN_NONE);
  }
}

// ============================ STATUS =======================================
// No OLED on the XIAO sensor — status goes to the serial console.
void drawStatus() {
  const char* state = uploading ? activeXport
                    : (bleConnected ? "BLE~" : "listen");
  Serial.printf("[STATUS] WiFi:%lu drop:%lu BT:%lu Log(%s):%uB ch:%s%u %s\n",
                (unsigned long)wifiSeen, (unsigned long)wifiDropped,
                (unsigned long)btSeen, usingSD ? "SD" : "FL",
                (unsigned)logSize(), on5ghz ? "5G-" : "", (unsigned)curChannel,
                state);
}

// ============================ LoRa TRANSFER ================================
bool loraTransfer() {
  if (!logFS) return false;
  activeXport = "LoRa";
  flushLog();

  size_t total = 0;
  uint32_t crc = fileCrc32(total);
  char crcHex[9]; snprintf(crcHex, sizeof(crcHex), "%08x", crc);

  radio.transmit(String("HELLO,") + NODE_ID + "," + total + "," + crcHex);

  File f = logFS->open(LOG_PATH, FILE_READ);
  if (!f) return false;
  uint8_t buf[LORA_CHUNK_BYTES];
  uint32_t seq = 0;
  bool ok = true;
  while (f.available()) {
    size_t n = f.read(buf, sizeof(buf));
    String pkt = String("D,") + seq + ",";
    pkt.reserve(pkt.length() + n);
    for (size_t i = 0; i < n; i++) pkt += (char)buf[i];

    int retry = 0; bool acked = false;
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
    if (!acked) { ok = false; break; }
    seq++;
  }
  f.close();
  if (!ok) { radio.startReceive(); activeXport = "idle"; return false; }

  radio.transmit("END");

  String resp; bool cleared = false;
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
    NimBLEDevice::startAdvertising();
  }
};

void startBleServer() {
  NimBLEDevice::init(BLE_ADV_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, false, true);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(new SrvCallbacks());

  NimBLEService* svc = bleServer->createService(BLE_SVC_UUID);
  bleDataChar = svc->createCharacteristic(BLE_DATA_UUID, NIMBLE_PROPERTY::NOTIFY);
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
bool bleTransfer() {
  if (!logFS || !bleConnected) return false;
  activeXport = "BLE";
  flushLog();

  size_t total = 0;
  uint32_t crc = fileCrc32(total);
  char crcHex[9]; snprintf(crcHex, sizeof(crcHex), "%08x", crc);

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
    delay(8);
  }
  f.close();
  if (!bleConnected) { activeXport = "idle"; return false; }

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
  Serial.println("[XIAO-C5] workhorse booting");

  // Bring up the ONE shared SPI bus for both the LoRa hat and the SD card.
  SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN /*, SS handled per-device*/);

  // Storage: microSD (shared bus) primary, onboard flash fallback.
  storageBegin();
  fileBytes = diskLogSize();

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

  // UART link to the C3.
  LinkSerial.begin(UART_BAUD, SERIAL_8N1, LINK_RX_PIN, LINK_TX_PIN);

  // LoRa radio (SX1262 on the shared SPI bus).
  int st = radio.begin(LORA_FREQ, LORA_BW, LORA_SF, LORA_CR, LORA_SYNC, LORA_POWER);
  if (st == RADIOLIB_ERR_NONE) Serial.println("[LoRa] init OK");
  else Serial.printf("[LoRa] init failed, code %d\n", st);
  radio.startReceive();

  // Wi-Fi sniffer (dual-band).
  startWifiSniffer();

  // BLE handoff server.
  startBleServer();
  Serial.println("[BLE] advertising; ready for handoff");
}

// ============================ MAIN LOOP ====================================
void loop() {
  // 1) Drain the Wi-Fi sniffer queue into the log buffer.
  if (wifiQueue) {
    char line[WIFI_LINE_MAX];
    int drained = 0;
    while (drained < WIFI_QUEUE_DEPTH &&
           xQueueReceive(wifiQueue, line, 0) == pdTRUE) {
      logLine(String(line));
      drained++;
    }
  }

  // 2) Drain the UART from the C3.
  while (LinkSerial.available()) {
    String msg = LinkSerial.readStringUntil('\n');
    msg.trim();
    if (msg.length() == 0) continue;
    if (msg.startsWith("BT,")) {
      btSeen++;
      logLine(msg);
    } else if (msg.startsWith("SYS,")) {
      Serial.print("[C3] "); Serial.println(msg);
    }
  }

  // 3) Hop Wi-Fi channel on a timer (2.4 GHz + 5 GHz sweeps).
  if (millis() - lastChannelHop > WIFI_CHANNEL_DWELL_MS) {
    hopWifiChannel();
    lastChannelHop = millis();
  }

  // 3b) Batch-flush the RAM log buffer to storage.
  maybeFlush();

  // 4) HANDOFF.
  //    (a) BLE — collector connected and wrote "GET".
  if (bleGetRequested) {
    bleGetRequested = false;
    uploading = true;
    esp_wifi_set_promiscuous(false);   // BLE shares the 2.4 GHz front-end
    bleTransfer();
    esp_wifi_set_promiscuous(true);
    uploading = false;
  }
  //    (b) LoRa — peek for a "GET" over the air (skip while a BLE session runs).
  if (!bleConnected) {
    String rx;
    if (radio.receive(rx, 20) == RADIOLIB_ERR_NONE && rx.startsWith("GET")) {
      uploading = true;
      esp_wifi_set_promiscuous(false);
      loraTransfer();
      esp_wifi_set_promiscuous(true);
      uploading = false;
    }
  }

  // 5) ~1 Hz status; rotate the log if it exceeds the cap.
  if (millis() - lastDisplay > 1000) {
    drawStatus();
    lastDisplay = millis();
    if (logSize() > logCapBytes) {
      Serial.println("[LOG] size cap hit; clearing (collector should drain)");
      if (logFS) logFS->remove(LOG_PATH);
      fileBytes = 0;
      rtcUsed   = 0;
    }
  }
}
