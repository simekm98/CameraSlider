#include "motion.h"
#include "config.h"
#include "settings.h"
#include "capture.h"
#include "camera.h"
#include <FastAccelStepper.h>
#include <TMCStepper.h>

static FastAccelStepperEngine engine;
static FastAccelStepper *stepper = nullptr;
static TMC2209Stepper driver(&Serial1, TMC_R_SENSE, TMC_ADDRESS);

static MotionState state = MotionState::Idle;
static bool homed = false;
static bool enabled = false;
static bool driverOk = false;
static uint32_t lastDriverCheck = 0;
static Motion::Notifier notifier = nullptr;

// --- homing ---
enum class HomePhase : uint8_t { Leave, Seek, SeekStop, Backoff, Fine, FineStop, ToZero };
static HomePhase hPhase;
static int32_t hTrigger;

// --- program (cyklus / 1 cyklus) ---
enum class ProgPhase : uint8_t { ToStart, ToB, WaitB, ToA, WaitA };
static ProgPhase pPhase;
static bool pCycleMode, pOneWay;
static uint32_t pDone, pTarget;   // pTarget 0 = nekonečně
static float pA, pB;              // Od, Do (zafixováno při startu)
static uint32_t waitStart, waitMs;
static bool waitManualNow = false;    // čeká se na tlačítko Continue
static volatile bool continueReq = false;

// --- ruční posuv ---
static int jogDir = 0;
static uint32_t jogKeepalive = 0;

// ============================================================================
static void emit(const char *msg, bool err) {
  Serial.printf("[motion] %s\n", msg);
  if (notifier) notifier(msg, err);
}

static float spm() { return Settings::stepsPerMm(); }
static int32_t toSteps(float mm) { return lroundf(mm * spm()); }
static float toMm(int32_t st) { return st / spm(); }
static float clampPos(float mm) { return constrain(mm, 0.0f, cfg.maxPos); }
static bool running() { return stepper->isRunning(); }
static float curMm() { return toMm(stepper->getCurrentPosition()); }

static void setProfile(float speedMm, float accelMm, float smoothMm) {
  uint32_t hz = (uint32_t)constrain(speedMm * spm(), 1.0f, (float)MAX_STEP_HZ);
  stepper->setSpeedInHz(hz);
  stepper->setAcceleration((int32_t)max(1.0f, accelMm * spm()));
  stepper->setLinearAcceleration((uint32_t)(smoothMm * spm()));
}

static void moveToMm(float mm, float speed) {
  setProfile(speed, cfg.accel, cfg.smooth);
  stepper->moveTo(toSteps(clampPos(mm)));
}

// rychlost podle směru: ven (od home) / zpět (k home)
static void startLeg(float mm) {
  moveToMm(mm, mm >= curMm() ? cfg.speedOut : cfg.speedIn);
}

static const char *needReady() {
  if (!stepper) return "Motor not initialized";
  if (!homed) return "Run homing first";
  return nullptr;
}

// ============================================================================
void Motion::begin() {
  pinMode(PIN_HALL, INPUT_PULLUP);
  Serial1.begin(TMC_BAUD, SERIAL_8N1, PIN_TMC_RX, PIN_TMC_TX);
  delay(50);   // linka v klidu -> TMC zahodí případné smetí z doby resetu

  engine.init();
  stepper = engine.stepperConnectToPin(PIN_STEP);
  if (!stepper) {
    emit("Cannot initialize STEP pin", true);
    return;
  }
  stepper->setDirectionPin(PIN_DIR);
  stepper->setEnablePin(PIN_EN);        // aktivní v LOW
  stepper->setAutoEnable(false);
  stepper->disableOutputs();

  applyDriverSettings();
}

void Motion::setNotifier(Notifier n) { notifier = n; }

static uint32_t drvFailTotal = 0;   // statistika chyb UART (diagnostika)
static uint8_t drvFailRow = 0;

// test_connection s opakováním – jedno zkreslené čtení není chyba
static bool driverResponds() {
  for (int i = 0; i < 3; i++) {
    if (driver.test_connection() == 0) return true;
    drvFailTotal++;
    delay(5);
  }
  return false;
}

