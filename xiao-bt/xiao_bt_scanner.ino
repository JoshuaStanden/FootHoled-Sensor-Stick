/*
 * sensor-node :: XIAO ESP32-C3  —  Bluetooth (BLE) scanner
 * ---------------------------------------------------------
 * Role: continuously scan for BLE advertisements and stream each
 *       observation to the Heltec V4 "workhorse" over UART.
 *
 * Why this board does only BLE: running BLE scanning and Wi-Fi
 * promiscuous sniffing on the same radio starves both. The XIAO
 * owns Bluetooth; the Heltec owns Wi-Fi + LoRa + logging.
 *
 * Link to Heltec: UART (one data wire + common ground).
 *   XIAO TX (GPIO21 / D6) ---> Heltec RX
 *   XIAO RX (GPIO20 / D7) <--- Heltec TX   (optional, for future cmds)
 *   XIAO GND            <-->   Heltec GND  (REQUIRED common ground)
 *
 * Line protocol (newline-terminated ASCII, easy to parse + log):
 *   BT,<mac>,<addrType>,<rssi>,<name>,<svcUUIDs>,<svcData>,<mfgData>
 * Fields:
 *   mac        : advertiser address, e.g. a4:c1:38:9f:22:10
 *   addrType   : pub | rpa | nrpa | rnd  (public, resolvable-private,
 *                non-resolvable-private, or static-random. rpa/nrpa are the
 *                privacy-rotating kind modern phones use.)
 *   rssi       : signal strength in dBm
 *   name       : advertised name (may be empty)
 *   svcUUIDs   : advertised service UUIDs, '|'-separated (may be empty).
 *                These tell you what the device IS without connecting
 *                (e.g. 180d = Heart Rate, 181a = Environmental Sensing).
 *   svcData    : <uuid>:<hexbytes> service data if broadcast (e.g. a live
 *                sensor reading), '|'-separated (may be empty)
 *   mfgData    : manufacturer-specific data as hex (first 2 bytes = vendor
 *                ID, e.g. 4c00 = Apple), (may be empty)
 * Commas inside a field are stripped; multi-values use '|' so the outer
 * CSV framing stays intact. All fields are what the device ADVERTISES —
 * no connection is made, so this is the advertised subset, not the full
 * GATT service list (that needs a connection).
 * Example:
 *   BT,a4:c1:38:9f:22:10,pub,-72,Govee_H5075,181a,181a:0a1c3b|,
 *
 * Board target: "XIAO_ESP32C3" in Arduino IDE (esp32 core by Espressif).
 * Libraries: NimBLE-Arduino v2.x (install via Library Manager) — far lighter
 *            on RAM/flash than the stock BLE stack, important on the C3.
 */

#include <Arduino.h>
#include <NimBLEDevice.h>

// ---- Configuration ---------------------------------------------------------
static const uint32_t UART_BAUD        = 115200;  // must match Heltec side
static const int      UART_TX_PIN      = 21;      // D6 on XIAO silkscreen
static const int      UART_RX_PIN      = 20;      // D7 on XIAO silkscreen
static const uint32_t SCAN_DURATION_S  = 5;       // length of each scan burst
static const bool     ACTIVE_SCAN      = true;    // also request scan response
                                                  // (gets device names; uses
                                                  //  a little more power)
static const int      REPORT_RSSI_FLOOR = -95;    // ignore extremely weak hits

// Bounds to keep each UART/log line reasonable (the Heltec stages lines in a
// fixed buffer, so very long lines get truncated there).
static const int      MAX_SVC_UUIDS    = 4;       // cap advertised UUIDs emitted
static const size_t   MAX_MFG_BYTES    = 16;      // cap manufacturer-data bytes
static const size_t   MAX_SVCDATA_BYTES = 16;     // cap per service-data blob

// Use UART1 on the C3 for the link, keep UART0/USB free for debugging.
HardwareSerial LinkSerial(1);

// Replace characters that would break the CSV framing (',' and newlines) and
// also '|' since we use it as the multi-value separator.
static std::string sanitize(std::string s) {
  for (char &c : s) {
    if (c == ',' || c == '|' || c == '\n' || c == '\r') c = ' ';
  }
  return s;
}

