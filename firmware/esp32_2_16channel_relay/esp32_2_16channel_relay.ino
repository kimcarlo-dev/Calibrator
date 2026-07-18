/* =============================================================================
 *  Calibrator - ESP32 #2 (16-channel relay board)
 *  ----------------------------------------------------------------------------
 *  Drives a 16-channel relay module.
 *  Source of truth: ../wiring.md
 *
 *  GPIO map (per wiring.md):
 *    Relay 1  -> GPIO16   Relay 9  -> GPIO26
 *    Relay 2  -> GPIO17   Relay 10 -> GPIO27
 *    Relay 3  -> GPIO18   Relay 11 -> GPIO32
 *    Relay 4  -> GPIO19   Relay 12 -> GPIO33
 *    Relay 5  -> GPIO21   Relay 13 -> GPIO4
 *    Relay 6  -> GPIO22   Relay 14 -> GPIO5
 *    Relay 7  -> GPIO23   Relay 15 -> GPIO12
 *    Relay 8  -> GPIO25   Relay 16 -> GPIO13
 *
 *  Safety:
 *    - All relays default OFF at boot.
 *    - ESP32 boot strapping pins (GPIO2, GPIO15) are NOT used.
 *    - GPIO12 must be LOW during boot; we drive it OFF before Wi-Fi.
 *    - Optional communication-loss fail-safe can turn every relay OFF if the
 *      dashboard stops responding. It is disabled by default so a browser or
 *      Wi-Fi interruption does not change commanded relay states.
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
  "YOUR_SSID",          // <-- TODO: replace
  "YOUR_PASSWORD"       // <-- TODO: replace
};

const char* HOSTNAME     = "calibrator-esp32-2";
const char* DEVICE_ID    = "esp2";
const uint16_t HTTP_PORT = 80;
const uint16_t WS_PORT   = 81;

/* Active-LOW relay modules drive the pin LOW to energise the relay.
 *  Set to false if your board is active-HIGH. */
const bool RELAY_ACTIVE_LOW = true;

/* Keep false for persistent ON/OFF commands. Set true only when every load is
 * safe to turn OFF after a dashboard/Wi-Fi interruption. */
const bool RELAY_FAILSAFE_ON_CONNECTION_LOSS = false;
/* Used only when RELAY_FAILSAFE_ON_CONNECTION_LOSS is true. */
const uint32_t WATCHDOG_TIMEOUT_MS = 30000;

/* Sequential test parameters (ms). */
const uint32_t TEST_DEFAULT_DURATION_MS = 1000;
const uint32_t TEST_DEFAULT_GAP_MS      = 250;

/* Component names (TODO: rename to match the actual load each relay controls). */
static const char* COMPONENT_NAMES[16] = {
  "Load 1",  "Load 2",  "Load 3",  "Load 4",
  "Load 5",  "Load 6",  "Load 7",  "Load 8",
  "Load 9",  "Load 10", "Load 11", "Load 12",
  "Load 13", "Load 14", "Load 15", "Load 16"
};

/* -------------------------- RELAY PIN MAP (per wiring.md) -------------------*/
static const uint8_t RELAY_PINS[16] = {
  16, 17, 18, 19, 21, 22, 23, 25,   // 1..8
  26, 27, 32, 33,  4,  5, 12, 13    // 9..16
};

/* ----------------------------- RUNTIME STATE -------------------------------*/
WebServer httpServer(HTTP_PORT);
WebSocketsServer wsServer = WebSocketsServer(WS_PORT);

bool     relayState[16]      = { false };
uint32_t lastDashboardActivityMs = 0;
bool     dashboardActivitySeen   = false;
bool     emergencyLatched    = false;

/* ------------------------- HARDWARE HELPERS --------------------------------*/
static void driveRelay(uint8_t ch, bool on) {
  if (ch >= 16) return;
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(RELAY_PINS[ch], level ? HIGH : LOW);
  relayState[ch] = on;
}

static void allOff() {
  for (uint8_t i = 0; i < 16; ++i) driveRelay(i, false);
}

static void noteDashboardActivity() {
  lastDashboardActivityMs = millis();
  dashboardActivitySeen = true;
}

/* ----------------------------- HTTP ROUTES ---------------------------------*/
static void handleRoot() {
  httpServer.send(200, "text/plain",
    "Calibrator ESP32 #2 (16-channel relay). Connect via WebSocket.");
}

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

static void handleState() {
  StaticJsonDocument<1536> doc;
  doc["type"]  = "state";
  doc["active_low"] = RELAY_ACTIVE_LOW;
  doc["connection_loss_failsafe"] = RELAY_FAILSAFE_ON_CONNECTION_LOSS;
  doc["watchdog_ms"] = WATCHDOG_TIMEOUT_MS;
  JsonArray arr = doc.createNestedArray("relays");
  for (uint8_t i = 0; i < 16; ++i) {
    JsonObject o = arr.createNestedObject();
    o["ch"]      = i + 1;
    o["gpio"]    = RELAY_PINS[i];
    o["name"]    = COMPONENT_NAMES[i];
    o["on"]      = relayState[i];
  }
  String out; serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

static bool applyCommandJson(JsonVariantConst cmd);

static void handleCommand() {
  if (!httpServer.hasArg("plain")) { httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"no body\"}"); return; }
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, httpServer.arg("plain"))) {
    httpServer.send(400, "application/json", "{\"ok\":false,\"error\":\"bad json\"}"); return;
  }
  noteDashboardActivity();
  bool ok = applyCommandJson(doc.as<JsonVariantConst>());
  httpServer.send(ok ? 200 : 400, "application/json",
                  ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rejected\"}");
}

