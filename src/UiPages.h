#pragma once
#include <TFT_eSPI.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "Config.h"

#define F_R9  (&FreeSans9pt7b)
#define F_R12 (&FreeSans12pt7b)
#define F_R18 (&FreeSans18pt7b)
#define F_R24 (&FreeSans24pt7b)
#define F_HERO F_R12              // speed / rpm numbers on the params page

// ---- Dial radii (196px sprite, centre 98,98) ----
constexpr int R_BEZEL = 97, R_BEZEL_IN = 93;
constexpr int R_BAND_OUT = 91, R_BAND_IN = 85;     // thick rpm band
constexpr int R_TICK = 82, R_NUM = 65;
constexpr int R_HUB = 53;                          // speed ring
constexpr int R_NEEDLE_IN = 56, R_NEEDLE_OUT = 84;
constexpr int BLK_ROWS = 14;                       // dimmed-push block height (196 = 14 x 14)

// ---- Side arc gauges: concentric with the dial, fully outside the sprite ----
constexpr int DCX = SCREEN_X + CX, DCY = SCREEN_Y + CY;   // 160,120
constexpr int SA_OUT = 129, SA_IN = 122, SA_HALF = 32;

// ---- Params page ----
constexpr int PX_W = 140, PBAR_H = 7;
struct Cell { const char *label; float lo, hi; bool center; };
static const Cell CELLS[8] = {
  {"COOLANT C", 40, 130, false}, {"INTAKE C", 0, 80, false},
  {"THR %", 0, 100, false},      {"LOAD %", 0, 100, false},
  {"MAF g/s", 0, 150, false},    {"TIMING deg", -10, 50, false},
  {"STFT %", -25, 25, true},     {"LTFT %", -25, 25, true}
};

// 3x5 pixel glyphs for the tiny dial labels (X 1 0 K M H /)
static uint16_t pixGlyph(char c) {
  switch (c) {
    case 'X': return 0b101101010101101;
    case '1': return 0b010110010010111;
    case '0': return 0b111101101101111;
    case 'K': return 0b101101110101101;
    case 'M': return 0b101111111101101;
    case 'H': return 0b101101111101101;
    case '/': return 0b001001010100100;
    default:  return 0;
  }
}

class UiRenderer {
private:
  TFT_eSPI &tft;
  TFT_eSprite &face;
  TFT_eSprite &work;
  TFT_eSprite row;                 // 308 x CFG_ROW_H off-screen row buffer (flicker-free settings rows)
  uint8_t dimS = 255;

  // Software dimming of the dial: sprite stays full-colour, brightness is applied while pushing
  uint16_t lut[256];
  bool lutValid = false;
  uint16_t blk[SPR * BLK_ROWS];

  // dial state
  float shownRpm = 0, lastDrawnRpm = -1;
  int lastSpeed = -1, lastCoolant = -999, lastLoad = -999;
  bool faceHasDtcBg = false, dtcShown = false;
  int BuiltRedline = -1;
  int sideKey[2] = {-1, -1};

  // params state
  static constexpr int NSLOT = 10;
  char slotTxt[NSLOT][24];
  uint16_t slotCol[NSLOT];
  uint32_t barKey[8];
  int valPad[8];
  int rpmBarKey = -1;
  bool paramsAlertBg = false;
  int lastBarKey = -1;

  // settings state (sized by CFG_ROWS - keep in sync with the menu)
  char rowKey[CFG_ROWS][48];
  int titleKey = -1;

  template <class G>
  void put(G &g, const GFXfont *f, uint8_t datum, uint16_t color, int x, int y, const char *s) {
    g.setFreeFont(f);
    g.setTextDatum(datum);
    g.setTextColor(color);            // transparent background; caller passes D(..) for direct-to-screen
    g.drawString(s, x, y, 1);
  }

  // Tiny pixel text (centred), scale 1 = 5px tall
  template <class G>
  void pix(G &g, const char *s, int cx, int cy, uint16_t color, int sc) {
    int n = strlen(s), w = n * 4 * sc - sc;
    int x = cx - w / 2, y = cy - (5 * sc) / 2;
    for (int i = 0; i < n; i++, x += 4 * sc) {
      uint16_t gl = pixGlyph(s[i]);
      for (int r = 0; r < 5; r++)
        for (int c = 0; c < 3; c++)
          if ((gl >> (14 - (r * 3 + c))) & 1) g.fillRect(x + c * sc, y + r * sc, sc, sc, color);
    }
  }

