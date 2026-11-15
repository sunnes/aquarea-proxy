#include "telnet_log.h"
#include "config.h"
#include <ESP8266WiFi.h>
#include "sniffer.h"
#include "protocol.h"
#include "registers.h"

namespace TelnetLog {

static WiFiServer server(23);
static WiFiClient client;
static uint32_t droppedLines = 0;
static bool showInvalid = true;
static bool verbose = false;      // raw frame log in addition to decoded events
static int  dumpNext = -1;        // register dump in progress (next reg number), -1 = idle

static const char *HELP =
  "Commands (single key):\r\n"
  "  s  stats                     d  dump all known registers\r\n"
  "  v  raw frame log on/off      r  show/hide BAD frames\r\n"
  "  f  toggle forwarding CP<->HP (default ON)\r\n"
  "  ?  this help\r\n";

static const uint32_t HEARTBEAT_MS = 10000;
static uint32_t lastHeartbeat = 0;

// snprintf returns the length it WOULD have written; never trust it as a byte count.
static int clampLen(int n, size_t size) {
  if (n < 0) return 0;
  return (size_t)n >= size ? (int)size - 1 : n;
}

static void out(const char *buf, int n) {
  if (client.availableForWrite() >= n) client.write((const uint8_t *)buf, n);
  else droppedLines++;
}

static void say(const char *msg) {
  char buf[200];
  int n = snprintf(buf, sizeof(buf), "%s\r\n", msg);
  out(buf, n);
}

static void printStats() {
  const Sniffer::Stats &s = Sniffer::stats();
  static char buf[512];
  int n = snprintf(buf, sizeof(buf),
    "-- line: %s\r\n"
    "-- CP(G%u): bytes=%lu chunks=%lu valid=%lu wrongdir=%lu ovf=%lu\r\n"
    "-- HP(G%u):  bytes=%lu chunks=%lu valid=%lu wrongdir=%lu ovf=%lu\r\n"
    "-- forwarding=%s CP>HP=%lu bytes HP>CP=%lu bytes | ring_dropped=%lu telnet_dropped=%lu\r\n"
    "-- decode: txn=%lu sets=%lu nak=%lu noreply=%lu unmatched=%lu\r\n",
    Sniffer::LINE_DESC,
    Sniffer::PIN_RX_CP,
    (unsigned long)s.bytes[0], (unsigned long)s.frames[0], (unsigned long)s.valid[0],
    (unsigned long)s.wrongDir[0], (unsigned long)s.overflow[0],
    Sniffer::PIN_RX_HP,
    (unsigned long)s.bytes[1], (unsigned long)s.frames[1], (unsigned long)s.valid[1],
    (unsigned long)s.wrongDir[1], (unsigned long)s.overflow[1],
    Sniffer::forwarding() ? "ON" : "OFF", (unsigned long)s.fwd[0], (unsigned long)s.fwd[1],
    (unsigned long)s.dropped, (unsigned long)droppedLines,
    (unsigned long)Registers::stats().txn, (unsigned long)Registers::stats().sets,
    (unsigned long)Registers::stats().naks, (unsigned long)Registers::stats().noreply,
    (unsigned long)Registers::stats().unmatched);
  out(buf, clampLen(n, sizeof(buf)));
}

static void handleInput() {
  while (client.available() > 0) {
    switch (client.read()) {
      case 's': printStats(); break;
      case 'f': Sniffer::setForwarding(!Sniffer::forwarding()); say(Sniffer::forwarding() ? "-- forwarding ON" : "-- forwarding OFF (passive listen)"); break;
      case 'd': dumpNext = 0; break;
      case 'v': verbose = !verbose; say(verbose ? "-- raw frame log: ON" : "-- raw frame log: OFF (decoded events only)"); break;
      case 'r': showInvalid = !showInvalid; say(showInvalid ? "-- invalid frames: shown" : "-- invalid frames: hidden"); break;
      case '?': out(HELP, strlen(HELP)); break;
      default: break;
    }
  }
}

static void printHeader() {
  static char buf[1200];
  int n = snprintf(buf, sizeof(buf),
    "\r\n=====================================================\r\n"
    " Aquarea bus proxy/sniffer (forwarding CP<->HP: %s)\r\n"
    " v%s build %s %s | up %lus | wifi %s rssi %d dBm\r\n"
    " line: %s | CP rx=G%u  HP rx=G%u\r\n"
    "-----------------------------------------------------\r\n"
    " Decoded events (only changes are printed, 'd' dumps everything):\r\n"
    "  [sec] #reg name = value           first time a register is seen\r\n"
    "  [sec] #reg name: old -> new       value changed\r\n"
    "  [sec] SET #reg name = v (was x)   panel wrote a value, HP acknowledged\r\n"
    "  [sec] !! ...                      anomaly: no reply / no-data reply / mismatch\r\n"
    "  [sec] G<pin> BAD len=N: <hex>     bytes failing checksum\r\n"
    "  -- hb ...                         heartbeat every 10 s (link alive)\r\n"
    "-----------------------------------------------------\r\n",
    Sniffer::forwarding() ? "ON" : "OFF",
    AQUAREA_VERSION, __DATE__, __TIME__, (unsigned long)(millis() / 1000),
    WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "AP mode",
    WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
    Sniffer::LINE_DESC, Sniffer::PIN_RX_CP, Sniffer::PIN_RX_HP);
  client.write((const uint8_t *)buf, clampLen(n, sizeof(buf)));
  client.print(HELP);
  client.print("=====================================================\r\n");
  printStats();
}

static void heartbeat() {
  const Sniffer::Stats &s = Sniffer::stats();
  char buf[160];
  int n = snprintf(buf, sizeof(buf),
    "-- hb up=%lus CP bytes=%lu valid=%lu | HP bytes=%lu valid=%lu\r\n",
    (unsigned long)(millis() / 1000),
    (unsigned long)s.bytes[0], (unsigned long)s.valid[0],
    (unsigned long)s.bytes[1], (unsigned long)s.valid[1]);
  out(buf, n);
}

static void onEvent(const Registers::Event &e) {
  if (!(client && client.connected())) return;
  char line[200];
  int n = 0;
  unsigned long sec = e.ts_ms / 1000, ms = e.ts_ms % 1000;
  n += snprintf(line, sizeof(line), "[%lu.%03lu] ", sec, ms);
  switch (e.kind) {
    case Registers::EV_NEW:
      n += snprintf(line + n, sizeof(line) - n, "#%u %s = %s", e.reg, Registers::name(e.reg), e.newText);
      break;
    case Registers::EV_CHANGED:
      n += snprintf(line + n, sizeof(line) - n, "#%u %s: %s -> %s", e.reg, Registers::name(e.reg), e.oldText, e.newText);
      break;
    case Registers::EV_SET:
      if (e.mismatch)
        n += snprintf(line + n, sizeof(line) - n, "!! SET #%u %s: panel wrote %s but HP answered %s",
                      e.reg, Registers::name(e.reg), e.oldText, e.newText);
      else
        n += snprintf(line + n, sizeof(line) - n, "SET #%u %s = %s (was %s) ack %ums",
                      e.reg, Registers::name(e.reg), e.newText, e.oldText, e.latencyMs);
      break;
    case Registers::EV_NAK:
      n += snprintf(line + n, sizeof(line) - n, "!! HP sent a no-data reply (55 FF 00) to the request for #%u %s",
                    e.reg, Registers::name(e.reg));
      break;
    case Registers::EV_NOREPLY:
      n += snprintf(line + n, sizeof(line) - n, "!! no reply from HP to the request for #%u %s",
                    e.reg, Registers::name(e.reg));
      break;
    case Registers::EV_UNMATCHED:
      n += snprintf(line + n, sizeof(line) - n, "!! HP answered #%u %s but the pending request was #%u",
                    e.reg, Registers::name(e.reg), e.otherReg);
      break;
  }
  n = clampLen(n, sizeof(line) - 2);
  line[n++] = '\r'; line[n++] = '\n';
  out(line, n);
}

// Print the register table a few lines per poll so a slow client never blocks us.
static void pumpDump() {
  while (dumpNext >= 0) {
    if (client.availableForWrite() < 120) return;      // continue on the next poll
    if (dumpNext > 255) {
      say("-- end of register dump");
      dumpNext = -1;
      return;
    }
    uint8_t r = (uint8_t)dumpNext++;
    if (!Registers::known(r) || Registers::isHighByte(r)) continue;
    char v[32], line[120];
    Registers::text(r, v, sizeof(v));
    int n = snprintf(line, sizeof(line), "  #%-3u %-24s = %-14s (raw %3u, %lus ago)\r\n", r, Registers::name(r), v,
                     Registers::value(r), (unsigned long)Registers::ageSeconds(r, millis()));
    out(line, clampLen(n, sizeof(line)));
  }
}

static void sendFrame(const Sniffer::Frame &f) {
  static char line[2600];   // worst case: 16 decoded chunks of ~150 chars
  int n = 0;
  unsigned long sec = f.ts_ms / 1000, ms = f.ts_ms % 1000;
  char label[8];
  snprintf(label, sizeof(label), "G%-2u", f.ch == 0 ? Sniffer::PIN_RX_CP : Sniffer::PIN_RX_HP);

  size_t chunks = Protocol::validChunks(f.data, f.len);
  if (chunks) {
    for (size_t c = 0; c < chunks; c++) {
      const uint8_t *d = f.data + c * Protocol::FRAME_LEN;
      bool req = d[0] == Protocol::START_REQ;
      const char *name = Protocol::regName(d[1]);
      char val[40];
      Protocol::describe(d[1], d[2], val, sizeof(val));
      if (req && d[2] == 0)
        n += snprintf(line + n, sizeof(line) - n, "[%lu.%03lu] %s CP>HP read  reg %3u %-22s",
                      sec, ms, label, d[1], name ? name : "?");
      else if (req)
        n += snprintf(line + n, sizeof(line) - n, "[%lu.%03lu] %s CP>HP WRITE reg %3u %-22s = %u (%s)",
                      sec, ms, label, d[1], name ? name : "?", d[2], val);
      else
        n += snprintf(line + n, sizeof(line) - n, "[%lu.%03lu] %s HP>CP value reg %3u %-22s = %u (%s)",
                      sec, ms, label, d[1], name ? name : "?", d[2], val);
      n += snprintf(line + n, sizeof(line) - n, "  [%02X %02X %02X %02X]\r\n", d[0], d[1], d[2], d[3]);
    }
  } else if (showInvalid) {
    n += snprintf(line + n, sizeof(line) - n, "[%lu.%03lu] %s BAD len=%u:", sec, ms, label, f.len);
    for (uint8_t i = 0; i < f.len; i++) n += snprintf(line + n, sizeof(line) - n, " %02X", f.data[i]);
    n += snprintf(line + n, sizeof(line) - n, "\r\n");
  }
  n = clampLen(n, sizeof(line));
  if (n > 0) out(line, n);
}

void begin() {
  server.begin();
  server.setNoDelay(true);
}

void poll() {
  if (server.hasClient()) {
    WiFiClient fresh = server.accept();
    if (client && client.connected()) client.stop();   // newest client wins
    client = fresh;
    client.setNoDelay(true);
    lastHeartbeat = millis();
    printHeader();
  }
  bool connected = client && client.connected();
  if (connected) handleInput();
  if (connected && millis() - lastHeartbeat >= HEARTBEAT_MS) {
    lastHeartbeat = millis();
    heartbeat();
  }

  Sniffer::Frame f;
  while (Sniffer::pop(f)) {
    Registers::feed(f, onEvent);              // always: keeps the register table current
    if (!connected) continue;
    bool valid = Protocol::validChunks(f.data, f.len) > 0;
    if (verbose || (!valid && showInvalid)) sendFrame(f);
  }
  if (connected) pumpDump();
}

}  // namespace TelnetLog
