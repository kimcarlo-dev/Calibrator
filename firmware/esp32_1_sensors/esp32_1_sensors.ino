/* =============================================================================
 *  Calibrator - ESP32 #1 (Sensors)
 *  ----------------------------------------------------------------------------
 *  This firmware runs on the ESP32 responsible for ALL sensor reads.
 *  Source of truth: ../wiring.md (do not change GPIO assignments here
 *  without updating wiring.md).
 *
 *  Sensors assigned to this board (per wiring.md):
 *    - 2 x DS18B20 temperature sensors  (OneWire, GPIO4, GPIO5)
 *    - 2 x TDS sensors                  (Analog, GPIO34, GPIO35)
 *    - 1 x DFRobot DO sensor            (Analog, GPIO33)
 *    - 1 x pH sensor (PH4502C)          (Analog, GPIO32)
 *    - 1 x MQ137 ammonia sensor         (Analog GPIO36, Digital GPIO25)
 *    - 3 x JSN-SR04T ultrasonic sensors (Trig/Echo, GPIO16/17, 18/19, 21/22)
 *
 *  Features:
 *    - Non-blocking sensor reads using millis() timers.
 *    - WebSocket + REST API for the Calibrator web dashboard.
 *    - Per-sensor calibration (offset/slope or library-specific routine)
 *      persisted with the ESP32 Preferences library.
 *    - "Reset calibration" command per-sensor or all.
 *    - Disconnection / out-of-range detection.
 *    - Heartbeat broadcast every 1s and full sensor JSON every interval.
 *
 *  Safety:
 *    - All GPIOs are inputs or OneWire; no high-voltage actuators on this
 *      board. Still, validate every value before applying.
 * ===========================================================================*/

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_task_wdt.h>

/* ----------------------------- USER SETTINGS --------------------------------
 * Edit these values for your network. Keep them in ONE place so that you
 * never accidentally hard-code credentials inside the rest of the file.
 * ---------------------------------------------------------------------------*/
struct WiFiCredentials {
  char ssid[32];
  char password[64];
};

const WiFiCredentials WIFI_CONFIG = {
  "YOUR_SSID",          // <-- TODO: replace with your Wi-Fi SSID
  "YOUR_PASSWORD"       // <-- TODO: replace with your Wi-Fi password
};

const char* HOSTNAME       = "calibrator-esp32-1";
const char* DEVICE_ID      = "esp1";
const uint16_t HTTP_PORT   = 80;
const uint16_t WS_PORT     = 81;

/* Broadcast intervals (ms). All non-blocking. */
const uint32_t SENSOR_INTERVAL_MS  = 1000;   // sensor read / broadcast
const uint32_t HEARTBEAT_INTERVAL_MS = 5000; // "alive" ping

/* ------------------------- SENSOR PIN ASSIGNMENTS ----------------------------
 * Mirror wiring.md exactly. Do not change without updating wiring.md.
 * ---------------------------------------------------------------------------*/
// DS18B20 (OneWire)
const uint8_t PIN_DS18B20_1 = 4;
const uint8_t PIN_DS18B20_2 = 5;
// Analog sensors (ADC1 only, so Wi-Fi does not conflict)
const uint8_t PIN_TDS_1     = 34;
const uint8_t PIN_TDS_2     = 35;
const uint8_t PIN_DO        = 33;
const uint8_t PIN_PH        = 32;
const uint8_t PIN_MQ137_AO  = 36;
const uint8_t PIN_MQ137_DO  = 25;   // digital threshold output
// Ultrasonic sensors (Trig -> output, Echo -> input)
const uint8_t PIN_US1_TRIG = 16, PIN_US1_ECHO = 17;
const uint8_t PIN_US2_TRIG = 18, PIN_US2_ECHO = 19;
const uint8_t PIN_US3_TRIG = 21, PIN_US3_ECHO = 22;

/* ----------------------------- LIBRARY OBJECTS ------------------------------*/
OneWire oneWire1(PIN_DS18B20_1);
OneWire oneWire2(PIN_DS18B20_2);
DallasTemperature ds18b20_1(&oneWire1);
DallasTemperature ds18b20_2(&oneWire2);

WebServer httpServer(HTTP_PORT);
WebSocketsServer wsServer = WebSocketsServer(WS_PORT);
Preferences prefs;

/* ----------------------------- DATA STRUCTURES ------------------------------*/
struct SensorCalibration {
  // Generic linear calibration: reading = raw * slope + offset
  float slope   = 1.0f;
  float offset  = 0.0f;
  // Ultrasonic specific
  float us_offset_cm = 0.0f;
  // TDS specific (cell constant)
  float tds_k = 1.0f;
  // MQ137 baseline
  float mq137_baseline = 1.0f;
  bool  valid = false;
};