  void ensureLut() {
    if (lutValid) return;
    for (int i = 0; i < 256; i++) lut[i] = dimColor(tft.color8to16((uint8_t)i), dimS);
    lutValid = true;
  }
  // colour exactly as it appears after the dial sprite's 8-bit quantisation + dimming
  uint16_t DQ(uint16_t c) { ensureLut(); return lut[tft.color16to8(c)]; }
  uint16_t bgDial(bool alert) { return alert ? DQ(COL_DARKRED) : TFT_BLACK; }

  void pushDimmed(TFT_eSprite &s, int x0, int y0) {
    ensureLut();
    const uint8_t *p = (const uint8_t *)s.getPointer();
    tft.startWrite();
    for (int by = 0; by < SPR; by += BLK_ROWS) {
      const uint8_t *src = p + by * SPR;
      for (int i = 0; i < SPR * BLK_ROWS; i++) blk[i] = lut[src[i]];
      tft.pushImage(x0, y0 + by, SPR, BLK_ROWS, blk);
    }
    tft.endWrite();
  }

public:
  UiRenderer(TFT_eSPI &t, TFT_eSprite &f, TFT_eSprite &w) : tft(t), face(f), work(w), row(&t) {
    resetParamsCache();
    resetConfigCache();
  }

  bool beginRow() { row.setColorDepth(16); return row.createSprite(308, CFG_ROW_H) != nullptr; }
  void setDim(uint8_t s) { dimS = s; lutValid = false; }
  uint16_t D(uint16_t c) const { return dimColor(c, dimS); }

  float valToAngle(float rpm, int maxRpm) {
    return ANG_START + (ANG_END - ANG_START) * (rpm / (float)maxRpm);
  }

  // 0 deg = 6 o'clock, clockwise (90 = 9 o'clock, 180 = 12, 270 = 3)
  void polar(float deg, float r, float &x, float &y, int cx = CX, int cy = CY) {
    float rad = deg * PI / 180.0f;
    x = cx - r * sinf(rad);
    y = cy + r * cosf(rad);
  }

  // ================= PAGE 0: DIAL (sprites use full-colour values; dimming happens on push) =================
  void buildLfaFace(const AppConfig &cfg, bool alert) {
    uint16_t bg = alert ? COL_DARKRED : TFT_BLACK;
    face.fillSprite(bg);

    int startAng = (int)valToAngle(0, cfg.maxRpm);
    int redAng   = (int)valToAngle(cfg.redline, cfg.maxRpm);
    int endAng   = (int)valToAngle(cfg.maxRpm, cfg.maxRpm);

    face.fillSmoothCircle(CX, CY, R_BEZEL, COL_BEZEL, bg);
    face.fillSmoothCircle(CX, CY, R_BEZEL_IN, bg, COL_BEZEL);
    face.drawSmoothCircle(CX, CY, R_BEZEL, COL_BEZEL_HI, COL_BEZEL);

    // inner glow track, then the outer rpm band (normal range + redline)
    face.drawSmoothArc(CX, CY, R_TICK - 2, R_NUM - 8, startAng, redAng, COL_TEAL_GLOW, bg, false);
    face.drawSmoothArc(CX, CY, R_TICK - 2, R_NUM - 8, redAng, endAng, COL_REDGLOW, bg, false);
    face.drawSmoothArc(CX, CY, R_BAND_OUT, R_BAND_IN, startAng, redAng, COL_TEAL, bg, false);
    face.drawSmoothArc(CX, CY, R_BAND_OUT, R_BAND_IN, redAng, endAng, COL_REDLINE, bg, false);

    for (int v = 0; v <= cfg.maxRpm; v += 200) {
      float ang = valToAngle(v, cfg.maxRpm), x0, y0, x1, y1;
      bool red = (v >= cfg.redline);
      polar(ang, R_TICK, x0, y0);
      if (v % 1000 == 0) {
        polar(ang, R_TICK - 8, x1, y1);
        face.drawWideLine(x0, y0, x1, y1, 2.5f, red ? COL_REDLINE : TFT_WHITE);
        float nx, ny;
        polar(ang, R_NUM, nx, ny);
        face.setFreeFont(F_R9);
        face.setTextDatum(MC_DATUM);
        face.setTextColor(COL_NUM);
        face.drawNumber(v / 1000, (int)nx, (int)ny, 1);
      } else if (v % 500 == 0) {
        polar(ang, R_TICK - 5, x1, y1);
        face.drawWideLine(x0, y0, x1, y1, 1.5f, red ? COL_REDLINE : COL_NUM);
      } else {
        polar(ang, R_TICK - 3, x1, y1);
        face.drawLine((int)x0, (int)y0, (int)x1, (int)y1, red ? COL_REDLINE : COL_LABEL);
      }
    }

    // speed ring + tiny unit labels
    face.drawSmoothCircle(CX, CY, R_HUB, COL_STEEL, bg);
    face.drawSmoothCircle(CX, CY, R_HUB - 1, COL_STEEL, bg);
    pix(face, "KM/H", CX, CY + 20, COL_LABEL, 2);
    pix(face, "X1000", CX, CY + 37, COL_LABEL, 1);

    faceHasDtcBg = alert;
    BuiltRedline = cfg.redline;
  }

