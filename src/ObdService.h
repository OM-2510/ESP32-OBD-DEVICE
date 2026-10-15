#pragma once
#include "Config.h"
#include <BluetoothSerial.h>
#include <ELMduino.h>

class ObdService {
private:
  BluetoothSerial SerialBT;
  ELM327 elm;
  uint8_t pidStep = 0;
  uint8_t fastCount = 0;
  uint8_t nextSlow = 2;
  unsigned long lastSimMs = 0;
  int simDir = 85;

  bool ok()       { return elm.nb_rx_state == ELM_SUCCESS; }
  bool finished() { return elm.nb_rx_state != ELM_GETTING_MSG; } // success OR error

public:
  bool connectBT(const char* deviceName = "OBDII", const char* pin = "1234") {
    if (!SerialBT.begin("ESP32_OBD_Dash", true)) return false;
    SerialBT.setPin(pin);                       // most ELM327 clones use 1234 or 0000
    if (!SerialBT.connect(deviceName)) return false;
    if (!elm.begin(SerialBT, false, 2000)) return false;
    return true;
  }

  void update(Telemetry &t, const AppConfig &cfg) {
    if (cfg.runDemo) { runSimulator(t, cfg); return; }

    if (!t.btConnected) return;
    if (!SerialBT.connected()) { t.btConnected = false; return; } // link lost

    switch (pidStep) {
      case 0: { // Fast: RPM
        float v = elm.rpm();
        if (ok()) t.rpm = constrain((int)v, 0, cfg.maxRpm);
        if (finished()) advanceStep();
        break;
      }
      case 1: { // Fast: Speed
        int32_t v = elm.kph();
        if (ok()) t.speed = (v * cfg.speedCorrPct) / 100;
        if (finished()) advanceStep();
        break;
      }
      case 2: { float v = elm.engineCoolantTemp();       if (ok()) t.coolant  = (int)v; if (finished()) pidStep = 0; break; }
      case 3: { float v = elm.engineLoad();              if (ok()) t.load     = (int)v; if (finished()) pidStep = 0; break; }
      case 4: { float v = elm.intakeAirTemp();           if (ok()) t.intake   = (int)v; if (finished()) pidStep = 0; break; }
      case 5: { float v = elm.throttle();                if (ok()) t.throttle = (int)v; if (finished()) pidStep = 0; break; }
      case 6: { float v = elm.mafRate();                 if (ok()) t.maf      = v;      if (finished()) pidStep = 0; break; }
      case 7: { float v = elm.timingAdvance();           if (ok()) t.timing   = v;      if (finished()) pidStep = 0; break; }
      case 8: { float v = elm.shortTermFuelTrimBank_1(); if (ok()) t.stft     = v;      if (finished()) pidStep = 0; break; }
      case 9: { float v = elm.longTermFuelTrimBank_1();  if (ok()) t.ltft     = v;      if (finished()) pidStep = 0; break; }
      case 10: { // PID 01 01: byte A = MIL bit (bit 7) + DTC count (bits 0-6)
        uint32_t v = elm.monitorStatus();
        if (ok()) {
          uint8_t codes = (v >> 24) & 0x7F;
          if (codes == 0) t.dtcAck = false;
          t.dtcCount = codes;
        }
        if (finished()) pidStep = 0;
        break;
      }
      default: pidStep = 0; break;
    }
  }

private:
  void advanceStep() {
    if (pidStep == 0) {
      pidStep = 1;                 // RPM is always followed by Speed
    } else {
      fastCount++;
      if (fastCount >= 3) {        // every 3rd fast cycle, one slow sensor (2..10)
        pidStep = nextSlow++;
        if (nextSlow > 10) nextSlow = 2;
        fastCount = 0;
      } else {
        pidStep = 0;
      }
    }
  }

  void runSimulator(Telemetry &t, const AppConfig &cfg) {
    if (millis() - lastSimMs < 40) return;
    lastSimMs = millis();

    t.rpm += simDir;
    if (t.rpm >= 7600) simDir = -110;
    if (t.rpm <= 800)  simDir = 85;

    t.speed    = ((t.rpm / 45) * cfg.speedCorrPct) / 100;
    t.coolant  = 86 + (t.rpm / 600);
    t.intake   = 32 + (t.rpm / 1000);
    t.throttle = map(t.rpm, 800, 8000, 5, 98);
    t.load     = map(t.rpm, 800, 8000, 14, 95);
    t.maf      = t.rpm * 0.015f;
    t.timing   = 12.5f + (t.rpm / 500.0f);
    t.stft     = -1.5f + (sinf(millis() / 1000.0f) * 3.0f);
    t.ltft     = 2.3f;
  }
};