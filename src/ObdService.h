#pragma once
#include <atomic>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <ELMduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include "Config.h"

// ---------------- Build-time defaults (override with -D in platformio.ini) ----------------
#ifndef OBD_BT_NAME
#define OBD_BT_NAME "OBDII"                 // Bluetooth name of the ELM327 dongle
#endif
#ifndef OBD_BT_PIN
#define OBD_BT_PIN "1234"                   // most clones: 1234 or 0000
#endif
#ifndef BACKEND_URL_DEFAULT
#define BACKEND_URL_DEFAULT "http://192.168.1.100:5000/api/esp_telemetry"   // editable in the WiFi portal
#endif
#ifndef BACKEND_KEY_DEFAULT
#define BACKEND_KEY_DEFAULT "i10-telemetry-live"                            // editable in the WiFi portal
#endif
#ifndef AUTO_PORTAL_IF_NO_CREDS
#define AUTO_PORTAL_IF_NO_CREDS 0           // 1 = open the portal at boot when no WiFi is saved
#endif
#ifndef POST_IN_DEMO
#define POST_IN_DEMO 1                      // 1 = also upload simulator data (handy for testing the backend)
#endif
constexpr uint32_t POST_INTERVAL_MS = 2000;
constexpr uint8_t  SLOW_EVERY       = 2;    // one slow PID after every N fast (RPM+speed) cycles

/*
 * Core 0 worker. Two FreeRTOS tasks, both pinned to Core 0 so the 30 FPS renderer on Core 1 is never touched:
 *   obdTask (prio 2): Bluetooth link, ELM327 init, auto-reconnect, PID state machine (or simulator).
 *   netTask (prio 1): WiFi, captive portal, HTTP upload. A slow/blocked HTTP request can only stall this task;
 *                     the higher-priority obdTask preempts it, so PID polling keeps running.
 * They coordinate only through the atomic flags below. The Bluetooth stack is always started/stopped by obdTask.
 */
class ObdService {
public:
  void begin(Telemetry *t, AppConfig *c) {
    tele = t;
    cfg = c;
    loadNetConfig();

    WiFi.persistent(true);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin();                                   // rejoin the saved network (returns immediately)

    xTaskCreatePinnedToCore(obdTrampoline, "obdTask", 8192, this, 2, nullptr, 0);
    xTaskCreatePinnedToCore(netTrampoline, "netTask", 12288, this, 1, nullptr, 0);
  }

  // Called from the UI (Core 1). Returns immediately; the portal runs on Core 0.
  void requestWifiSetup() { portalReq = true; }

private:
  BluetoothSerial SerialBT;
  ELM327 elm;
  Telemetry *tele = nullptr;
  AppConfig *cfg = nullptr;

  std::atomic<bool> portalReq{false};     // UI -> net task
  std::atomic<bool> btReleaseReq{false};  // net task -> obd task ("drop Bluetooth now")
  std::atomic<bool> btReleased{false};    // obd task -> net task ("Bluetooth is off")

  bool btStarted = false, elmReady = false;
  char url[112] = BACKEND_URL_DEFAULT;
  char key[48]  = BACKEND_KEY_DEFAULT;

  uint8_t pidStep = 0, fastCount = 0, nextSlow = 2, errCount = 0;
  unsigned long lastSimMs = 0;
  int simDir = 85;

  static void obdTrampoline(void *a) { ((ObdService *)a)->obdLoop(); vTaskDelete(nullptr); }
  static void netTrampoline(void *a) { ((ObdService *)a)->netLoop(); vTaskDelete(nullptr); }