  void initDialPage(const AppConfig &cfg, const Telemetry &t) {
    bool alertBg = (t.dtcCount > 0 && !t.dtcAck);
    tft.fillScreen(bgDial(alertBg));
    buildLfaFace(cfg, alertBg);
    drawSideStatic(true, "TEMP");
    drawSideStatic(false, "LOAD");
    lastDrawnRpm = -1; lastSpeed = -1; lastCoolant = -999; lastLoad = -999;
    sideKey[0] = sideKey[1] = -1;
    dtcShown = false;
  }

  void renderDialPage(const Telemetry &t, const AppConfig &cfg) {
    bool alertBg = (t.dtcCount > 0 && !t.dtcAck);
    if (alertBg != faceHasDtcBg || cfg.redline != BuiltRedline) initDialPage(cfg, t);
    uint16_t bg = bgDial(alertBg);

    if (t.coolant != lastCoolant) { updateSide(true,  t.coolant, 40, 130, "C", cfg.warnCoolant, bg); lastCoolant = t.coolant; }
    if (t.load != lastLoad)       { updateSide(false, t.load,     0, 100, "%", 90,              bg); lastLoad = t.load; }

    bool showDtc = t.dtcCount > 0;
    if (showDtc != dtcShown) {
      dtcShown = showDtc;
      tft.fillRect(100, 221, 120, 19, showDtc ? D(COL_REDLINE) : bg);
      if (showDtc) put(tft, F_R9, MC_DATUM, D(TFT_WHITE), 160, 231, "DTC FAULT");
    }

    shownRpm += (t.rpm - shownRpm) * 0.28f;
    if (fabsf(t.rpm - shownRpm) < 1.0f) shownRpm = t.rpm;

    if (shownRpm != lastDrawnRpm || t.speed != lastSpeed) {
      face.pushToSprite(&work, 0, 0);

      float a = valToAngle(shownRpm, cfg.maxRpm), x0, y0, x1, y1;
      polar(a, R_NEEDLE_IN, x0, y0);
      polar(a, R_NEEDLE_OUT, x1, y1);
      work.drawWideLine(x0, y0, x1, y1, 3.0f, shownRpm >= cfg.redline ? COL_REDLINE : COL_NEEDLE);
      work.drawWideLine(x0, y0, x1, y1, 1.5f, TFT_WHITE);

      work.setFreeFont(F_R24);
      work.setTextDatum(MC_DATUM);
      work.setTextColor(TFT_WHITE);
      work.drawNumber(t.speed, CX, CY - 9, 1);

      pushDimmed(work, SCREEN_X, SCREEN_Y);
      lastDrawnRpm = shownRpm;
      lastSpeed = t.speed;
    }
  }

