#pragma once
#include <ESP8266WebServer.h>

// HTTP API for the command line (all endpoints need Basic auth, user "aquarea"):
//   GET  /metrics                   Prometheus metrics (text format)
//   GET  /api/state                 decoded registers + control status (JSON)
//   GET  /api/read?reg=N            read one register from the heat pump
//   POST /api/power?on=1|0          heating on / off
//   POST /api/mode?m=<mode>         heat cool auto tank heat+tank cool+tank auto+tank (keeps power)
//   POST /api/reset_error           clear the error (as the panel does)
namespace Api {
void begin(ESP8266WebServer &server);
// Basic auth check (user "aquarea", DEVICE_PASSWORD). Sends the 401 challenge itself and returns false
// when the request is not authorized, so a handler just does: if (!Api::authorized()) return;
// Also used by the /wifi setup page in main.cpp.
bool authorized();
}
