#pragma once
#include <Arduino.h>

// Prometheus text exposition (format 0.0.4) of the heat pump state and the bus health.
// render() is hardware independent: the caller passes the few ESP values in Env and a sink
// that receives the text in chunks (so a slow client never blocks the bus).
namespace Metrics {

struct Env {
  uint32_t nowMs;      // millis()
  uint32_t freeHeap;   // ESP.getFreeHeap()
  int      rssi;       // dBm, 0 = Wi-Fi not connected
};

typedef void (*Sink)(const char *data, size_t len, void *ctx);

void render(const Env &env, Sink sink, void *ctx);

}  // namespace Metrics
