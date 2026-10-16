#pragma once

#include <atomic>
#include <new>
#include <Preferences.h>
#include <BluetoothSerial.h>
#include <ELMduino.h>
#include <esp_bt.h>
#include "Config.h"

#define OBD_BT_PIN "1234"
constexpr uint8_t BT_DEVICE_LIMIT = 6;
constexpr uint32_t BT_SCAN_MS = 7680;
constexpr uint32_t BT_RETRY_MAX_MS = 30000;

class ObdService {
public:
  bool begin(Telemetry *t, AppConfig *c) {
    tele = t;
    cfg = c;
    elm.payload = nullptr;
    elm.elm_port = nullptr;
    loadPaired();
    if (xTaskCreatePinnedToCore(taskEntry, "obdTask", 8192, this, 1, &task, 0) != pdPASS) {
      Serial.println("[bt] ERROR: could not create Bluetooth/OBD task");
      return false;
    }
    return true;
  }

  void update(Telemetry &t, const AppConfig &config) {
    if (config.runDemo && !t.btConnected) runSimulator(t, config);
  }

  void requestScan() {
    scanCount = 0;
    scanRequest = true;
    tele->btState = BT_SCANNING;
  }

  void selectDevice(int index) {
    selectRequest = index;
  }

  void forgetDevice() {
    forgetRequest = true;
  }

  int nearbyCount() const {
    return scanCount.load();
  }

  const char *nearbyName(int index) const {
    return index >= 0 && index < scanCount.load() ? devices[index].name : "";
  }

  bool hasPairedDevice() const {
    return tele && tele->btName[0] != '\0';
  }

private:
  struct Device {
    char name[24];
    uint8_t mac[6];
  };

  BluetoothSerial *bt = nullptr;
  ELM327 elm;
  Telemetry *tele = nullptr;
  AppConfig *cfg = nullptr;
  TaskHandle_t task = nullptr;
  Device devices[BT_DEVICE_LIMIT];
  uint8_t pairedMac[6] = {};
  std::atomic<int> scanCount{0};
  std::atomic<int> selectRequest{-1};
  std::atomic<bool> scanRequest{false};
  std::atomic<bool> forgetRequest{false};
  bool paired = false;
  bool btStarted = false;
  bool bleMemoryReleased = false;
  bool elmReady = false;
  uint8_t pidStep = 0, fastCount = 0, nextSlow = 2;
  uint32_t lastSimMs = 0;
  int simDirection = 85;

  static void taskEntry(void *arg) {
    static_cast<ObdService *>(arg)->taskLoop();
    vTaskDelete(nullptr);
  }

  void setBtState(uint8_t state) {
    tele->btState = state;
    tele->btConnected = state == BT_CONNECTED;
  }