struct SensorState {
  bool      present      = false;
  bool      errorFlag    = false;
  float     raw          = NAN;
  float     value        = NAN;
  uint32_t  lastReadMs   = 0;
  SensorCalibration cal;
};

enum SensorId : uint8_t {
  SID_DS18B20_1 = 0,
  SID_DS18B20_2,
  SID_TDS_1,
  SID_TDS_2,
  SID_DO,
  SID_PH,
  SID_MQ137,
  SID_US_1,
  SID_US_2,
  SID_US_3,
  SID_COUNT
};

static const char* SENSOR_NAMES[SID_COUNT] = {
  "DS18B20 #1",
  "DS18B20 #2",
  "TDS #1",
  "TDS #2",
  "DO",
  "pH",
  "MQ137",
  "Ultrasonic #1",
  "Ultrasonic #2",
  "Ultrasonic #3"
};

SensorState sensors[SID_COUNT];

/* ----------------------- CALIBRATION PERSISTENCE --------------------------- *
 * Calibration values are stored in NVS (Preferences). One namespace per sensor.
 * ---------------------------------------------------------------------------*/
static void loadCalibration(uint8_t id) {
  if (id >= SID_COUNT) return;
  char ns[16]; snprintf(ns, sizeof(ns), "cal_%u", id);
  prefs.begin(ns, true);
  SensorCalibration &c = sensors[id].cal;
  c.slope        = prefs.getFloat("slope",  1.0f);
  c.offset       = prefs.getFloat("offset", 0.0f);
  c.us_offset_cm = prefs.getFloat("us_off", 0.0f);
  c.tds_k        = prefs.getFloat("tds_k",  1.0f);
  c.mq137_baseline = prefs.getFloat("mq_b",  1.0f);
  c.valid        = prefs.getBool ("valid",  false);
  prefs.end();
  Serial.printf("[CAL] Loaded %s: slope=%.4f offset=%.4f us_off=%.2f tds_k=%.3f mq_b=%.3f valid=%d\n",
                SENSOR_NAMES[id], c.slope, c.offset, c.us_offset_cm, c.tds_k, c.mq137_baseline, c.valid);
}

static void saveCalibration(uint8_t id) {
  if (id >= SID_COUNT) return;
  char ns[16]; snprintf(ns, sizeof(ns), "cal_%u", id);
  prefs.begin(ns, false);
  const SensorCalibration &c = sensors[id].cal;
  prefs.putFloat("slope",  c.slope);
  prefs.putFloat("offset", c.offset);
  prefs.putFloat("us_off", c.us_offset_cm);
  prefs.putFloat("tds_k",  c.tds_k);
  prefs.putFloat("mq_b",   c.mq137_baseline);
  prefs.putBool ("valid",  c.valid);
  prefs.end();
  Serial.printf("[CAL] Saved %s\n", SENSOR_NAMES[id]);
}

static void resetCalibration(uint8_t id) {
  if (id >= SID_COUNT) return;
  sensors[id].cal = SensorCalibration();
  saveCalibration(id);
}

static void resetAllCalibration() {
  for (uint8_t i = 0; i < SID_COUNT; ++i) resetCalibration(i);
}

/* ----------------------------- SENSOR READS ---------------------------------*/
static float readAnalogAverage(uint8_t pin, uint8_t samples = 16) {
  uint32_t acc = 0;
  for (uint8_t i = 0; i < samples; ++i) acc += analogRead(pin);
  return (float)acc / (float)samples;
}

static float readDS18B20(DallasTemperature &bus) {
  bus.requestTemperatures();
  float t = bus.getTempCByIndex(0);
  if (t == DEVICE_DISCONNECTED_C) return NAN;
  if (t < -55.0f || t > 125.0f)  return NAN;     // out-of-range guard
  return t;
}

// Simple NewPing-style manual measurement (no extra dep).
// Returns distance in cm or NAN on timeout.
static float readUltrasonic(uint8_t trig, uint8_t echo) {
  digitalWrite(trig, LOW);  delayMicroseconds(2);
  digitalWrite(trig, HIGH); delayMicroseconds(10);
  digitalWrite(trig, LOW);
  uint32_t duration = pulseIn(echo, HIGH, 30000UL); // 30 ms ~ 5 m cap
  if (duration == 0) return NAN;
  return (float)duration * 0.0343f / 2.0f;
}