// Encode a byte blob as lowercase hex, capped at maxBytes.
static std::string toHex(const uint8_t* data, size_t len, size_t maxBytes) {
  static const char* H = "0123456789abcdef";
  std::string out;
  size_t n = (len > maxBytes) ? maxBytes : len;
  out.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    out += H[(data[i] >> 4) & 0xF];
    out += H[data[i] & 0xF];
  }
  return out;
}

// Simple de-dup: don't spam the same MAC more than once per scan window.
// (Heltec also de-dups, but trimming here saves UART bandwidth + power.)

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    int rssi = dev->getRSSI();
    if (rssi < REPORT_RSSI_FLOOR) return;

    std::string mac  = dev->getAddress().toString();
    // Address type: public, or a random variant. Random/resolvable addresses
    // are the privacy-rotating kind your own phones use.
    const NimBLEAddress& addr = dev->getAddress();
    std::string addrType = addr.isPublic() ? "pub"
                         : addr.isRpa()    ? "rpa"   // resolvable private
                         : addr.isNrpa()   ? "nrpa"  // non-resolvable private
                         :                   "rnd";  // static random / other
    std::string name = sanitize(dev->haveName() ? dev->getName() : "");

    // --- Advertised service UUIDs (what the device IS, no connect needed) ---
    std::string svcUUIDs;
    int uuidCount = dev->getServiceUUIDCount();
    for (int i = 0; i < uuidCount && i < MAX_SVC_UUIDS; i++) {
      if (!svcUUIDs.empty()) svcUUIDs += '|';
      svcUUIDs += dev->getServiceUUID(i).toString();
    }

    // --- Service data (e.g. a live sensor reading broadcast in the ad) ------
    std::string svcData;
    int sdCount = dev->getServiceDataCount();
    for (int i = 0; i < sdCount; i++) {
      std::string blob = dev->getServiceData(i);
      std::string uuid = dev->getServiceDataUUID(i).toString();
      if (!svcData.empty()) svcData += '|';
      svcData += uuid + ":" +
                 toHex((const uint8_t*)blob.data(), blob.size(), MAX_SVCDATA_BYTES);
    }

    // --- Manufacturer-specific data (first 2 bytes = vendor ID) -------------
    std::string mfg;
    if (dev->haveManufacturerData()) {
      std::string m = dev->getManufacturerData();
      mfg = toHex((const uint8_t*)m.data(), m.size(), MAX_MFG_BYTES);
    }

    // Emit the enriched line.
    LinkSerial.print("BT,");
    LinkSerial.print(mac.c_str());        LinkSerial.print(',');
    LinkSerial.print(addrType.c_str());   LinkSerial.print(',');
    LinkSerial.print(rssi);               LinkSerial.print(',');
    LinkSerial.print(name.c_str());       LinkSerial.print(',');
    LinkSerial.print(svcUUIDs.c_str());   LinkSerial.print(',');
    LinkSerial.print(svcData.c_str());    LinkSerial.print(',');
    LinkSerial.println(mfg.c_str());
  }
};

static ScanCallbacks scanCallbacks;
static NimBLEScan* pScan = nullptr;

void setup() {
  Serial.begin(115200);                 // USB debug console
  LinkSerial.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  delay(200);
  Serial.println("[XIAO] BLE scanner booting");

  NimBLEDevice::init("");               // no local name; we are scanning only
  NimBLEDevice::setPower(ESP_PWR_LVL_P9); // max TX only matters for active scan req

  pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(&scanCallbacks, /*wantDuplicates=*/false);
  pScan->setActiveScan(ACTIVE_SCAN);
  pScan->setInterval(100);              // ms
  pScan->setWindow(99);                 // ms (must be <= interval)
  pScan->setMaxResults(0);              // 0 = report via callback, store nothing

  // Tell the Heltec we are alive (lets it detect a dead BT board).
  LinkSerial.println("SYS,xiao-bt,online");
  Serial.println("[XIAO] scanning...");
}

void loop() {
  // Blocking scan burst; results arrive via onResult(). Then loop again.
  pScan->getResults(SCAN_DURATION_S * 1000, /*deleteResults=*/true);
  pScan->clearResults();

  // Heartbeat so the workhorse knows the BT path is still live even in a
  // quiet RF environment with zero devices around.
  LinkSerial.println("SYS,xiao-bt,heartbeat");
}
