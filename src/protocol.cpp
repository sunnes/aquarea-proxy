#include "protocol.h"

namespace Protocol {

bool validFrame(const uint8_t *d) {
  if (d[0] != START_REQ && d[0] != START_RSP) return false;
  return d[3] == (uint8_t)(d[0] + d[1] + d[2]);
}

size_t validChunks(const uint8_t *d, size_t len) {
  if (len < FRAME_LEN || len % FRAME_LEN) return 0;
  for (size_t i = 0; i < len; i += FRAME_LEN)
    if (!validFrame(d + i)) return 0;
  return len / FRAME_LEN;
}

const char *regName(uint8_t reg) {
  switch (reg) {
    case 17: return "defrost";
    case 18: return "outdoor temp";
    case 19: return "flow temp";
    case 20: return "current error";
    case 21: return "return temp";
    case 22: return "tank temp";
    case 23: return "compressor freq";
    case 24: return "last error";
    case 27: return "status flags";
    case 29: return "heat power lo";
    case 30: return "heat power hi";
    case 31: return "cool power lo";
    case 32: return "cool power hi";
    case 33: return "tank power lo";
    case 34: return "tank power hi";
    case 35: return "pump speed";
    case 129: return "reset error";
    case 130: return "low outdoor temp set";
    case 131: return "high outdoor temp set";
    case 132: return "flow temp @low outdoor";
    case 133: return "flow temp @high outdoor";
    case 134: return "heating off outdoor temp";
    case 135: return "backup heater on temp";
    case 136: return "cool set";
    case 137: return "tank temp set";
    case 138: return "heat curve shift";
    case 144: return "power control";
    case 177: return "cool enable";
    case 192: return "config flags";
    case 193: return "heatup time heat/cool";
    case 194: return "heatup time tank";
    case 196: return "disinfect temp";
    case 197: return "disinfect duration";
    case 198: return "config flags 2";
    case 255: return "no-data reply (NAK?)";
    default: return nullptr;
  }
}

int describe(uint8_t reg, uint8_t val, char *buf, size_t n) {
  switch (reg) {
    case 18: case 19: case 21: case 22:
    case 130: case 131: case 132: case 133: case 134: case 135: case 136: case 137: case 196:
      return snprintf(buf, n, "%d C", (int8_t)val);
    case 17:
      return snprintf(buf, n, "%s", val == 64 ? "defrosting" : (val == 0 ? "no" : "?"));
    case 20: case 24:
      if (val == 0) return snprintf(buf, n, "none");
      if (val == 38) return snprintf(buf, n, "H72 tank sensor");
      if (val == 156) return snprintf(buf, n, "H76 remote comm");
      return snprintf(buf, n, "code %u", val);
    case 23:
      return snprintf(buf, n, "%u Hz", val);
    case 27:   // status flags: bit 0 on, 2 heat, 4 cool, 16 tank (bit 5 unknown)
      return snprintf(buf, n, "%s%s%s%s%s (raw %u)", (val & 1) ? "ON" : "OFF",
                      (val & 2) ? " heat" : "", (val & 4) ? " cool" : "",
                      (val & 16) ? " tank" : "", (val & 64) ? " quiet" : "", val);
    case 35:
      return snprintf(buf, n, "level %u", val >> 4);
    case 144:  // power/mode command: bit 0 power, 2 heat, 4 cool, 16 tank, 32 auto (inferred)
      return snprintf(buf, n, "%s%s%s%s%s (raw %u)", (val & 1) ? "ON" : "OFF",
                      (val & 2) ? " heat" : "", (val & 4) ? " cool" : "",
                      (val & 32) ? " auto" : "", (val & 16) ? " tank" : "", val);
    default:
      return snprintf(buf, n, "%u", val);
  }
}

}  // namespace Protocol
