#pragma once

// Telnet (port 23) viewer. One client at a time, non-blocking:
// if the client is slow, lines are dropped instead of stalling loop().
// On connect: banner + help + stats; heartbeat line every 10 s. Press ? for commands.
namespace TelnetLog {
void begin();
void poll();
}
