#pragma once
#include <Arduino.h>
#include <SoftwareSerial.h>

// Aquarea bus proxy/sniffer: forwards CP <-> HP and records the frames.
// Channels: 0 = CP side, 1 = HP side (pins below).
// Fixed line settings of the Aquarea remote link: 960 baud, 8E1, inverted logic.
namespace Sniffer {

const uint8_t  PIN_RX_CP = 13;
const uint8_t  PIN_RX_HP = 5;
const uint8_t  PIN_TX_HP = 4;    // TX line, used for forwarding
const uint8_t  PIN_TX_CP = 12;   // TX line, used for forwarding
const uint32_t LINE_BAUD = 960;
const bool     LINE_INVERT = true;      // 8E1 is set in sniffer.cpp
const char    *const LINE_DESC = "960 8E1 inverted";
const size_t   MAX_FRAME = 64;
const size_t   RING_SIZE = 32;

struct Frame {
  uint32_t ts_ms;
  uint8_t  ch;       // 0 = CP line, 1 = HP line
  uint8_t  len;
  uint8_t  data[MAX_FRAME];
};

struct Stats {
  uint32_t bytes[2];
  uint32_t frames[2];      // gap-delimited chunks
  uint32_t valid[2];       // valid 4-byte protocol frames (checksum ok)
  uint32_t wrongDir[2];    // valid frame whose start byte doesn't match the line label
  uint32_t overflow[2];
  uint32_t fwd[2];         // bytes forwarded: [0] CP->HP, [1] HP->CP
  uint32_t dropped;
};

void begin();
void poll();                       // call every loop(), never blocks
bool pop(Frame &f);
const Stats &stats();

// Request injection: the ESP sends its own request to the heat pump. It is sent right
// after an HP reply completes (bus idle); the CP's next request is held back until the
// HP has answered ours, and the HP's answer is NOT forwarded to the CP.
enum InjectState : uint8_t {
  INJ_IDLE, INJ_QUEUED, INJ_AWAIT,        // working states
  INJ_OK, INJ_MISMATCH, INJ_TIMEOUT, INJ_NOSLOT   // results
};
struct InjectStatus {
  InjectState state;
  uint8_t reg, sent, reply;               // register, value sent (0 = read), value HP answered
  uint32_t doneMs;
};
bool injectRequest(uint8_t reg, uint8_t val);   // false if busy or forwarding is off
bool injectBusy();                              // QUEUED or AWAIT
InjectStatus injectStatus();

// Transparent forwarding CP <-> HP (on by default). Off = passive listen only.
void setForwarding(bool on);
bool forwarding();

}  // namespace Sniffer