/* ----------------------- CALIBRATION APPLICATION ----------------------------*/
static float applyLinear(float raw, const SensorCalibration &c) {
  return raw * c.slope + c.offset;
}

static float calcTDSppm(float rawV, float k, float tempC) {
  // Approx conversion; users typically fine-tune with known solution.
  float comp = 1.0f + 0.02f * (tempC - 25.0f);
  return (rawV * k) * 500.0f * comp;  // 500 ppm/V is a rough default
}

static float calcDO_mgL(float rawV) {
  // DFRobot bench conversion; adjust via calibration.
  return rawV;   // user maps voltage->mg/L in cal slope/offset
}

static float calcMQ137(float rawV, float baseline) {
  // Simple Rs/R0 ratio -> ppm estimate (not lab accurate, fine for alerts).
  if (baseline <= 0.0f) return NAN;
  return (rawV / baseline);
}

static float calcPH(float rawV) {
  // pH4502C ~3.3V == 14 pH (approx), 0V == 0 pH; tuned by calibration.
  return rawV; // user stores slope/offset from buffer calibration
}

/* ----------------------------- READ LOOP -----------------------------------*/
static void readAllSensors() {
  uint32_t now = millis();

  // ---- DS18B20 (slow conversion; schedule separately) --------------------
  // We request once a second; the bus handles conversion asynchronously.
  static uint32_t dsRequest = 0;
  if (now - dsRequest > SENSOR_INTERVAL_MS) {
    dsRequest = now;
    sensors[SID_DS18B20_1].raw    = readDS18B20(ds18b20_1);
    sensors[SID_DS18B20_1].present = !isnan(sensors[SID_DS18B20_1].raw);
    sensors[SID_DS18B20_2].raw    = readDS18B20(ds18b20_2);
    sensors[SID_DS18B20_2].present = !isnan(sensors[SID_DS18B20_2].raw);
  }

  // ---- Analog sensors ----------------------------------------------------
  sensors[SID_TDS_1].raw    = readAnalogAverage(PIN_TDS_1) * (3.3f / 4095.0f);
  sensors[SID_TDS_1].present = sensors[SID_TDS_1].raw > 0.05f;
  sensors[SID_TDS_2].raw    = readAnalogAverage(PIN_TDS_2) * (3.3f / 4095.0f);
  sensors[SID_TDS_2].present = sensors[SID_TDS_2].raw > 0.05f;
  sensors[SID_DO].raw       = readAnalogAverage(PIN_DO)    * (3.3f / 4095.0f);
  sensors[SID_DO].present   = sensors[SID_DO].raw > 0.05f;
  sensors[SID_PH].raw       = readAnalogAverage(PIN_PH)    * (3.3f / 4095.0f);
  sensors[SID_PH].present   = sensors[SID_PH].raw > 0.05f;
  sensors[SID_MQ137].raw    = readAnalogAverage(PIN_MQ137_AO) * (5.0f / 4095.0f); // sensor is 5V
  sensors[SID_MQ137].present = sensors[SID_MQ137].raw > 0.05f;
  sensors[SID_MQ137].errorFlag = (digitalRead(PIN_MQ137_DO) == LOW); // DO is "alert" output

  // ---- Apply calibration -------------------------------------------------
  for (uint8_t i = 0; i < SID_TDS_1; ++i) {
    sensors[i].value = applyLinear(sensors[i].raw, sensors[i].cal);
  }
  float tempC = sensors[SID_DS18B20_1].present ? sensors[SID_DS18B20_1].value : 25.0f;
  sensors[SID_TDS_1].value = calcTDSppm(sensors[SID_TDS_1].raw, sensors[SID_TDS_1].cal.tds_k, tempC);
  sensors[SID_TDS_2].value = calcTDSppm(sensors[SID_TDS_2].raw, sensors[SID_TDS_2].cal.tds_k, tempC);
  sensors[SID_DO].value    = applyLinear(calcDO_mgL(sensors[SID_DO].raw), sensors[SID_DO].cal);
  sensors[SID_PH].value    = applyLinear(calcPH(sensors[SID_PH].raw), sensors[SID_PH].cal);
  sensors[SID_MQ137].value = calcMQ137(sensors[SID_MQ137].raw, sensors[SID_MQ137].cal.mq137_baseline);

  // ---- Ultrasonic --------------------------------------------------------
  for (uint8_t i = 0; i < 3; ++i) {
    uint8_t sid = SID_US_1 + i;
    uint8_t trig = (i == 0 ? PIN_US1_TRIG : i == 1 ? PIN_US2_TRIG : PIN_US3_TRIG);
    uint8_t echo = (i == 0 ? PIN_US1_ECHO : i == 1 ? PIN_US2_ECHO : PIN_US3_ECHO);
    sensors[sid].raw = readUltrasonic(trig, echo);
    sensors[sid].present = !isnan(sensors[sid].raw);
    if (sensors[sid].present)
      sensors[sid].value = sensors[sid].raw + sensors[sid].cal.us_offset_cm;
    else
      sensors[sid].value = NAN;
  }

  for (uint8_t i = 0; i < SID_COUNT; ++i) sensors[i].lastReadMs = now;
}

