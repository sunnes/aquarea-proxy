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
};

Result readRegister(uint8_t reg);   // same request the panel sends (144 and 129 always read 0)
Result setPower(bool on);           // reg 144 bit 0; write-only, other bits from the panel's last write
Result resetError();                // reg 129 = 2, as the panel does

// Operating mode = register 144 bits (bit 0 stays the power bit). Observed on the panel:
//   heat 2, cool 4, auto 32 (name inferred), tank 16, heat+tank 18, cool+tank 20, auto+tank 48
bool modeBits(const char *name, uint8_t &bits);   // false if not one of the observed modes
Result setMode(uint8_t bits);                     // keeps the current power state

// Temperature settings (registers 130..137, signed degrees C, the same bytes the panel writes).
// Whitelisted by name with conservative limits; the value 0 cannot be written because the
// protocol treats it as a query, so 0 is rejected unless the setting is zeroIsWrite.
// zeroIsWrite: on this register a request with value 0 is a write of 0, not a query (observed:
// a plain read of 138 reset the heat curve shift to 0). Such a register is never read.
struct Setting { const char *key; uint8_t reg; int8_t min, max; bool zeroIsWrite; };
const Setting *findSetting(const char *key);      // by key (e.g. "tank"), nullptr if unknown
const Setting *settingAt(size_t i);               // iterate; nullptr past the end
Result setSetting(const Setting &s, int value);   // range-checked write, HP echo verified

// register numbers (verified from panel captures)
const uint8_t REG_POWER = 144;
const uint8_t REG_RESET_ERROR = 129;
const uint8_t REG_STATUS = 27;       // read-only status flags (bit 0 on, 2 heat, 4 cool, 16 tank)

}  // namespace Control
