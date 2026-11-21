#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "FreeSansBold24pt7b.h"

// ==================================================
// GLUCORING - GLUCOSE DISPLAY (BLE + GC9A01 240x240)
// ==================================================

// DISPLAY PINS
Arduino_DataBus *bus = new Arduino_ESP32SPI(2, 5, 18, 23, -1);
Arduino_GFX *gfx = new Arduino_GC9A01(bus, 4, 0, true);

// GlucoTrail broadcast service UUID
const char *GLUCOTRAIL_SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
BLEUUID glucoUuid(GLUCOTRAIL_SERVICE_UUID);

// Latest reading (glucose kept in mg/dL internally)
volatile int latestGlucose = 0;
volatile int latestTrend = 0;
volatile int latestQuality = 0;
volatile int latestSensorMinute = 0;
volatile bool latestValid = false;
volatile bool latestMmol = false;
volatile int lowLimitMgDl = 70;
volatile int highLimitMgDl = 180;
volatile unsigned long latestReadingMs = 0;

volatile unsigned long adsSeen = 0;
volatile unsigned long glucoAdsSeen = 0;

// COLORS (RGB565)
#define BLACK       0x0000
#define WHITE       0xFFFF
#define GREEN       0x4FEA
#define DARKGREEN   0x11C3
#define RED         0xF9C7
#define DARKRED     0x6002
#define AMBER       0xFD20
#define GREY        0xAD55
#define BLUE        0x347E
#define DARKBLUE    0x114A

// ==================================================
// BLE SCAN CALLBACK
// ==================================================

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
    if (data.length() >= 12) {
      lowLimitMgDl = (uint8_t)data[8] | ((uint8_t)data[9] << 8);
      highLimitMgDl = (uint8_t)data[10] | ((uint8_t)data[11] << 8);
    }
    latestReadingMs = millis();
    glucoAdsSeen++;

    Serial.printf("GlucoTrail: %d mg/dL trend=%d q=%d min=%d valid=%d rssi=%d\n",
                  latestGlucose, latestTrend, latestQuality, latestSensorMinute,
                  latestValid ? 1 : 0, dev.getRSSI());
  }
};

// BLEScan::start() blocks forever in this library, so run it in its own task.
void startScanTask(void *param) {
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new GlucoScanCallbacks());
  scan->setActiveScan(true);
  scan->setInterval(1349);
  scan->setWindow(200);
  scan->start(0, true);
  vTaskDelete(NULL);
}

// ==================================================
// CENTER TEXT
// ==================================================

void drawCenteredText(const String &text, int baselineY, int textSize, uint16_t color, const GFXfont *font = NULL) {
  gfx->setFont(font);
  gfx->setTextSize(textSize);
  gfx->setTextColor(color);
  gfx->setTextWrap(false);

  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(text.c_str(), 0, 0, &x1, &y1, &w, &h);

  int x = (240 - w) / 2 - x1;
  gfx->setCursor(x, baselineY);
  gfx->print(text);

  gfx->setFont(NULL);
  gfx->setTextSize(1);
}

// ==================================================
// STATE HELPERS
// ==================================================

bool staleNow() {
  return latestReadingMs == 0 || (millis() - latestReadingMs > 90000);
}

bool belowLowNow() {
  return latestValid && !staleNow() && latestGlucose < lowLimitMgDl;
}

bool ultraHighNow() {
  return latestValid && !staleNow() && latestGlucose > highLimitMgDl + 90;
}

bool highOnlyNow() {
  return latestValid && !staleNow() && latestGlucose > highLimitMgDl && !ultraHighNow();
}

bool criticalNow() {
  return belowLowNow() || ultraHighNow();
}

bool warmupNow() {
  return !staleNow() && latestSensorMinute >= 0 && latestSensorMinute < 60;
}

// Static status color (no flash) — used by the status badge.
uint16_t getStatusColor() {
  if (staleNow()) return GREY;
  if (warmupNow()) return BLUE;
  if (!latestValid) return GREY;
  if (criticalNow()) return RED;
  if (highOnlyNow()) return AMBER;
  return GREEN;
}

// Ring color — flashes when critical.
uint16_t getRingColor() {
  if (staleNow()) return GREY;
  if (warmupNow()) return BLUE;
  if (!latestValid) return GREY;
  if (criticalNow()) {
    return ((millis() / 600) % 2 == 0) ? RED : DARKRED;
  }
  if (highOnlyNow()) return AMBER;
  return GREEN;
}

// ==================================================
// OUTER RING
// ==================================================

void drawRingColor(uint16_t color) {
  for (int radius = 117; radius >= 111; radius--) {
    gfx->drawCircle(120, 120, radius, color);
  }
}

void drawRing(uint16_t color) {
  gfx->drawCircle(120, 120, 118, DARKGREEN);
  drawRingColor(color);
  gfx->drawCircle(120, 120, 109, DARKGREEN);
}

// ==================================================
// LARGE GLUCOSE NUMBER
// ==================================================