  // =====================================================================================
  //                                   OBD TASK (Core 0)
  // =====================================================================================
  void obdLoop() {
    uint32_t retryMs = 1000;
    uint8_t fails = 0;

    for (;;) {
      if (btReleaseReq) { releaseBt(); vTaskDelay(pdMS_TO_TICKS(100)); continue; }   // WiFi portal needs the radio
      btReleased = false;

      if (cfg->runDemo) { runSimulator(); vTaskDelay(pdMS_TO_TICKS(40)); continue; }

      // ---- (re)connect + ELM327 init ----
      if (!elmReady) {
        tele->btConnected = false;
        if (connectAndInit()) {
          elmReady = true;
          tele->btConnected = true;
          fails = 0; retryMs = 1000;
          resetPollState();
          Serial.printf("ELM327 ready. Free heap: %u\n", ESP.getFreeHeap());
        } else {
          if (++fails >= 4) { stopBt(); fails = 0; }            // every 4th failure: full Bluetooth stack restart
          vTaskDelay(pdMS_TO_TICKS(retryMs));
          retryMs = (retryMs * 2 > 15000) ? 15000 : retryMs * 2;  // 1s, 2s, 4s, 8s, 15s ...
        }
        continue;
      }

      // ---- link watchdog ----
      if (!SerialBT.connected()) {
        Serial.println("Bluetooth link lost - reconnecting");
        elmReady = false;
        tele->btConnected = false;
        continue;
      }

      // ---- non-blocking PID polling ----
      pollElm();
      if (errCount >= 8) {                                      // ECU silent (ignition off?): show zeros, slow down
        tele->rpm = 0;
        tele->speed = 0;
        vTaskDelay(pdMS_TO_TICKS(250));
      }
      if (errCount >= 40) { elmReady = false; errCount = 0; }   // dongle probably hung: redo the handshake
      vTaskDelay(pdMS_TO_TICKS(2));                             // yield every cycle
    }
  }

  bool connectAndInit() {
    if (!btStarted) {
      if (!SerialBT.begin("ESP32_OBD_Dash", true)) return false;   // master mode
      SerialBT.setPin(OBD_BT_PIN);
      btStarted = true;
    }
    if (SerialBT.connected()) SerialBT.disconnect();               // clean slate
    if (!SerialBT.connect(OBD_BT_NAME)) return false;              // blocks this task only (it yields)
    if (btReleaseReq) return false;
    if (!initElm()) { SerialBT.disconnect(); return false; }
    return true;
  }

  // Cooperative handshake: proves the dongle answers before ELMduino's own (blocking) init runs.
  bool initElm() {
    if (!atCommand("ATZ", 3500, false)) return false;              // reset, wait for the '>' prompt
    if (!elm.begin(SerialBT, false, 2000)) return false;           // ELMduino AT init (echo off, spaces off, protocol)
    atCommand("ATH0", 1000, true);                                 // headers off  (best effort)
    atCommand("ATL0", 1000, true);                                 // linefeeds off (best effort)
    return true;
  }

