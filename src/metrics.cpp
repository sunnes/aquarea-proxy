#include "metrics.h"
#include <stdarg.h>
#include "registers.h"
#include "sniffer.h"

namespace Metrics {

namespace {

struct Out {
  char buf[1100];
  size_t len;
  Sink sink;
  void *ctx;

  void flush() {
    if (len) sink(buf, len, ctx);
    len = 0;
  }

  void line(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + len, sizeof(buf) - len, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(buf) - len) {      // did not fit: send what we have, retry on an empty buffer
      flush();
      va_start(ap, fmt);
      n = vsnprintf(buf, sizeof(buf), fmt, ap);
      va_end(ap);
      if (n < 0 || (size_t)n >= sizeof(buf)) return;
    }
    len += n;
  }

  void head(const char *name, const char *type, const char *help) {
    line("# HELP %s %s\n# TYPE %s %s\n", name, help, name, type);
  }
};

// label values: escape backslash, double quote and newline
void esc(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (; *in && o + 2 < n; in++) {
    if (*in == '\\' || *in == '"') out[o++] = '\\';
    if (*in == '\n') { out[o++] = '\\'; out[o++] = 'n'; continue; }
    out[o++] = *in;
  }
  out[o] = '\0';
}

bool reg(uint8_t r, uint8_t &v) {
  if (!Registers::known(r)) return false;
  v = Registers::value(r);
  return true;
}

// a gauge for one register; temperatures are signed bytes
void gauge(Out &o, const char *name, const char *help, uint8_t r, bool isSigned, int div = 1) {
  uint8_t v;
  if (!reg(r, v)) return;
  o.head(name, "gauge", help);
  o.line("%s %d\n", name, (isSigned ? (int)(int8_t)v : (int)v) / div);
}

}  // namespace

