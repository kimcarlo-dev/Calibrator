# Calibrator

A four-ESP32 calibration and testing rig for a hydroponics / aquaponics
controller. The single source of truth for every GPIO, sensor, and relay
assignment is [`wiring.md`](wiring.md); this repository is generated from
that file and **must not** redefine any pin.

## Folder structure

```
calibrator/
  README.md                <- you are here
  wiring.md                <- authoritative pin/sensor map (do not edit lightly)
  website/                 <- browser dashboard (HTML/CSS/JS, no build step)
    index.html
    style.css
    app.js
    assets/                <- logos, icons, etc.
  firmware/
    esp32_1_sensors/       <- 2x DS18B20, 2x TDS, DO, pH, MQ137, 3x JSN-SR04T
    esp32_2_16channel_relay/  <- 16-channel relay module
    esp32_3_8channel_4channel_relay/  <- 8-CH + 4-CH relay modules
    esp32_4_extras/        <- 2x servos, buzzer, alert LEDs, TB6600 stepper
  docs/
    calibration-guide.md
    api-reference.md
    setup-guide.md
```

## ESP32 responsibilities

| Board | Responsibilities |
|-------|------------------|
| #1 sensors | All analog/OneWire/ultrasonic sensors. Calibration storage via Preferences. |
| #2 16-relay | 16-channel relay module. Per-channel ON/OFF/pulse test, all-OFF, all-ON (with confirm), sequential test, optional connection-loss fail-safe. |
| #3 8+4 relay | Two relay modules (8 + 4 channels). Independent board-level all-OFF, emergency stop, optional connection-loss fail-safe. |
| #4 extras  | Pan/tilt servos, buzzer, red+green alert LEDs, TB6600 stepper driver. |

## Required Arduino libraries

Install via the Arduino IDE Library Manager (or PlatformIO `lib_deps`):

- `WiFi` (bundled with the ESP32 core)
- `ESPmDNS` (bundled; provides the stable `.local` hostnames)
- `WebServer` (bundled)
- `WebSocketsServer` by Markus Sattler
- `ArduinoJson` by Benoit Blanchon (v6.x)
- `OneWire`
- `DallasTemperature`
- `ESP32Servo` (built-in fork, by Kevin Harrington)
- `Preferences` (bundled)
- `esp_task_wdt` (bundled)

Optional alternatives are also documented: `NewPing`, `DFRobot_DO`,
`AsyncTCP`, `ESPAsyncWebServer`, and `AccelStepper`. The supplied firmware does
not require them, but they are included in the complete inventory in
[`libraries.md`](libraries.md).

## Quick start

1. Edit `wiring.md` if you need to re-pin anything (then update the firmware
   to match).
2. Open each `firmware/*/*.ino` in the Arduino IDE.
3. Set your Wi-Fi SSID/password in the marked `WIFI_CONFIG` block at the top
   of every sketch. Leave DHCP enabled and keep the supplied hostnames unique;
   each ESP32 will receive a different IP automatically.
4. Select the matching ESP32 board and the right **Partition Scheme** if you
   store large Preferences (default works fine).
5. Upload each sketch and confirm that Serial Monitor prints a different DHCP
   IP and the expected `.local` hostname for every board.
6. Open `website/index.html` in any modern browser and click **Connect All**.
   The recommended hostnames are pre-filled; unique numeric IPs remain a
   fallback for networks that do not resolve mDNS.

Detailed walkthroughs live in `docs/`.

## Safety warnings (read before powering anything)

- Relay boards may switch **mains AC**. Never test relays with anything
  dangerous attached while you are still writing firmware. Keep an
  E-stop (a hardwired switch on the load side) in series.
- All relays default **OFF** at boot. The firmware enforces this; do not
  bypass it.
- The TB6600 stepper enable line is held in **disable** at boot. The motor
  must never move during boot. Use the **EMERGENCY STOP** button if needed.
- Disconnect inductive loads (motors, solenoids) before firmware upload.
- The DS18B20, pH, MQ137, and ultrasonic sensors in this project operate at
  3.3 V or 5 V only. Never connect them to mains.

## Troubleshooting

- **ESP32 cannot connect to Wi-Fi.** Double-check `WIFI_CONFIG`. Some 2.4 GHz
  networks are incompatible; try a 2.4 GHz-only SSID.
- **A `.local` hostname does not resolve.** Enter that board's unique DHCP IP
  from Serial Monitor, and create a DHCP reservation in the router using the
  printed MAC address. Never reuse one IP for multiple boards.
- **Wrong IP shown.** The IP is printed at boot; also check your router's
  DHCP table or use the ESP32's mDNS name (`calibrator-esp32-N.local`).
- **WebSocket fails to open.** Some browsers block WS on HTTP pages served
  from `file://`. Serve the `website/` folder with a tiny static server
  (`python -m http.server`) and open `http://localhost:8000`.
- **A relay turns OFF by itself.** Use **ON** for a persistent state; **Pulse**
  and sequential tests intentionally turn relays OFF after their test time.
  The connection-loss fail-safe is disabled by default. If Serial Monitor
  shows repeated `[BOOT]` messages, the ESP32 is rebooting—check for supply
  voltage drop and power relay coils from a properly rated separate supply.
- **Relays click but the load does not switch.** You may be powering the
  relay board from the same supply that drives the ESP32 without enough
  current. Provide a separate 5 V supply for the relay coils.
