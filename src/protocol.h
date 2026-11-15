#pragma once
#include <Arduino.h>

// Panasonic Aquarea remote <-> heat pump link (4-byte frames).
//   Remote -> HP : 0xAA, register, value (0 = query, else write), checksum
//   HP -> Remote : 0x55, register, value,                        checksum
// checksum = (b0 + b1 + b2) & 0xFF
// Source: github.com/v-s-c-o-p-e/geisha_aquarea_panasonic_arduino_esp8266_proxy
namespace Protocol {

const uint8_t START_REQ = 0xAA;
const uint8_t START_RSP = 0x55;
const size_t  FRAME_LEN = 4;

bool validFrame(const uint8_t *d);                    // start byte + checksum
size_t validChunks(const uint8_t *d, size_t len);     // len/4 if all chunks valid, else 0
const char *regName(uint8_t reg);                     // nullptr if unknown
// Human readable value, e.g. "8 C" or "heat,on". Returns chars written.
int describe(uint8_t reg, uint8_t val, char *buf, size_t n);

}  // namespace Protocol
