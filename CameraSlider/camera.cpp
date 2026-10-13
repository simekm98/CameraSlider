#include "camera.h"
#include "config.h"
#include <Preferences.h>
#include <NimBLEDevice.h>

// ============================================================================
//  Canon BLE dálkové ovládání (protokol BR-E1)
//  Služba 00050000-…, párování = zápis [0x03, jméno, 0x00] do 00050002,
//  spoušť = zápis 0x8C (stisk) a 0x0C (uvolnění) do 00050003.
//  Ověřeno v projektech BR-M5 a ESP32-Canon-BLE-Remote.
// ============================================================================
static const char *SVC_UUID      = "00050000-0000-1000-0000-d8492fffa821";
static const char *CHR_PAIR_UUID = "00050002-0000-1000-0000-d8492fffa821";
static const char *CHR_SHUT_UUID = "00050003-0000-1000-0000-d8492fffa821";
static const uint8_t MODE_IMMEDIATE = 0x0C;
static const uint8_t BTN_RELEASE    = 0x80;

static constexpr uint32_t CAM_VERSION = 0x43414D01;  // při změně struktury zvýšit

CamConfig cam;
static Preferences prefs;
static Camera::Notifier notifier = nullptr;

enum class BleState : uint8_t { Off, Unpaired, Pairing, Connecting, Connected, Disconnected };
static volatile BleState bleState = BleState::Off;
static volatile bool pairReq = false, forgetReq = false;
static uint32_t bleShots = 0, bleFails = 0;   // jen statistika
static QueueHandle_t fireQ;      // požadavky na výstřel pro BLE task
static QueueHandle_t msgQ;       // zprávy z BLE tasku -> loop() -> UI

static NimBLEClient *client = nullptr;
static NimBLERemoteCharacteristic *chrShutter = nullptr;

// drát
static bool wireOn = false;
static uint32_t wireOffAt = 0;
static bool progActive = false;

struct Msg { char text[96]; bool err; };

// ============================================================================
static void setDefaults() {
  cam = CamConfig{};
  cam.version = CAM_VERSION;
  cam.enabled = false;
  cam.trigger = 0;
  cam.dirMode = 0;
  cam.spacing = 100;
  cam.startOffset = 0;
  cam.pressMs = 200;
  cam.minIntervalMs = 400;
  cam.waitManual = false;
  cam.holdFocus = true;
}

static bool bleWanted() { return cam.trigger == 0 || cam.trigger == 2; }
static bool wireWanted() { return cam.trigger == 1 || cam.trigger == 2; }

// zpráva z libovolného tasku (do UI ji pošle loop())
static void post(bool err, const char *fmt, ...) {
  Msg m;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m.text, sizeof(m.text), fmt, ap);
  va_end(ap);
  m.err = err;
  Serial.printf("[cam] %s\n", m.text);
  xQueueSend(msgQ, &m, 0);
}

// ============================================================================
//  BLE (vlastní task – připojování a zápisy blokují, nesmí brzdit loop())
// ============================================================================
static bool bleConnect() {
  if (!cam.bleAddr[0]) return false;
  bleState = BleState::Connecting;
  chrShutter = nullptr;
  NimBLEAddress addr(std::string(cam.bleAddr), cam.bleAddrType);
  if (!client->connect(addr)) {
    bleState = BleState::Disconnected;
    return false;
  }
  NimBLERemoteService *svc = client->getService(SVC_UUID);
  if (svc) chrShutter = svc->getCharacteristic(CHR_SHUT_UUID);
  if (!chrShutter) {
    client->disconnect();
    bleState = BleState::Disconnected;
    post(true, "Camera: remote-control service not found – pair again");
    return false;
  }
  bleState = BleState::Connected;
  post(false, "Camera connected (Bluetooth)");
  return true;
}

