# Setup Guide

This guide takes you from a fresh checkout to a fully running rig.

## 1. Hardware preparation

1. Wire every component exactly as described in `wiring.md`. Do not change
   pin numbers in the firmware — the dashboards and API rely on them.
2. Provide a stable 5 V supply for the ESP32 boards and any 5 V sensors.
3. **Add a hardware E-stop in series with the mains side of every relay
   load.** The firmware emergency stop is a software convenience, not a
   safety device.

## 2. Arduino IDE configuration

1. Install the **ESP32 board support** (Espressif):
   `File > Preferences > Additional boards manager URLs`:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
2. `Tools > Board > ESP32 Arduino > ESP32 Dev Module`.
3. Install the libraries listed in the README (Library Manager).

## 3. Configure each firmware sketch

Open every `firmware/*/*.ino` and edit the `WIFI_CONFIG` block:

```cpp
const WiFiCredentials WIFI_CONFIG = {
  "YOUR_SSID",
  "YOUR_PASSWORD"
};
```

Keep IP assignment on automatic DHCP. Each ESP32 will receive its own unique
local IP; do not put the same static IP in multiple sketches. Keep the supplied
hostnames unique:

| Board | Stable dashboard address |
|---|---|
| ESP32 #1 | `calibrator-esp32-1.local` |
| ESP32 #2 | `calibrator-esp32-2.local` |
| ESP32 #3 | `calibrator-esp32-3.local` |
| ESP32 #4 | `calibrator-esp32-4.local` |

The firmware prints both its DHCP address and Wi-Fi MAC address in Serial
Monitor. For the most reliable installation, create one DHCP reservation per
MAC address in your router. This keeps the numeric addresses stable without
hard-coding network details in the firmware.

## 4. Upload firmware

For each ESP32:

1. Plug the board in via USB.
2. Open its `.ino` file in the Arduino IDE.
3. Select the right serial port.
4. Click **Upload**.
5. Open the **Serial Monitor** at 115200 baud. You should see the
   assigned IP address and a `[BOOT] ready` line.

Repeat for the other three boards. Each IP must be different. Normally the
dashboard uses the stable `.local` hostname, so you only need to write down the
IP addresses if your network does not resolve mDNS names.

## 5. Run the dashboard

The website is a static bundle. Easiest path is the built-in Python server:

```bash
cd website
python -m http.server 8000
```

Open `http://localhost:8000` in Chrome / Edge / Firefox.

In the **Connection** tab:

- Leave the four recommended `.local` hostnames in place.
- Click **Save Addresses**, then **Connect All**.
- If `.local` does not resolve on your network, enter the four unique IPs
  printed by the firmware instead.
- The connection cards should turn green within a few seconds.

You can also double-click `index.html` to open it directly, but some
browsers restrict `WebSocket` from `file://` origins. Use the static
server if you see a "WebSocket connection failed" error.

## 6. Smoke test

1. On the **Relay 16** tab, click **All OFF** to confirm the firmware
   responds.
2. Run **Sequential Test** for a few cycles. If the relays click but no
   load switches, you are powering the relay board from the same supply
   that drives the ESP32 without enough current.
3. On the **Sensors** tab, the cards should populate within a few seconds
   of the WebSocket opening.
4. On the **Extras** tab, click **Move to center** for each servo. The
   servos should sweep to 90°.

## 7. Update cycle

After editing any `.ino`, just **Upload** again. Calibration values stored
in `Preferences` are preserved across uploads unless you explicitly erase
the flash with `Tools > Erase Flash > All Flash Contents`.