void drawGlucose() {
  bool stale = staleNow();

  if (warmupNow()) {
    int remaining = 60 - latestSensorMinute;
    if (remaining < 0) remaining = 0;
    drawCenteredText("Warm-up", 138, 1, WHITE, &FreeSansBold24pt7b);
    drawCenteredText(String(remaining) + " min remaining", 170, 2, GREY);
    return;
  }

  String value;
  if (!latestValid || stale) {
    value = "--";
  } else if (latestMmol) {
    value = String(latestGlucose / 18.0, 1);
  } else {
    value = String(latestGlucose);
  }

  gfx->setFont(&FreeSansBold24pt7b);
  gfx->setTextSize(2);

  int16_t bx, by;
  uint16_t bw, bh;
  gfx->getTextBounds(value.c_str(), 0, 0, &bx, &by, &bw, &bh);

  int fontScale = 2;
  if (bw > 200 || bh > 93) {
    fontScale = 1;
  }

  drawCenteredText(value, fontScale == 2 ? 143 : 129, fontScale, WHITE, &FreeSansBold24pt7b);
}

// ==================================================
// STATUS BADGE
// ==================================================

void drawStatus() {
  String label;
  uint16_t background;
  uint16_t border = getStatusColor();

  if (warmupNow()) {
    label = "WARM-UP";
    background = DARKBLUE;
  } else if (!latestValid) {
    label = "WAITING";
    background = BLACK;
  } else if (staleNow()) {
    label = "STALE";
    background = BLACK;
  } else if (belowLowNow()) {
    label = "LOW";
    background = DARKRED;
  } else if (highOnlyNow() || ultraHighNow()) {
    label = "HIGH";
    background = BLACK;
  } else {
    label = "IN RANGE";
    background = DARKGREEN;
  }

  gfx->fillRoundRect(54, 160, 132, 37, 18, background);
  gfx->drawRoundRect(54, 160, 132, 37, 18, border);

  if (label == "IN RANGE") {
    gfx->fillCircle(77, 178, 11, GREEN);
    gfx->drawLine(71, 178, 76, 183, BLACK);
    gfx->drawLine(76, 183, 84, 173, BLACK);
    gfx->setFont(NULL);
    gfx->setTextSize(1);
    gfx->setTextColor(WHITE);
    gfx->setCursor(94, 175);
    gfx->print("IN RANGE");
  } else {
    // Pill spans y=160..197; 171 puts the size-2 label centered vertically.
    drawCenteredText(label, 171, 2, WHITE);
  }
}

// ==================================================
// DRAW DISPLAY (full)
// ==================================================

void drawDisplay() {
  gfx->fillScreen(BLACK);
  drawRing(getRingColor());
  drawCenteredText("GLUCOTRAIL", 42, 2, WHITE);
  drawGlucose();
  drawStatus();
}

// ==================================================
// SETUP
// ==================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("=== GlucoRing: boot ===");

  if (!gfx->begin()) {
    Serial.println("Display initialization failed");
    return;
  }

  drawDisplay();

  BLEDevice::init("GlucoRing");
  xTaskCreatePinnedToCore(startScanTask, "bleScan", 8192, NULL, 1, NULL, 0);

  Serial.println("Scanning for GlucoTrail broadcasts...");
}

// ==================================================
// LOOP
// ==================================================

void loop() {
  // Heartbeat every 5 seconds.
  static unsigned long lastBeat = 0;
  if (millis() - lastBeat >= 5000) {
    lastBeat = millis();
    unsigned long ageMs = (latestReadingMs == 0) ? 0 : (millis() - latestReadingMs);
    Serial.printf("--- heartbeat: ads=%lu gluco=%lu age=%lus stale=%d heap=%lu ---\n",
                  adsSeen, glucoAdsSeen, ageMs / 1000UL,
                  staleNow() ? 1 : 0, (unsigned long)ESP.getFreeHeap());
  }

  uint16_t ringColor = getRingColor();   // includes the blink phase

  static uint16_t lastRing = 0xFFFF;
  static int lastGlucose = 0x7FFFFFFF;
  static bool lastMmol = false;
  static bool lastValid = false;
  static bool lastStale = false;
  static int lastLow = -1;
  static int lastHigh = -1;
  static bool first = true;

  bool stale = staleNow();

  // One-shot console message when the display enters or leaves the stale state.
  if (stale != lastStale) {
    if (stale && latestReadingMs != 0) {
      unsigned long ageMs = millis() - latestReadingMs;
      Serial.printf(">>> STALE: no broadcast for %lus (last min=%d)\n",
                    ageMs / 1000UL, latestSensorMinute);
    } else if (!stale) {
      Serial.printf(">>> RECOVERED: broadcast received (min=%d)\n",
                    latestSensorMinute);
    }
  }

  bool dataChanged = first ||
    latestGlucose != lastGlucose || latestMmol != lastMmol ||
    latestValid != lastValid || stale != lastStale ||
    lowLimitMgDl != lastLow || highLimitMgDl != lastHigh;

  if (dataChanged) {
    first = false;
    lastGlucose = latestGlucose;
    lastMmol = latestMmol;
    lastValid = latestValid;
    lastStale = stale;
    lastLow = lowLimitMgDl;
    lastHigh = highLimitMgDl;
    lastRing = ringColor;
    drawDisplay();
  } else if (ringColor != lastRing) {
    // Only the blink phase changed — redraw just the ring (no flicker).
    lastRing = ringColor;
    drawRingColor(ringColor);
  }

  delay(100);
}