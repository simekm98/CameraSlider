#include "capture.h"
#include "camera.h"
#include "settings.h"
#include <time.h>

// ============================================================================
//  Snímky se spouští podle POLOHY (každých cam.spacing mm), ne podle času –
//  rozestup sedí i při rozjezdu a brzdění.
//  Každý průjezd s focením = jeden „pass“ (= jedna plocha desky).
// ============================================================================
static constexpr uint16_t MAX_PASSES = 256;
static constexpr uint32_t MAX_SHOTS = 4000;

struct Shot { uint16_t pass, idx; float pos; uint32_t t; uint8_t ok; };
struct Pass { uint16_t n; uint8_t forward; float from, to; uint32_t tStart, tEnd; uint16_t planned, shots, failed; };

static Shot *shots = nullptr;
static Pass *passes = nullptr;
static volatile uint32_t nShots = 0;
static volatile uint16_t nPasses = 0;
static uint32_t session = 0;
static uint32_t t0 = 0;
static time_t epoch0 = 0;
static bool progRunning = false, lastAborted = false;

// aktuální průjezd
static bool legOn = false;
static float lFrom, lSign, lSpacing, lOffset;
static uint16_t lPlanned, lNext;

static bool alloc() {
  if (shots && passes) return true;
  size_t sb = sizeof(Shot) * MAX_SHOTS, pb = sizeof(Pass) * MAX_PASSES;
  shots = (Shot *)(psramFound() ? ps_malloc(sb) : malloc(sb));
  passes = (Pass *)(psramFound() ? ps_malloc(pb) : malloc(pb));
  return shots && passes;
}

static float legSpeed(float from, float to) { return to >= from ? cfg.speedOut : cfg.speedIn; }

static bool dirEnabled(bool forward) {
  return cam.dirMode == 0 || (cam.dirMode == 1 && forward) || (cam.dirMode == 2 && !forward);
}

static uint16_t plannedShots(float from, float to) {
  float len = fabsf(to - from) - cam.startOffset;
  if (len < 0) return 0;
  return (uint16_t)min(floorf(len / cam.spacing + 1e-3f) + 1.0f, 65000.0f);
}

// ============================================================================
const char *Capture::checkStart(float a, float b, bool oneWay) {
  if (!cam.enabled) return nullptr;
  static char err[120];
  for (int i = 0; i < (oneWay ? 1 : 2); i++) {
    bool fwd = i == 0;
    if (!dirEnabled(fwd)) continue;
    float from = fwd ? a : b, to = fwd ? b : a;
    float ms = cam.spacing / legSpeed(from, to) * 1000.0f;
    if (ms < cam.minIntervalMs) {
      snprintf(err, sizeof(err), "Too fast for the camera: a shot every %.0f ms (min %u ms) – increase spacing or reduce speed",
               ms, cam.minIntervalMs);
      return err;
    }
  }
  if (auto e = Camera::checkReady()) return e;
  if (!alloc()) return "Not enough memory for the shot log";
  return nullptr;
}

void Capture::programStart() {
  progRunning = true;
  lastAborted = false;
  if (!cam.enabled) return;
  nShots = 0;
  nPasses = 0;
  session++;
  t0 = millis();
  time_t now = time(nullptr);
  epoch0 = now > 1600000000 ? now : 0;
  Camera::programActive(true);
}

void Capture::programEnd(bool aborted) {
  if (legOn) legEnd(NAN);
  progRunning = false;
  lastAborted = aborted;
  Camera::programActive(false);
}

static void shoot(float pos) {
  bool ok = Camera::fire();
  Pass &p = passes[nPasses - 1];
  if (nShots < MAX_SHOTS) {
    Shot &s = shots[nShots];
    s.pass = p.n;
    s.idx = lNext + 1;
    s.pos = pos;
    s.t = millis() - t0;
    s.ok = ok;
    nShots = nShots + 1;
  }
  p.shots++;
  if (!ok) p.failed++;
  lNext++;
}

