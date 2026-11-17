#include "api.h"
#include "config.h"
#include "control.h"
#include "metrics.h"
#include <ESP8266WiFi.h>
#include "registers.h"
#include "sniffer.h"

namespace Api {

static ESP8266WebServer *srv;

bool authorized() {
  if (srv->authenticate(API_USER, DEVICE_PASSWORD)) return true;
  srv->requestAuthentication();
  return false;
}

static const char *stateName(Sniffer::InjectState s) {
  switch (s) {
    case Sniffer::INJ_IDLE: return "idle";
    case Sniffer::INJ_QUEUED: return "queued";
    case Sniffer::INJ_AWAIT: return "waiting";
    case Sniffer::INJ_OK: return "ok";
    case Sniffer::INJ_MISMATCH: return "mismatch";
    case Sniffer::INJ_TIMEOUT: return "timeout";
    default: return "no_slot";
  }
}

static String modeName() {
  char m[24];
  Registers::mode(m, sizeof(m));
  return String(m);
}

static void sendResult(const Control::Result &r) {
  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"ok\":%s,\"refused\":%s,\"message\":\"%s\",\"reg\":%u,\"sent\":%u,\"reply\":%u}\n",
           r.ok ? "true" : "false", r.refused ? "true" : "false", r.message, r.reg, r.sent, r.reply);
  // 409: a safety rule refused the command and nothing was sent; 502: the bus exchange failed.
  srv->send(r.ok ? 200 : (r.refused ? 409 : 502), "application/json", buf);
}

static void handleState() {
  if (!authorized()) return;
  String j;
  j.reserve(4096);
  j += String("{\"version\":\"") + AQUAREA_VERSION + "\",\"uptime_s\":" + String(millis() / 1000);
  if (Registers::known(27)) j += String(",\"power\":\"") + ((Registers::value(27) & 1) ? "on" : "off") + "\"";
  j += ",\"mode\":\"" + modeName() + "\"";
  if (Registers::known(27)) j += String(",\"flags\":") + Registers::value(27);
  j += ",\"forwarding\":";
  j += Sniffer::forwarding() ? "true" : "false";
  Sniffer::InjectStatus st = Sniffer::injectStatus();
  j += String(",\"control\":{\"state\":\"") + stateName(st.state) + "\",\"reg\":" + st.reg +
       ",\"sent\":" + st.sent + ",\"reply\":" + st.reply + "}";
  const Registers::Stats &rs = Registers::stats();
  j += String(",\"stats\":{\"txn\":") + rs.txn + ",\"sets\":" + rs.sets + ",\"naks\":" + rs.naks +
       ",\"noreply\":" + rs.noreply + ",\"unmatched\":" + rs.unmatched + "}";
  j += ",\"registers\":{";
  bool first = true;
  for (int r = 0; r < 256; r++) {
    if (!Registers::known(r)) continue;
    char text[32];
    Registers::text(r, text, sizeof(text));
    if (!first) j += ",";
    first = false;
    j += String("\"") + r + "\":{\"name\":\"" + Registers::name(r) + "\",\"raw\":" + Registers::value(r) +
         ",\"text\":\"" + text + "\",\"age_s\":" + Registers::ageSeconds(r, millis());
    for (size_t i = 0; const Control::Setting *st = Control::settingAt(i); i++)
      if (st->reg == r) j += String(",\"set_key\":\"") + st->key + "\"";     // usable with /api/set?k=
    j += "}";
  }
  j += "}}\n";
  srv->send(200, "application/json", j);
}

// Prometheus scrape endpoint. The text is sent in chunks and the bus is serviced between
// chunks, so even a slow scraper cannot delay the forwarding.
static void metricsSink(const char *data, size_t len, void *) {
  srv->sendContent(data, len);
  Sniffer::poll();
}

static void handleMetrics() {
  if (!authorized()) return;
  Metrics::Env env = {millis(), ESP.getFreeHeap(), WiFi.status() == WL_CONNECTED ? (int)WiFi.RSSI() : 0};
  srv->setContentLength(CONTENT_LENGTH_UNKNOWN);
  srv->send(200, "text/plain; version=0.0.4; charset=utf-8", "");
  Metrics::render(env, metricsSink, nullptr);
  srv->sendContent("");     // end of the chunked body
}

static void handleRead() {
  if (!authorized()) return;
  if (!srv->hasArg("reg")) { srv->send(400, "application/json", "{\"ok\":false,\"message\":\"reg required\"}\n"); return; }
  int reg = srv->arg("reg").toInt();
  if (reg < 0 || reg > 254) { srv->send(400, "application/json", "{\"ok\":false,\"message\":\"reg 0..254\"}\n"); return; }
  sendResult(Control::readRegister((uint8_t)reg));
}

