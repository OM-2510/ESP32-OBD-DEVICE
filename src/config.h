#pragma once
#include <Arduino.h>
#include <Preferences.h>

// ================= Hardware Pins =================
#ifndef ENC_CLK
#define ENC_CLK 32
#endif
#ifndef ENC_DT
#define ENC_DT 33
#endif
#ifndef ENC_SW
#define ENC_SW 27
#endif

#ifndef PORTAL_AP_NAME
#define PORTAL_AP_NAME "i10_Dash_Setup"
#endif

// ================= Dial sprite =================
constexpr int SPR = 196;
constexpr int CX = 98, CY = 98;
constexpr int SCREEN_X = 62, SCREEN_Y = 22;       // dial centre lands on screen (160,120)

constexpr int ANG_START = 0;     // 0 deg = 6 o'clock
constexpr int ANG_END   = 300;   // sweeps 300 deg clockwise

// ================= Settings menu layout =================
enum CfgRow { ROW_BRIGHT, ROW_REDLINE, ROW_SPEEDCORR, ROW_COOLANT, ROW_SOURCE, ROW_DTC, ROW_WIFI, ROW_SAVE, CFG_ROWS };
constexpr int CFG_ROW_Y0 = 32, CFG_ROW_PITCH = 26, CFG_ROW_H = 22;   // 8 rows end at y=236 (screen is 240)

// ================= Color palette (RGB565) =================
constexpr uint16_t COL_ACCENT    = 0x1C9F; // electric blue
constexpr uint16_t COL_ACCENT2   = 0x05FF; // sky blue
constexpr uint16_t COL_NEEDLE    = 0x3DBF; // ice blue
constexpr uint16_t COL_NUM       = 0xEF7E; // soft white
constexpr uint16_t COL_LABEL     = 0x6BD1; // blue-grey
constexpr uint16_t COL_LINE      = 0x21A9; // dark steel
constexpr uint16_t COL_TRACK     = 0x1926; // navy (empty bar/gauge track)
constexpr uint16_t COL_SEL       = 0x096B; // navy (selected menu row)
constexpr uint16_t COL_BEZEL     = 0x2966;
constexpr uint16_t COL_BEZEL_HI  = 0x5AEC;
constexpr uint16_t COL_TEAL      = 0x039B; // cobalt blue (rpm band)
constexpr uint16_t COL_TEAL_GLOW = 0x01C9; // ghost blue (inner glow track)
constexpr uint16_t COL_STEEL     = 0x4A6A; // titanium silver (speed ring)
constexpr uint16_t COL_REDLINE   = 0xE905;
constexpr uint16_t COL_REDGLOW   = 0x4841;
constexpr uint16_t COL_AMBER     = 0xFD20;
constexpr uint16_t COL_DARKRED   = 0x7800; // DTC alert background

// Software brightness: scales every RGB565 colour (an "alpha over black" layer)
inline uint16_t dimColor(uint16_t c, uint8_t s) {
  uint32_t r = ((c >> 11) & 31) * s / 255;
  uint32_t g = ((c >> 5) & 63) * s / 255;
  uint32_t b = (c & 31) * s / 255;
  return (r << 11) | (g << 5) | b;
}

// ================= Telemetry Data =================
// Written by the Core 0 worker, read by the Core 1 renderer. Every field is a 32-bit (or smaller)
// aligned primitive, so single reads/writes are atomic on the ESP32; the UI tolerates a frame where
// two fields come from different poll cycles.
constexpr uint8_t NET_OFF = 0, NET_CONNECTING = 1, NET_ONLINE = 2, NET_PORTAL = 3;

struct Telemetry {
  int rpm         = 0;
  int speed       = 0;
  int coolant     = 0;
  int intake      = 0;
  int throttle    = 0;
  float maf       = 0.0f;
  float timing    = 0.0f;
  int load        = 0;
  float stft      = 0.0f;
  float ltft      = 0.0f;
  uint8_t dtcCount = 0;
  bool dtcAck     = false;
  bool btConnected = false;
  uint8_t wifiState = NET_OFF;
};

// ================= Persistent Settings (NVS) =================
struct AppConfig {
  uint8_t brightness = 100;   // 20..100 % (software dimming)
  int redline        = 6500;
  int maxRpm         = 8000;
  int speedCorrPct   = 100;
  int warnCoolant    = 105;
  bool demoMode      = true;  // SAVED setting
  bool runDemo       = true;  // RUNTIME mode (not saved)

  uint8_t dimScale() const { return (uint8_t)map(brightness, 0, 100, 0, 255); }

  void load() {
    Preferences prefs;
    prefs.begin("obd_cfg", true);
    brightness   = constrain(prefs.getUChar("bright", 100), 20, 100);
    redline      = prefs.getInt("redline", 6500);
    speedCorrPct = prefs.getInt("spdcorr", 100);
    warnCoolant  = prefs.getInt("warncool", 105);
    demoMode     = prefs.getBool("demo", true);
    prefs.end();
    runDemo = demoMode;
  }

  void save() {
    Preferences prefs;
    prefs.begin("obd_cfg", false);
    prefs.putUChar("bright", brightness);
    prefs.putInt("redline", redline);
    prefs.putInt("spdcorr", speedCorrPct);
    prefs.putInt("warncool", warnCoolant);
    prefs.putBool("demo", demoMode);
    prefs.end();
  }
};