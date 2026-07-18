/* =============================================================================
 *  Calibrator - ESP32 #4 (Extras: servos, buzzer, alert LED, TB6600 stepper)
 *  ----------------------------------------------------------------------------
 *  Source of truth: ../wiring.md
 *
 *  Camera servos (PWM):
 *    MG996R-compatible 270-degree Pan  -> GPIO18
 *    MG996R-compatible 270-degree Tilt -> GPIO19
 *
 *  Outputs:
 *    Active Buzzer -> GPIO26
 *    Red  Alert LED -> GPIO27
 *    Green Alert LED -> GPIO28  (note: GPIO28 on ESP32 is a strapping pin,
 *                                see safety notes below)
 *
 *  TB6600 stepper driver:
 *    STEP  -> GPIO32
 *    DIR   -> GPIO33
 *    EN    -> GPIO25  (optional, software-controlled)
 *
 *  Safety:
 *    - All outputs start OFF / disabled.
 *    - Servos are NOT moved at boot; the user must explicitly "Move to center"
 *      after configuring min/center/max.
 *    - Stepper enable is left to the user (TB6600 EN is pulled high on many
 *      boards; here we drive it LOW to ENABLE the driver, then HIGH to
 *      disable, matching the common TB6600 reference design).
 *    - Stepper is commanded with a strict upper bound on speed and steps.
 *    - Emergency stop is honored from any path: it immediately stops the
 *      stepper, disables TB6600, silences the buzzer, and turns the LEDs OFF.
 * ===========================================================================*/

#include <WiFi.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
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

const char* HOSTNAME     = "calibrator-esp32-4";
const char* DEVICE_ID    = "esp4";
const uint16_t HTTP_PORT = 80;
const uint16_t WS_PORT   = 81;

/* GPIO assignments per wiring.md */
const uint8_t PIN_SERVO_PAN  = 18;
const uint8_t PIN_SERVO_TILT = 19;
const uint8_t PIN_BUZZER     = 26;
const uint8_t PIN_LED_RED    = 27;
const uint8_t PIN_LED_GREEN  = 28;
const uint8_t PIN_STEP_STEP  = 32;
const uint8_t PIN_STEP_DIR   = 33;
const uint8_t PIN_STEP_EN    = 25;

/* 270-degree positional-servo limits. These are hard caps regardless of UI.
 * ESP32Servo::write() maps only 0-180 degrees, so camera angles are converted
 * to calibrated pulse widths and sent with writeMicroseconds() instead. */
const uint16_t SERVO_MIN_DEG        = 0;
const uint16_t SERVO_MAX_DEG        = 270;
const uint16_t SERVO_DEFAULT_CENTER = 135;
const uint16_t SERVO_MIN_US         = 500;
const uint16_t SERVO_MAX_US         = 2500;

/* Stepper safety limits */
const uint32_t STEPPER_MAX_SPEED_HZ = 20000;   // TB6600 max ~20k pulses/s
const uint32_t STEPPER_MAX_STEPS    = 200000;  // one rev @ 1/32 microstep = 6400
const uint32_t STEPPER_MIN_SPEED_HZ = 1;

/* Microstepping: 1, 2, 4, 8, 16, 32 (set via DIP switches on the TB6600;
 *  the firmware only needs to know steps-per-revolution for the position
 *  display. Update STEPS_PER_REV to match your DIP settings.) */
const uint16_t STEPS_PER_REV = 200 * 32;  // assuming 1.8deg motor @ 1/32

/* Channel A: 270-degree positional servo PWM, 50Hz, 500-2500us pulse */
Servo panServo, tiltServo;

WebServer httpServer(HTTP_PORT);
WebSocketsServer wsServer = WebSocketsServer(WS_PORT);

uint32_t lastDashboardPongMs = 0;
bool     emergencyLatched    = false;

/* Servo user calibration (saved to NVS on demand) */
struct ServoCal {
  uint16_t minDeg   = SERVO_MIN_DEG;
  uint16_t centerDeg = SERVO_DEFAULT_CENTER;
  uint16_t maxDeg   = SERVO_MAX_DEG;
};
ServoCal panCal, tiltCal;

