#include "control.h"
#include <string.h>
#include "sniffer.h"
#include "telnet_log.h"
#include "registers.h"

namespace Control {

static const uint32_t WAIT_MS = 4000;   // slot (<= 5 s) + HP answer, with margin

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
  Result r = run(REG_POWER, want);
  if (r.ok) r.message = note;
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
  Result r = run(REG_POWER, want);
  if (r.ok) r.message = note;
  return r;
}

// Observed on the panel: 130=-15 131=15 132=39 133=28 134=20 135=-5 136=10 137=48 138=1,5.
static const Setting SETTINGS[] = {
  {"low_outdoor",      130, -20,  5, false},   // heating curve: low outdoor temperature point
  {"high_outdoor",     131,   5, 25, false},   // heating curve: high outdoor temperature point
  {"flow_low",         132,  20, 60, false},   // flow temperature at the low outdoor point
  {"flow_high",        133,  20, 60, false},   // flow temperature at the high outdoor point
  {"heat_off_outdoor", 134,   5, 35, false},   // heating switches off above this outdoor temperature
  {"backup_on",        135, -20, 10, false},   // backup heater on below this outdoor temperature
  {"cool_set",         136,   5, 25, false},   // cooling flow temperature
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

Result setSetting(const Setting &s, int value) {
  Result r = {false, "", s.reg, 0, 0};
  if (value == 0 && !s.zeroIsWrite) { r.message = "0 cannot be written (the protocol reads 0 as a query)"; return r; }
  if (value < s.min || value > s.max) { r.message = "value out of range"; return r; }
  if (value == 0) return run(s.reg, 0);       // zeroIsWrite: the request itself is the write
  return run(s.reg, (uint8_t)(int8_t)value);
}

Result resetError() { return run(REG_RESET_ERROR, 2); }

}  // namespace Control
