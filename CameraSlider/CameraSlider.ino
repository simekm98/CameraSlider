/*
  CameraSlider – ESP32-S3 + TMC2209 + NEMA17 + Hallovo homovací čidlo

  Knihovny (Library Manager):
    - FastAccelStepper   (gin66)
    - TMCStepper         (teemuatlut)
    - ESPAsyncWebServer  (ESP32Async)
    - AsyncTCP           (ESP32Async)
    - ArduinoJson        (Benoit Blanchon, v7)
    - NimBLE-Arduino     (h2zero, v2.x)  – Bluetooth spoušť Canon

  Web UI je v LittleFS (složka data/) – nahrát přes plugin arduino-littlefs-upload.
  UI: WiFi "CameraSlider" -> http://cameraslider.local (nebo http://192.168.4.1)
*/
#include "config.h"
#include "settings.h"
#include "motion.h"
#include "net.h"
#include "camera.h"

static bool jumperWasPresent = false;
static uint32_t jumperOpenSince = 0;

static bool jumperPresent() {
  return digitalRead(PIN_RESET_JMP) == LOW;
}

// Rozpojení jumperu za běhu -> smazat nastavení a restart
static void checkJumper() {
  if (jumperPresent()) {
    jumperWasPresent = true;
    jumperOpenSince = 0;
    return;
  }
  if (!jumperWasPresent) return;          // rozpojený už při startu -> vyřešeno v setup()
  if (!jumperOpenSince) jumperOpenSince = millis();
  if (millis() - jumperOpenSince > 300) {  // odrušení
    Serial.println("[reset] jumper rozpojen -> tovární nastavení");
    Motion::cmdDisable();
    Settings::factoryReset();
    Net::sendMsg("Reset jumper removed – settings cleared, restarting…", true);
    delay(500);
    ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== CameraSlider ===");

  pinMode(PIN_RESET_JMP, INPUT_PULLUP);
  delay(10);

  Settings::begin();
  if (!jumperPresent()) {
    Serial.println("[reset] jumper rozpojen při startu -> tovární nastavení");
    Settings::factoryReset();
  }
  jumperWasPresent = jumperPresent();

  Motion::begin();
  Camera::begin();
  Net::begin();
  Motion::setNotifier(Net::sendMsg);
  Camera::setNotifier(Net::sendMsg);
}

void loop() {
  Motion::update();
  Camera::update();
  Net::update();
  checkJumper();
}