/* ----------------------------- WEBSOCKET EVENTS ----------------------------*/
static void wsEvent(uint8_t clientId, WStype_t type, uint8_t* payload, size_t len);

static void sendState(uint8_t clientId) {
  StaticJsonDocument<1536> doc;
  doc["type"] = "state";
  doc["connection_loss_failsafe"] = RELAY_FAILSAFE_ON_CONNECTION_LOSS;
  doc["watchdog_ms"] = WATCHDOG_TIMEOUT_MS;
  JsonArray arr = doc.createNestedArray("relays");
  for (uint8_t i = 0; i < 16; ++i) {
    JsonObject o = arr.createNestedObject();
    o["ch"] = i + 1; o["gpio"] = RELAY_PINS[i];
    o["name"] = COMPONENT_NAMES[i]; o["on"] = relayState[i];
  }
  String out; serializeJson(doc, out);
  wsServer.sendTXT(clientId, out);
}

static bool applyCommandJson(JsonVariantConst cmd) {
  const char* t = cmd["type"] | "";
  if (!strcmp(t, "set")) {
    int ch = cmd["channel"] | 0;            // 1..16
    bool on = cmd["on"]     | false;
    if (ch < 1 || ch > 16) return false;
    if (emergencyLatched) return false;     // must clear emergency first
    driveRelay((uint8_t)(ch - 1), on);
    return true;
  }
  if (!strcmp(t, "all_off")) { allOff(); emergencyLatched = false; return true; }
  if (!strcmp(t, "all_on"))  {
    bool confirm = cmd["confirm"] | false;
    if (!confirm) return false;
    for (uint8_t i = 0; i < 16; ++i) driveRelay(i, true);
    return true;
  }
  if (!strcmp(t, "emergency_stop")) {
    allOff();
    emergencyLatched = true;
    return true;
  }
  if (!strcmp(t, "test_sequential")) {
    uint32_t dur  = cmd["duration_ms"] | TEST_DEFAULT_DURATION_MS;
    uint32_t gap  = cmd["gap_ms"]      | TEST_DEFAULT_GAP_MS;
    if (dur > 10000) dur = 10000;          // safety cap
    if (gap > 5000)  gap = 5000;
    for (uint8_t i = 0; i < 16 && !emergencyLatched; ++i) {
      driveRelay(i, true);  delay(dur);
      driveRelay(i, false); delay(gap);
    }
    return true;
  }
  if (!strcmp(t, "test_channel")) {
    int ch = cmd["channel"] | 0;
    uint32_t dur = cmd["duration_ms"] | TEST_DEFAULT_DURATION_MS;
    if (ch < 1 || ch > 16 || dur > 5000) return false;
    driveRelay((uint8_t)(ch - 1), true);
    delay(dur);
    driveRelay((uint8_t)(ch - 1), false);
    return true;
  }
  if (!strcmp(t, "rename")) {
    int ch = cmd["channel"] | 0;
    const char* name = cmd["name"] | "";
    if (ch < 1 || ch > 16 || strlen(name) == 0 || strlen(name) > 31) return false;
    COMPONENT_NAMES[ch - 1] = name;       // note: stored in RAM only
    return true;
  }
  return false;
}

static void wsEvent(uint8_t clientId, WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED: {
      Serial.printf("[WS] client %u connected\n", clientId);
      StaticJsonDocument<128> hello; hello["type"]="hello"; hello["board"]="esp32_2_relays16";
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
      if (!strcmp(t, "pong") || !strcmp(t, "alive")) {
        // Activity was recorded above.
      } else if (!strcmp(t, "get_state")) {
        sendState(clientId);
      } else {
        bool ok = applyCommandJson(doc.as<JsonVariantConst>());
        StaticJsonDocument<128> resp;
        resp["type"] = "ack"; resp["cmd"] = t; resp["ok"] = ok;
        String out; serializeJson(resp, out);
        wsServer.sendTXT(clientId, out);
      }
      break;
    }
    case WStype_DISCONNECTED: Serial.printf("[WS] client %u disconnected\n", clientId); break;
    default: break;
  }
}

/* ----------------------------- WIFI ----------------------------------------*/
static void setupWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_CONFIG.ssid, WIFI_CONFIG.password);
  Serial.printf("[WIFI] connecting to %s\n", WIFI_CONFIG.ssid);
  uint8_t tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500); Serial.print('.'); tries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WIFI] connected, IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[WIFI] DHCP reservation MAC: %s\n", WiFi.macAddress().c_str());
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", HTTP_PORT);
      MDNS.addService("ws", "tcp", WS_PORT);
      Serial.printf("[MDNS] http://%s.local/\n", HOSTNAME);
    } else {
      Serial.println("[MDNS] failed; use the DHCP address above");
    }
  } else {
    Serial.println("\n[WIFI] FAILED, restart in 5s");
    delay(5000); ESP.restart();
  }
}

static void setupHttp() {
  httpServer.enableCORS(true);
  httpServer.on("/",     HTTP_GET,  handleRoot);
  httpServer.on("/info", HTTP_GET,  handleInfo);
  httpServer.on("/state",HTTP_GET,  handleState);
  httpServer.on("/cmd",  HTTP_POST, handleCommand);
  httpServer.on("/cmd",  HTTP_OPTIONS, handleCorsPreflight);
  httpServer.on("/info", HTTP_OPTIONS, handleCorsPreflight);
  httpServer.on("/state",HTTP_OPTIONS, handleCorsPreflight);
  httpServer.begin();
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[BOOT] Calibrator ESP32 #2 (16-channel relays) starting");

  // Configure relay pins and force OFF before anything else.
  for (uint8_t i = 0; i < 16; ++i) {
    pinMode(RELAY_PINS[i], OUTPUT);
    driveRelay(i, false);
  }

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