void Motion::applyDriverSettings() {
  driver.begin();
  driver.toff(4);
  driver.blank_time(24);
  driver.I_scale_analog(false);
  driver.internal_Rsense(false);
  driver.mstep_reg_select(true);   // mikrokroky z registru, ne z pinů MS1/MS2
  driver.pdn_disable(true);        // PDN pin = UART
  driver.rms_current(cfg.currentMa);
  driver.microsteps(cfg.microsteps);
  driver.intpol(true);
  driver.en_spreadCycle(!cfg.stealthChop);
  driver.pwm_autoscale(true);
  driver.shaft(cfg.dirInvert);
  driver.GSTAT(0b111);             // smazat příznak resetu

  // ověření zpětným čtením – ověří i to, že zápisy prošly
  uint16_t msRead = driver.microsteps();           // TMCStepper vrací 0 pro celé kroky
  driverOk = driverResponds() && (msRead ? msRead : 1) == cfg.microsteps;
  drvFailRow = 0;
  if (!driverOk) emit("TMC2209 not responding over UART – check PDN_UART, TX/RX and VIO supply", true);
  else Serial.printf("[motion] TMC2209 OK, %u mA, 1/%u\n", cfg.currentMa, cfg.microsteps);
}

// Průběžná kontrola driveru: naskočí-li VM později nebo driver projde resetem
// (výpadek VM), znovu se nakonfiguruje. Výpadek během provozu ruší homing.
static void checkDriver() {
  bool ok = driver.test_connection() == 0;
  if (!ok) {
    drvFailTotal++;
    Serial.printf("[tmc] čtení selhalo (celkem %lu)\n", (unsigned long)drvFailTotal);
    if (driverOk && ++drvFailRow < 3) return;   // výpadek až po 3 neúspěších za sebou
  } else {
    drvFailRow = 0;
  }

  if (ok && !driverOk) {
    Motion::applyDriverSettings();
    if (driverOk) emit("TMC2209 connected", false);
  } else if (ok && (driver.GSTAT() & 0x01)) {
    Motion::applyDriverSettings();
    if (homed) {
      homed = false;
      emit("TMC2209 was reset (VM power loss?) – homing required", true);
    }
  } else if (!ok && driverOk) {
    driverOk = false;
    emit("TMC2209 lost – check VM supply and UART wiring", true);
  }
}

void Motion::invalidateHome() {
  homed = false;
}

// ============================================================================
//  Homing
//  1) je-li čidlo aktivní, odjede ven
//  2) hledá čidlo směrem k 0, zapamatuje si krok sepnutí, plynule zabrzdí
//  3) souřadnice posune tak, aby bod sepnutí = 0
//  4) odjede o backoff; pokud čidlo pustilo, zopakuje hledání pomalu (přesnost)
//     (u západkového čidla SS41F nemusí pustit -> jemné hledání se přeskočí)
//  5) najede na 0
// ============================================================================
static void homeFail(const char *msg) {
  stepper->forceStop();
  state = MotionState::Idle;
  emit(msg, true);
}

static void startSeek() {
  hPhase = HomePhase::Seek;
  setProfile(cfg.homeSpeed, HOME_ACCEL_MMS2, 0);
  stepper->moveTo(stepper->getCurrentPosition() - toSteps(cfg.maxPos + 50));
}

const char *Motion::cmdHome() {
  if (!stepper) return "Motor not initialized";
  if (state != MotionState::Idle) return "Stop the motion first";
  if (!driverOk) applyDriverSettings();   // zkusit znovu

  stepper->enableOutputs();
  enabled = true;
  homed = false;
  stepper->setCurrentPosition(0);
  state = MotionState::Homing;

  if (hallActive()) {
    hPhase = HomePhase::Leave;
    setProfile(cfg.homeSpeed, HOME_ACCEL_MMS2, 0);
    stepper->moveTo(toSteps(cfg.homeBackoff * 2));
  } else {
    startSeek();
  }
  emit("Homing…", false);
  return nullptr;
}

static void updateHoming() {
  switch (hPhase) {
    case HomePhase::Leave:
      if (!running()) {
        if (Motion::hallActive()) return homeFail("Sensor still active after moving away – check magnet / sensor type");
        startSeek();
      }
      break;

    case HomePhase::Seek:
      if (Motion::hallActive()) {
        hTrigger = stepper->getCurrentPosition();
        stepper->stopMove();
        hPhase = HomePhase::SeekStop;
      } else if (!running()) {
        homeFail("Home sensor not found");
      }
      break;

    case HomePhase::SeekStop:
      if (!running()) {
        stepper->setCurrentPosition(stepper->getCurrentPosition() - hTrigger);
        hPhase = HomePhase::Backoff;
        setProfile(cfg.homeSpeed, HOME_ACCEL_MMS2, 0);
        stepper->moveTo(toSteps(cfg.homeBackoff));
      }
      break;

    case HomePhase::Backoff:
      if (!running()) {
        if (Motion::hallActive()) {
          // západkové čidlo nepustilo -> bereme hrubý výsledek
          hPhase = HomePhase::ToZero;
          setProfile(cfg.homeSpeed, HOME_ACCEL_MMS2, 0);
          stepper->moveTo(0);
        } else {
          hPhase = HomePhase::Fine;
          setProfile(max(cfg.homeSpeed * 0.2f, 0.5f), HOME_ACCEL_MMS2, 0);
          stepper->moveTo(-toSteps(cfg.homeBackoff));
        }
      }
      break;

    case HomePhase::Fine:
      if (Motion::hallActive()) {
        hTrigger = stepper->getCurrentPosition();
        stepper->stopMove();
        hPhase = HomePhase::FineStop;
      } else if (!running()) {
        homeFail("Home sensor not found during fine approach");
      }
      break;

    case HomePhase::FineStop:
      if (!running()) {
        stepper->setCurrentPosition(stepper->getCurrentPosition() - hTrigger);
        hPhase = HomePhase::ToZero;
        setProfile(cfg.homeSpeed, HOME_ACCEL_MMS2, 0);
        stepper->moveTo(0);
      }
      break;

    case HomePhase::ToZero:
      if (!running()) {
        homed = true;
        state = MotionState::Idle;
        emit("Homing complete", false);
      }
      break;
  }
}