/* ----------------------------- JSON HELPERS --------------------------------*/
static void sendSensorJson(uint8_t clientId) {
  StaticJsonDocument<2048> doc;
  doc["type"] = "sensors";
  JsonArray arr = doc.createNestedArray("data");
  for (uint8_t i = 0; i < SID_COUNT; ++i) {
    JsonObject o = arr.createNestedObject();
    o["id"]     = i;
    o["name"]   = SENSOR_NAMES[i];
    o["present"]= sensors[i].present;
    o["error"]  = sensors[i].errorFlag;
    o["raw"]    = sensors[i].raw;
    o["value"]  = sensors[i].value;
    JsonObject c = o.createNestedObject("cal");
    c["slope"]   = sensors[i].cal.slope;
    c["offset"]  = sensors[i].cal.offset;
    c["us_off"]  = sensors[i].cal.us_offset_cm;
    c["tds_k"]   = sensors[i].cal.tds_k;
    c["mq_b"]    = sensors[i].cal.mq137_baseline;
  }
  String out; serializeJson(doc, out);
  wsServer.sendTXT(clientId, out);
}

static void sendCalibrationResult(uint8_t clientId, uint8_t id, bool ok) {
  StaticJsonDocument<256> doc;
  doc["type"]   = "cal_result";
  doc["id"]     = id;
  doc["ok"]     = ok;
  doc["name"]   = SENSOR_NAMES[id];
  String out; serializeJson(doc, out);
  wsServer.sendTXT(clientId, out);
}

static void applyCalibrationCommand(uint8_t clientId, JsonObject cmd) {
  int id = cmd["id"] | -1;
  if (id < 0 || id >= (int)SID_COUNT) {
    if (clientId != 255) {
      StaticJsonDocument<128> doc;
      doc["type"]="error"; doc["msg"]="invalid sensor id";
      String out; serializeJson(doc, out); wsServer.sendTXT(clientId, out);
    }
    return;
  }
  SensorCalibration &c = sensors[id].cal;
  if (cmd.containsKey("slope"))   c.slope   = cmd["slope"].as<float>();
  if (cmd.containsKey("offset"))  c.offset  = cmd["offset"].as<float>();
  if (cmd.containsKey("us_off"))  c.us_offset_cm = cmd["us_off"].as<float>();
  if (cmd.containsKey("tds_k"))   c.tds_k   = cmd["tds_k"].as<float>();
  if (cmd.containsKey("mq_b"))    c.mq137_baseline = cmd["mq_b"].as<float>();
  c.valid = true;
  saveCalibration(id);
  Serial.printf("[CAL] %s updated: slope=%.4f off=%.4f us_off=%.2f tds_k=%.3f mq_b=%.3f\n",
                SENSOR_NAMES[id], c.slope, c.offset, c.us_offset_cm, c.tds_k, c.mq137_baseline);
  if (clientId != 255) sendCalibrationResult(clientId, id, true);
}

/* ----------------------------- WEBSOCKET EVENTS ----------------------------*/
static void wsEvent(uint8_t clientId, WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED: {
      Serial.printf("[WS] client %u connected\n", clientId);
      StaticJsonDocument<128> doc;
      doc["type"] = "hello"; doc["board"] = "esp32_1_sensors";
      doc["device_id"] = DEVICE_ID;
      String out; serializeJson(doc, out); wsServer.sendTXT(clientId, out);
      sendSensorJson(clientId);
      break;
    }
    case WStype_TEXT: {
      StaticJsonDocument<512> doc;
      if (deserializeJson(doc, payload, len)) return;
      const char* t = doc["type"] | "";
      if (!strcmp(t, "get_sensors"))          sendSensorJson(clientId);
      else if (!strcmp(t, "set_cal"))         applyCalibrationCommand(clientId, doc.as<JsonObject>());
      else if (!strcmp(t, "reset_cal")) {
        int id = doc["id"] | -1;
        if (id < 0) resetAllCalibration();
        else        resetCalibration((uint8_t)id);
        sendSensorJson(clientId);
      }
      break;
    }
    case WStype_DISCONNECTED: Serial.printf("[WS] client %u disconnected\n", clientId); break;
    default: break;
  }
}

