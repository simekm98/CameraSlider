#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// HW vrstva: TMC2209 (UART) + generování kroků (FastAccelStepper) + stavový automat
enum class MotionState : uint8_t { Idle, Homing, Moving, Jogging, Program };

namespace Motion {
  typedef void (*Notifier)(const char *msg, bool isError);

  void begin();
  void update();                 // volat co nejčastěji z loop()
  void setNotifier(Notifier n);
  void applyDriverSettings();    // po změně konfigurace
  void invalidateHome();

  // příkazy – vrací nullptr při úspěchu, jinak text chyby
  const char *cmdHome();
  const char *cmdGoto(float mm);
  const char *cmdJogStep(float deltaMm);
  const char *cmdJogHold(int dir);   // opakovat každých ~200 ms (keepalive)
  void cmdJogRelease();
  const char *cmdStartProgram(bool cycle);  // true = cyklický režim, false = 1 cyklus
  void cmdStop();
  void cmdContinue();             // pokračovat po čekání na tlačítko
  void cmdDisable();

  bool isBusy();
  bool isHomed();
  bool hallActive();
  void fillStatus(JsonObject o);
}
