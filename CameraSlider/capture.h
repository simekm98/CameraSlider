#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Focení podle polohy během programu + záznam průjezdů a snímků (pro třídění fotek v PC)
namespace Capture {
  // kontrola před startem programu; nullptr = OK
  const char *checkStart(float a, float b, bool oneWay);

  void programStart();                      // nový sken = nová session, smaže záznam
  void programEnd(bool aborted);
  void legStart(float from, float to, bool forward);   // forward = Od→Do
  void poll(float posMm);                   // volat co nejčastěji během průjezdu
  void legEnd(float posMm);
  bool legActive();

  void fillStatus(JsonObject o);
  void writeJson(Print &p);
  void writeCsv(Print &p);
}
