#include <Arduino.h>
#include <TFT_eSPI.h>
#include <ESP32Encoder.h>
#include "Config.h"
#include "ObdService.h"
#include "UiPages.h"

// ============================== Core 1: UI / rendering ==============================
// All Bluetooth, ELM327, WiFi and HTTP work lives in ObdService (two tasks pinned to Core 0).
// This file only reads `tele`, so nothing here can be stalled by the radios.

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite face = TFT_eSprite(&tft);
TFT_eSprite work = TFT_eSprite(&tft);
ESP32Encoder encoder;

AppConfig cfg;
Telemetry tele;
ObdService obdService;
UiRenderer ui(tft, face, work);

enum Page { PAGE_DIAL = 0, PAGE_PARAMS = 1, PAGE_CONFIG = 2 };
Page currentPage = PAGE_DIAL;

// ---- Controls ----
// ROTATE (outside menu) : Dial -> Params -> Settings
// CLICK                 : Dial/Params: acknowledge DTC alert | Settings: enter menu
// In menu               : rotate = scroll rows (wraps) / change value, click = edit on/off
//                         click on "Wifi Setup" = start captive portal, click on "Save & Exit" = save
//                         HOLD = save & exit
bool inMenu = false;
bool editingValue = false;
int configCursor = 0;

// Full-quadrature counts per tactile click on this encoder (2 for most small rotary encoders).
// One click skips/doubles a step? Try 1, 2 or 4.
constexpr int ENC_COUNTS_PER_DETENT = 2;
constexpr unsigned long LONG_PRESS_MS = 700;

int64_t lastEncCount = 0;
int encAcc = 0;
bool lastBtnState = false, longFired = false;
unsigned long pressStartMs = 0, lastFrameMs = 0;

void drawConfig() { ui.renderConfigPage(cfg, tele, configCursor, editingValue, inMenu); }

void switchPage(Page p) {
  currentPage = p;
  tft.fillScreen(TFT_BLACK);
  if (p == PAGE_DIAL)        ui.initDialPage(cfg, tele);
  else if (p == PAGE_PARAMS) ui.initParamsPage(tele);
  else { ui.initConfigPage(); drawConfig(); }
}

void saveAndExit() {
  cfg.save();
  inMenu = false;
  editingValue = false;
  if (cfg.demoMode != cfg.runDemo) { delay(200); ESP.restart(); }   // data source changed
  switchPage(PAGE_DIAL);
}

// Signed number of whole clicks since the last call. Accumulates raw counts, so bounce and
// fast spins can't lose or invert a click, and both directions behave identically.
int readEncoderSteps() {
  int64_t c = encoder.getCount();
  int64_t d = c - lastEncCount;
  if (d == 0) return 0;
  lastEncCount = c;
  encAcc += (int)d;
  int steps = encAcc / ENC_COUNTS_PER_DETENT;        // truncates toward zero -> symmetric
  encAcc -= steps * ENC_COUNTS_PER_DETENT;
  return steps;
}

void applyEdit(int steps) {
  switch (configCursor) {
    case ROW_BRIGHT:    cfg.brightness = constrain(cfg.brightness + steps * 10, 20, 100); ui.setDim(cfg.dimScale()); break;
    case ROW_REDLINE:   cfg.redline = constrain(cfg.redline + steps * 250, 4500, 7500); break;
    case ROW_SPEEDCORR: cfg.speedCorrPct = constrain(cfg.speedCorrPct + steps, 85, 115); break;
    case ROW_COOLANT:   cfg.warnCoolant = constrain(cfg.warnCoolant + steps * 2, 85, 120); break;
    case ROW_SOURCE:    cfg.demoMode = !cfg.demoMode; break;          // applied after Save (restart)
    case ROW_DTC:       tele.dtcCount = (tele.dtcCount > 0) ? 0 : 1; tele.dtcAck = false; break;
    default: break;
  }
}

void onRotate(int steps) {
  if (currentPage == PAGE_CONFIG && inMenu) {
    if (!editingValue) configCursor = ((configCursor + steps % CFG_ROWS) + CFG_ROWS) % CFG_ROWS;   // wraps both ways
    else applyEdit(steps);
    drawConfig();
    return;
  }
  int np = ((int)currentPage + steps % 3 + 3) % 3;
  if (np != (int)currentPage) switchPage((Page)np);
}

void onShortPress() {
  if (currentPage != PAGE_CONFIG) {
    if (tele.dtcCount > 0 && !tele.dtcAck) { tele.dtcAck = true; switchPage(currentPage); }
    return;
  }
  if (!inMenu) { inMenu = true; configCursor = 0; editingValue = false; drawConfig(); return; }

  switch (configCursor) {
    case ROW_SAVE: saveAndExit(); return;
    case ROW_WIFI: obdService.requestWifiSetup(); break;      // portal runs on Core 0
    default:       editingValue = !editingValue; break;
  }
  drawConfig();
}

void handleInput() {
  int steps = readEncoderSteps();
  if (steps) onRotate(steps);

  bool btn = (digitalRead(ENC_SW) == LOW);
  unsigned long now = millis();
  if (btn && !lastBtnState) { pressStartMs = now; longFired = false; }
  if (btn && !longFired && (now - pressStartMs >= LONG_PRESS_MS)) {
    longFired = true;
    if (currentPage == PAGE_CONFIG && inMenu) saveAndExit();
  }
  if (!btn && lastBtnState && !longFired && (now - pressStartMs > 30)) onShortPress();
  lastBtnState = btn;
}

void setup() {
  Serial.begin(115200);
  cfg.load();
  ui.setDim(cfg.dimScale());

  tft.init();
  tft.invertDisplay(false);   // if colors look inverted (black looks white), try true
  tft.setSwapBytes(true);     // needed for the dimmed dial push
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  // Allocate every sprite FIRST, before Bluetooth/WiFi fragment the heap
  face.setColorDepth(8);
  work.setColorDepth(8);
  if (!face.createSprite(SPR, SPR) || !work.createSprite(SPR, SPR) || !ui.beginRow()) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Sprite RAM alloc failed!", 10, 10, 1);
    while (true) delay(1000);
  }
  Serial.printf("Sprites ready. Free heap: %u\n", ESP.getFreeHeap());

  ESP32Encoder::useInternalWeakPullResistors = puType::UP;
  encoder.attachFullQuad(ENC_DT, ENC_CLK);
  encoder.setCount(0);
  pinMode(ENC_SW, INPUT_PULLUP);

  obdService.begin(&tele, &cfg);          // starts the Core 0 tasks (Bluetooth / ELM327 / WiFi / upload)
  switchPage(PAGE_DIAL);
}

void loop() {
  handleInput();

  if (millis() - lastFrameMs < 33) { delay(1); return; }   // ~30 FPS, yields to the idle task
  lastFrameMs = millis();

  if (currentPage == PAGE_DIAL)        ui.renderDialPage(tele, cfg);
  else if (currentPage == PAGE_PARAMS) ui.renderParamsPage(tele, cfg);
  else                                 drawConfig();        // cheap: only changed rows are redrawn (picks up WiFi status)
}