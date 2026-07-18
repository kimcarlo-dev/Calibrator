/* =============================================================================
 *  Calibrator - ESP32 #3 (8-channel + 4-channel relay boards)
 *  ----------------------------------------------------------------------------
 *  Drives a combined 12 relay channels split between two physical modules.
 *  Source of truth: ../wiring.md
 *
 *  8-channel board:
 *    Ch 1 -> GPIO16   Ch 5 -> GPIO21
 *    Ch 2 -> GPIO17   Ch 6 -> GPIO22
 *    Ch 3 -> GPIO18   Ch 7 -> GPIO23
 *    Ch 4 -> GPIO19   Ch 8 -> GPIO25
 *
 *  4-channel board:
 *    Ch 9  -> GPIO26
 *    Ch 10 -> GPIO27
 *    Ch 11 -> GPIO32
 *    Ch 12 -> GPIO33
 *
 *  Safety: identical policy to ESP32 #2 (all-OFF default, active-LOW support,
 *  optional communication-loss fail-safe, emergency stop).
 * ===========================================================================*/

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

/* ----------------------------- USER SETTINGS --------------------------------*/
struct WiFiCredentials {
  char ssid[32];
  char password[64];
};
const WiFiCredentials WIFI_CONFIG = {
  "YOUR_SSID",          // <-- TODO
  "YOUR_PASSWORD"       // <-- TODO
};

const char* HOSTNAME     = "calibrator-esp32-3";
const char* DEVICE_ID    = "esp3";
const uint16_t HTTP_PORT = 80;
const uint16_t WS_PORT   = 81;

const bool RELAY_ACTIVE_LOW = true;
/* Keep false for persistent ON/OFF commands. Set true only when every load is
 * safe to turn OFF after a dashboard/Wi-Fi interruption. */
const bool RELAY_FAILSAFE_ON_CONNECTION_LOSS = false;
/* Used only when RELAY_FAILSAFE_ON_CONNECTION_LOSS is true. */
const uint32_t WATCHDOG_TIMEOUT_MS = 30000;

static const char* COMPONENT_NAMES[12] = {
  "8CH Load 1","8CH Load 2","8CH Load 3","8CH Load 4",
  "8CH Load 5","8CH Load 6","8CH Load 7","8CH Load 8",
  "4CH Load 1","4CH Load 2","4CH Load 3","4CH Load 4"
};

static const uint8_t RELAY_PINS[12] = {
  16, 17, 18, 19, 21, 22, 23, 25,   // 8-channel
  26, 27, 32, 33                   // 4-channel
};
static const bool IS_8CH[12] = {
  true,true,true,true,true,true,true,true,
  false,false,false,false
};

WebServer httpServer(HTTP_PORT);
WebSocketsServer wsServer = WebSocketsServer(WS_PORT);

bool     relayState[12]      = { false };
uint32_t lastDashboardActivityMs = 0;
bool     dashboardActivitySeen   = false;
bool     emergencyLatched    = false;

static void driveRelay(uint8_t ch, bool on) {
  if (ch >= 12) return;
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(RELAY_PINS[ch], level ? HIGH : LOW);
  relayState[ch] = on;
}

static void allOff() {
  for (uint8_t i = 0; i < 12; ++i) driveRelay(i, false);
}

static void noteDashboardActivity() {
  lastDashboardActivityMs = millis();
  dashboardActivitySeen = true;
}

static void allOffBoard(bool eightCh) {
  for (uint8_t i = 0; i < 12; ++i)
    if (IS_8CH[i] == eightCh) driveRelay(i, false);
}

static void sendState(uint8_t clientId) {
  StaticJsonDocument<1280> doc;
  doc["type"] = "state";
  doc["connection_loss_failsafe"] = RELAY_FAILSAFE_ON_CONNECTION_LOSS;
  doc["watchdog_ms"] = WATCHDOG_TIMEOUT_MS;
  JsonArray arr = doc.createNestedArray("relays");
  for (uint8_t i = 0; i < 12; ++i) {
    JsonObject o = arr.createNestedObject();
    o["ch"]   = i + 1;
    o["gpio"] = RELAY_PINS[i];
    o["name"] = COMPONENT_NAMES[i];
    o["board"]= IS_8CH[i] ? "8ch" : "4ch";
    o["on"]   = relayState[i];
  }
  String out; serializeJson(doc, out);
  wsServer.sendTXT(clientId, out);
}

