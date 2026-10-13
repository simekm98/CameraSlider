#include "net.h"
#include "config.h"
#include "settings.h"
#include "motion.h"
#include "camera.h"
#include "capture.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");

// Příkazy z WebSocketu přicházejí v tasku async_tcp -> předáme je do loop()
static QueueHandle_t rxQueue;
static volatile bool pushSettings = false;
static volatile bool clientLeft = false;

static uint32_t lastStatus = 0;
static bool scanRequested = false;

static uint32_t staAttemptMs = 0;
static uint32_t staRetryAt = 0;
static bool staWasConnected = false;

static const char FALLBACK_HTML[] PROGMEM =
  "<!DOCTYPE html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
  "<body style='font-family:sans-serif;background:#111;color:#eee;padding:24px'>"
  "<h2>CameraSlider</h2><p>Web files not found in LittleFS.</p>"
  "<p>In Arduino IDE: <b>Ctrl+Shift+P &rarr; Upload LittleFS to Pico/ESP8266/ESP32</b> "
  "(<code>data</code> folder next to the .ino).</p>";

// ============================================================================
static void sendDoc(JsonDocument &d) {
  String s;
  serializeJson(d, s);
  ws.textAll(s);
}

void Net::sendMsg(const char *text, bool isError) {
  if (!ws.count()) return;
  JsonDocument d;
  d["type"] = "msg";
  d["text"] = text;
  d["err"] = isError;
  sendDoc(d);
}

static void sendSettings() {
  JsonDocument d;
  d["type"] = "settings";
  Settings::toJson(d["s"].to<JsonObject>());
  Camera::toJson(d["cam"].to<JsonObject>());
  sendDoc(d);
}

static void sendStatus() {
  JsonDocument d;
  d["type"] = "status";
  Motion::fillStatus(d["m"].to<JsonObject>());
  JsonObject c = d["c"].to<JsonObject>();
  Camera::fillStatus(c);
  Capture::fillStatus(c);

  JsonObject w = d["w"].to<JsonObject>();
  bool sta = WiFi.status() == WL_CONNECTED;
  w["sta"] = sta;
  w["ssid"] = cfg.ssid;
  if (sta) {
    w["ip"] = WiFi.localIP().toString();
    w["rssi"] = WiFi.RSSI();
  }
  w["apIp"] = WiFi.softAPIP().toString();
  w["apCl"] = WiFi.softAPgetStationNum();

  d["jmp"] = digitalRead(PIN_RESET_JMP) == LOW;
  sendDoc(d);
}

// ============================================================================
//  WiFi klient
// ============================================================================
static void startSta() {
  WiFi.disconnect();
  staRetryAt = 0;
  staWasConnected = false;
  if (!cfg.ssid[0]) return;
  Serial.printf("[wifi] připojuji k '%s'\n", cfg.ssid);
  WiFi.begin(cfg.ssid, cfg.pass);
  staAttemptMs = millis();
}

// Neúspěšné připojování ruší AP (ESP skenuje kanály) -> po timeoutu pauza
static void handleSta() {
  if (!cfg.ssid[0]) return;
  if (WiFi.status() == WL_CONNECTED) {
    staAttemptMs = millis();
    if (!staWasConnected) {
      staWasConnected = true;
      String m = "Connected to " + String(cfg.ssid) + ", IP " + WiFi.localIP().toString();
      Serial.println("[wifi] " + m);
      Net::sendMsg(m.c_str(), false);
    }
    return;
  }
  staWasConnected = false;
  if (staRetryAt) {
    if ((int32_t)(millis() - staRetryAt) >= 0) startSta();
    return;
  }
  if (millis() - staAttemptMs > STA_CONNECT_TIMEOUT_MS) {
    Serial.println("[wifi] připojení selhalo, další pokus za 60 s");
    WiFi.disconnect();
    staRetryAt = millis() + STA_RETRY_MS;
    if (!staRetryAt) staRetryAt = 1;
  }
}