static void blePair() {
  bleState = BleState::Pairing;
  if (client->isConnected()) client->disconnect();
  post(false, "Searching for camera… (camera: Bluetooth → Remote → Pairing)");

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults res = scan->getResults(20000, false);

  bool found = false;
  NimBLEAddress addr;
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice *d = res.getDevice(i);
    if (d->isAdvertisingService(NimBLEUUID(SVC_UUID))) {
      addr = d->getAddress();
      found = true;
      break;
    }
  }
  scan->clearResults();

  if (!found) {
    bleState = cam.bleAddr[0] ? BleState::Disconnected : BleState::Unpaired;
    post(true, "Camera not found – open the pairing screen on the camera and try again");
    return;
  }

  if (!client->connect(addr)) {
    bleState = cam.bleAddr[0] ? BleState::Disconnected : BleState::Unpaired;
    post(true, "Camera found but connection failed – try again");
    return;
  }

  NimBLERemoteService *svc = client->getService(SVC_UUID);
  NimBLERemoteCharacteristic *chrPair = svc ? svc->getCharacteristic(CHR_PAIR_UUID) : nullptr;
  if (!chrPair) {
    client->disconnect();
    bleState = BleState::Unpaired;
    post(true, "Camera: pairing characteristic not found");
    return;
  }

  // [0x03][jméno][0x00] – jméno se zobrazí ve fotoaparátu
  uint8_t buf[32];
  size_t n = strlen(HOSTNAME);
  if (n > sizeof(buf) - 2) n = sizeof(buf) - 2;
  buf[0] = 0x03;
  memcpy(buf + 1, HOSTNAME, n);
  buf[n + 1] = 0x00;
  chrPair->writeValue(buf, n + 2, false);
  vTaskDelay(pdMS_TO_TICKS(300));
  client->disconnect();
  vTaskDelay(pdMS_TO_TICKS(500));

  strlcpy(cam.bleAddr, addr.toString().c_str(), sizeof(cam.bleAddr));
  cam.bleAddrType = addr.getType();
  Camera::save();
  post(false, "Camera paired (%s)", cam.bleAddr);
  bleConnect();
}

static void bleForget() {
  if (client->isConnected()) client->disconnect();
  if (cam.bleAddr[0]) NimBLEDevice::deleteBond(NimBLEAddress(std::string(cam.bleAddr), cam.bleAddrType));
  cam.bleAddr[0] = 0;
  cam.bleAddrType = 0;
  chrShutter = nullptr;
  Camera::save();
  bleState = BleState::Unpaired;
  post(false, "Camera pairing removed");
}

static void blePress() {
  if (!chrShutter || !client->isConnected()) {
    bleFails++;
    return;
  }
  uint8_t v = MODE_IMMEDIATE | BTN_RELEASE;
  bool ok = chrShutter->writeValue(&v, 1, false);
  vTaskDelay(pdMS_TO_TICKS(cam.pressMs));
  v = MODE_IMMEDIATE;
  ok = chrShutter->writeValue(&v, 1, false) && ok;
  if (ok) bleShots++;
  else bleFails++;
}

static void bleTask(void *) {
  uint32_t lastTry = 0;
  for (;;) {
    if (forgetReq) { forgetReq = false; bleForget(); }
    if (pairReq) { pairReq = false; blePair(); lastTry = millis(); }

    if (!bleWanted()) {
      if (client->isConnected()) client->disconnect();
      bleState = BleState::Off;
    } else if (!cam.bleAddr[0]) {
      bleState = BleState::Unpaired;
    } else if (!client->isConnected()) {
      if (bleState == BleState::Connected) {
        bleState = BleState::Disconnected;
        post(true, "Camera disconnected (Bluetooth) – reconnecting…");
      }
      if (millis() - lastTry > 3000) {
        lastTry = millis();
        bleConnect();
      }
    }

    uint8_t ev;
    if (xQueueReceive(fireQ, &ev, pdMS_TO_TICKS(50)) == pdTRUE) blePress();
  }
}

// ============================================================================
void Camera::begin() {
  prefs.begin("camera", false);
  CamConfig tmp;
  if (prefs.getBytesLength("cam") == sizeof(CamConfig) &&
      prefs.getBytes("cam", &tmp, sizeof(CamConfig)) == sizeof(CamConfig) &&
      tmp.version == CAM_VERSION) {
    cam = tmp;
    cam.bleAddr[sizeof(cam.bleAddr) - 1] = 0;
  } else {
    setDefaults();
  }

  pinMode(PIN_CAM_SHUTTER, OUTPUT);
  pinMode(PIN_CAM_FOCUS, OUTPUT);
  digitalWrite(PIN_CAM_SHUTTER, LOW);
  digitalWrite(PIN_CAM_FOCUS, LOW);

  fireQ = xQueueCreate(8, sizeof(uint8_t));
  msgQ = xQueueCreate(8, sizeof(Msg));

  NimBLEDevice::init(HOSTNAME);
  NimBLEDevice::setSecurityAuth(true, false, false);   // bonding, bez PIN (Canon „Just Works“)
  client = NimBLEDevice::createClient();
  client->setConnectTimeout(5000);

  xTaskCreatePinnedToCore(bleTask, "camBle", 6144, nullptr, 1, nullptr, 0);
}

