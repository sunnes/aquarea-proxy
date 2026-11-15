#include <Arduino.h>
#include <EEPROM.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ArduinoOTA.h>
#include <string.h>
#include "sniffer.h"
#include "telnet_log.h"
#include "api.h"
#include "config.h"

// Configuration for fallback access point
const char * AP_ssid = "ESP8266_AP";
const char * AP_password = DEVICE_PASSWORD;
IPAddress AP_IP = IPAddress(10,1,1,1);
IPAddress AP_subnet = IPAddress(255,255,255,0);

// EEPROM layout
const int SSID_ADDR = 0;
const int PASS_ADDR = 64;
const int SSID_MAX  = 50;
const int PASS_MAX  = 50;
const int EEPROM_SIZE = 512;   // plenty for our 114 bytes

struct WifiConf {
  char wifi_ssid[SSID_MAX];
  char wifi_password[PASS_MAX];
};
WifiConf wifiConf;

ESP8266WebServer server(80);

// Status LED (active LOW on ESP-07 GPIO2)
const uint8_t LED_PIN = 2;  // LED_BUILTIN is GPIO1 (UART TX) on this board, so use GPIO2 explicitly
const unsigned long LED_PERIOD_MS = 10000;  // one short "alive" flash per period
const unsigned long LED_FLASH_MS = 50;      // how long the LED stays lit
unsigned long ledCycleStart = 0UL - LED_PERIOD_MS;  // so the first flash comes right after boot
bool ledLit = false;

// Forward declarations
void readWifiConf();
void writeWifiConf();
bool connectToWiFi();
void setUpAccessPoint();
void setUpWebServer();
void handleWebServerRequest();
void setUpOverTheAirProgramming();
void blinkLed();

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);  // off
  Serial.begin(9600);
  Serial.println("Booting...");
  Sniffer::begin();

  // Give flash and WiFi stack a moment to settle before touching EEPROM.
  delay(100);

  EEPROM.begin(EEPROM_SIZE);
  readWifiConf();

  if (!connectToWiFi()) {
    setUpAccessPoint();
  }
  setUpWebServer();
  setUpOverTheAirProgramming();
  TelnetLog::begin();
}

void readWifiConf() {
  int i;
  for (i = 0; i < SSID_MAX - 1; i++) {
    wifiConf.wifi_ssid[i] = EEPROM.read(SSID_ADDR + i);
    if (wifiConf.wifi_ssid[i] == 0) break;
  }
  wifiConf.wifi_ssid[SSID_MAX - 1] = '\0';

  for (i = 0; i < PASS_MAX - 1; i++) {
    wifiConf.wifi_password[i] = EEPROM.read(PASS_ADDR + i);
    if (wifiConf.wifi_password[i] == 0) break;
  }
  wifiConf.wifi_password[PASS_MAX - 1] = '\0';

  Serial.printf("Read SSID: '%s' (len=%d)\n",
                wifiConf.wifi_ssid, strlen(wifiConf.wifi_ssid));
  Serial.printf("Read pass length: %d\n", strlen(wifiConf.wifi_password));
}

void writeWifiConf() {
  // Zero the entire region we use.
  for (int i = 0; i < PASS_ADDR + PASS_MAX; i++) {
    EEPROM.write(i, 0);
  }

  size_t ssidLen = strlen(wifiConf.wifi_ssid);
  if (ssidLen > SSID_MAX - 1) ssidLen = SSID_MAX - 1;
  for (size_t i = 0; i < ssidLen; i++) {
    EEPROM.write(SSID_ADDR + i, wifiConf.wifi_ssid[i]);
  }

  size_t passLen = strlen(wifiConf.wifi_password);
  if (passLen > PASS_MAX - 1) passLen = PASS_MAX - 1;
  for (size_t i = 0; i < passLen; i++) {
    EEPROM.write(PASS_ADDR + i, wifiConf.wifi_password[i]);
  }

  bool ok = EEPROM.commit();
  Serial.printf("EEPROM.commit() = %s\n", ok ? "true" : "false");

  // Read back from the flash buffer and verify.
  char checkSsid[SSID_MAX];
  char checkPass[PASS_MAX];
  for (int i = 0; i < SSID_MAX; i++) checkSsid[i] = EEPROM.read(SSID_ADDR + i);
  for (int i = 0; i < PASS_MAX; i++) checkPass[i] = EEPROM.read(PASS_ADDR + i);
  checkSsid[SSID_MAX - 1] = '\0';
  checkPass[PASS_MAX - 1] = '\0';

  Serial.printf("Verify SSID: '%s' pass len=%d\n",
                checkSsid, strlen(checkPass));
}

