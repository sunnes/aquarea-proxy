#pragma once
// DEVICE_PASSWORD is injected at build time from secrets.env (scripts/pio_secrets.py).
// It protects OTA upload, the fallback access point and the HTTP API (user name API_USER).
// HTTP user name for the API and the /wifi page.
constexpr const char *API_USER = "aquarea";

#ifndef DEVICE_PASSWORD
#error "DEVICE_PASSWORD is not defined: build with PlatformIO and create secrets.env (see secrets.env.example)"
#endif

// Firmware version from the VERSION file (scripts/pio_secrets.py).
#ifndef AQUAREA_VERSION
#define AQUAREA_VERSION "dev"
#endif