// ============================================================================
//  Manuální polohování
// ============================================================================
const char *Motion::cmdGoto(float mm) {
  if (auto e = needReady()) return e;
  if (state != MotionState::Idle && state != MotionState::Moving) return "Another motion in progress – press Stop first";
  state = MotionState::Moving;
  startLeg(mm);
  return nullptr;
}

const char *Motion::cmdJogStep(float d) {
  if (auto e = needReady()) return e;
  if (state != MotionState::Idle && state != MotionState::Moving) return "Another motion in progress – press Stop first";
  float base = (state == MotionState::Moving) ? toMm(stepper->targetPos()) : curMm();
  state = MotionState::Moving;
  moveToMm(base + d, cfg.jogSpeed);
  return nullptr;
}

const char *Motion::cmdJogHold(int dir) {
  if (auto e = needReady()) return e;
  dir = dir > 0 ? 1 : -1;
  if (state == MotionState::Jogging && dir == jogDir) {
    jogKeepalive = millis();
    return nullptr;
  }
  if (state != MotionState::Idle && state != MotionState::Moving && state != MotionState::Jogging)
    return "Another motion in progress – press Stop first";
  jogDir = dir;
  jogKeepalive = millis();
  state = MotionState::Jogging;
  moveToMm(dir > 0 ? cfg.maxPos : 0, cfg.jogSpeed);
  return nullptr;
}

void Motion::cmdJogRelease() {
  if (state == MotionState::Jogging && jogDir) {
    stepper->stopMove();
    jogDir = 0;
  }
}

// ============================================================================
//  Program: cyklický režim / 1 cyklus
//  Najetí na Od -> Od→Do -> čekání -> Do→Od -> čekání -> ...
// ============================================================================
const char *Motion::cmdStartProgram(bool cycle) {
  if (auto e = needReady()) return e;
  if (state != MotionState::Idle) return "Motion in progress – press Stop first";
  pA = clampPos(cfg.rangeFrom);
  pB = clampPos(cfg.rangeTo);
  if (fabsf(pA - pB) < 0.1f) return "From–To range is zero";
  if (auto e = Capture::checkStart(pA, pB, !cycle && cfg.singleOneWay)) return e;

  pCycleMode = cycle;
  pOneWay = !cycle && cfg.singleOneWay;
  pTarget = cycle ? (cfg.cycleInfinite ? 0 : cfg.cycleCount) : 1;
  pDone = 0;
  continueReq = false;
  waitManualNow = false;
  Capture::programStart();
  state = MotionState::Program;
  pPhase = ProgPhase::ToStart;
  startLeg(pA);
  emit(cycle ? "Cycling started" : "Single cycle started", false);
  return nullptr;
}

static void startWait(bool atB) {
  float here = atB ? pB : pA, other = atB ? pA : pB;
  float s = (here >= other) ? cfg.waitFar : cfg.waitNear;
  waitMs = (uint32_t)(s * 1000.0f);
  waitStart = millis();
  waitManualNow = cam.waitManual;
  continueReq = false;
  if (waitManualNow) emit("Waiting – turn the board and press Continue", false);
  pPhase = atB ? ProgPhase::WaitB : ProgPhase::WaitA;
}

static void finishProgram() {
  Capture::programEnd(false);
  state = MotionState::Idle;
  emit("Program finished", false);
}

static bool waitDone() {
  if (waitManualNow) return continueReq;
  return millis() - waitStart >= waitMs;
}

static void legTo(bool toB) {
  pPhase = toB ? ProgPhase::ToB : ProgPhase::ToA;
  waitManualNow = false;
  continueReq = false;
  startLeg(toB ? pB : pA);
  Capture::legStart(toB ? pA : pB, toB ? pB : pA, toB);
}

