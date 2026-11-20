#include <Arduino_GFX_Library.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ---- Display ----
Arduino_DataBus *bus = new Arduino_ESP32SPI(2, 5, 18, 23, -1);   // DC, CS, SCK, MOSI, MISO
Arduino_GFX *gfx = new Arduino_GC9A01(bus, 4, 0, true);          // RST, rotation, IPS

// ---- GlucoTrail broadcast protocol ----
// GlucoTrail advertises the latest reading as BLE service data under this UUID.
// Payload (8 bytes):
//   byte 0        version = 0x01
//   byte 1        flags   (bit 0 = valid, bit 1 = mmol/L if set, else mg/dL)
//   byte 2..3     glucose mg/dL, uint16 little-endian
//   byte 4        trend raw, int8 (units of 0.1 mg/dL per minute)
//   byte 5        quality (0..100)
//   byte 6..7     sensor minute, uint16 little-endian
const char *GLUCOTRAIL_SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
BLEUUID glucoUuid(GLUCOTRAIL_SERVICE_UUID);

// ---- Latest reading (updated from the BLE callback) ----
volatile int latestGlucose = 0;
volatile int latestTrend = 0;
volatile int latestQuality = 0;
volatile int latestSensorMinute = 0;
volatile bool latestValid = false;
volatile bool latestMmol = false;
volatile unsigned long latestReadingMs = 0;

volatile unsigned long adsSeen = 0;
volatile unsigned long glucoAdsSeen = 0;

// ASCII arrows only: the default GFX font has no Unicode glyphs.
const char *trendArrow(int trendRaw) {
  float rate = trendRaw / 10.0f;   // mg/dL per minute
  if (rate >= 3.0f) return "^^";
  if (rate >= 2.0f) return "^";
  if (rate >= 1.0f) return ">";
  if (rate <= -3.0f) return "vv";
  if (rate <= -2.0f) return "v";
  if (rate <= -1.0f) return "<";
  return "-";
}

class GlucoScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) {
    adsSeen++;

    if (!dev.haveServiceData()) return;
    if (!dev.getServiceDataUUID().equals(glucoUuid)) return;

    String data = dev.getServiceData();
    if (data.length() < 8) return;

    uint8_t version = (uint8_t)data[0];
    if (version != 0x01) return;

    uint8_t flags = (uint8_t)data[1];
    latestValid = (flags & 0x01) != 0;
    latestMmol = (flags & 0x02) != 0;
    latestGlucose = (uint8_t)data[2] | ((uint8_t)data[3] << 8);
    latestTrend = (int8_t)data[4];
    latestQuality = (uint8_t)data[5];
    latestSensorMinute = (uint8_t)data[6] | ((uint8_t)data[7] << 8);
    latestReadingMs = millis();

    glucoAdsSeen++;

    Serial.printf(
        "GlucoTrail: %d mg/dL trend=%d q=%d min=%d valid=%d rssi=%d\n",
        latestGlucose, latestTrend, latestQuality, latestSensorMinute,
        latestValid ? 1 : 0, dev.getRSSI());
  }
};

void drawCentered(const String &text, uint8_t size, int16_t y, uint16_t color) {
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  int16_t w = 6 * size * text.length();
  gfx->setCursor((gfx->width() - w) / 2, y);
  gfx->print(text);
}

void drawReading() {
  static unsigned long lastDbg = 0;
  unsigned long now = millis();
  if (now - lastDbg >= 2000) {
    lastDbg = now;
    Serial.printf("DRAW valid=%d glu=%d trend=%d\n",
                  (int)latestValid, latestGlucose, latestTrend);
  }

  gfx->fillScreen(gfx->color565(0, 0, 0));

  unsigned long age = latestReadingMs ? (unsigned long)(millis() - latestReadingMs) : 0;
  bool stale = age > 90000;   // CGM updates about once a minute

  uint16_t trendColor = gfx->color565(120, 200, 255);
  if (latestTrend >= 20) {
    trendColor = gfx->color565(255, 180, 60);
  } else if (latestTrend <= -20) {
    trendColor = gfx->color565(255, 90, 90);
  }
  drawCentered(String(trendArrow(latestTrend)), 4, 40, trendColor);

  uint16_t valueColor = gfx->color565(255, 255, 255);
  if (!latestValid || stale) valueColor = gfx->color565(150, 150, 150);

  String value;
  if (!latestValid || stale) {
    value = "--";
  } else if (latestMmol) {
    value = String(latestGlucose / 18.0, 1);   // one decimal place
  } else {
    value = String(latestGlucose);
  }
  drawCentered(value, 7, 90, valueColor);

  const char *unitLabel = latestMmol ? "mmol/L" : "mg/dL";
  drawCentered(unitLabel, 2, 160, gfx->color565(180, 180, 180));

  String status;
  if (!latestValid) {
    status = "waiting";
  } else if (stale) {
    status = "stale " + String(age / 1000) + "s";
  } else {
    status = "live";
  }
  drawCentered(status, 2, 195, gfx->color565(120, 200, 120));
}

// BLEScan::start() blocks forever in this library (it never returns even though
// the scan runs). Run it in its own FreeRTOS task so the main loop stays free.
void startScanTask(void *param) {
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new GlucoScanCallbacks());
  scan->setActiveScan(true);
  scan->setInterval(1349);   // 0.625 ms units (~843 ms)
  scan->setWindow(200);      // ~125 ms window
  scan->start(0, true);      // blocks here forever; the scan itself runs fine
  vTaskDelete(NULL);         // never reached
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== GlucoRing: boot ===");

  gfx->begin();
  gfx->fillScreen(gfx->color565(0, 0, 0));
  drawCentered("GlucoRing", 2, 110, gfx->color565(255, 255, 255));
  drawCentered("waiting...", 2, 140, gfx->color565(150, 150, 150));

  BLEDevice::init("GlucoRing");
  xTaskCreatePinnedToCore(startScanTask, "bleScan", 8192, NULL, 1, NULL, 0);

  Serial.println("Scanning for GlucoTrail broadcasts...");
}

void loop() {
  static bool firstLoop = true;
  if (firstLoop) {
    firstLoop = false;
    Serial.println("L0 loop started");
  }

  static unsigned long lastBeat = 0;
  static unsigned long lastDraw = 0;

  // Refresh the display twice a second so the stale indicator stays honest.
  if (millis() - lastDraw >= 500) {
    lastDraw = millis();
    drawReading();
  }

  // Heartbeat every 5 seconds so you can confirm it's still alive.
  if (millis() - lastBeat >= 5000) {
    lastBeat = millis();
    Serial.printf("--- heartbeat: ads=%lu gluco=%lu heap=%lu ---\n",
                  adsSeen, glucoAdsSeen, (unsigned long)ESP.getFreeHeap());
  }

  delay(50);
}