  bool startBluetooth() {
    if (btStarted) return true;
    if (!bt) {
      bt = new (std::nothrow) BluetoothSerial();
      if (!bt) {
        Serial.println("[bt] ERROR: Bluetooth object allocation failed");
        return false;
      }
    }
    if (!bleMemoryReleased) {
      esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
      if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        Serial.printf("[bt] BLE memory release failed: %s\n", esp_err_to_name(err));
      }
      bleMemoryReleased = true;
    }
    if (!bt->begin("ESP32_OBD_Dash", true)) {
      Serial.printf("[bt] ERROR: Bluetooth start failed (heap=%u)\n", ESP.getFreeHeap());
      delete bt;
      bt = nullptr;
      return false;
    }
    bt->setPin(OBD_BT_PIN);
    btStarted = true;
    return true;
  }

  void restartBluetooth() {
    elmReady = false;
    elm.elm_port = nullptr;
    tele->btConnected = false;
    if (bt) {
      delete bt;
      bt = nullptr;
    }
    btStarted = false;
  }

  void loadPaired() {
    Preferences prefs;
    if (!prefs.begin("bt", true)) return;
    paired = prefs.getBytes("mac", pairedMac, sizeof pairedMac) == sizeof pairedMac;
    if (paired && prefs.isKey("name")) {
      String name = prefs.getString("name");
      strlcpy(tele->btName, name.c_str(), sizeof tele->btName);
    }
    prefs.end();
    setBtState(paired ? BT_CONNECTING : BT_NOT_PAIRED);
  }

  bool savePaired(const Device &device) {
    Preferences prefs;
    if (!prefs.begin("bt", false)) {
      Serial.println("[bt] ERROR: could not open pairing storage");
      return false;
    }
    size_t macBytes = prefs.putBytes("mac", device.mac, sizeof device.mac);
    size_t nameBytes = prefs.putString("name", device.name);
    prefs.end();
    if (macBytes != sizeof device.mac || nameBytes == 0) {
      Serial.println("[bt] ERROR: could not persist selected Bluetooth device");
      return false;
    }
    memcpy(pairedMac, device.mac, sizeof pairedMac);
    strlcpy(tele->btName, device.name, sizeof tele->btName);
    paired = true;
    return true;
  }

  void removePaired() {
    if (bt && btStarted && paired) bt->unpairDevice(pairedMac);
    restartBluetooth();
    Preferences prefs;
    if (prefs.begin("bt", false)) {
      prefs.remove("mac");
      prefs.remove("name");
      prefs.end();
    } else {
      Serial.println("[bt] ERROR: could not open pairing storage to remove device");
    }
    memset(pairedMac, 0, sizeof pairedMac);
    tele->btName[0] = '\0';
    paired = false;
    setBtState(BT_NOT_PAIRED);
    Serial.println("[bt] saved device removed");
  }

  int scanNearby() {
    restartBluetooth();
    if (!startBluetooth()) return -1;

    BTScanResults *results = bt->discover(BT_SCAN_MS);
    int found = 0;
    if (results) {
      int total = results->getCount();
      for (int i = 0; i < total && found < BT_DEVICE_LIMIT; i++) {
        BTAdvertisedDevice *device = results->getDevice(i);
        if (!device) continue;
        BTAddress address = device->getAddress();
        memcpy(devices[found].mac, *address.getNative(), sizeof devices[found].mac);
        std::string name = device->getName();
        strlcpy(devices[found].name,
                name.empty() ? address.toString(true).c_str() : name.c_str(),
                sizeof devices[found].name);
        found++;
      }
    }
    scanCount.store(found);
    Serial.printf("[bt] scan complete: %d device(s)\n", found);
    return found;
  }

  bool elmCommand(const char *command, uint32_t timeoutMs, bool expectOk = false) {
    elm.timeout_ms = timeoutMs;
    elm.sendCommand(command);
    uint32_t start = millis();
    while (elm.nb_rx_state == ELM_GETTING_MSG && millis() - start <= timeoutMs) {
      elm.get_response();
      if (elm.nb_rx_state == ELM_GETTING_MSG) vTaskDelay(1);
    }
    return elm.nb_rx_state == ELM_SUCCESS &&
           (!expectOk || strstr(elm.payload, "OK") != nullptr);
  }

  bool initializeElm() {
    elm.elm_port = bt;
    elm.PAYLOAD_LEN = 128;
    elm.debugMode = false;
    elm.timeout_ms = 2000;
    if (!elm.payload) elm.payload = static_cast<char *>(malloc(elm.PAYLOAD_LEN + 1));
    if (!elm.payload) {
      Serial.println("[obd] ERROR: ELM response buffer allocation failed");
      return false;
    }

    uint32_t start = millis();
    if (!elmCommand(SET_ALL_TO_DEFAULTS, 2000, true)) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!elmCommand(RESET_ALL, 3000)) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!elmCommand(ECHO_OFF, 2000, true)) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!elmCommand(PRINTING_SPACES_OFF, 2000, true)) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!elmCommand(ALLOW_LONG_MESSAGES, 2000, true)) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!elmCommand("AT ST 00", 2000, true)) return false;
    if (!elmCommand("AT SP A0", 2000, true)) return false;
    if (!elmCommand("0100", 30000)) return false;

    elm.connected = true;
    elm.timeout_ms = 2000;
    elmCommand(HEADERS_OFF, 1000, true);
    elmCommand(LINEFEEDS_OFF, 1000, true);
    Serial.printf("[obd] ELM ready in %u ms\n", (unsigned)(millis() - start));
    return true;
  }

  bool connectSavedDevice() {
    if (!startBluetooth()) return false;
    setBtState(BT_CONNECTING);
    Serial.printf("[bt] reconnecting saved device: %s\n", tele->btName);
    if (!bt->connect(pairedMac)) return false;
    if (!initializeElm()) {
      Serial.println("[obd] ELM327 initialization failed");
      bt->disconnect();
      return false;
    }
    elmReady = true;
    pidStep = 0;
    fastCount = 0;
    nextSlow = 2;
    setBtState(BT_CONNECTED);
    return true;
  }

  void taskLoop() {
    uint32_t retryMs = 1000;
    uint32_t nextConnect = 0;

    for (;;) {
      if (forgetRequest.exchange(false)) {
        removePaired();
        retryMs = 1000;
        continue;
      }

      if (scanRequest.exchange(false)) {
        setBtState(BT_SCANNING);
        int found = scanNearby();
        if (found < 0) setBtState(BT_ERROR);
        else setBtState(paired ? BT_CONNECTING : BT_NOT_PAIRED);
        continue;
      }

      int selected = selectRequest.exchange(-1);
      if (selected >= 0) {
        if (selected < scanCount.load()) {
          if (bt && btStarted && paired) bt->unpairDevice(pairedMac);
          restartBluetooth();
          if (savePaired(devices[selected])) {
            Serial.printf("[bt] saved device: %s\n", tele->btName);
            retryMs = 1000;
            nextConnect = 0;
          } else {
            setBtState(BT_ERROR);
          }
        }
      }

      if (!paired) {
        if (!scanRequest) setBtState(BT_NOT_PAIRED);
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }

      if (!elmReady) {
        uint32_t now = millis();
        if ((int32_t)(now - nextConnect) < 0) {
          vTaskDelay(pdMS_TO_TICKS(100));
          continue;
        }
        if (connectSavedDevice()) {
          retryMs = 1000;
        } else {
          setBtState(BT_ERROR);
          Serial.printf("[bt] connection failed; retry in %u s\n", (unsigned)(retryMs / 1000));
          nextConnect = millis() + retryMs;
          retryMs = retryMs >= BT_RETRY_MAX_MS / 2 ? BT_RETRY_MAX_MS : retryMs * 2;
          vTaskDelay(pdMS_TO_TICKS(100));
        }
        continue;
      }

      if (!bt || !bt->connected()) {
        elmReady = false;
        tele->btConnected = false;
        setBtState(BT_CONNECTING);
        nextConnect = millis();
        continue;
      }

      pollElm();
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }

  bool ok() const { return elm.nb_rx_state == ELM_SUCCESS; }
  bool finished() const { return elm.nb_rx_state != ELM_GETTING_MSG; }

  void pollElm() {
    switch (pidStep) {
      case 0: {
        float value = elm.rpm();
        if (ok()) tele->rpm = constrain((int)value, 0, cfg->maxRpm);
        if (finished()) advanceFastStep(true);
        break;
      }
      case 1: {
        int32_t value = elm.kph();
        if (ok()) tele->speed = (value * cfg->speedCorrPct) / 100;
        if (finished()) advanceFastStep(true);
        break;
      }
      case 2: { float value = elm.engineCoolantTemp(); if (ok()) tele->coolant = (int)value; if (finished()) pidStep = 0; break; }
      case 3: { float value = elm.engineLoad(); if (ok()) tele->load = (int)value; if (finished()) pidStep = 0; break; }
      case 4: { float value = elm.intakeAirTemp(); if (ok()) tele->intake = (int)value; if (finished()) pidStep = 0; break; }
      case 5: { float value = elm.throttle(); if (ok()) tele->throttle = (int)value; if (finished()) pidStep = 0; break; }
      case 6: { float value = elm.manifoldPressure(); if (ok()) tele->maf = value; if (finished()) pidStep = 0; break; }
      case 7: { float value = elm.timingAdvance(); if (ok()) tele->timing = value; if (finished()) pidStep = 0; break; }
      case 8: { float value = elm.shortTermFuelTrimBank_1(); if (ok()) tele->stft = value; if (finished()) pidStep = 0; break; }
      case 9: { float value = elm.longTermFuelTrimBank_1(); if (ok()) tele->ltft = value; if (finished()) pidStep = 0; break; }
      case 10: {
        uint32_t value = elm.monitorStatus();
        if (ok()) {
          uint8_t codes = (value >> 24) & 0x7F;
          if (!codes) tele->dtcAck = false;
          tele->dtcCount = codes;
        }
        if (finished()) pidStep = 0;
        break;
      }
      default:
        pidStep = 0;
        break;
    }
  }

  void advanceFastStep(bool fast) {
    if (!fast) {
      pidStep = 0;
      return;
    }
    if (pidStep == 0) {
      pidStep = 1;
      return;
    }
    if (++fastCount >= 3) {
      fastCount = 0;
      pidStep = nextSlow;
      nextSlow = nextSlow >= 10 ? 2 : nextSlow + 1;
    } else {
      pidStep = 0;
    }
  }

  void runSimulator(Telemetry &telemetry, const AppConfig &config) {
    if (millis() - lastSimMs < 40) return;
    lastSimMs = millis();
    telemetry.rpm += simDirection;
    if (telemetry.rpm >= 7600) simDirection = -110;
    if (telemetry.rpm <= 800) simDirection = 85;
    telemetry.speed = ((telemetry.rpm / 45) * config.speedCorrPct) / 100;
    telemetry.coolant = 86 + (telemetry.rpm / 600);
    telemetry.intake = 32 + (telemetry.rpm / 1000);
    telemetry.throttle = map(telemetry.rpm, 800, 8000, 5, 98);
    telemetry.load = map(telemetry.rpm, 800, 8000, 14, 95);
    telemetry.maf = telemetry.rpm * 0.015f;
    telemetry.timing = 12.5f + (telemetry.rpm / 500.0f);
    telemetry.stft = -1.5f + sinf(millis() / 1000.0f) * 3.0f;
    telemetry.ltft = 2.3f;
  }
};