static void updateProgram() {
  switch (pPhase) {
    case ProgPhase::ToStart:
      if (!running()) legTo(true);
      break;
    case ProgPhase::ToB:
      Capture::poll(curMm());
      if (!running()) {
        Capture::legEnd(curMm());
        if (pOneWay) finishProgram();
        else startWait(true);
      }
      break;
    case ProgPhase::WaitB:
      if (waitDone()) legTo(false);
      break;
    case ProgPhase::ToA:
      Capture::poll(curMm());
      if (!running()) {
        Capture::legEnd(curMm());
        pDone++;
        if (pTarget && pDone >= pTarget) finishProgram();
        else startWait(false);
      }
      break;
    case ProgPhase::WaitA:
      if (waitDone()) legTo(true);
      break;
  }
}

void Motion::cmdContinue() {
  if (state == MotionState::Program && (pPhase == ProgPhase::WaitA || pPhase == ProgPhase::WaitB))
    continueReq = true;
}

// ============================================================================
void Motion::cmdStop() {
  if (!stepper || state == MotionState::Idle) return;
  if (state == MotionState::Program) Capture::programEnd(true);
  stepper->stopMove();           // plynulé zabrzdění
  jogDir = 0;
  state = MotionState::Moving;   // -> Idle až po zastavení
  emit("Stop", false);
}

void Motion::cmdDisable() {
  if (!stepper) return;
  if (state == MotionState::Program) Capture::programEnd(true);
  stepper->forceStop();
  stepper->disableOutputs();
  enabled = false;
  homed = false;
  jogDir = 0;
  state = MotionState::Idle;
  emit("Motor released – homing required", false);
}

void Motion::update() {
  if (!stepper) return;

  // UART čtení může blokovat pár ms -> ne během homingu (čtení čidla)
  // a ne během průjezdu s focením (zpoždění spouště)
  if (state != MotionState::Homing && !Capture::legActive() && millis() - lastDriverCheck > 1000) {
    lastDriverCheck = millis();
    checkDriver();
  }

  switch (state) {
    case MotionState::Idle: break;
    case MotionState::Homing: updateHoming(); break;
    case MotionState::Moving:
      if (!running()) state = MotionState::Idle;
      break;
    case MotionState::Jogging:
      if (jogDir && millis() - jogKeepalive > JOG_TIMEOUT_MS) {
        stepper->stopMove();
        jogDir = 0;
      }
      if (!running()) { state = MotionState::Idle; jogDir = 0; }
      break;
    case MotionState::Program: updateProgram(); break;
  }
}

bool Motion::isBusy() { return state != MotionState::Idle; }
bool Motion::isHomed() { return homed; }

bool Motion::hallActive() {
  bool lvl = digitalRead(PIN_HALL);
  return cfg.hallActiveLow ? !lvl : lvl;
}

static const char *stateName() {
  switch (state) {
    case MotionState::Idle: return "Idle";
    case MotionState::Homing: return "Homing";
    case MotionState::Moving: return "Moving";
    case MotionState::Jogging: return "Jogging";
    case MotionState::Program: return "Program";
  }
  return "?";
}

static const char *phaseName() {
  switch (pPhase) {
    case ProgPhase::ToStart: return "ToStart";
    case ProgPhase::ToB: return "ToB";
    case ProgPhase::WaitB: return "WaitB";
    case ProgPhase::ToA: return "ToA";
    case ProgPhase::WaitA: return "WaitA";
  }
  return "";
}

static float r2(float v) { return roundf(v * 100.0f) / 100.0f; }

void Motion::fillStatus(JsonObject o) {
  o["state"] = stateName();
  o["homed"] = homed;
  o["enabled"] = enabled;
  o["hall"] = hallActive();
  o["driver"] = driverOk;
  o["drvFails"] = drvFailTotal;
  if (!stepper) return;

  o["pos"] = r2(curMm());
  o["target"] = r2(toMm(stepper->targetPos()));
  o["speed"] = r2(stepper->getCurrentSpeedInMilliHz() / 1000.0f / spm());

  if (state == MotionState::Program) {
    o["phase"] = phaseName();
    o["mode"] = pCycleMode ? "cycle" : "single";
    o["done"] = pDone;
    o["total"] = pTarget;
    if (pPhase == ProgPhase::WaitA || pPhase == ProgPhase::WaitB) {
      uint32_t el = millis() - waitStart;
      if (waitManualNow) o["waitManual"] = true;
      else o["waitLeft"] = r2(el < waitMs ? (waitMs - el) / 1000.0f : 0);
    }
  }
}