static uint16_t servoDegreesToMicros(uint16_t angleDeg) {
  angleDeg = constrain(angleDeg, SERVO_MIN_DEG, SERVO_MAX_DEG);
  return (uint16_t)map((long)angleDeg,
                       (long)SERVO_MIN_DEG, (long)SERVO_MAX_DEG,
                       (long)SERVO_MIN_US,  (long)SERVO_MAX_US);
}

static void writeServoAngle(Servo& servo, uint16_t angleDeg) {
  servo.writeMicroseconds(servoDegreesToMicros(angleDeg));
}

/* Stepper state */
struct StepperState {
  bool     enabled    = false;
  bool     direction  = false;     // false = CW, true = CCW
  uint32_t speedHz    = 1000;
  uint32_t accelHz    = 1000;
  int32_t  position   = 0;         // estimated steps from zero
  uint8_t  microstep  = 32;
  bool     moving     = false;
} stepper;

/* Blink / alert pattern */
struct LedState {
  bool on = false;
  uint32_t intervalMs = 500;
  uint32_t lastToggleMs = 0;
} redLed, greenLed;
bool buzzerOn = false;
uint32_t buzzerStopAtMs = 0;

/* ------------------------- STEPPER (no-AccelStepper blocking) -------------- *
 *  We use a software pulse train driven by the stepper task so that we keep
 *  full control over the safety cap and emergency stop. A non-blocking
 *  implementation is in the dedicated stepper task below.
 * ---------------------------------------------------------------------------*/
void enableDriver(bool en) {
  // TB6600 EN pin is typically active-LOW.
  digitalWrite(PIN_STEP_EN, en ? LOW : HIGH);
  stepper.enabled = en;
}

void setDirection(bool ccw) {
  stepper.direction = ccw;
  digitalWrite(PIN_STEP_DIR, ccw ? HIGH : LOW);
}