  // ================= PAGE 1: PARAMETERS =================
  void initParamsPage(const Telemetry &t) {
    bool alertBg = (t.dtcCount > 0 && !t.dtcAck);
    tft.fillScreen(alertBg ? D(COL_DARKRED) : TFT_BLACK);

    tft.drawFastHLine(0, 54, 320, D(COL_LINE));
    tft.drawFastVLine(160, 56, 156, D(COL_LINE));

    put(tft, F_R9, TL_DATUM, D(COL_ACCENT2), 10, 2, "SPEED(km/h)");
    put(tft, F_R9, TL_DATUM, D(COL_ACCENT2), 170, 2, "ENGINE RPM");

    tft.setFreeFont(F_R9);
    for (int i = 0; i < 8; i++) {
      int cx = (i < 4) ? 10 : 170;
      put(tft, F_R9, TL_DATUM, D(COL_LABEL), cx, 60 + (i & 3) * 40, CELLS[i].label);
      // value padding stops exactly where the label ends, so it can never wipe the label
      int labelEnd = cx + tft.textWidth(CELLS[i].label, 1);
      valPad[i] = max(24, cx + PX_W - labelEnd - 6);
    }

    paramsAlertBg = alertBg;
    resetParamsCache();
  }

  void renderParamsPage(const Telemetry &t, const AppConfig &cfg) {
    bool alert = (t.dtcCount > 0 && !t.dtcAck);
    if (alert != paramsAlertBg) initParamsPage(t);
    uint16_t bg = alert ? D(COL_DARKRED) : TFT_BLACK;
    char b[24];

    snprintf(b, sizeof b, "%d", t.speed);
    drawSlot(0, b, COL_NEEDLE, bg, 10, 17, 120, TL_DATUM, F_HERO);
    snprintf(b, sizeof b, "%d", t.rpm);
    drawSlot(1, b, t.rpm >= cfg.redline ? COL_REDLINE : TFT_WHITE, bg, 170, 17, 120, TL_DATUM, F_HERO);
    drawRpmBar(t.rpm, cfg);

    float v[8] = {(float)t.coolant, (float)t.intake, (float)t.throttle, (float)t.load, t.maf, t.timing, t.stft, t.ltft};
    for (int i = 0; i < 8; i++) {
      int cx = (i < 4) ? 10 : 170, y0 = 60 + (i & 3) * 40;
      uint16_t col = COL_ACCENT, tcol = TFT_WHITE;
      if (i == 0 && t.coolant >= cfg.warnCoolant)                         col = tcol = COL_REDLINE;
      else if ((i == 3 && t.load >= 90) || (i >= 6 && fabsf(v[i]) > 10)) col = tcol = COL_AMBER;

      if (i < 4) snprintf(b, sizeof b, "%d", (int)v[i]);
      else       snprintf(b, sizeof b, "%.1f", v[i]);
      drawSlot(2 + i, b, tcol, bg, cx + PX_W, y0, valPad[i], TR_DATUM, F_R9);
      drawBar(i, cx, y0 + 20, v[i], CELLS[i].lo, CELLS[i].hi, CELLS[i].center, col);
    }

    // Bottom strip only appears for a DTC fault or a lost OBD link
    int key = t.dtcCount > 0 ? 100 + t.dtcCount : ((!cfg.runDemo && !t.btConnected) ? 1 : 0);
    if (key != lastBarKey) {
      lastBarKey = key;
      tft.setTextPadding(0);
      if (t.dtcCount > 0) {
        tft.fillRect(0, 216, 320, 24, D(COL_REDLINE));
        snprintf(b, sizeof b, "DTC FAULT: %d CODES", t.dtcCount);
        put(tft, F_R9, MC_DATUM, D(TFT_WHITE), 160, 228, b);
      } else {
        tft.fillRect(0, 216, 320, 24, bg);
        if (key == 1) put(tft, F_R9, MC_DATUM, D(COL_AMBER), 160, 228, "OBD LINK LOST");
      }
    }
  }

  // ================= PAGE 2: SETTINGS (8 rows, y = 32..236) =================
  void initConfigPage() { resetConfigCache(); }

