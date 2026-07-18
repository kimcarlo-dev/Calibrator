# Arduino Libraries

A single inventory of every required library, ESP32-bundled library, and
optional alternative for the four firmware sketches. Items marked **bundled**
arrive with the ESP32 board package; the others can be installed through the
Arduino IDE **Library Manager** (`Sketch > Include Library > Manage Libraries…`).

## Required and bundled libraries

| Library | Used by | Why we need it | Notes |
|---------|---------|----------------|-------|
| **WiFi**            | All four sketches | Built-in station-mode Wi-Fi connection | Bundled with the **esp32** board package (no install needed). |
| **ESPmDNS**         | All four sketches | Advertises a stable, unique `.local` hostname for each DHCP client | Bundled with the **esp32** board package. |
| **WebServer**       | All four sketches | Tiny HTTP server for `/state`, `/sensors`, `/calibration`, `/cmd`, etc. | Bundled with the **esp32** board package. |
| **WebSocketsServer**| All four sketches | Real-time command/ack channel on port 81 | Author: **Markus Sattler**. Search the Library Manager for `WebSocketsServer` and install the latest release. |
| **ArduinoJson**     | All four sketches | Parse and build the JSON messages | Author: **Benoit Blanchon**. Install version **6.x** (the firmware uses the v6 API). |
| **Preferences**     | ESP32 #1 (sensors) | Saves calibration values to NVS | Bundled with the **esp32** board package. |
| **OneWire**         | ESP32 #1 (sensors) | DS18B20 1-Wire bus driver | Author: **Paul Stoffregen**. |
| **DallasTemperature** | ESP32 #1 (sensors) | High-level DS18B20 API (parses ROM codes, starts conversion) | Author: **Miles Burton**. |
| **ESP32Servo**      | ESP32 #4 (extras) | MG996R servo PWM on any GPIO | Author: **Kevin Harrington** (also available as a built-in fork via the ESP32 board package — either is fine). |
| **esp_task_wdt**    | All four sketches | Watchdog timer so a stuck loop causes a clean reboot | Bundled with the **esp32** board package. |

## Installed together with the ESP32 board package

These are not "libraries" in the Library Manager sense — they ship with the
**esp32** core by Espressif. If the Arduino IDE complains that any of them
is missing, you do not have the esp32 core installed yet.

`Tools > Board > Boards Manager > esp32 > Install` (version **2.0.x** or newer).

## Install steps (quick)

1. **Add the board URL** in `File > Preferences > Additional boards manager URLs`:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
2. **Install the ESP32 core** via `Tools > Board > Boards Manager`.
3. **Install the five external libraries** above via `Sketch > Include Library > Manage Libraries…`: `WebSocketsServer`, `ArduinoJson`, `OneWire`, `DallasTemperature`, and `ESP32Servo`.
4. **Select the board**: `Tools > Board > ESP32 Arduino > ESP32 Dev Module`.
5. **Open each sketch** in `firmware/*/*.ino` and **Upload**.

## Optional libraries and alternatives

These are included for completeness and may be installed, but the supplied
firmware compiles without them because it uses the implementations noted below.

| Library | Possible use | What the supplied firmware uses instead |
|---------|--------------|-----------------------------------------|
| **NewPing** | JSN-SR04T ultrasonic helper | A bounded `pulseIn()` measurement in ESP32 #1. |
| **DFRobot_DO** | DFRobot dissolved-oxygen probe helper | Analog input with generic slope/offset calibration. |
| **AsyncTCP** | Asynchronous TCP transport | The synchronous networking bundled with the ESP32 core. |
| **ESPAsyncWebServer** | Asynchronous HTTP routes | Bundled `WebServer` plus `WebSocketsServer`. |
| **AccelStepper** | Stepper acceleration and motion profiles | Direct TB6600 step/dir control with explicit safety caps and emergency stop. |

If you want every alternative available in the Arduino IDE, install these five
as well. Do not add their `#include` directives to the supplied sketches unless
you also replace the corresponding direct implementation.

## Recommended versions (known-good)

| Library | Version that compiles against the provided sketches |
|---------|------------------------------------------------------|
| esp32 core | 2.0.14 or newer (3.x also fine) |
| WebSocketsServer | 2.3.x or newer |
| ArduinoJson | 6.21.x or newer (do **not** pick 7.x — the v6 API is used) |
| OneWire | 2.3.8 or newer |
| DallasTemperature | 3.11.0 or newer |
| ESP32Servo | 1.1.0 or newer (or the bundled version) |

After installing, restart the Arduino IDE once so it picks up the new
toolchain files, then upload `firmware/esp32_1_sensors/esp32_1_sensors.ino`
first to confirm everything compiles.
