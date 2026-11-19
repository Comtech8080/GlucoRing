#include <Arduino_GFX_Library.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

Arduino_DataBus *bus = new Arduino_ESP32SPI(2, 5, 18, 23, -1);   // DC, CS, SCK, MOSI, MISO
Arduino_GFX *gfx = new Arduino_GC9A01(bus, 4, 0, true);          // RST, rotation, IPS

// Counters (updated from callback)
volatile unsigned long adsSeen = 0;
volatile unsigned long aidexSeen = 0;
volatile unsigned long lastAdMillis = 0;

class DebugCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    adsSeen++;
    lastAdMillis = millis();

    Serial.println("=== ADVERTISEMENT ===");
    Serial.print("  address: ");
    Serial.println(dev.getAddress().toString().c_str());
    Serial.print("  rssi:    ");
    Serial.println(dev.getRSSI());
    Serial.print("  name:    ");
    Serial.println(dev.haveName() ? dev.getName().c_str() : "(none)");
    Serial.print("  type:    ");
    Serial.println(dev.getAdvType());

    // Manufacturer data (this is where AiDEX lives)
    if (dev.haveManufacturerData()) {
      String m = dev.getManufacturerData();
      Serial.print("  mfg len: ");
      Serial.println(m.length());
      Serial.print("  mfg hex: ");
      for (int i = 0; i < m.length(); i++) {
        if ((uint8_t)m[i] < 0x10) Serial.print("0");
        Serial.print((uint8_t)m[i], HEX);
        Serial.print(" ");
      }
      Serial.println();

      if (m.length() >= 2 && (uint8_t)m[0] == 0x59 && (uint8_t)m[1] == 0x00) {
        aidexSeen++;
        Serial.println("  >>> AIDEX SENSOR (manufacturer 0x0059)");
      }
    } else {
      Serial.println("  mfg data: (none)");
    }

    // Service data (sometimes useful)
    if (dev.haveServiceData()) {
      Serial.print("  svc UUID: ");
      Serial.println(dev.getServiceDataUUID().toString().c_str());
      Serial.print("  svc data: ");
      String sd = dev.getServiceData();
      for (int i = 0; i < sd.length(); i++) {
        if ((uint8_t)sd[i] < 0x10) Serial.print("0");
        Serial.print((uint8_t)sd[i], HEX);
        Serial.print(" ");
      }
      Serial.println();
    }

    Serial.println();
  }
};

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("=== AiDEX debug: boot ===");

  gfx->begin();
  gfx->fillScreen(gfx->color565(0, 0, 0));
  gfx->setTextColor(gfx->color565(255, 255, 255));
  gfx->setTextSize(2);
  gfx->setCursor(20, 110);
  gfx->print("Debug scan");

  Serial.println("Initializing BLE...");
  BLEDevice::init("AidexDebug");
  Serial.println("BLE initialized.");

  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new DebugCallbacks());
  scan->setActiveScan(true);      // also request scan responses
  scan->setInterval(1349);        // 0.625 ms units
  scan->setWindow(1349);          // scan 100% of the time

  Serial.println("Starting scan...");
  scan->start(0, false);          // 0 = scan forever
  Serial.println("Scan started. Waiting for advertisements...");
  Serial.println();
}

void loop() {
  // Heartbeat every 5 seconds so you can confirm it's still alive
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat >= 5000) {
    lastBeat = millis();
    Serial.println("--- heartbeat ---");
    Serial.printf("  ads seen total: %lu\n", adsSeen);
    Serial.printf("  aidex ads seen: %lu\n", aidexSeen);
    Serial.printf("  ms since last ad: %lu\n", lastAdMillis ? (unsigned long)(millis() - lastAdMillis) : 0);
    Serial.printf("  free heap: %lu\n", (unsigned long)ESP.getFreeHeap());
    Serial.println();
  }
  delay(50);
}