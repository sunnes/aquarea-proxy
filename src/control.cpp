#include "control.h"
#include <string.h>
#include "sniffer.h"
#include "telnet_log.h"
#include "registers.h"

namespace Control {

static const uint32_t WAIT_MS = 4000;   // slot (<= 5 s) + HP answer, with margin

// Timing guards (see control.h). Service manual WH-SDC03/05E3E5:
//  13.1.1 "The compressor will not start for three minutes after stop of operation."
//  16.4   protection errors lock out after repeats within 20-30 minutes (F16 3x/20 min,
//         F20 4x/30 min, F22 3x/30 min, ...); resetting in a loop would hide such a fault.
static const uint32_t COMPRESSOR_RESTART_MS = 3UL * 60 * 1000;
static const uint32_t RESET_INTERVAL_MS = 30UL * 60 * 1000;
static const uint8_t REG_CURRENT_ERROR = 20;
static const uint8_t ERR_H76 = 156;     // panel <-> main board link error (raw value seen on the bus)

struct Stamp { bool set; uint32_t ms; };
static Stamp lastPowerOff, lastModeChange, lastReset;

static void stamp(Stamp &s) { s.set = true; s.ms = millis(); }
static uint32_t remainingMs(const Stamp &s, uint32_t window) {
  if (!s.set) return 0;
  uint32_t elapsed = millis() - s.ms;          // unsigned: survives the millis() wrap
  return elapsed >= window ? 0 : window - elapsed;
}

static char refuseText[112];
static Result refuse(uint8_t reg, uint8_t val, const char *why, uint32_t retryMs) {
  if (retryMs) snprintf(refuseText, sizeof(refuseText), "refused: %s, retry in %lu s", why,
                        (unsigned long)((retryMs + 999) / 1000));
  else snprintf(refuseText, sizeof(refuseText), "refused: %s", why);
  Result r = {false, refuseText, reg, val, 0, true};
  return r;
}

// Queue one request and keep servicing the bus until it finishes.
static Result run(uint8_t reg, uint8_t val) {
  Result r = {false, "", reg, val, 0};
  if (!Sniffer::injectRequest(reg, val)) {
    r.message = "busy, or forwarding is off";
    return r;
  }
  uint32_t t0 = millis();
  while (Sniffer::injectBusy() && millis() - t0 < WAIT_MS) {
    Sniffer::poll();
    TelnetLog::poll();     // drains frames so the register table sees our exchange
    yield();
  }
  TelnetLog::poll();
  Sniffer::InjectStatus st = Sniffer::injectStatus();
  r.reply = st.reply;
  switch (st.state) {
    case Sniffer::INJ_OK:       r.ok = true; r.message = "ok"; break;
    case Sniffer::INJ_MISMATCH: r.message = "heat pump answered a different value"; break;
    case Sniffer::INJ_TIMEOUT:  r.message = "no answer from heat pump"; break;
    case Sniffer::INJ_NOSLOT:   r.message = "no idle slot on the bus (is the panel polling?)"; break;
    default:                    r.message = "still busy"; break;
  }
  return r;
}

Result readRegister(uint8_t reg) {
  for (size_t i = 0; const Setting *s = settingAt(i); i++) {
    if (s->reg == reg && s->zeroIsWrite) {
      Result r = {false, "refused: a read of this register writes 0 to it", reg, 0, 0};
      return r;
    }
  }
  return run(reg, 0);
}

// Register 144 is write-only (a read returns 0). The panel writes 3 for ON and 2 for OFF,
// so bit 0 is power and the other bits are kept from the panel's last write.
Result setPower(bool on) {
  uint8_t base = 2;                          // heat-mode bits seen in the panel captures
  const char *note = "ok (assumed base value 2: no panel write seen yet)";
  if (Registers::known(REG_POWER)) {
    base = Registers::value(REG_POWER);
    note = "ok (other bits kept from the panel's last write)";
  }
  uint8_t want = (uint8_t)((base & ~1u) | (on ? 1u : 0u));
  if (on) {                                  // OFF is always allowed: it is the safe direction
    uint32_t wait = remainingMs(lastPowerOff, COMPRESSOR_RESTART_MS);
    if (wait) return refuse(REG_POWER, want, "the compressor restart delay (3 min) after power off is running", wait);
  }
  Result r = run(REG_POWER, want);
  if (r.ok) {
    r.message = note;
    if (!on) stamp(lastPowerOff);
  }
  return r;
}

struct ModeName { const char *name; uint8_t bits; };
static const ModeName MODES[] = {
  {"heat", 2}, {"cool", 4}, {"auto", 32}, {"tank", 16},
  {"heat+tank", 18}, {"cool+tank", 20}, {"auto+tank", 48},
};

bool modeBits(const char *name, uint8_t &bits) {
  for (const ModeName &m : MODES) {
    if (strcmp(m.name, name) == 0) { bits = m.bits; return true; }
  }
  return false;
}

Result setMode(uint8_t bits) {
  const uint8_t MODE_MASK = 2 | 4 | 16 | 32;
  uint8_t base = Registers::known(REG_POWER) ? Registers::value(REG_POWER) : 0;
  // Current power state: the status flags (register 27) are the truth; fall back to the last write.
  uint8_t power;
  const char *note;
  if (Registers::known(REG_STATUS)) {
    power = Registers::value(REG_STATUS) & 1;
    note = "ok (power state kept from the status flags)";
  } else {
    power = base & 1;
    note = "ok (power state assumed from the last write)";
  }
  uint8_t want = (uint8_t)((base & ~(MODE_MASK | 1u)) | (bits & MODE_MASK) | power);
  // While running, a mode change can reverse the 4-way valve (heat <-> cool) or move the 3-way
  // valve (room <-> tank); keep such changes at least the compressor restart delay apart.
  bool changes = !Registers::known(REG_POWER) || Registers::value(REG_POWER) != want;
  if (power && changes) {
    uint32_t wait = remainingMs(lastModeChange, COMPRESSOR_RESTART_MS);
    if (wait) return refuse(REG_POWER, want, "mode changed less than 3 min ago while running", wait);
  }
  Result r = run(REG_POWER, want);
  if (r.ok) {
    r.message = note;
    if (changes) stamp(lastModeChange);
  }
  return r;
}

// Observed on the panel: 130=-15 131=15 132=39 133=28 134=20 135=-5 136=10 137=48 138=1,5.
// Limits: control panel ranges from the service manual WH-SDC03/05E3E5 (section 4.1.1, button 21
// "System Setting Mode"); cooling flow 5..20 C is the cooling water outlet operation range (section 3).
static const Setting SETTINGS[] = {
  {"low_outdoor",      130, -15, 15, false},   // heating curve: low outdoor temperature point
  {"high_outdoor",     131, -15, 15, false},   // heating curve: high outdoor temperature point
  {"flow_low",         132,  25, 55, false},   // flow temperature at the low outdoor point
  {"flow_high",        133,  25, 55, false},   // flow temperature at the high outdoor point
  {"heat_off_outdoor", 134,   5, 35, false},   // heating switches off above this outdoor temperature
  {"backup_on",        135, -15, 20, false},   // backup heater on below this outdoor temperature
  {"cool_set",         136,   5, 20, false},   // cooling flow temperature
  // Tank target: the heat pump alone heats the tank to at most 51 C (manual 12.1.6); above that
  // the tank's booster heater does the rest (needs a booster heater, uses direct electric heat).
  {"tank_set",         137,  40, 75, false},   // hot water tank target
  // Heat curve shift, panel range -5..5, sent as a signed byte (-1 = 255, confirmed echo).
  // A request with value 0 is a WRITE of 0 here (a read of 138 zeroed the shift), which is how
  // the panel resets it. So 0 is allowed, and this register must never be read.
  {"heat_shift",       138,  -5,  5, true},
};

const Setting *findSetting(const char *key) {
  for (const Setting &s : SETTINGS) if (strcmp(s.key, key) == 0) return &s;
  return nullptr;
}

const Setting *settingAt(size_t i) { return i < sizeof(SETTINGS) / sizeof(SETTINGS[0]) ? &SETTINGS[i] : nullptr; }

// The heating curve (manual 12.1.8) is flat below the low outdoor point and above the high one,
// with the flow temperature falling in between. Refuse writes that would fold it: the outdoor
// points must stay in order (equal points would make the slope undefined) and the colder point
// must not get the cooler flow. Unknown other point: no check (it is polled by the panel).
static char curveText[80];
static const char *curveConflict(uint8_t reg, int v) {
  struct Rule { uint8_t reg, other; bool otherAbove; bool strict; const char *what; };
  static const Rule RULES[] = {
    {130, 131, true,  true,  "low_outdoor must stay below high_outdoor"},
    {131, 130, false, true,  "high_outdoor must stay above low_outdoor"},
    {132, 133, false, false, "flow_low must not be below flow_high"},
    {133, 132, true,  false, "flow_high must not be above flow_low"},
  };
  for (const Rule &rule : RULES) {
    if (rule.reg != reg || !Registers::known(rule.other)) continue;
    int o = (int8_t)Registers::value(rule.other);
    bool bad = rule.otherAbove ? (rule.strict ? v >= o : v > o) : (rule.strict ? v <= o : v < o);
    if (!bad) return nullptr;
    snprintf(curveText, sizeof(curveText), "%s (now %d)", rule.what, o);
    return curveText;
  }
  return nullptr;
}

Result setSetting(const Setting &s, int value) {
  Result r = {false, "", s.reg, 0, 0};
  if (value == 0 && !s.zeroIsWrite) { r.message = "0 cannot be written (the protocol reads 0 as a query)"; return r; }
  if (value < s.min || value > s.max) { r.message = "value out of range"; return r; }
  if (const char *why = curveConflict(s.reg, value)) return refuse(s.reg, (uint8_t)(int8_t)value, why, 0);
  if (value == 0) return run(s.reg, 0);       // zeroIsWrite: the request itself is the write
  return run(s.reg, (uint8_t)(int8_t)value);
}

Result resetError() {
  bool linkError = Registers::known(REG_CURRENT_ERROR) && Registers::value(REG_CURRENT_ERROR) == ERR_H76;
  if (!linkError) {
    uint32_t wait = remainingMs(lastReset, RESET_INTERVAL_MS);
    if (wait) return refuse(REG_RESET_ERROR, 2, "an error was reset less than 30 min ago; a repeating fault needs a look", wait);
  }
  Result r = run(REG_RESET_ERROR, 2);
  if (r.ok) stamp(lastReset);
  return r;
}

}  // namespace Control