  // Only rows whose content changed are redrawn, each in one burst via the row buffer (no flicker).
  void renderConfigPage(const AppConfig &cfg, const Telemetry &t, int cursor, bool editing, bool inMenu) {
    int ts = (!inMenu ? 0 : (editing ? 2 : 1)) * 256 + dimS;
    if (ts != titleKey) {
      titleKey = ts;
      tft.setTextPadding(300);
      tft.setFreeFont(F_R9);
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(D(COL_ACCENT2), TFT_BLACK);
      tft.drawString(!inMenu ? "SETTINGS  |  CLICK TO ENTER" : (editing ? "EDITING  |  CLICK = DONE" : "SELECT  |  HOLD = SAVE"), 12, 6, 1);
      tft.setTextPadding(0);
      tft.drawFastHLine(0, 28, 320, D(COL_LINE));
    }

    static const char *labels[CFG_ROWS] = {"Brightness", "Redline RPM", "Speed Correction", "Coolant Warning",
                                           "Data Source", "Test DTC Fault", "Wifi Setup", "Save & Exit"};
    char vals[CFG_ROWS][24];
    snprintf(vals[ROW_BRIGHT],    24, "%d %%", cfg.brightness);
    snprintf(vals[ROW_REDLINE],   24, "%d RPM", cfg.redline);
    snprintf(vals[ROW_SPEEDCORR], 24, "%d %%", cfg.speedCorrPct);
    snprintf(vals[ROW_COOLANT],   24, "%d C", cfg.warnCoolant);
    snprintf(vals[ROW_SOURCE],    24, "%s", cfg.demoMode ? "DEMO SIM" : "LIVE OBDII");
    snprintf(vals[ROW_DTC],       24, "%s", t.dtcCount > 0 ? "ACTIVE" : "CLEAR");
    snprintf(vals[ROW_WIFI],      24, "%s",
             t.wifiState == NET_ONLINE ? "ONLINE" :
             t.wifiState == NET_CONNECTING ? "CONNECTING" :
             t.wifiState == NET_PORTAL ? "JOIN " PORTAL_AP_NAME : "NOT SET");
    vals[ROW_SAVE][0] = 0;

    for (int i = 0; i < CFG_ROWS; i++) {
      bool sel = inMenu && (i == cursor), edit = sel && editing;
      char k[48];
      snprintf(k, sizeof k, "%s|%d%d|%d", vals[i], sel, edit, dimS);
      if (strcmp(k, rowKey[i]) == 0) continue;
      strlcpy(rowKey[i], k, sizeof rowKey[i]);

      row.fillSprite(D(sel ? (edit ? COL_ACCENT : COL_SEL) : TFT_BLACK));
      row.setFreeFont(F_R9);
      row.setTextDatum(TL_DATUM);
      row.setTextColor(D(sel ? TFT_WHITE : COL_LABEL));
      row.drawString(labels[i], 8, 4, 1);
      if (vals[i][0]) {
        row.setTextDatum(TR_DATUM);
        row.setTextColor(D(sel ? (edit ? TFT_WHITE : COL_NEEDLE) : TFT_WHITE));
        row.drawString(vals[i], 300, 4, 1);
      }
      row.pushSprite(6, CFG_ROW_Y0 + i * CFG_ROW_PITCH);
    }
  }

private:
  void resetParamsCache() {
    for (int i = 0; i < NSLOT; i++) { slotTxt[i][0] = '\x01'; slotTxt[i][1] = 0; slotCol[i] = 0; }
    for (int i = 0; i < 8; i++) barKey[i] = 0xFFFFFFFF;
    rpmBarKey = -1; lastBarKey = -1;
  }
  void resetConfigCache() {
    for (int i = 0; i < CFG_ROWS; i++) rowKey[i][0] = 0;
    titleKey = -1;
  }

  void drawSlot(int i, const char *txt, uint16_t col, uint16_t bg, int x, int y, int pad, uint8_t datum, const GFXfont *font) {
    if (slotCol[i] == col && strcmp(slotTxt[i], txt) == 0) return;
    strlcpy(slotTxt[i], txt, sizeof slotTxt[i]);
    slotCol[i] = col;
    tft.setFreeFont(font);
    tft.setTextDatum(datum);
    tft.setTextPadding(pad);
    tft.setTextColor(D(col), bg);
    tft.drawString(txt, x, y, 1);
    tft.setTextPadding(0);
  }