void Camera::setNotifier(Notifier n) { notifier = n; }

void Camera::update() {
  Msg m;
  while (xQueueReceive(msgQ, &m, 0) == pdTRUE)
    if (notifier) notifier(m.text, m.err);

  if (wireOn && (int32_t)(millis() - wireOffAt) >= 0) {
    digitalWrite(PIN_CAM_SHUTTER, LOW);
    wireOn = false;
  }
  bool focus = wireWanted() && progActive && cam.holdFocus;
  digitalWrite(PIN_CAM_FOCUS, focus ? HIGH : LOW);
}

bool Camera::fire() {
  bool ok = false;
  if (wireWanted()) {
    digitalWrite(PIN_CAM_SHUTTER, HIGH);
    wireOn = true;
    wireOffAt = millis() + cam.pressMs;
    ok = true;
  }
  if (bleWanted()) {
    uint8_t ev = 0;
    if (bleState == BleState::Connected && xQueueSend(fireQ, &ev, 0) == pdTRUE) ok = true;
    else if (!wireWanted()) bleFails++;
  }
  return ok;
}

const char *Camera::checkReady() {
  if (bleWanted() && !wireWanted()) {
    if (!cam.bleAddr[0]) return "Camera not paired – pair it in the Camera section";
    if (bleState != BleState::Connected) return "Camera not connected over Bluetooth – wake the camera";
  }
  return nullptr;
}

void Camera::programActive(bool on) { progActive = on; }

const char *Camera::cmdPair() {
  if (!bleWanted()) return "Select Bluetooth trigger first";
  if (bleState == BleState::Pairing) return "Pairing already running";
  pairReq = true;
  return nullptr;
}

void Camera::cmdForget() { forgetReq = true; }

const char *Camera::cmdTest() {
  if (auto e = checkReady()) return e;
  if (!fire()) return "Trigger not ready";
  return nullptr;
}

// ============================================================================
void Camera::save() {
  prefs.putBytes("cam", &cam, sizeof(CamConfig));
}

void Camera::toJson(JsonObject o) {
  o["enabled"] = cam.enabled;
  o["trigger"] = cam.trigger;
  o["dirMode"] = cam.dirMode;
  o["spacing"] = cam.spacing;
  o["startOffset"] = cam.startOffset;
  o["pressMs"] = cam.pressMs;
  o["minIntervalMs"] = cam.minIntervalMs;
  o["waitManual"] = cam.waitManual;
  o["holdFocus"] = cam.holdFocus;
  o["bleAddr"] = cam.bleAddr;
}

void Camera::apply(JsonObjectConst o) {
  JsonVariantConst v;
  if ((v = o["enabled"]).is<bool>()) cam.enabled = v.as<bool>();
  if ((v = o["waitManual"]).is<bool>()) cam.waitManual = v.as<bool>();
  if ((v = o["holdFocus"]).is<bool>()) cam.holdFocus = v.as<bool>();
  if ((v = o["trigger"]).is<float>()) cam.trigger = constrain((int)v.as<float>(), 0, 2);
  if ((v = o["dirMode"]).is<float>()) cam.dirMode = constrain((int)v.as<float>(), 0, 2);
  if ((v = o["spacing"]).is<float>()) cam.spacing = constrain(v.as<float>(), 1.0f, 5000.0f);
  if ((v = o["startOffset"]).is<float>()) cam.startOffset = constrain(v.as<float>(), 0.0f, 5000.0f);
  if ((v = o["pressMs"]).is<float>()) cam.pressMs = constrain((int)v.as<float>(), 20, 2000);
  if ((v = o["minIntervalMs"]).is<float>()) cam.minIntervalMs = constrain((int)v.as<float>(), 50, 60000);
}

static const char *bleName() {
  switch (bleState) {
    case BleState::Off: return "off";
    case BleState::Unpaired: return "unpaired";
    case BleState::Pairing: return "pairing";
    case BleState::Connecting: return "connecting";
    case BleState::Connected: return "connected";
    case BleState::Disconnected: return "disconnected";
  }
  return "?";
}

void Camera::fillStatus(JsonObject o) {
  o["ble"] = bleName();
  o["addr"] = cam.bleAddr;
  o["bleShots"] = bleShots;
  o["bleFails"] = bleFails;
}
