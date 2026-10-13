#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Spoušť fotoaparátu: Bluetooth (emulace dálkového ovladače Canon BR-E1) a/nebo drát (optočlen -> jack E3)
// Nastavení se ukládá zvlášť od Config (vlastní klíč v NVS) -> aktualizace nesmaže kalibraci pojezdu.
struct CamConfig {
  uint32_t version;
  bool enabled;            // fotit během programu (cyklus / 1 cyklus)
  uint8_t trigger;         // 0 = Bluetooth, 1 = drát, 2 = oboje
  uint8_t dirMode;         // 0 = oba směry, 1 = jen Od→Do, 2 = jen Do→Od
  float spacing;           // mm mezi snímky
  float startOffset;       // mm – první snímek až za tolik mm od začátku průjezdu
  uint16_t pressMs;        // délka stisku spouště
  uint16_t minIntervalMs;  // nejkratší povolený interval mezi snímky (ochrana proti vynechání)
  bool waitManual;         // na koncích čekat na tlačítko „Continue“ místo časovače
  bool holdFocus;          // drát: držet ostření (fotoaparát nespí) po dobu programu
  char bleAddr[18];        // spárovaný fotoaparát "aa:bb:cc:dd:ee:ff"
  uint8_t bleAddrType;
};

extern CamConfig cam;

namespace Camera {
  typedef void (*Notifier)(const char *msg, bool isError);

  void begin();
  void update();                       // volat z loop()
  void setNotifier(Notifier n);

  bool fire();                         // neblokující výstřel; false = spoušť není připravená
  const char *checkReady();            // nullptr = připraveno, jinak text chyby
  void programActive(bool on);         // drát: držení ostření během programu

  const char *cmdPair();
  void cmdForget();
  const char *cmdTest();

  void toJson(JsonObject o);
  void apply(JsonObjectConst o);
  void save();
  void fillStatus(JsonObject o);
}
