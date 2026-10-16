/*
 * sensor-node :: XIAO ESP32-C3  —  Bluetooth (BLE) scanner
 *               (XIAO build / "new thinking" — paired with the C5 workhorse)
 * ---------------------------------------------------------------------------
 * Role: continuously scan for BLE advertisements and stream each observation
 *       to the XIAO ESP32-C5 workhorse over UART.
 *
 * This is unchanged in logic from the original two-MCU design — the C3's job
 * and pinout are identical whether its partner is the Heltec or the C5. The
 * only thing to know for the XIAO build is what it connects to:
 *
 *   C3 TX (D6/GPIO21) ---> C5 RX (D7/GPIO12)
 *   C3 RX (D7/GPIO20) <--- C5 TX (D6/GPIO11)   (optional, for future cmds)
 *   C3 GND            <-->  C5 GND             (REQUIRED common ground)
 *
 * TX<->RX crosses over. Verified GPIO numbers are in docs/xiao/WIRING.md.
 *
 * Why this board does only BLE: running BLE scanning and Wi-Fi promiscuous
 * sniffing on one radio starves both. The C3 owns Bluetooth; the C5 owns
 * Wi-Fi (2.4 + 5 GHz) + LoRa + SD logging.
 *
 * Line protocol (newline-terminated ASCII):
 *   BT,<mac>,<addrType>,<rssi>,<name>,<svcUUIDs>,<svcData>,<mfgData>
 *   SYS,xiao-bt,<online|heartbeat>
 * (Field semantics identical to the original scanner; see below.)
 *
 * Board target: "XIAO_ESP32C3" in Arduino IDE (esp32 core by Espressif).
 * Libraries: NimBLE-Arduino v2.x.
 */

#include <Arduino.h>
#include <NimBLEDevice.h>

// ---- Configuration ---------------------------------------------------------
static const uint32_t UART_BAUD         = 115200;  // must match the C5 side
static const int      UART_TX_PIN       = 21;      // D6 on XIAO silkscreen
static const int      UART_RX_PIN       = 20;      // D7 on XIAO silkscreen
static const uint32_t SCAN_DURATION_S   = 5;       // length of each scan burst
static const bool     ACTIVE_SCAN       = true;    // request scan response (names)
static const int      REPORT_RSSI_FLOOR = -95;     // ignore very weak hits

static const int      MAX_SVC_UUIDS     = 4;
static const size_t   MAX_MFG_BYTES     = 16;
static const size_t   MAX_SVCDATA_BYTES = 16;

HardwareSerial LinkSerial(1);   // UART1; keep UART0/USB free for debugging

static std::string sanitize(std::string s) {
  for (char &c : s) {
    if (c == ',' || c == '|' || c == '\n' || c == '\r') c = ' ';
  }
  return s;
}

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

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    int rssi = dev->getRSSI();
    if (rssi < REPORT_RSSI_FLOOR) return;

    std::string mac = dev->getAddress().toString();
    const NimBLEAddress& addr = dev->getAddress();
    std::string addrType = addr.isPublic() ? "pub"
                         : addr.isRpa()    ? "rpa"
                         : addr.isNrpa()   ? "nrpa"
                         :                   "rnd";
    std::string name = sanitize(dev->haveName() ? dev->getName() : "");

    std::string svcUUIDs;
    int uuidCount = dev->getServiceUUIDCount();
    for (int i = 0; i < uuidCount && i < MAX_SVC_UUIDS; i++) {
      if (!svcUUIDs.empty()) svcUUIDs += '|';
      svcUUIDs += dev->getServiceUUID(i).toString();
    }

    std::string svcData;
    int sdCount = dev->getServiceDataCount();
    for (int i = 0; i < sdCount; i++) {
      std::string blob = dev->getServiceData(i);
      std::string uuid = dev->getServiceDataUUID(i).toString();
      if (!svcData.empty()) svcData += '|';
      svcData += uuid + ":" +
                 toHex((const uint8_t*)blob.data(), blob.size(), MAX_SVCDATA_BYTES);
    }

    std::string mfg;
    if (dev->haveManufacturerData()) {
      std::string m = dev->getManufacturerData();
      mfg = toHex((const uint8_t*)m.data(), m.size(), MAX_MFG_BYTES);
    }

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
  Serial.println("[C3] BLE scanner booting (XIAO build)");

  NimBLEDevice::init("");               // scanning only; no local name
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(&scanCallbacks, /*wantDuplicates=*/false);
  pScan->setActiveScan(ACTIVE_SCAN);
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->setMaxResults(0);

  LinkSerial.println("SYS,xiao-bt,online");
  Serial.println("[C3] scanning...");
}

void loop() {
  pScan->getResults(SCAN_DURATION_S * 1000, /*deleteResults=*/true);
  pScan->clearResults();
  LinkSerial.println("SYS,xiao-bt,heartbeat");
}
