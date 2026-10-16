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
bool inBtScreen = false;
bool showingBtResults = false;
int btCursor = 0;

constexpr int ENC_COUNTS_PER_STEP = 1;
constexpr unsigned long LONG_PRESS_MS = 700;

bool lastBtnState = false, longFired = false;
unsigned long pressStartMs = 0, lastFrameMs = 0;

void drawConfig() { ui.renderConfigPage(cfg, tele, configCursor, editingValue, inMenu); }
void drawBluetooth();

void switchPage(Page p) {
  currentPage = p;
  obdService.setActivePage((uint8_t)p);
  tft.fillScreen(TFT_BLACK);
  if (p == PAGE_DIAL)        ui.initDialPage(cfg, tele);
  else if (p == PAGE_PARAMS) ui.initParamsPage(tele);
  else { ui.initConfigPage(); drawConfig(); }
}

void enterBluetooth() {
  inBtScreen = true;
  showingBtResults = false;
  btCursor = 0;
  ui.initConfigPage();
  drawBluetooth();
}

void leaveBluetooth() {
  inBtScreen = false;
  showingBtResults = false;
  switchPage(PAGE_CONFIG);
}

void drawBluetooth() {
  char rows[CONFIG_ROWS][48] = {};
  const char *items[CONFIG_ROWS] = {};
  int count = 0;

  if (showingBtResults) {
    if (tele.btState == BT_SCANNING) {
      snprintf(rows[count++], sizeof rows[0], "Searching nearby devices...");
    } else {
      int nearby = obdService.nearbyCount();
      for (int i = 0; i < nearby && count < CONFIG_ROWS - 2; i++)
        snprintf(rows[count++], sizeof rows[0], "%s", obdService.nearbyName(i));
      snprintf(rows[count++], sizeof rows[0], "[ Search again ]");
      snprintf(rows[count++], sizeof rows[0], "[ Back ]");
    }
  } else {
    snprintf(rows[count++], sizeof rows[0], "Saved: %s", tele.btName[0] ? tele.btName : "none");
    const char *status = tele.btState == BT_CONNECTED ? "Connected" :
                         tele.btState == BT_CONNECTING ? "Connecting..." :
                         tele.btState == BT_SCANNING ? "Searching..." :
                         tele.btState == BT_ERROR ? "Connection failed" :
                         tele.btName[0] ? "Saved / offline" : "Not paired";
    snprintf(rows[count++], sizeof rows[0], "Status: %s", status);
    snprintf(rows[count++], sizeof rows[0], "[ Search nearby devices ]");
    snprintf(rows[count++], sizeof rows[0], "%s",
             tele.btName[0] ? "[ Remove device ]" : "[ No device to remove ]");
    snprintf(rows[count++], sizeof rows[0], "[ Back ]");
  }

  for (int i = 0; i < count; i++) items[i] = rows[i];
  const char *title = showingBtResults ? "BLUETOOTH  |  PIN 1234  |  CLICK TO PAIR" :
                                         "BLUETOOTH DEVICE  |  PIN 1234";
  ui.renderBluetoothPage(title, items, count, btCursor);
}

void saveAndExit() {
  cfg.save();
  inMenu = false;
  editingValue = false;
  if (cfg.demoMode != cfg.runDemo) { delay(200); ESP.restart(); }   // data source changed
  switchPage(PAGE_DIAL);
}

void onRotate(int dir) {
  if (inBtScreen) {
    int count;
    if (showingBtResults) {
      count = tele.btState == BT_SCANNING ? 1 : obdService.nearbyCount() + 2;
    } else {
      count = 5;
    }
    if (count < 1) count = 1;
    btCursor = constrain(btCursor + dir, 0, count - 1);
    drawBluetooth();
    return;
  }

  if (currentPage == PAGE_CONFIG && inMenu) {
    if (!editingValue) {
      int old = configCursor;
      configCursor = constrain(configCursor + dir, 0, CONFIG_ROWS - 1);
      if (configCursor == old) return;
    } else {
      switch (configCursor) {
        case ROW_BRIGHT:    cfg.brightness = constrain(cfg.brightness + dir * 5, 20, 100); ui.setDim(cfg.dimScale()); break;
        case ROW_REDLINE:   cfg.redline = constrain(cfg.redline + dir * 10, 4500, 7500); break;
        case ROW_SPEEDCORR: cfg.speedCorrPct = constrain(cfg.speedCorrPct + dir, 85, 115); break;
        case ROW_COOLANT:   cfg.warnCoolant = constrain(cfg.warnCoolant + dir * 2, 85, 120); break;
        case ROW_SOURCE:    cfg.demoMode = !cfg.demoMode; break;
        case ROW_DTC:       tele.dtcCount = (tele.dtcCount > 0) ? 0 : 1; tele.dtcAck = false; break;
      }
    }
    drawConfig();
    return;
  }
  switchPage((Page)(((int)currentPage + dir + 3) % 3));
}

void onShortPress() {
  if (inBtScreen) {
    if (showingBtResults) {
      if (tele.btState == BT_SCANNING) return;
      int nearby = obdService.nearbyCount();
      if (btCursor < nearby) {
        cfg.demoMode = false;
        cfg.runDemo = false;
        cfg.save();
        obdService.selectDevice(btCursor);
        showingBtResults = false;
        btCursor = 0;
      } else if (btCursor == nearby) {
        obdService.requestScan();
        btCursor = 0;
      } else {
        leaveBluetooth();
        return;
      }
    } else {
      if (btCursor == 2) {
        showingBtResults = true;
        btCursor = 0;
        obdService.requestScan();
      } else if (btCursor == 3 && tele.btName[0]) {
        cfg.demoMode = true;
        cfg.runDemo = true;
        cfg.save();
        obdService.forgetDevice();
        btCursor = 0;
      } else if (btCursor == 4) {
        leaveBluetooth();
        return;
      }
    }
    ui.initConfigPage();
    drawBluetooth();
    return;
  }

  if (currentPage != PAGE_CONFIG) {
    if (tele.dtcCount > 0 && !tele.dtcAck) { tele.dtcAck = true; switchPage(currentPage); }
    return;
  }
  if (!inMenu) { inMenu = true; configCursor = 0; editingValue = false; }
  else if (configCursor == ROW_BT) { enterBluetooth(); return; }
  else if (configCursor == ROW_SAVE) { saveAndExit(); return; }
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
    if (inBtScreen) leaveBluetooth();
    else if (currentPage == PAGE_CONFIG && inMenu) saveAndExit();
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

  if (!obdService.begin(&tele, &cfg)) {
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Bluetooth service failed", 10, 10, 1);
    while (true) delay(1000);
  }

  switchPage(PAGE_DIAL);
}

void loop() {
  handleInput();
  obdService.update(tele, cfg);

  if (millis() - lastFrameMs < 33) { delay(1); return; }
  lastFrameMs = millis();

  if (inBtScreen)                       drawBluetooth();
  else if (currentPage == PAGE_DIAL)    ui.renderDialPage(tele, cfg);
  else if (currentPage == PAGE_PARAMS) ui.renderParamsPage(tele, cfg);
}