void render(const Env &e, Sink sink, void *ctx) {
  static Out o;
  o.len = 0; o.sink = sink; o.ctx = ctx;
  uint8_t v;

  // ---- the proxy itself ----
  o.head("aquarea_uptime_seconds", "gauge", "Seconds since the proxy booted.");
  o.line("aquarea_uptime_seconds %lu\n", (unsigned long)(e.nowMs / 1000));
  o.head("aquarea_free_heap_bytes", "gauge", "Free heap of the ESP8266.");
  o.line("aquarea_free_heap_bytes %lu\n", (unsigned long)e.freeHeap);
  if (e.rssi != 0) {
    o.head("aquarea_wifi_rssi_dbm", "gauge", "Wi-Fi signal strength.");
    o.line("aquarea_wifi_rssi_dbm %d\n", e.rssi);
  }
  o.head("aquarea_forwarding_enabled", "gauge", "1 if the proxy forwards traffic between panel and heat pump.");
  o.line("aquarea_forwarding_enabled %d\n", Sniffer::forwarding() ? 1 : 0);
  if (Registers::known(18)) {
    o.head("aquarea_data_age_seconds", "gauge", "Age of the freshest polled value (outdoor temperature). Large = no traffic.");
    o.line("aquarea_data_age_seconds %lu\n", (unsigned long)Registers::ageSeconds(18, e.nowMs));
  }

  // ---- heat pump state ----
  if (reg(27, v)) {
    o.head("aquarea_power_on", "gauge", "1 if the heat pump is switched on (status flags bit 0).");
    o.line("aquarea_power_on %d\n", v & 1);
    o.head("aquarea_status_flags", "gauge", "Raw status flags (register 27).");
    o.line("aquarea_status_flags %u\n", v);
  }
  char mode[24], modeEsc[48];
  Registers::mode(mode, sizeof(mode));
  esc(mode, modeEsc, sizeof(modeEsc));
  o.head("aquarea_mode_info", "gauge", "Operating mode, as a label (heat, cool, auto, tank and combinations).");
  o.line("aquarea_mode_info{mode=\"%s\"} 1\n", modeEsc);

  gauge(o, "aquarea_outdoor_temperature_celsius", "Outdoor temperature.", 18, true);
  gauge(o, "aquarea_flow_temperature_celsius", "Water flow (outlet) temperature.", 19, true);
  gauge(o, "aquarea_return_temperature_celsius", "Water return temperature.", 21, true);
  gauge(o, "aquarea_tank_temperature_celsius", "Hot water tank temperature.", 22, true);
  gauge(o, "aquarea_compressor_frequency_hertz", "Compressor frequency.", 23, false);
  if (reg(35, v)) {
    o.head("aquarea_pump_speed_level", "gauge", "Water pump speed level (raw value / 16).");
    o.line("aquarea_pump_speed_level %u\n", v >> 4);
  }
  if (reg(17, v)) {
    o.head("aquarea_defrost_active", "gauge", "1 while the heat pump is defrosting.");
    o.line("aquarea_defrost_active %d\n", v == 64 ? 1 : 0);
  }

  // power draw: low byte + high byte registers, in watts
  static const struct { uint8_t lo, hi; const char *circuit; } power[] = {
    {29, 30, "heating"}, {31, 32, "cooling"}, {33, 34, "tank"},
  };
  bool powerHead = false;
  for (const auto &p : power) {
    if (!Registers::known(p.lo) && !Registers::known(p.hi)) continue;
    if (!powerHead) {
      o.head("aquarea_power_watts", "gauge", "Electric power drawn, by circuit.");
      powerHead = true;
    }
    o.line("aquarea_power_watts{circuit=\"%s\"} %u\n", p.circuit,
           (unsigned)Registers::value(p.hi) * 256u + Registers::value(p.lo));
  }

  bool errHead = false;
  static const struct { uint8_t reg; const char *kind; } errs[] = {{20, "current"}, {24, "last"}};
  for (const auto &er : errs) {
    if (!reg(er.reg, v)) continue;
    if (!errHead) {
      o.head("aquarea_error_code", "gauge", "Heat pump error code (0 = none; 156 = H76, 38 = H72).");
      errHead = true;
    }
    o.line("aquarea_error_code{kind=\"%s\"} %u\n", er.kind, v);
  }

  // settings stored in the heat pump (read by the panel at startup)
  static const struct { uint8_t reg; const char *name; } settings[] = {
    {130, "low_outdoor_temp"}, {131, "high_outdoor_temp"}, {132, "flow_at_low_outdoor"},
    {133, "flow_at_high_outdoor"}, {134, "heating_off_outdoor_temp"}, {135, "backup_heater_on_temp"},
    {136, "cool_set"}, {137, "tank_target"}, {196, "disinfect_temp"},
  };
  bool setHead = false;
  for (const auto &s : settings) {
    if (!reg(s.reg, v)) continue;
    if (!setHead) {
      o.head("aquarea_setting_celsius", "gauge", "Temperature settings stored in the heat pump.");
      setHead = true;
    }
    o.line("aquarea_setting_celsius{setting=\"%s\"} %d\n", s.name, (int)(int8_t)v);
  }

  // every register seen so far, raw (also the way to watch the still-unknown ones)
  bool rawHead = false;
  for (int r = 0; r < 256; r++) {
    if (!Registers::known(r)) continue;
    if (!rawHead) {
      o.head("aquarea_register_raw", "gauge", "Last raw value of every register seen on the bus.");
      rawHead = true;
    }
    char name[48];
    esc(Registers::name(r), name, sizeof(name));
    o.line("aquarea_register_raw{register=\"%d\",name=\"%s\"} %u\n", r, name, Registers::value(r));
  }

  // ---- bus health (counters since boot) ----
  const Sniffer::Stats &s = Sniffer::stats();
  const Registers::Stats &rs = Registers::stats();
  o.head("aquarea_bus_bytes_total", "counter", "Bytes received per side.");
  o.line("aquarea_bus_bytes_total{side=\"panel\"} %lu\naquarea_bus_bytes_total{side=\"heatpump\"} %lu\n",
         (unsigned long)s.bytes[0], (unsigned long)s.bytes[1]);
  o.head("aquarea_bus_valid_frames_total", "counter", "Frames with a valid start byte and checksum, per side.");
  o.line("aquarea_bus_valid_frames_total{side=\"panel\"} %lu\naquarea_bus_valid_frames_total{side=\"heatpump\"} %lu\n",
         (unsigned long)s.valid[0], (unsigned long)s.valid[1]);
  o.head("aquarea_bus_forwarded_bytes_total", "counter", "Bytes forwarded between panel and heat pump.");
  o.line("aquarea_bus_forwarded_bytes_total{direction=\"panel_to_heatpump\"} %lu\n"
         "aquarea_bus_forwarded_bytes_total{direction=\"heatpump_to_panel\"} %lu\n",
         (unsigned long)s.fwd[0], (unsigned long)s.fwd[1]);
  o.head("aquarea_bus_serial_overflow_total", "counter", "Software serial buffer overflows, per side.");
  o.line("aquarea_bus_serial_overflow_total{side=\"panel\"} %lu\naquarea_bus_serial_overflow_total{side=\"heatpump\"} %lu\n",
         (unsigned long)s.overflow[0], (unsigned long)s.overflow[1]);
  o.head("aquarea_bus_dropped_frames_total", "counter", "Frames lost because the internal ring was full.");
  o.line("aquarea_bus_dropped_frames_total %lu\n", (unsigned long)s.dropped);
  o.head("aquarea_bus_transactions_total", "counter", "Completed request/reply pairs.");
  o.line("aquarea_bus_transactions_total %lu\n", (unsigned long)rs.txn);
  o.head("aquarea_bus_sets_total", "counter", "Write commands acknowledged by the heat pump (panel and proxy).");
  o.line("aquarea_bus_sets_total %lu\n", (unsigned long)rs.sets);
  o.head("aquarea_bus_naks_total", "counter", "No-data replies (55 FF 00) from the heat pump.");
  o.line("aquarea_bus_naks_total %lu\n", (unsigned long)rs.naks);
  o.head("aquarea_bus_noreply_total", "counter", "Requests that never got a reply.");
  o.line("aquarea_bus_noreply_total %lu\n", (unsigned long)rs.noreply);
  o.head("aquarea_bus_unmatched_total", "counter", "Replies that did not match the pending request.");
  o.line("aquarea_bus_unmatched_total %lu\n", (unsigned long)rs.unmatched);

  o.flush();
}

}  // namespace Metrics
