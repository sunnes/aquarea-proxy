#include "registers.h"
#include "protocol.h"

namespace Registers {

static uint8_t  val[256];
static uint32_t updated[256];          // 0 = unknown
static Stats st;

static struct {
  bool     active;
  uint8_t  reg, val;
  uint32_t ts;
} pending;

// Power is split over two registers (low byte, high byte), in watts.
static bool powerPair(uint8_t reg, uint8_t &lo, uint8_t &hi) {
  switch (reg) {
    case 29: case 30: lo = 29; hi = 30; return true;
    case 31: case 32: lo = 31; hi = 32; return true;
    case 33: case 34: lo = 33; hi = 34; return true;
    default: return false;
  }
}

// Write-only command registers: a read returns 0, so a read reply says nothing about the
// last written value and must not overwrite it (144 = power control, 129 = reset error).
static bool writeOnly(uint8_t reg) { return reg == 144 || reg == 129; }

bool isHighByte(uint8_t reg) { return reg == 30 || reg == 32 || reg == 34; }

const char *name(uint8_t reg) {
  switch (reg) {
    case 29: case 30: return "heat power";
    case 31: case 32: return "cool power";
    case 33: case 34: return "tank power";
    case 255: return "no-data reply";
    default: {
      const char *n = Protocol::regName(reg);
      return n ? n : "unknown";
    }
  }
}

// Decode `reg`, pretending register `ovr` holds `ovrVal` (so we can describe old/new values).
static void textWith(uint8_t reg, uint8_t ovr, uint8_t ovrVal, char *buf, size_t n) {
  uint8_t lo, hi;
  if (powerPair(reg, lo, hi)) {
    uint8_t l = (lo == ovr) ? ovrVal : val[lo];
    uint8_t h = (hi == ovr) ? ovrVal : val[hi];
    snprintf(buf, n, "%u W", (unsigned)h * 256u + l);
    return;
  }
  Protocol::describe(reg, reg == ovr ? ovrVal : val[reg], buf, n);
}

void text(uint8_t reg, char *buf, size_t n) { textWith(reg, reg, val[reg], buf, n); }

bool known(uint8_t reg) { return updated[reg] != 0; }
uint8_t value(uint8_t reg) { return val[reg]; }
uint32_t updatedMs(uint8_t reg) { return updated[reg]; }
// updated[] is stamped with millis() | 1, which can be 1 ms ahead of 'now': never underflow.
uint32_t ageSeconds(uint8_t reg, uint32_t nowMs) {
  uint32_t u = updated[reg];
  return nowMs >= u ? (nowMs - u) / 1000 : 0;
}
const Stats &stats() { return st; }

void mode(char *buf, size_t n) {
  uint8_t v = 0;
  bool fromCmd = known(144);
  if (fromCmd) v = val[144];
  else if (known(27)) v = val[27] & ~32;       // the status flags have no auto bit
  else { snprintf(buf, n, "unknown"); return; }
  size_t len = 0;
  buf[0] = '\0';
  const struct { uint8_t bit; const char *name; } parts[] = {{2, "heat"}, {4, "cool"}, {32, "auto"}, {16, "tank"}};
  for (const auto &p : parts) {
    if (!(v & p.bit)) continue;
    len += snprintf(buf + len, n - len, "%s%s", len ? "+" : "", p.name);
  }
  if (!len) snprintf(buf, n, "unknown");
}

static void emit(EventFn cb, const Event &e) { if (cb) cb(e); }

// Store a value and report NEW / CHANGED.
static void store(uint8_t reg, uint8_t v, uint32_t ts, uint16_t latency, EventFn cb) {
  Event e = {};
  e.ts_ms = ts; e.reg = reg; e.latencyMs = latency;
  bool had = known(reg);
  bool changed = had && val[reg] != v;
  if (!had || changed) {
    if (had) textWith(reg, reg, val[reg], e.oldText, sizeof(e.oldText));
    textWith(reg, reg, v, e.newText, sizeof(e.newText));
    e.kind = had ? EV_CHANGED : EV_NEW;
  }
  val[reg] = v;
  updated[reg] = millis() | 1;     // never 0
  if (!had || changed) emit(cb, e);
}

static void handleRequest(const uint8_t *d, uint32_t ts, EventFn cb) {
  if (pending.active) {            // previous request was never answered
    st.noreply++;
    Event e = {};
    e.ts_ms = ts; e.kind = EV_NOREPLY; e.reg = pending.reg;
    emit(cb, e);
  }
  pending.active = true;
  pending.reg = d[1];
  pending.val = d[2];
  pending.ts = ts;
}

static void handleReply(const uint8_t *d, uint32_t ts, EventFn cb) {
  uint8_t reg = d[1], v = d[2];

  if (reg == 255 && v == 0) {      // "no data" answer
    st.naks++;
    Event e = {};
    e.ts_ms = ts; e.kind = EV_NAK; e.reg = pending.active ? pending.reg : 255;
    emit(cb, e);
    pending.active = false;
    return;
  }

  uint32_t lat = pending.active ? ts - pending.ts : 0;
  uint16_t latency = lat > 65535 ? 65535 : (uint16_t)lat;

  if (!pending.active || pending.reg != reg) {
    st.unmatched++;
    Event e = {};
    e.ts_ms = ts; e.kind = EV_UNMATCHED; e.reg = reg; e.otherReg = pending.active ? pending.reg : 0;
    emit(cb, e);
    if (!writeOnly(reg)) store(reg, v, ts, 0, cb);   // the data itself is still valid
    pending.active = false;
    return;
  }

  st.txn++;
  if (pending.val != 0) {          // panel wrote a value; HP echoes what it stored
    st.sets++;
    Event e = {};
    e.ts_ms = ts; e.kind = EV_SET; e.reg = reg; e.latencyMs = latency;
    e.mismatch = (v != pending.val);
    if (known(reg)) textWith(reg, reg, val[reg], e.oldText, sizeof(e.oldText));
    else snprintf(e.oldText, sizeof(e.oldText), "?");
    textWith(reg, reg, v, e.newText, sizeof(e.newText));
    if (e.mismatch) {              // oldText becomes "what the panel asked for"
      textWith(reg, reg, pending.val, e.oldText, sizeof(e.oldText));
    }
    val[reg] = v;
    updated[reg] = millis() | 1;
    emit(cb, e);
  } else if (!writeOnly(reg)) {
    store(reg, v, ts, latency, cb);
  }
  pending.active = false;
}

void feed(const Sniffer::Frame &f, EventFn cb) {
  size_t chunks = Protocol::validChunks(f.data, f.len);
  for (size_t c = 0; c < chunks; c++) {
    const uint8_t *d = f.data + c * Protocol::FRAME_LEN;
    if (d[0] == Protocol::START_REQ) handleRequest(d, f.ts_ms, cb);
    else handleReply(d, f.ts_ms, cb);
  }
}

}  // namespace Registers