/* ----------------------------- HTTP ROUTES ---------------------------------*/
static void handleRoot() {
  httpServer.send(200, "text/plain",
    "Calibrator ESP32 #1 (sensors). Connect via WebSocket.");
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

static void handleSensors() {
  StaticJsonDocument<2048> doc;
  doc["type"] = "sensors";
  JsonArray arr = doc.createNestedArray("data");
  for (uint8_t i = 0; i < SID_COUNT; ++i) {
    JsonObject o = arr.createNestedObject();
    o["id"]=i; o["name"]=SENSOR_NAMES[i];
    o["raw"]=sensors[i].raw; o["value"]=sensors[i].value;
    o["present"]=sensors[i].present; o["error"]=sensors[i].errorFlag;
    JsonObject c = o.createNestedObject("cal");
    c["slope"]=sensors[i].cal.slope; c["offset"]=sensors[i].cal.offset;
    c["us_off"]=sensors[i].cal.us_offset_cm;
    c["tds_k"]=sensors[i].cal.tds_k; c["mq_b"]=sensors[i].cal.mq137_baseline;
  }
  String out; serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

static void handleCalibrationPost() {
  if (!httpServer.hasArg("plain")) { httpServer.send(400); return; }
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, httpServer.arg("plain"))) { httpServer.send(400); return; }
  applyCalibrationCommand(255, doc.as<JsonObject>());
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

static void handleReset() {
  if (!httpServer.hasArg("plain")) { httpServer.send(400); return; }
  StaticJsonDocument<128> doc;
  if (deserializeJson(doc, httpServer.arg("plain"))) { httpServer.send(400); return; }
  int id = doc["id"] | -1;
  if (id < 0) resetAllCalibration(); else resetCalibration((uint8_t)id);
  httpServer.send(200, "application/json", "{\"ok\":true}");
}

static void setupHttp() {
  httpServer.enableCORS(true);
  httpServer.on("/",        HTTP_GET,  handleRoot);
  httpServer.on("/info",    HTTP_GET,  handleInfo);
  httpServer.on("/sensors", HTTP_GET,  handleSensors);
  httpServer.on("/calibration", HTTP_POST, handleCalibrationPost);
  httpServer.on("/reset",   HTTP_POST, handleReset);
  httpServer.on("/calibration", HTTP_OPTIONS, handleCorsPreflight);
  httpServer.on("/reset",   HTTP_OPTIONS, handleCorsPreflight);
  httpServer.on("/info",    HTTP_OPTIONS, handleCorsPreflight);
  httpServer.on("/sensors", HTTP_OPTIONS, handleCorsPreflight);
  httpServer.begin();
  Serial.println("[HTTP] server started on port 80");
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

/* ----------------------------- ARDUINO SETUP -------------------------------*/
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[BOOT] Calibrator ESP32 #1 (sensors) starting");

  // Pin modes
  pinMode(PIN_MQ137_DO, INPUT);
  pinMode(PIN_US1_TRIG, OUTPUT); pinMode(PIN_US1_ECHO, INPUT);
  pinMode(PIN_US2_TRIG, OUTPUT); pinMode(PIN_US2_ECHO, INPUT);
  pinMode(PIN_US3_TRIG, OUTPUT); pinMode(PIN_US3_ECHO, INPUT);
  for (uint8_t t : {PIN_US1_TRIG, PIN_US2_TRIG, PIN_US3_TRIG}) digitalWrite(t, LOW);

  ds18b20_1.begin();
  ds18b20_2.begin();
  ds18b20_1.setResolution(12);
  ds18b20_2.setResolution(12);

  for (uint8_t i = 0; i < SID_COUNT; ++i) loadCalibration(i);

  setupWifi();
  wsServer.begin();
  wsServer.onEvent(wsEvent);
  setupHttp();

  // Watchdog: 10s. Long DS18B20 conversion can stall, so keep it generous.
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

uint32_t lastSensorMs = 0;
uint32_t lastHeartbeatMs = 0;
void loop() {
  esp_task_wdt_reset();
  wsServer.loop();
  httpServer.handleClient();

  uint32_t now = millis();
  if (now - lastSensorMs >= SENSOR_INTERVAL_MS) {
    lastSensorMs = now;
    readAllSensors();
  }
  if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    Serial.printf("[HB] uptime=%lus, wifi=%d, ip=%s\n",
                  (unsigned long)(now/1000), (int)WiFi.RSSI(),
                  WiFi.localIP().toString().c_str());
  }
}