  void drawBar(int i, int x, int y, float v, float lo, float hi, bool center, uint16_t col) {
    int p = (int)(constrain((v - lo) / (hi - lo), 0.0f, 1.0f) * PX_W);
    int a = center ? min(p, PX_W / 2) : 0;
    int b = center ? max(p, PX_W / 2) : p;
    uint32_t key = (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)col << 16);
    if (barKey[i] == key) return;
    barKey[i] = key;
    uint16_t track = D(COL_TRACK);
    if (a > 0)    tft.fillRect(x, y, a, PBAR_H, track);
    if (b > a)    tft.fillRect(x + a, y, b - a, PBAR_H, D(col));
    if (b < PX_W) tft.fillRect(x + b, y, PX_W - b, PBAR_H, track);
    if (center)   tft.drawFastVLine(x + PX_W / 2, y - 2, PBAR_H + 4, D(COL_NUM));
  }

  void drawRpmBar(int rpm, const AppConfig &cfg) {
    const int X = 10, Y = 41, W = 300, H = 7;
    int fill = (int)(constrain(rpm / (float)cfg.maxRpm, 0.0f, 1.0f) * W);
    int redX = (int)((float)cfg.redline / cfg.maxRpm * W);
    int key = fill | (redX << 10);
    if (key == rpmBarKey) return;
    rpmBarKey = key;
    int blueEnd = min(fill, redX), redEnd = max(fill, redX);
    tft.fillRect(X, Y, blueEnd, H, D(COL_ACCENT));
    if (redX > blueEnd) tft.fillRect(X + blueEnd, Y, redX - blueEnd, H, D(COL_TRACK));
    if (redEnd > redX)  tft.fillRect(X + redX, Y, redEnd - redX, H, D(COL_REDLINE));
    if (W > redEnd)     tft.fillRect(X + redEnd, Y, W - redEnd, H, D(COL_REDGLOW));
  }

  void drawSideStatic(bool left, const char *label) {
    int lo = (left ? 90 : 270) - SA_HALF;
    for (int k = 0; k <= 6; k++) {
      float a = lo + k * (2.0f * SA_HALF / 6.0f), x0, y0, x1, y1;
      polar(a, SA_OUT + 3, x0, y0, DCX, DCY);
      polar(a, SA_OUT + 8, x1, y1, DCX, DCY);
      tft.drawLine((int)x0, (int)y0, (int)x1, (int)y1, D(COL_LABEL));
    }
    tft.setTextPadding(0);
    put(tft, F_R9, MC_DATUM, D(COL_ACCENT2), left ? 35 : 285, 30, label);
  }

  void updateSide(bool left, int value, int vmin, int vmax, const char *unit, int warnAt, uint16_t bg) {
    float f = constrain((float)(value - vmin) / (vmax - vmin), 0.0f, 1.0f);
    int lo = (left ? 90 : 270) - SA_HALF, hi = lo + 2 * SA_HALF;
    int span = (int)(f * 2 * SA_HALF);
    int a = left ? lo + span : hi - span;
    bool warn = value >= warnAt;
    int key = a * 2 + (warn ? 1 : 0);

    if (key != sideKey[left ? 0 : 1]) {
      sideKey[left ? 0 : 1] = key;
      uint16_t col = D(warn ? COL_REDLINE : COL_ACCENT);
      int fs = left ? lo : a, fe = left ? a : hi;
      int es = left ? a : lo, ee = left ? hi : a;
      if (fe > fs) tft.drawSmoothArc(DCX, DCY, SA_OUT, SA_IN, fs, fe, col, bg, false);
      if (ee > es) tft.drawSmoothArc(DCX, DCY, SA_OUT, SA_IN, es, ee, D(COL_TRACK), bg, false);
    }

    char b[12];
    snprintf(b, sizeof b, "%d%s", value, unit);
    tft.setFreeFont(F_R9);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(D(TFT_WHITE), bg);
    tft.setTextPadding(46);
    tft.drawString(b, left ? 35 : 285, 206, 1);
    tft.setTextPadding(0);
  }
};