static void handleScan() {
  if (!scanRequested) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  scanRequested = false;
  if (n < 0) {
    Net::sendMsg("WiFi scan failed, try again", true);
    return;
  }
  JsonDocument d;
  d["type"] = "scan";
  JsonArray list = d["list"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;
    for (JsonObject e : list) if (ssid == e["ssid"].as<const char *>()) { dup = true; break; }
    if (dup) continue;
    JsonObject e = list.add<JsonObject>();
    e["ssid"] = ssid;
    e["rssi"] = WiFi.RSSI(i);
    e["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendDoc(d);
}

// ============================================================================
//  Příkazy z UI
// ============================================================================
static void handleCommand(const char *txt) {
  JsonDocument doc;
  if (deserializeJson(doc, txt)) return;
  const char *cmd = doc["cmd"] | "";
  const char *err = nullptr;

  if (!strcmp(cmd, "home")) err = Motion::cmdHome();
  else if (!strcmp(cmd, "stop")) Motion::cmdStop();
  else if (!strcmp(cmd, "disable")) Motion::cmdDisable();
  else if (!strcmp(cmd, "goto")) err = Motion::cmdGoto(doc["pos"] | 0.0f);
  else if (!strcmp(cmd, "jogStep")) err = Motion::cmdJogStep(doc["d"] | 0.0f);
  else if (!strcmp(cmd, "jogHold")) err = Motion::cmdJogHold(doc["dir"] | 0);
  else if (!strcmp(cmd, "jogRelease")) Motion::cmdJogRelease();
  else if (!strcmp(cmd, "cycle")) err = Motion::cmdStartProgram(true);
  else if (!strcmp(cmd, "single")) err = Motion::cmdStartProgram(false);
  else if (!strcmp(cmd, "continue")) Motion::cmdContinue();

  else if (!strcmp(cmd, "camSave")) {
    Camera::apply(doc["s"].as<JsonObjectConst>());
    Camera::save();
    sendSettings();
  }
  else if (!strcmp(cmd, "camPair")) {
    if (Motion::isBusy()) err = "Stop the motion before pairing";
    else err = Camera::cmdPair();
  }
  else if (!strcmp(cmd, "camForget")) Camera::cmdForget();
  else if (!strcmp(cmd, "camTest")) {
    err = Camera::cmdTest();
    if (!err) Net::sendMsg("Test shot", false);
  }

  else if (!strcmp(cmd, "saveMotion")) {
    Settings::applyMotion(doc["s"].as<JsonObjectConst>());
    Settings::save();
    sendSettings();
  }
  else if (!strcmp(cmd, "saveConfig")) {
    if (Motion::isBusy()) err = "Configuration can only be changed when idle – press Stop first";
    else {
      bool rehome = false;
      Settings::applyConfig(doc["s"].as<JsonObjectConst>(), rehome);
      Settings::save();
      Motion::applyDriverSettings();
      if (rehome && Motion::isHomed()) {
        Motion::invalidateHome();
        Net::sendMsg("Configuration saved – microstep/direction change requires new homing", false);
      } else {
        Net::sendMsg("Configuration saved", false);
      }
      sendSettings();
    }
  }
  else if (!strcmp(cmd, "saveWifi")) {
    strlcpy(cfg.ssid, doc["ssid"] | "", sizeof(cfg.ssid));
    strlcpy(cfg.pass, doc["pass"] | "", sizeof(cfg.pass));
    Settings::save();
    startSta();
    Net::sendMsg(cfg.ssid[0] ? "WiFi saved, connecting…" : "WiFi connection removed", false);
    sendSettings();
  }
  else if (!strcmp(cmd, "scan")) {
    if (!scanRequested) {
      int r = WiFi.scanNetworks(true);
      if (r == WIFI_SCAN_FAILED) err = "Cannot start scan (ESP is connecting)";
      else scanRequested = true;
    }
  }
  else if (!strcmp(cmd, "factoryReset")) {
    Motion::cmdDisable();
    Settings::factoryReset();
    Net::sendMsg("Factory settings restored, restarting…", false);
    delay(500);
    ESP.restart();
  }

  if (err) Net::sendMsg(err, true);
}

static void onWsEvent(AsyncWebSocket *, AsyncWebSocketClient *, AwsEventType type,
                      void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    pushSettings = true;
  } else if (type == WS_EVT_DISCONNECT) {
    clientLeft = true;      // pojistka pro ruční posuv
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      char *copy = (char *)malloc(len + 1);
      if (!copy) return;
      memcpy(copy, data, len);
      copy[len] = 0;
      if (xQueueSend(rxQueue, &copy, 0) != pdTRUE) free(copy);
    }
  }
}

// ============================================================================
void Net::begin() {
  rxQueue = xQueueCreate(16, sizeof(char *));

  if (!LittleFS.begin(true)) Serial.println("[fs] LittleFS nelze připojit");
  else if (!LittleFS.exists("/index.html")) Serial.println("[fs] chybí /index.html – nahraj složku data");

  WiFi.persistent(false);
  WiFi.setHostname(HOSTNAME);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.setAutoReconnect(true);
  Serial.printf("[wifi] AP '%s' IP %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
  startSta();
  configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org");   // čas pro záznam (jen při připojení k síti)

  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("[mdns] http://" MDNS_NAME ".local");
  }

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  // záznam snímků pro třídění fotek v PC
  server.on("/api/shots", HTTP_GET, [](AsyncWebServerRequest *r) {
    AsyncResponseStream *res = r->beginResponseStream("application/json");
    res->addHeader("Access-Control-Allow-Origin", "*");
    Capture::writeJson(*res);
    r->send(res);
  });
  server.on("/shots.csv", HTTP_GET, [](AsyncWebServerRequest *r) {
    AsyncResponseStream *res = r->beginResponseStream("text/csv");
    res->addHeader("Access-Control-Allow-Origin", "*");
    res->addHeader("Content-Disposition", "attachment; filename=\"shots.csv\"");
    Capture::writeCsv(*res);
    r->send(res);
  });

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (LittleFS.exists("/index.html")) r->send(LittleFS, "/index.html", "text/html");
    else r->send(200, "text/html", FALLBACK_HTML);
  });
  server.serveStatic("/", LittleFS, "/").setCacheControl("no-cache");
  server.onNotFound([](AsyncWebServerRequest *r) { r->redirect("/"); });
  server.begin();
}

void Net::update() {
  char *msg;
  while (xQueueReceive(rxQueue, &msg, 0) == pdTRUE) {
    handleCommand(msg);
    free(msg);
  }
  if (clientLeft) { clientLeft = false; Motion::cmdJogRelease(); }
  if (pushSettings) { pushSettings = false; sendSettings(); }

  uint32_t now = millis();
  if (now - lastStatus >= STATUS_INTERVAL_MS) {
    lastStatus = now;
    if (ws.count()) sendStatus();
    ws.cleanupClients();
  }
  handleScan();
  handleSta();
}