bool connectToWiFi() {
  Serial.printf("Connecting to '%s'\n", wifiConf.wifi_ssid);

  if (strlen(wifiConf.wifi_ssid) == 0) {
    Serial.println("SSID is empty, skip connection.");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiConf.wifi_ssid, wifiConf.wifi_password);

  // Non-blocking wait: keep capturing bus data and blinking while connecting.
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 30000) {
    Sniffer::poll();
    blinkLed();
    yield();
  }
  int8_t status = WiFi.status();

  if (status == WL_CONNECTED) {
    Serial.print("Connected. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.printf("Connection failed. Status code: %d\n", status);
  switch (status) {
    case WL_NO_SSID_AVAIL:  Serial.println("SSID not found"); break;
    case WL_CONNECT_FAILED: Serial.println("Connection failed (check password)"); break;
    case WL_WRONG_PASSWORD: Serial.println("Wrong password"); break;
    default:                Serial.println("Timeout or other error"); break;
  }
  return false;
}

void setUpAccessPoint() {
  Serial.println("Setting up access point.");
  Serial.printf("SSID: %s\n", AP_ssid);
  Serial.printf("Password: %s\n", AP_password);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(AP_IP, AP_IP, AP_subnet);
  if (WiFi.softAP(AP_ssid, AP_password)) {
    Serial.print("Ready. Access point IP: ");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("Setting up access point failed!");
  }
}

void setUpWebServer() {
  server.on("/wifi", handleWebServerRequest);
  Api::begin(server);
  server.begin();
}

void handleWebServerRequest() {
  if (!Api::authorized()) return;     // same login as the API: user API_USER, DEVICE_PASSWORD
  bool save = false;

  if (server.hasArg("ssid") && server.hasArg("password")) {
    server.arg("ssid").toCharArray(
      wifiConf.wifi_ssid, sizeof(wifiConf.wifi_ssid));
    server.arg("password").toCharArray(
      wifiConf.wifi_password, sizeof(wifiConf.wifi_password));

    Serial.printf("Saving SSID: '%s'\n", wifiConf.wifi_ssid);
    Serial.printf("Saving pass length: %d\n", strlen(wifiConf.wifi_password));

    writeWifiConf();
    save = true;
  }

  String message = "";
  message += "<!DOCTYPE html>";
  message += "<html><head><title>ESP8266 conf</title></head><body>";
  if (save) {
    message += "<div>Saved! Rebooting...</div>";
  } else {
    message += "<h1>Wi-Fi conf</h1>";
    message += "<form action='/wifi' method='POST'>";
    message += "<div>SSID:</div>";
    message += "<div><input type='text' name='ssid' value='" + String(wifiConf.wifi_ssid) + "'/></div>";
    message += "<div>Password:</div>";
    message += "<div><input type='password' name='password' value='" + String(wifiConf.wifi_password) + "'/></div>";
    message += "<div><input type='submit' value='Save'/></div>";
    message += "</form>";
  }
  message += "</body></html>";
  server.send(200, "text/html", message);

  if (save) {
    Serial.println("Wi-Fi conf saved. Rebooting in 2s...");
    delay(2000);          // give flash time to flush
    ESP.restart();
  }
}

void setUpOverTheAirProgramming() {
  ArduinoOTA.setHostname("aquarea");
  ArduinoOTA.setPassword(DEVICE_PASSWORD);
  ArduinoOTA.begin();
}

// Non-blocking: a short flash every LED_PERIOD_MS (LED is active LOW).
void blinkLed() {
  unsigned long now = millis();
  if (!ledLit && now - ledCycleStart >= LED_PERIOD_MS) {
    ledLit = true;
    ledCycleStart = now;
    digitalWrite(LED_PIN, LOW);
  } else if (ledLit && now - ledCycleStart >= LED_FLASH_MS) {
    ledLit = false;
    digitalWrite(LED_PIN, HIGH);
  }
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();
  Sniffer::poll();
  TelnetLog::poll();

  blinkLed();
}
