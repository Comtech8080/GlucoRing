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

// Draw a filled trend arrow (triangle) centered at (cx, cy).
void drawTrendGlyph(int trendRaw, int cx, int cy, uint16_t color) {
  float rate = trendRaw / 10.0f;   // mg/dL per minute
  if (rate >= 3.0f) {              // rapid rise -> double up
    gfx->fillTriangle(cx - 22, cy + 12, cx - 9, cy + 12, cx - 15, cy - 10, color);
    gfx->fillTriangle(cx + 9, cy + 12, cx + 22, cy + 12, cx + 15, cy - 10, color);
  } else if (rate >= 2.0f) {       // rise -> up
    gfx->fillTriangle(cx - 22, cy + 12, cx + 22, cy + 12, cx, cy - 16, color);
  } else if (rate >= 1.0f) {       // slight rise -> diagonal up-right
    gfx->fillTriangle(cx - 20, cy + 12, cx + 12, cy + 12, cx + 12, cy - 20, color);
  } else if (rate <= -3.0f) {      // rapid fall -> double down
    gfx->fillTriangle(cx - 22, cy - 12, cx - 9, cy - 12, cx - 15, cy + 10, color);
    gfx->fillTriangle(cx + 9, cy - 12, cx + 22, cy - 12, cx + 15, cy + 10, color);
  } else if (rate <= -2.0f) {      // fall -> down
    gfx->fillTriangle(cx - 22, cy - 12, cx + 22, cy - 12, cx, cy + 16, color);
  } else if (rate <= -1.0f) {      // slight fall -> diagonal down-right
    gfx->fillTriangle(cx - 20, cy - 12, cx + 12, cy - 12, cx + 12, cy + 20, color);
  } else {                         // steady -> horizontal bar
    gfx->fillRoundRect(cx - 24, cy - 5, 48, 10, 5, color);
  }
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
  unsigned long age = latestReadingMs ? (unsigned long)(millis() - latestReadingMs) : 0;
  bool stale = age > 90000;   // CGM updates about once a minute

  int cx = gfx->width() / 2;
  int cy = gfx->height() / 2;

  // Color the whole face by glucose status.
  uint16_t badge;
  if (!latestValid || stale)      badge = gfx->color565(45, 50, 60);   // gray
  else if (latestGlucose < 70)    badge = gfx->color565(196, 42, 48);  // red (low)
  else if (latestGlucose > 250)   badge = gfx->color565(196, 42, 48);  // red (very high)
  else if (latestGlucose > 180)   badge = gfx->color565(232, 150, 28); // amber (high)
  else                            badge = gfx->color565(24, 158, 96);  // green (in range)

  gfx->fillScreen(gfx->color565(10, 12, 14));                  // near-black edge
  gfx->fillCircle(cx, cy, 112, gfx->color565(255, 255, 255));  // white ring
  gfx->fillCircle(cx, cy, 109, badge);                         // colored face

  uint16_t fg = gfx->color565(255, 255, 255);

  drawTrendGlyph(latestTrend, cx, 76, fg);

  String value;
  if (!latestValid || stale) {
    value = "--";
  } else if (latestMmol) {
    value = String(latestGlucose / 18.0, 1);   // one decimal place
  } else {
    value = String(latestGlucose);
  }
  drawCentered(value, 6, 112, fg);

  const char *unitLabel = latestMmol ? "mmol/L" : "mg/dL";
  drawCentered(unitLabel, 2, 172, fg);

  String status;
  if (!latestValid)      status = "WAITING";
  else if (stale)        status = "STALE";
  else                   status = "LIVE";
  drawCentered(status, 2, 200, gfx->color565(225, 225, 225));
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
  gfx->fillScreen(gfx->color565(10, 12, 14));
  gfx->fillCircle(gfx->width() / 2, gfx->height() / 2, 112, gfx->color565(255, 255, 255));
  gfx->fillCircle(gfx->width() / 2, gfx->height() / 2, 109, gfx->color565(45, 50, 60));
  drawCentered("GlucoRing", 2, 108, gfx->color565(255, 255, 255));
  drawCentered("waiting...", 2, 140, gfx->color565(210, 210, 210));

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