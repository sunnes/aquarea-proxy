#pragma once
#include <Arduino.h>
#include "sniffer.h"

// Register state tracker: pairs each CP request with the HP reply, keeps the last
// known value of every register and reports changes / anomalies as events.
namespace Registers {

enum Kind : uint8_t {
  EV_NEW,        // register seen for the first time
  EV_CHANGED,    // value differs from last known
  EV_SET,        // panel wrote a value (HP acknowledged)
  EV_NAK,        // HP replied "55 FF 00" (no data) to a request
  EV_NOREPLY,    // request got no answer before the next request
  EV_UNMATCHED   // HP reply doesn't belong to the pending request
};

struct Event {
  uint32_t ts_ms;
  Kind     kind;
  uint8_t  reg;
  uint8_t  otherReg;      // EV_UNMATCHED: the register that was requested
  uint16_t latencyMs;     // request -> reply (approximate)
  bool     mismatch;      // EV_SET: HP answered a different value than written
  char     oldText[32];   // previous value (or what was written for EV_SET mismatch)
  char     newText[32];
};

typedef void (*EventFn)(const Event &);

struct Stats {
  uint32_t txn;        // completed request/reply pairs
  uint32_t sets;
  uint32_t naks;
  uint32_t noreply;
  uint32_t unmatched;
};

void feed(const Sniffer::Frame &f, EventFn cb);   // call for every frame, cb may be null

bool known(uint8_t reg);
uint8_t value(uint8_t reg);
uint32_t updatedMs(uint8_t reg);          // millis() of last update, 0 = never
uint32_t ageSeconds(uint8_t reg, uint32_t nowMs);   // seconds since the last update (never negative)
const char *name(uint8_t reg);            // never null
void text(uint8_t reg, char *buf, size_t n);   // current value, decoded (e.g. "21 C", "120 W")
// Operating mode as text: "heat", "tank", "heat+tank", "cool", "auto", ... or "unknown".
// Taken from the last power/mode command (register 144, write-only, every write passes
// through this proxy); before any write was seen, from the status flags (register 27).
void mode(char *buf, size_t n);
bool isHighByte(uint8_t reg);             // 30/32/34: shown together with their low byte
const Stats &stats();

}  // namespace Registers
