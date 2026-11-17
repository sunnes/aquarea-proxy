#pragma once
#include <Arduino.h>

// Whitelisted heat pump commands, built on Sniffer request injection.
// Only the commands below exist; there is deliberately no "write any register".
namespace Control {

struct Result {
  bool ok;
  const char *message;   // short explanation (error or note)
  uint8_t reg;
  uint8_t sent;          // value written (0 for a read)
  uint8_t reply;         // value the heat pump answered
  bool refused;          // a safety rule blocked the command; nothing was sent
};

Result readRegister(uint8_t reg);   // same request the panel sends (144 and 129 always read 0)
// Timing guards from the service manual (WH-SDC03/05E3E5, sections 13.1.1 and 16.3):
//  - the compressor does not restart for 3 minutes after it stops, so power ON within 3 minutes of
//    our own power OFF, and mode changes while running closer than 3 minutes apart, are refused;
//  - protection errors (F16, F20, F23, ...) lock out only after repeating N times within 20-30
//    minutes, and a reset clears that count, so resets are limited to one per 30 minutes
//    (H76, the panel link error this proxy can itself cause, is exempt). Power OFF is never refused.
Result setPower(bool on);           // reg 144 bit 0; write-only, other bits from the panel's last write
Result resetError();                // reg 129 = 2, as the panel does

// Operating mode = register 144 bits (bit 0 stays the power bit). Observed on the panel:
//   heat 2, cool 4, auto 32 (name inferred), tank 16, heat+tank 18, cool+tank 20, auto+tank 48
bool modeBits(const char *name, uint8_t &bits);   // false if not one of the observed modes
Result setMode(uint8_t bits);                     // keeps the current power state

// Temperature settings (registers 130..138, signed degrees C, the same bytes the panel writes).
// Whitelisted by name; the limits are the control panel ranges from the service manual. The value
// 0 cannot be written because the protocol treats it as a query, so 0 is rejected unless the
// setting is zeroIsWrite. The heating curve must keep its shape: low_outdoor < high_outdoor and
// flow_low >= flow_high (checked against the other point's last known value).
// zeroIsWrite: on this register a request with value 0 is a write of 0, not a query (observed:
// a plain read of 138 reset the heat curve shift to 0). Such a register is never read.
struct Setting { const char *key; uint8_t reg; int8_t min, max; bool zeroIsWrite; };
const Setting *findSetting(const char *key);      // by key (e.g. "tank"), nullptr if unknown
const Setting *settingAt(size_t i);               // iterate; nullptr past the end
Result setSetting(const Setting &s, int value);   // range- and curve-checked write, HP echo verified

// register numbers (verified from panel captures)
const uint8_t REG_POWER = 144;
const uint8_t REG_RESET_ERROR = 129;
const uint8_t REG_STATUS = 27;       // read-only status flags (bit 0 on, 2 heat, 4 cool, 16 tank)

}  // namespace Control