static bool applyCommandJson(JsonVariantConst cmd) {
  const char* t = cmd["type"] | "";
  if (!strcmp(t, "set")) {
    int ch = cmd["channel"] | 0;
    bool on = cmd["on"] | false;
    if (ch < 1 || ch > 12) return false;
    if (emergencyLatched) return false;
    driveRelay((uint8_t)(ch - 1), on);
    return true;
  }
  if (!strcmp(t, "all_off"))       { allOff(); emergencyLatched = false; return true; }
  if (!strcmp(t, "all_off_board")) {
    const char* b = cmd["board"] | "";
    if (!strcmp(b, "8ch")) { allOffBoard(true); return true; }
    if (!strcmp(b, "4ch")) { allOffBoard(false); return true; }
    return false;
  }
  if (!strcmp(t, "all_on")) {
    bool confirm = cmd["confirm"] | false;
    if (!confirm) return false;
    for (uint8_t i = 0; i < 12; ++i) driveRelay(i, true);
    return true;
  }
  if (!strcmp(t, "emergency_stop")) { allOff(); emergencyLatched = true; return true; }
  if (!strcmp(t, "test_channel")) {
    int ch = cmd["channel"] | 0;
    uint32_t dur = cmd["duration_ms"] | 1000;
    if (ch < 1 || ch > 12 || dur > 5000) return false;
    driveRelay((uint8_t)(ch - 1), true);
    delay(dur);
    driveRelay((uint8_t)(ch - 1), false);
    return true;
  }
  if (!strcmp(t, "rename")) {
    int ch = cmd["channel"] | 0;
    const char* name = cmd["name"] | "";
    if (ch < 1 || ch > 12 || strlen(name) == 0 || strlen(name) > 31) return false;
    COMPONENT_NAMES[ch - 1] = name;
    return true;
  }
  return false;
}

static void wsEvent(uint8_t clientId, WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED: {
      Serial.printf("[WS] client %u connected\n", clientId);
      StaticJsonDocument<128> hello; hello["type"]="hello"; hello["board"]="esp32_3_relays8_4";
      hello["device_id"] = DEVICE_ID;
      String out; serializeJson(hello, out); wsServer.sendTXT(clientId, out);
      sendState(clientId);
      noteDashboardActivity();
      break;
    }
    case WStype_TEXT: {
      StaticJsonDocument<512> doc;
      if (deserializeJson(doc, payload, len)) return;
      noteDashboardActivity();
      const char* t = doc["type"] | "";
      if (!strcmp(t, "pong") || !strcmp(t, "alive")) { /* activity recorded above */ }
      else if (!strcmp(t, "get_state")) sendState(clientId);
      else {
        bool ok = applyCommandJson(doc.as<JsonVariantConst>());
        StaticJsonDocument<128> resp; resp["type"]="ack"; resp["cmd"]=t; resp["ok"]=ok;
        String out; serializeJson(resp, out); wsServer.sendTXT(clientId, out);
      }
      break;
    }
    case WStype_DISCONNECTED: Serial.printf("[WS] client %u disconnected\n", clientId); break;
    default: break;
  }
}

static void handleRoot()   { httpServer.send(200, "text/plain", "Calibrator ESP32 #3 (8+4 relays)"); }
static void handleInfo() {
  StaticJsonDocument<256> doc;
  doc["type"] = "device_info";
  doc["device_id"] = DEVICE_ID;
  doc["hostname"] = HOSTNAME;
  doc["ip"] = WiFi.localIP().toString();
  doc["mac"] = WiFi.macAddress();
  doc["http_port"] = HTTP_PORT;
  doc["ws_port"] = WS_PORT;
  String out; serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}
