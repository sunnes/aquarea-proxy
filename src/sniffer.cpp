#include "sniffer.h"
#include "protocol.h"

namespace Sniffer {

static SoftwareSerial rxSerial[2];
static const uint8_t rxPins[2] = {PIN_RX_CP, PIN_RX_HP};
static const uint8_t txPins[2] = {PIN_TX_CP, PIN_TX_HP};  // txPins[i] writes back toward side i
static bool fwdOn = true;

// one character = 11 bits (start + 8 data + parity + stop); frame gap = 2.5 characters
static const uint32_t gapUs = (uint32_t)(11000000ULL / LINE_BAUD * 5 / 2);

static Frame ring[RING_SIZE];
static size_t ringHead = 0, ringTail = 0;

static Frame cur[2];
static uint32_t lastByteUs[2];
static Stats st;

static void startPorts() {
  for (int i = 0; i < 2; i++) {
    rxSerial[i].end();
    rxSerial[i].begin(LINE_BAUD, SWSERIAL_8E1, rxPins[i], txPins[i], LINE_INVERT, 256, 256);
    cur[i].len = 0;
  }
}

void begin() {
  startPorts();
}

// Cut-through forwarding: re-send one byte out of the opposite side as soon as it
// arrives (adds ~1 byte time, ~11 ms at 960 baud, instead of a whole frame each way).
// write() blocks for that byte, but RX on both ports is interrupt driven and keeps buffering.
static void forwardByte(int ch, uint8_t b) {
  if (!fwdOn) return;
  rxSerial[1 - ch].write(b);
  st.fwd[ch]++;
}

static void ringPush(const Frame &f) {
  size_t next = (ringHead + 1) % RING_SIZE;
  if (next == ringTail) st.dropped++;
  else { ring[ringHead] = f; ringHead = next; }
}

static void pushFrame(int ch) {
  Frame &c = cur[ch];
  if (c.len == 0) return;
  size_t chunks = Protocol::validChunks(c.data, c.len);
  if (chunks) {
    st.valid[ch] += chunks;
    uint8_t expected = (ch == 0) ? Protocol::START_REQ : Protocol::START_RSP;
    if (c.data[0] != expected) st.wrongDir[ch] += chunks;
  }
  ringPush(c);
  st.frames[ch]++;
  c.len = 0;
}

// ---- request injection ------------------------------------------------------
static struct {
  InjectState state;
  uint8_t reg, val, reply;
  uint32_t queuedMs, t0, doneMs;
  uint8_t replyBuf[4], replyLen;
  uint8_t hold[16], holdLen;       // CP bytes received while we wait for the HP
} inj;

static void maybeStartInject();

// One received byte through the normal path: record, forward, close frames.
static void processByte(int ch, uint8_t b) {
  Frame &c = cur[ch];
  if (c.len >= MAX_FRAME) pushFrame(ch);   // oversize: flush, keep going
  if (c.len == 0) { c.ts_ms = millis(); c.ch = ch; }
  c.data[c.len++] = b;
  forwardByte(ch, b);
  lastByteUs[ch] = micros();
  st.bytes[ch]++;
  // complete valid protocol frame: close it now instead of waiting for the idle gap
  if (c.len == Protocol::FRAME_LEN && Protocol::validFrame(c.data)) {
    pushFrame(ch);
    if (ch == 1) maybeStartInject();       // HP reply just finished: the bus is idle
  }
}

static void pushSynthetic(int ch, const uint8_t *d) {
  Frame f = {};
  f.ts_ms = millis(); f.ch = ch; f.len = Protocol::FRAME_LEN;
  memcpy(f.data, d, Protocol::FRAME_LEN);
  ringPush(f);
}

static void finishInject(InjectState result) {
  inj.state = result;
  inj.doneMs = millis();
  uint8_t n = inj.holdLen;
  inj.holdLen = 0;
  for (uint8_t i = 0; i < n; i++) processByte(0, inj.hold[i]);   // release the CP's request
}

static void maybeStartInject() {
  if (inj.state != INJ_QUEUED || !fwdOn) return;
  // CP already started its next request: wait for the next idle slot.
  if (cur[0].len != 0 || rxSerial[0].available() > 0) return;
  uint8_t f[4] = {Protocol::START_REQ, inj.reg, inj.val, (uint8_t)(Protocol::START_REQ + inj.reg + inj.val)};
  rxSerial[1].write(f, 4);                 // toward the heat pump
  pushSynthetic(0, f);                     // so Registers sees the request
  inj.state = INJ_AWAIT;
  inj.t0 = millis();
  inj.replyLen = 0;
  inj.holdLen = 0;
}

// Byte received while an injected request is in flight.
static void injectByte(int ch, uint8_t b) {
  st.bytes[ch]++;
  if (ch == 0) {                           // CP is talking: hold its bytes back
    if (inj.holdLen < sizeof(inj.hold)) inj.hold[inj.holdLen++] = b;
    return;
  }
  if (inj.replyLen < sizeof(inj.replyBuf)) inj.replyBuf[inj.replyLen++] = b;
  if (inj.replyLen < Protocol::FRAME_LEN) return;
  bool good = Protocol::validFrame(inj.replyBuf) && inj.replyBuf[0] == Protocol::START_RSP &&
              inj.replyBuf[1] == inj.reg;
  inj.reply = inj.replyBuf[2];
  if (good) pushSynthetic(1, inj.replyBuf);
  finishInject(good && (inj.val == 0 || inj.reply == inj.val) ? INJ_OK : INJ_MISMATCH);
}

bool injectRequest(uint8_t reg, uint8_t val) {
  if (!fwdOn || inj.state == INJ_QUEUED || inj.state == INJ_AWAIT) return false;
  inj.reg = reg; inj.val = val; inj.reply = 0;
  inj.queuedMs = millis();
  inj.state = INJ_QUEUED;
  return true;
}
bool injectBusy() { return inj.state == INJ_QUEUED || inj.state == INJ_AWAIT; }
InjectStatus injectStatus() { return {inj.state, inj.reg, inj.val, inj.reply, inj.doneMs}; }

void poll() {
  if (inj.state == INJ_AWAIT && millis() - inj.t0 > 300) finishInject(INJ_TIMEOUT);
  else if (inj.state == INJ_QUEUED && millis() - inj.queuedMs > 5000) {
    inj.state = INJ_NOSLOT;                // the CP never left an idle slot
    inj.doneMs = millis();
  }
  uint32_t nowUs = micros();
  for (int ch = 0; ch < 2; ch++) {
    SoftwareSerial &s = rxSerial[ch];
    if (s.overflow()) st.overflow[ch]++;
    while (s.available() > 0) {
      int b = s.read();
      if (b < 0) break;
      if (inj.state == INJ_AWAIT) injectByte(ch, (uint8_t)b);
      else processByte(ch, (uint8_t)b);
    }
    nowUs = micros();
    if (cur[ch].len > 0 && (uint32_t)(nowUs - lastByteUs[ch]) > gapUs) pushFrame(ch);
  }
}

bool pop(Frame &f) {
  if (ringTail == ringHead) return false;
  f = ring[ringTail];
  ringTail = (ringTail + 1) % RING_SIZE;
  return true;
}

const Stats &stats() { return st; }

void setForwarding(bool on) { fwdOn = on; }
bool forwarding() { return fwdOn; }

}  // namespace Sniffer