static void handlePower() {
  if (!authorized()) return;
  if (srv->method() != HTTP_POST) { srv->send(405, "application/json", "{\"ok\":false,\"message\":\"use POST\"}\n"); return; }
  if (!srv->hasArg("on") || (srv->arg("on") != "0" && srv->arg("on") != "1")) {
    srv->send(400, "application/json", "{\"ok\":false,\"message\":\"on=1 or on=0 required\"}\n");
    return;
  }
  sendResult(Control::setPower(srv->arg("on") == "1"));
}

static void handleMode() {
  if (!authorized()) return;
  if (srv->method() != HTTP_POST) { srv->send(405, "application/json", "{\"ok\":false,\"message\":\"use POST\"}\n"); return; }
  String m = srv->arg("m");
  m.toLowerCase();
  m.replace(' ', '+');      // an unencoded '+' in a URL arrives as a space
  m.replace('_', '+');
  uint8_t bits;
  if (!Control::modeBits(m.c_str(), bits)) {
    srv->send(400, "application/json",
              "{\"ok\":false,\"message\":\"m must be one of heat cool auto tank heat+tank cool+tank auto+tank\"}\n");
    return;
  }
  sendResult(Control::setMode(bits));
}

// GET /api/settings[?refresh=1]: whitelist with limits, current value, raw byte and age.
// refresh=1 first reads every setting from the heat pump (one injected read each, can take
// several seconds); without it the values the panel polled last are shown ("?" if none yet).
static void handleSettings() {
  if (!authorized()) return;
  bool refresh = srv->arg("refresh") == "1";
  String j;
  j.reserve(1024);
  j += "{\"settings\":{";
  bool firstRow = true;
  for (size_t i = 0; const Control::Setting *s = Control::settingAt(i); i++) {
    const char *err = nullptr;
    if (refresh) {
      if (s->zeroIsWrite) err = "not read: a read would reset it (value as last seen on the bus)";
      else {
        Control::Result r = Control::readRegister(s->reg);
        if (!r.ok) err = r.message;
      }
    }
    if (!firstRow) j += ",";
    firstRow = false;
    j += String("\"") + s->key + "\":{\"reg\":" + s->reg + ",\"min\":" + s->min + ",\"max\":" + s->max;
    if (Registers::known(s->reg)) {
      char t[32];
      Registers::text(s->reg, t, sizeof(t));
      j += String(",\"value\":") + (int)(int8_t)Registers::value(s->reg) + ",\"raw\":" + Registers::value(s->reg) +
           ",\"text\":\"" + t + "\",\"age_s\":" + Registers::ageSeconds(s->reg, millis());
    } else {
      j += ",\"value\":null,\"text\":\"?\"";
    }
    if (err) j += String(",\"error\":\"") + err + "\"";
    j += "}";
  }
  j += "}}\n";
  srv->send(200, "application/json", j);
}

static void handleSet() {
  if (!authorized()) return;
  if (srv->method() != HTTP_POST) { srv->send(405, "application/json", "{\"ok\":false,\"message\":\"use POST\"}\n"); return; }
  const Control::Setting *s = Control::findSetting(srv->arg("k").c_str());
  if (!s) { srv->send(400, "application/json", "{\"ok\":false,\"message\":\"unknown k, see GET /api/settings\"}\n"); return; }
  String v = srv->arg("v");
  char *end = nullptr;
  long n = strtol(v.c_str(), &end, 10);
  if (v.length() == 0 || *end != '\0') { srv->send(400, "application/json", "{\"ok\":false,\"message\":\"integer v required\"}\n"); return; }
  if (n < -128 || n > 127) n = 1000;          // out of byte range: setSetting reports it
  sendResult(Control::setSetting(*s, (int)n));
}

static void handleResetError() {
  if (!authorized()) return;
  if (srv->method() != HTTP_POST) { srv->send(405, "application/json", "{\"ok\":false,\"message\":\"use POST\"}\n"); return; }
  sendResult(Control::resetError());
}

void begin(ESP8266WebServer &server) {
  srv = &server;
  server.on("/api/state", handleState);
  server.on("/metrics", handleMetrics);
  server.on("/api/read", handleRead);
  server.on("/api/power", handlePower);
  server.on("/api/mode", handleMode);
  server.on("/api/settings", handleSettings);
  server.on("/api/set", handleSet);
  server.on("/api/reset_error", handleResetError);
}

}  // namespace Api