static void handleCorsPreflight() {
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  httpServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  httpServer.sendHeader("Access-Control-Allow-Private-Network", "true");
  httpServer.send(204);
}
static void handleStateHttp() {
  StaticJsonDocument<1280> doc; doc["type"]="state";
  doc["connection_loss_failsafe"] = RELAY_FAILSAFE_ON_CONNECTION_LOSS;
  doc["watchdog_ms"] = WATCHDOG_TIMEOUT_MS;
  JsonArray arr = doc.createNestedArray("relays");
  for (uint8_t i = 0; i < 12; ++i) {
    JsonObject o = arr.createNestedObject();
    o["ch"]=i+1; o["gpio"]=RELAY_PINS[i]; o["name"]=COMPONENT_NAMES[i];
    o["board"]=IS_8CH[i]?"8ch":"4ch"; o["on"]=relayState[i];
  }
  String out; serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}
static void handleCommand() {
  if (!httpServer.hasArg("plain")) { httpServer.send(400); return; }
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, httpServer.arg("plain"))) { httpServer.send(400); return; }
  noteDashboardActivity();
  bool ok = applyCommandJson(doc.as<JsonVariantConst>());
  httpServer.send(ok ? 200 : 400, "application/json",
                  ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void setupHttp() {
  httpServer.enableCORS(true);
  httpServer.on("/", HTTP_GET, handleRoot);
  httpServer.on("/info", HTTP_GET, handleInfo);
  httpServer.on("/state", HTTP_GET, handleStateHttp);
  httpServer.on("/cmd", HTTP_POST, handleCommand);
  httpServer.on("/cmd", HTTP_OPTIONS, handleCorsPreflight);
  httpServer.on("/info", HTTP_OPTIONS, handleCorsPreflight);
  httpServer.on("/state", HTTP_OPTIONS, handleCorsPreflight);
  httpServer.begin();
}

static void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_CONFIG.ssid, WIFI_CONFIG.password);
  uint8_t tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) { delay(500); tries++; }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WIFI] DHCP reservation MAC: %s\n", WiFi.macAddress().c_str());
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", HTTP_PORT);
      MDNS.addService("ws", "tcp", WS_PORT);
      Serial.printf("[MDNS] http://%s.local/\n", HOSTNAME);
    } else {
      Serial.println("[MDNS] failed; use the DHCP address above");
    }
  }
  else { Serial.println("[WIFI] FAILED, restart"); delay(5000); ESP.restart(); }
}

void setup() {
  Serial.begin(115200); delay(100);
  Serial.println("\n[BOOT] Calibrator ESP32 #3 (8+4 relays) starting");
  for (uint8_t i = 0; i < 12; ++i) { pinMode(RELAY_PINS[i], OUTPUT); driveRelay(i, false); }
  setupWifi();
  wsServer.begin();
  wsServer.onEvent(wsEvent);
  setupHttp();
  const esp_task_wdt_config_t taskWdtConfig = {
    10000,                                  // timeout_ms
    (1U << portNUM_PROCESSORS) - 1U,        // monitor each CPU's idle task
    true                                    // trigger_panic
  };
  const esp_err_t taskWdtState = esp_task_wdt_status(NULL);
  const esp_err_t taskWdtResult =
    taskWdtState == ESP_ERR_INVALID_STATE
      ? esp_task_wdt_init(&taskWdtConfig)
      : esp_task_wdt_reconfigure(&taskWdtConfig);
  if (taskWdtResult == ESP_OK && esp_task_wdt_status(NULL) == ESP_ERR_NOT_FOUND) {
    esp_task_wdt_add(NULL);
  }
  Serial.println("[BOOT] ready");
}

void loop() {
  esp_task_wdt_reset();
  wsServer.loop();
  httpServer.handleClient();
  // Optional communication-loss fail-safe. Disabled by default because
  // browser timer throttling or a brief Wi-Fi drop must not change outputs.
  if (RELAY_FAILSAFE_ON_CONNECTION_LOSS && dashboardActivitySeen &&
      WATCHDOG_TIMEOUT_MS > 0 &&
      millis() - lastDashboardActivityMs > WATCHDOG_TIMEOUT_MS) {
    allOff();
    dashboardActivitySeen = false;  // re-arm on the next dashboard message
    Serial.println("[FAILSAFE] dashboard timeout, all relays OFF");
  }
}