  // Sends one AT command and waits for the ELM327 '>' prompt. Polls the UART every 5 ms with vTaskDelay,
  // so it never busy-waits, and aborts at once if a Bluetooth release is requested.
  bool atCommand(const char *cmd, uint32_t timeoutMs, bool needOk) {
    while (SerialBT.available()) SerialBT.read();
    SerialBT.print(cmd);
    SerialBT.print('\r');

    char buf[48];
    size_t n = 0;
    for (uint32_t t0 = millis(); millis() - t0 < timeoutMs;) {
      if (btReleaseReq) return false;
      while (SerialBT.available()) {
        char ch = (char)SerialBT.read();
        if (ch == '>') { buf[n] = 0; return !needOk || strstr(buf, "OK") != nullptr; }
        if (n < sizeof(buf) - 1) buf[n++] = ch;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
  }

  void stopBt() {
    if (btStarted) { SerialBT.end(); btStarted = false; }
    elmReady = false;
    tele->btConnected = false;
  }
  void releaseBt() { stopBt(); btReleased = true; }

  // ---- PID state machine: 0=RPM 1=Speed (fast) | 2..10 slow sensors, interleaved ----
  bool ok()       { return elm.nb_rx_state == ELM_SUCCESS; }
  bool finished() { return elm.nb_rx_state != ELM_GETTING_MSG; }

  void resetPollState() { pidStep = 0; fastCount = 0; nextSlow = 2; errCount = 0; }

  void onDone(bool success, bool fast) {
    if (!fast) { pidStep = 0; return; }
    errCount = success ? 0 : (errCount < 255 ? errCount + 1 : 255);
    if (pidStep == 0) pidStep = 1;                                  // RPM is always followed by speed
    else if (++fastCount >= SLOW_EVERY) {                           // then one slow sensor every N cycles
      fastCount = 0;
      pidStep = nextSlow;
      nextSlow = (nextSlow >= 10) ? 2 : nextSlow + 1;
    } else pidStep = 0;
  }

  void pollElm() {
    switch (pidStep) {
      case 0:  { float v = elm.rpm();   if (ok()) tele->rpm = constrain((int)v, 0, cfg->maxRpm);       if (finished()) onDone(ok(), true);  break; }
      case 1:  { int32_t v = elm.kph(); if (ok()) tele->speed = (v * cfg->speedCorrPct) / 100;         if (finished()) onDone(ok(), true);  break; }
      case 2:  { float v = elm.engineCoolantTemp();       if (ok()) tele->coolant  = (int)v; if (finished()) onDone(ok(), false); break; }
      case 3:  { float v = elm.engineLoad();              if (ok()) tele->load     = (int)v; if (finished()) onDone(ok(), false); break; }
      case 4:  { float v = elm.intakeAirTemp();           if (ok()) tele->intake   = (int)v; if (finished()) onDone(ok(), false); break; }
      case 5:  { float v = elm.throttle();                if (ok()) tele->throttle = (int)v; if (finished()) onDone(ok(), false); break; }
      case 6:  { float v = elm.mafRate();                 if (ok()) tele->maf      = v;      if (finished()) onDone(ok(), false); break; }
      case 7:  { float v = elm.timingAdvance();           if (ok()) tele->timing   = v;      if (finished()) onDone(ok(), false); break; }
      case 8:  { float v = elm.shortTermFuelTrimBank_1(); if (ok()) tele->stft     = v;      if (finished()) onDone(ok(), false); break; }
      case 9:  { float v = elm.longTermFuelTrimBank_1();  if (ok()) tele->ltft     = v;      if (finished()) onDone(ok(), false); break; }
      case 10: {                                                  // PID 01 01: byte A = MIL bit + DTC count
        uint32_t v = elm.monitorStatus();
        if (ok()) {
          uint8_t codes = (v >> 24) & 0x7F;
          if (codes == 0) tele->dtcAck = false;
          tele->dtcCount = codes;
        }
        if (finished()) onDone(ok(), false);
        break;
      }
      default: pidStep = 0; break;
    }
  }

  void runSimulator() {
    if (millis() - lastSimMs < 40) return;
    lastSimMs = millis();
    Telemetry &t = *tele;
    t.rpm += simDir;
    if (t.rpm >= 7600) simDir = -110;
    if (t.rpm <= 800)  simDir = 85;
    t.speed    = ((t.rpm / 45) * cfg->speedCorrPct) / 100;
    t.coolant  = 86 + (t.rpm / 600);
    t.intake   = 32 + (t.rpm / 1000);
    t.throttle = map(t.rpm, 800, 8000, 5, 98);
    t.load     = map(t.rpm, 800, 8000, 14, 95);
    t.maf      = t.rpm * 0.015f;
    t.timing   = 12.5f + (t.rpm / 500.0f);
    t.stft     = -1.5f + sinf(millis() / 1000.0f) * 3.0f;
    t.ltft     = 2.3f;
  }

  // =====================================================================================
  //                                   NETWORK TASK (Core 0)
  // =====================================================================================
  void netLoop() {
    uint32_t lastPost = 0, backoff = 0, lastCredCheck = 0;
    bool hasCreds = WiFi.SSID().length() > 0;
    bool autoPortalDone = false;

    for (;;) {
      if (portalReq) { runPortal(); hasCreds = WiFi.SSID().length() > 0; continue; }

      if (millis() - lastCredCheck > 2000) { lastCredCheck = millis(); hasCreds = WiFi.SSID().length() > 0; }
#if AUTO_PORTAL_IF_NO_CREDS
      if (!autoPortalDone && !hasCreds) { autoPortalDone = true; portalReq = true; continue; }
#endif
      (void)autoPortalDone;

      bool online = (WiFi.status() == WL_CONNECTED);
      tele->wifiState = online ? NET_ONLINE : (hasCreds ? NET_CONNECTING : NET_OFF);

      if (online && (POST_IN_DEMO || !cfg->runDemo) && millis() - lastPost >= POST_INTERVAL_MS + backoff) {
        lastPost = millis();
        if (postTelemetry()) backoff = 0;
        else backoff = (backoff + 2000 > 16000) ? 16000 : backoff + 2000;   // back off while the server is unreachable
      }
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  // Blocking only inside this task. Short timeouts keep it responsive; obdTask is never affected.
  bool postTelemetry() {
    Telemetry s = *tele;                                           // quick snapshot
    char body[224];
    int n = snprintf(body, sizeof body,
      "{\"RPM\":%d,\"SPEED\":%d,\"LOAD\":%d,\"COOLANT\":%d,\"THROTTLE\":%d,\"INTAKE\":%d,"
      "\"MAF\":%.1f,\"TIMING\":%.1f,\"STFT\":%.1f,\"LTFT\":%.1f,\"DTC\":%u}",
      s.rpm, s.speed, s.load, s.coolant, s.throttle, s.intake, s.maf, s.timing, s.stft, s.ltft, s.dtcCount);

    HTTPClient http;
    http.setConnectTimeout(1500);
    http.setTimeout(1500);
    http.setReuse(false);
    if (!http.begin(url)) return false;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("x-api-key", key);
    int code = http.POST((uint8_t *)body, (size_t)n);
    http.end();
    return code > 0 && code < 400;
  }

  // Captive portal: Bluetooth is shut down first so the two radios never compete.
  void runPortal() {
    tele->wifiState = NET_PORTAL;
    btReleaseReq = true;                                           // obdTask drops Bluetooth
    for (uint32_t t0 = millis(); !btReleased && millis() - t0 < 12000;) vTaskDelay(pdMS_TO_TICKS(50));

    WiFi.disconnect(true, false);                                  // radio off, saved credentials kept
    vTaskDelay(pdMS_TO_TICKS(200));

    WiFiManager wm;
    WiFiManagerParameter pUrl("url", "Backend URL", url, sizeof url - 1);
    WiFiManagerParameter pKey("key", "API key", key, sizeof key - 1);
    wm.addParameter(&pUrl);
    wm.addParameter(&pKey);
    wm.setConfigPortalTimeout(180);                                // give up after 3 minutes
    wm.setConnectTimeout(20);
    wm.setBreakAfterConfig(true);
    wm.startConfigPortal(PORTAL_AP_NAME);                          // blocks THIS task until saved / timed out
    saveNetConfig(pUrl.getValue(), pKey.getValue());

    WiFi.mode(WIFI_STA);
    WiFi.begin();                                                  // resume with whatever is stored
    portalReq = false;
    btReleaseReq = false;                                          // obdTask restarts Bluetooth
  }

  void loadNetConfig() {
    Preferences p;
    p.begin("net", true);
    p.getString("url", BACKEND_URL_DEFAULT).toCharArray(url, sizeof url);
    p.getString("key", BACKEND_KEY_DEFAULT).toCharArray(key, sizeof key);
    p.end();
  }

  void saveNetConfig(const char *u, const char *k) {
    if (u && *u) strlcpy(url, u, sizeof url);
    if (k && *k) strlcpy(key, k, sizeof key);
    Preferences p;
    p.begin("net", false);
    p.putString("url", url);
    p.putString("key", key);
    p.end();
  }
};