void Capture::legStart(float from, float to, bool forward) {
  legOn = false;
  if (!cam.enabled || !shots || !dirEnabled(forward) || nPasses >= MAX_PASSES) return;

  lFrom = from;
  lSign = to >= from ? 1.0f : -1.0f;
  lSpacing = cam.spacing;
  lOffset = cam.startOffset;
  lPlanned = plannedShots(from, to);
  lNext = 0;

  Pass &p = passes[nPasses];
  p.n = nPasses + 1;
  p.forward = forward;
  p.from = from;
  p.to = to;
  p.tStart = millis() - t0;
  p.tEnd = 0;
  p.planned = lPlanned;
  p.shots = 0;
  p.failed = 0;
  nPasses = nPasses + 1;
  legOn = lPlanned > 0;

  poll(from);   // první snímek hned na startu (při startOffset = 0)
}

void Capture::poll(float pos) {
  if (!legOn || lNext >= lPlanned) return;
  float target = lFrom + lSign * (lOffset + lNext * lSpacing);
  if (lSign > 0 ? pos >= target - 0.01f : pos <= target + 0.01f) shoot(pos);   // max. 1 snímek za průchod smyčkou
}

void Capture::legEnd(float pos) {
  if (!legOn) return;
  // poslední pozice může kvůli zaokrouhlení vyjít přesně na konec
  if (!isnan(pos) && lNext + 1 == lPlanned) {
    float target = lFrom + lSign * (lOffset + lNext * lSpacing);
    if (fabsf(pos - target) < 1.0f) shoot(pos);
  }
  passes[nPasses - 1].tEnd = millis() - t0;
  legOn = false;
}

bool Capture::legActive() { return legOn; }

// ============================================================================
void Capture::fillStatus(JsonObject o) {
  o["enabled"] = cam.enabled;
  o["session"] = session;
  o["passes"] = nPasses;
  o["shots"] = nShots;
  o["leg"] = legOn;
  if (nPasses) {
    const Pass &p = passes[nPasses - 1];
    o["pShots"] = p.shots;
    o["pPlanned"] = p.planned;
    uint32_t failed = 0;
    for (uint16_t i = 0; i < nPasses; i++) failed += passes[i].failed;
    o["failed"] = failed;
  }
}

// Záznam je jen připisovaný – čtení bez zámku stačí (počty se načtou jednou).
void Capture::writeJson(Print &out) {
  uint16_t np = nPasses;
  uint32_t ns = nShots;
  out.printf("{\"session\":%lu,\"startEpoch\":%ld,\"running\":%s,\"aborted\":%s,\"spacing\":%.2f,\"passes\":[",
             (unsigned long)session, (long)epoch0, progRunning ? "true" : "false", lastAborted ? "true" : "false", cam.spacing);
  for (uint16_t i = 0; i < np; i++) {
    const Pass &p = passes[i];
    out.printf("%s{\"pass\":%u,\"dir\":\"%s\",\"from\":%.2f,\"to\":%.2f,\"tStart\":%lu,\"tEnd\":%lu,\"planned\":%u,\"shots\":%u,\"failed\":%u}",
               i ? "," : "", p.n, p.forward ? "fwd" : "back", p.from, p.to,
               (unsigned long)p.tStart, (unsigned long)p.tEnd, p.planned, p.shots, p.failed);
  }
  out.print("],\"shots\":[");
  for (uint32_t i = 0; i < ns; i++) {
    const Shot &s = shots[i];
    out.printf("%s[%u,%u,%.2f,%lu,%u]", i ? "," : "", s.pass, s.idx, s.pos, (unsigned long)s.t, s.ok);
  }
  out.print("],\"shotFields\":[\"pass\",\"idx\",\"pos_mm\",\"t_ms\",\"ok\"]}");
}

void Capture::writeCsv(Print &out) {
  uint16_t np = nPasses;
  uint32_t ns = nShots;
  out.printf("# session;%lu;start_epoch;%ld;passes;%u;shots;%lu\n", (unsigned long)session, (long)epoch0, np, (unsigned long)ns);
  out.print("pass;dir;shot;pos_mm;t_ms;ok\n");
  for (uint32_t i = 0; i < ns; i++) {
    const Shot &s = shots[i];
    bool fwd = s.pass >= 1 && s.pass <= np ? passes[s.pass - 1].forward : true;
    out.printf("%u;%s;%u;%.2f;%lu;%u\n", s.pass, fwd ? "fwd" : "back", s.idx, s.pos, (unsigned long)s.t, s.ok);
  }
}
