#include <Arduino.h>
#include <TFT_eSPI.h>
#include <ESP32Encoder.h>
#include "Config.h"
#include "ObdService.h"
#include "UiPages.h"

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
// In menu               : rotate = scroll rows / change value, click = edit on/off, HOLD = save & exit
bool inMenu = false;
bool editingValue = false;
int configCursor = 0;

constexpr int ENC_COUNTS_PER_STEP = 1;
constexpr unsigned long LONG_PRESS_MS = 700;

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

void onRotate(int dir) {
  if (currentPage == PAGE_CONFIG && inMenu) {
    if (!editingValue) {
      int old = configCursor;
      configCursor = constrain(configCursor + dir, 0, 6);
      if (configCursor == old) return;
    } else {
      switch (configCursor) {
        case 0: cfg.brightness = constrain(cfg.brightness + dir * 10, 20, 100); ui.setDim(cfg.dimScale()); break;
        case 1: cfg.redline = constrain(cfg.redline + dir * 250, 4500, 7500); break;
        case 2: cfg.speedCorrPct = constrain(cfg.speedCorrPct + dir, 85, 115); break;
        case 3: cfg.warnCoolant = constrain(cfg.warnCoolant + dir * 2, 85, 120); break;
        case 4: cfg.demoMode = !cfg.demoMode; break;   // applied after Save (restart)
        case 5: tele.dtcCount = (tele.dtcCount > 0) ? 0 : 1; tele.dtcAck = false; break;
      }
    }
    drawConfig();
    return;
  }
  switchPage((Page)(((int)currentPage + dir + 3) % 3));
}

void onShortPress() {
  if (currentPage != PAGE_CONFIG) {
    if (tele.dtcCount > 0 && !tele.dtcAck) { tele.dtcAck = true; switchPage(currentPage); }
    return;
  }
  if (!inMenu) { inMenu = true; configCursor = 0; editingValue = false; }
  else if (configCursor == 6) { saveAndExit(); return; }
  else editingValue = !editingValue;
  drawConfig();
}

void handleInput() {
  long c = encoder.getCount();
  if (labs(c) >= ENC_COUNTS_PER_STEP) {
    encoder.setCount(0);
    onRotate(c > 0 ? 1 : -1);
  }

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

  // Allocate all sprites FIRST (before Bluetooth fragments the heap)
  face.setColorDepth(8);
  work.setColorDepth(8);
  if (!face.createSprite(SPR, SPR) || !work.createSprite(SPR, SPR) || !ui.beginRow()) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Sprite RAM alloc failed!", 10, 10, 1);
    while (true) delay(1000);
  }
  Serial.printf("Sprites ready. Free heap: %u\n", ESP.getFreeHeap());

  ESP32Encoder::useInternalWeakPullResistors = puType::up;
  encoder.attachHalfQuad(ENC_DT, ENC_CLK);
  encoder.setCount(0);
  pinMode(ENC_SW, INPUT_PULLUP);

  if (!cfg.runDemo) {
    tft.setFreeFont(F_R12);
    tft.setTextColor(ui.D(TFT_WHITE), TFT_BLACK);
    tft.drawString("Connecting to OBDII...", 20, 100, 1);
    tele.btConnected = obdService.connectBT("OBDII");
    Serial.printf("BT connected: %d. Free heap: %u\n", tele.btConnected, ESP.getFreeHeap());
    if (!tele.btConnected) {
      tft.setFreeFont(F_R9);
      tft.setTextColor(ui.D(TFT_RED), TFT_BLACK);
      tft.drawString("OBDII failed - using demo", 20, 140, 1);
      delay(2000);
      cfg.runDemo = true;
    }
  }

  switchPage(PAGE_DIAL);
}

void loop() {
  handleInput();
  obdService.update(tele, cfg);

  if (millis() - lastFrameMs < 33) return;
  lastFrameMs = millis();

  if (currentPage == PAGE_DIAL)        ui.renderDialPage(tele, cfg);
  else if (currentPage == PAGE_PARAMS) ui.renderParamsPage(tele, cfg);
}