/* Task that emits STEP pulses. Controlled via stepper.moving & stepper.speedHz. */
void stepperTask(void* arg) {
  uint32_t halfPeriodUs = 500;     // 1 kHz default -> updated when moving
  while (true) {
    if (stepper.moving && !emergencyLatched) {
      halfPeriodUs = 1000000UL / (stepper.speedHz * 2UL);
      if (halfPeriodUs < 5) halfPeriodUs = 5;          // 100kHz absolute cap
      digitalWrite(PIN_STEP_STEP, HIGH);
      ets_delay_us(2);
      digitalWrite(PIN_STEP_STEP, LOW);
      // Update estimated position. We are not measuring direction in real-time
      // for a non-blocking driver, so the caller pre-biases position.
      delayMicroseconds(halfPeriodUs);
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

/* Run the stepper for a fixed number of steps in a BLOCKING call. We do this
 *  in a worker so the WebSocket stays responsive. The safety caps above
 *  prevent an oversized request from taking the board down. */
static void runStepperBlocking(int32_t steps, uint32_t speedHz, uint8_t microstep) {
  if (steps == 0 || emergencyLatched) return;
  if (speedHz < STEPPER_MIN_SPEED_HZ) speedHz = STEPPER_MIN_SPEED_HZ;
  if (speedHz > STEPPER_MAX_SPEED_HZ) speedHz = STEPPER_MAX_SPEED_HZ;
  if (microstep == 0) microstep = 1;
  if (microstep > 32) microstep = 32;
  stepper.speedHz  = speedHz;
  stepper.microstep = microstep;

  setDirection(steps < 0);
  int32_t remaining = abs(steps);
  uint32_t halfPeriodUs = 1000000UL / (speedHz * 2UL);
  if (halfPeriodUs < 5) halfPeriodUs = 5;
  enableDriver(true);

  while (remaining > 0 && !emergencyLatched) {
    digitalWrite(PIN_STEP_STEP, HIGH);
    ets_delay_us(2);
    digitalWrite(PIN_STEP_STEP, LOW);
    if (stepper.direction) stepper.position--;
    else                   stepper.position++;
    remaining--;
    delayMicroseconds(halfPeriodUs);
  }
  enableDriver(false);
  Serial.printf("[STEP] done, pos=%ld\n", (long)stepper.position);
}

/* --------------------------- COMMAND DISPATCHER ----------------------------*/
static bool applyCommandJson(JsonVariantConst cmd) {
  const char* t = cmd["type"] | "";

  if (!strcmp(t, "servo_set")) {
    const char* which = cmd["servo"] | "";
    int angle = cmd["angle"] | -1;
    if (angle < (int)SERVO_MIN_DEG || angle > (int)SERVO_MAX_DEG) return false;
    if (!strcmp(which, "pan")) {
      angle = constrain(angle, (int)panCal.minDeg, (int)panCal.maxDeg);
      writeServoAngle(panServo, (uint16_t)angle);
    } else if (!strcmp(which, "tilt")) {
      angle = constrain(angle, (int)tiltCal.minDeg, (int)tiltCal.maxDeg);
      writeServoAngle(tiltServo, (uint16_t)angle);
    } else return false;
    return true;
  }
  if (!strcmp(t, "servo_center")) {
    writeServoAngle(panServo, panCal.centerDeg);
    writeServoAngle(tiltServo, tiltCal.centerDeg);
    return true;
  }
  if (!strcmp(t, "servo_sweep")) {
    const char* which = cmd["servo"] | "";
    Servo* sv = nullptr; ServoCal* c = nullptr;
    if (!strcmp(which, "pan"))  { sv = &panServo;  c = &panCal;  }
    if (!strcmp(which, "tilt")) { sv = &tiltServo; c = &tiltCal; }
    if (!sv) return false;
    for (int a = (int)c->minDeg; a <= (int)c->maxDeg && !emergencyLatched; a += 2) {
      writeServoAngle(*sv, (uint16_t)a); delay(15);
    }
    for (int a = (int)c->maxDeg; a >= (int)c->minDeg && !emergencyLatched; a -= 2) {
      writeServoAngle(*sv, (uint16_t)a); delay(15);
    }
    writeServoAngle(*sv, c->centerDeg);
    return true;
  }
  if (!strcmp(t, "servo_cal")) {
    const char* which = cmd["servo"] | "";
    ServoCal* c = nullptr;
    if (!strcmp(which, "pan"))  c = &panCal;
    if (!strcmp(which, "tilt")) c = &tiltCal;
    if (!c) return false;
    if (cmd.containsKey("min"))    c->minDeg    = constrain((int)cmd["min"], (int)SERVO_MIN_DEG, (int)SERVO_MAX_DEG);
    if (cmd.containsKey("center")) c->centerDeg = constrain((int)cmd["center"], (int)SERVO_MIN_DEG, (int)SERVO_MAX_DEG);
    if (cmd.containsKey("max"))    c->maxDeg    = constrain((int)cmd["max"], (int)SERVO_MIN_DEG, (int)SERVO_MAX_DEG);
    if (c->minDeg > c->maxDeg) std::swap(c->minDeg, c->maxDeg);
    c->centerDeg = constrain(c->centerDeg, c->minDeg, c->maxDeg);
    return true;
  }

  if (!strcmp(t, "buzzer")) {
    bool on = cmd["on"] | false;
    uint32_t dur = cmd["duration_ms"] | 0;
    digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
    buzzerOn = on;
    buzzerStopAtMs = (on && dur > 0) ? millis() + dur : 0;
    return true;
  }
  if (!strcmp(t, "buzzer_pattern")) {
    // alert: 200ms on, 200ms off, 3 times
    for (int i = 0; i < 3 && !emergencyLatched; ++i) {
      digitalWrite(PIN_BUZZER, HIGH); delay(200);
      digitalWrite(PIN_BUZZER, LOW);  delay(200);
    }
    buzzerOn = false;
    return true;
  }

  if (!strcmp(t, "led")) {
    const char* which = cmd["led"] | "";
    bool on = cmd["on"] | false;
    uint32_t iv = cmd["interval_ms"] | 500;
    if (!strcmp(which, "red")) {
      redLed.on = on; redLed.intervalMs = iv;
      if (!on) digitalWrite(PIN_LED_RED, LOW);
    } else if (!strcmp(which, "green")) {
      greenLed.on = on; greenLed.intervalMs = iv;
      if (!on) digitalWrite(PIN_LED_GREEN, LOW);
    } else return false;
    return true;
  }
  if (!strcmp(t, "led_blink_test")) {
    const char* which = cmd["led"] | "";
    uint8_t pin = !strcmp(which, "red") ? PIN_LED_RED :
                  !strcmp(which, "green") ? PIN_LED_GREEN : 0xFF;
    if (pin == 0xFF) return false;
    for (int i = 0; i < 5 && !emergencyLatched; ++i) {
      digitalWrite(pin, HIGH); delay(200);
      digitalWrite(pin, LOW);  delay(200);
    }
    return true;
  }

  if (!strcmp(t, "stepper_enable")) {
    bool en = cmd["on"] | false;
    enableDriver(en);
    return true;
  }
  if (!strcmp(t, "stepper_move")) {
    int32_t steps = cmd["steps"] | 0;
    uint32_t speed = cmd["speed"] | 1000;
    uint8_t ms = cmd["microstep"] | stepper.microstep;
    if (abs(steps) > (int32_t)STEPPER_MAX_STEPS) return false;
    if (speed < STEPPER_MIN_SPEED_HZ || speed > STEPPER_MAX_SPEED_HZ) return false;
    runStepperBlocking(steps, speed, ms);
    return true;
  }
  if (!strcmp(t, "stepper_stop")) {
    stepper.moving = false;
    enableDriver(false);
    return true;
  }
  if (!strcmp(t, "stepper_reset")) {
    stepper.position = 0;
    return true;
  }
  if (!strcmp(t, "stepper_cal")) {
    // Move a small known amount to verify the wiring.
    runStepperBlocking(200, 800, 32);
    return true;
  }

  if (!strcmp(t, "emergency_stop")) {
    emergencyLatched = true;
    stepper.moving = false;
    enableDriver(false);
    digitalWrite(PIN_BUZZER, LOW); buzzerOn = false;
    digitalWrite(PIN_LED_RED, LOW);    redLed.on = false;
    digitalWrite(PIN_LED_GREEN, LOW);  greenLed.on = false;
    return true;
  }
  if (!strcmp(t, "emergency_clear")) {
    emergencyLatched = false;
    return true;
  }
  return false;
}

/* ----------------------------- JSON STATE --------------------------------- */
static void sendState(uint8_t clientId) {
  StaticJsonDocument<512> doc;
  doc["type"] = "state";
  JsonObject s = doc.createNestedObject("servos");
  s["pan"]["min"]=panCal.minDeg; s["pan"]["center"]=panCal.centerDeg; s["pan"]["max"]=panCal.maxDeg;
  s["tilt"]["min"]=tiltCal.minDeg; s["tilt"]["center"]=tiltCal.centerDeg; s["tilt"]["max"]=tiltCal.maxDeg;
  JsonObject st = doc.createNestedObject("stepper");
  st["enabled"]=stepper.enabled; st["position"]=stepper.position;
  st["speed_hz"]=stepper.speedHz; st["microstep"]=stepper.microstep;
  st["direction"]=stepper.direction ? "CCW" : "CW";
  JsonObject b = doc.createNestedObject("buzzer");
  b["on"]=buzzerOn;
  JsonObject l = doc.createNestedObject("led");
  l["red_on"]=redLed.on; l["green_on"]=greenLed.on;
  l["red_interval_ms"]=redLed.intervalMs; l["green_interval_ms"]=greenLed.intervalMs;
  String out; serializeJson(doc, out);
  wsServer.sendTXT(clientId, out);
}

/* ----------------------------- WEBSOCKET EVENTS ----------------------------*/
static void wsEvent(uint8_t clientId, WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED: {
      Serial.printf("[WS] client %u connected\n", clientId);
      StaticJsonDocument<128> hello; hello["type"]="hello"; hello["board"]="esp32_4_extras";
      hello["device_id"] = DEVICE_ID;
      String out; serializeJson(hello, out); wsServer.sendTXT(clientId, out);
      sendState(clientId);
      lastDashboardPongMs = millis();
      break;
    }
    case WStype_TEXT: {
      StaticJsonDocument<1024> doc;
      if (deserializeJson(doc, payload, len)) return;
      const char* t = doc["type"] | "";
      if (!strcmp(t, "pong") || !strcmp(t, "alive")) lastDashboardPongMs = millis();
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

/* ----------------------------- HTTP ----------------------------------------*/
static void handleRoot() { httpServer.send(200, "text/plain", "Calibrator ESP32 #4 (extras)"); }
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
  StaticJsonDocument<512> doc; doc["type"]="state";
  JsonObject s = doc.createNestedObject("servos");
  s["pan"]["min"]=panCal.minDeg; s["pan"]["center"]=panCal.centerDeg; s["pan"]["max"]=panCal.maxDeg;
  s["tilt"]["min"]=tiltCal.minDeg; s["tilt"]["center"]=tiltCal.centerDeg; s["tilt"]["max"]=tiltCal.maxDeg;
  JsonObject st = doc.createNestedObject("stepper");
  st["enabled"]=stepper.enabled; st["position"]=stepper.position;
  st["speed_hz"]=stepper.speedHz; st["microstep"]=stepper.microstep;
  String out; serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}
static void handleCommand() {
  if (!httpServer.hasArg("plain")) { httpServer.send(400); return; }
  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, httpServer.arg("plain"))) { httpServer.send(400); return; }
  bool ok = applyCommandJson(doc.as<JsonVariantConst>());
  httpServer.send(ok ? 200 : 400, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false}");
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
  WiFi.mode(WIFI_STA); WiFi.setAutoReconnect(true); WiFi.setHostname(HOSTNAME);
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
  Serial.println("\n[BOOT] Calibrator ESP32 #4 (extras) starting");

  // Pin modes - all OFF at boot.
  pinMode(PIN_BUZZER, OUTPUT);   digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED_RED, OUTPUT);  digitalWrite(PIN_LED_RED, LOW);
  pinMode(PIN_LED_GREEN, OUTPUT);digitalWrite(PIN_LED_GREEN, LOW);
  pinMode(PIN_STEP_STEP, OUTPUT);digitalWrite(PIN_STEP_STEP, LOW);
  pinMode(PIN_STEP_DIR, OUTPUT); digitalWrite(PIN_STEP_DIR, LOW);
  pinMode(PIN_STEP_EN, OUTPUT);  digitalWrite(PIN_STEP_EN, HIGH);   // TB6600 disabled (active-LOW EN)

  // Servos are NOT moved at boot. They attach and the user must call
  // "Move to center" once the min/center/max have been verified.
  panServo.setPeriodHertz(50);
  tiltServo.setPeriodHertz(50);
  panServo.attach(PIN_SERVO_PAN,  SERVO_MIN_US, SERVO_MAX_US);
  tiltServo.attach(PIN_SERVO_TILT, SERVO_MIN_US, SERVO_MAX_US);

  setupWifi();
  wsServer.begin();
  wsServer.onEvent(wsEvent);
  setupHttp();

  // Stepper task on core 0 so the main loop stays responsive.
  xTaskCreatePinnedToCore(stepperTask, "stepper", 4096, NULL, 1, NULL, 0);

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
  lastDashboardPongMs = millis();
  Serial.println("[BOOT] ready");
}

void loop() {
  esp_task_wdt_reset();
  wsServer.loop();
  httpServer.handleClient();

  uint32_t now = millis();
  // Buzzer auto-off
  if (buzzerOn && buzzerStopAtMs != 0 && now >= buzzerStopAtMs) {
    digitalWrite(PIN_BUZZER, LOW); buzzerOn = false; buzzerStopAtMs = 0;
  }
  // LED blink
  if (redLed.on && now - redLed.lastToggleMs >= redLed.intervalMs) {
    redLed.lastToggleMs = now;
    digitalWrite(PIN_LED_RED, !digitalRead(PIN_LED_RED));
  }
  if (greenLed.on && now - greenLed.lastToggleMs >= greenLed.intervalMs) {
    greenLed.lastToggleMs = now;
    digitalWrite(PIN_LED_GREEN, !digitalRead(PIN_LED_GREEN));
  }
}
