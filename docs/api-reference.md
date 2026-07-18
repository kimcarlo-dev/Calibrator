# API Reference

All four ESP32 boards expose a tiny HTTP REST API and a WebSocket on port 81.

- HTTP base:  `http://<ip>/`
- WebSocket:  `ws://<ip>:81/`
- Stable names: `calibrator-esp32-1.local` through
  `calibrator-esp32-4.local`

Every board also exposes `GET /info` for safe discovery and identity checking:

```json
{
  "type": "device_info",
  "device_id": "esp1",
  "hostname": "calibrator-esp32-1",
  "ip": "192.168.1.51",
  "mac": "AA:BB:CC:DD:EE:FF",
  "http_port": 80,
  "ws_port": 81
}
```

`device_id` is uniquely assigned by role (`esp1` through `esp4`). The website
checks it before accepting a WebSocket connection, which prevents an address
entered under the wrong board from controlling the wrong hardware.

The exact route set differs per board; the JSON **command** schema is the
same. Every command sent over WebSocket is acknowledged with a small JSON
`{ "type":"ack", "cmd":<name>, "ok":<bool> }` reply.

---

## ESP32 #1 (sensors)

### WebSocket commands

```jsonc
{ "type": "get_sensors" }                    // -> pushes a 'sensors' message
{ "type": "set_cal", "id": 0, "slope": 1.0, "offset": 0.0,
                            "us_off": 0, "tds_k": 1, "mq_b": 1 }
{ "type": "reset_cal", "id": 0 }             // or "id": -1 to reset all
{ "type": "pong" }                           // keep-alive
```

### HTTP

- `GET  /sensors` — full sensor snapshot as JSON.
- `POST /calibration` — body: a `set_cal` payload.
- `POST /reset` — body: `{ "id": 0 }` (or omit `id` to reset all).

### `sensors` message

```json
{
  "type": "sensors",
  "data": [
    { "id":0, "name":"DS18B20 #1", "present":true, "error":false,
      "raw":24.6, "value":24.6,
      "cal": {"slope":1, "offset":0, "us_off":0, "tds_k":1, "mq_b":1} }
  ]
}
```

Sensor IDs: 0=DS18B20 #1, 1=DS18B20 #2, 2=TDS #1, 3=TDS #2, 4=DO,
5=pH, 6=MQ137, 7=US #1, 8=US #2, 9=US #3.

---

## ESP32 #2 (16-channel relay)

```jsonc
{ "type":"get_state" }
{ "type":"set",          "channel": 1, "on": true }      // channel 1..16
{ "type":"all_off" }
{ "type":"all_on",       "confirm": true }
{ "type":"emergency_stop" }
{ "type":"test_channel", "channel": 5, "duration_ms": 800 }
{ "type":"test_sequential", "duration_ms": 800, "gap_ms": 200 }
{ "type":"rename",       "channel": 3, "name": "Aerator" } // RAM only
```

Normal `set` and `all_on` states persist until an explicit OFF command,
emergency stop, or ESP32 reboot. `test_channel` is intentionally a pulse: it
turns the selected relay OFF after `duration_ms`. The optional
`RELAY_FAILSAFE_ON_CONNECTION_LOSS` firmware setting defaults to `false`; only
enable it when every attached load is safe to turn OFF after a dashboard or
Wi-Fi interruption.

HTTP:
- `GET  /state`
- `POST /cmd` (body: any of the commands above)

---

## ESP32 #3 (8 + 4 channel relay)

Same schema as ESP32 #2, with `channel` in `1..12` and a board selector:

```jsonc
{ "type":"all_off_board", "board": "8ch" }     // or "4ch"
```

---

## ESP32 #4 (extras)

```jsonc
{ "type":"servo_set",      "servo":"pan",  "angle": 90 }
{ "type":"servo_center" }
{ "type":"servo_sweep",    "servo":"tilt" }
{ "type":"servo_cal",      "servo":"pan",  "min":0, "center":90, "max":180 }

{ "type":"buzzer",         "on": true,  "duration_ms": 500 }
{ "type":"buzzer",         "on": false }
{ "type":"buzzer_pattern" }

{ "type":"led",            "led":"red",   "on": true,  "interval_ms": 500 }
{ "type":"led",            "led":"green", "on": false }
{ "type":"led_blink_test", "led":"red" }

{ "type":"stepper_enable", "on": true }
{ "type":"stepper_move",   "steps": 1600, "speed": 1000, "microstep": 32 }
{ "type":"stepper_stop" }
{ "type":"stepper_cal" }                         // 200 step sanity motion
{ "type":"stepper_reset" }                       // zero internal counter
{ "type":"emergency_stop" }                      // disables stepper, all OFF
{ "type":"emergency_clear" }
```

### `state` message

```json
{
  "type": "state",
  "servos": {
    "pan":  { "min":0, "center":90, "max":180 },
    "tilt": { "min":0, "center":90, "max":180 }
  },
  "stepper": { "enabled":false, "position":0, "speed_hz":1000,
               "microstep":32, "direction":"CW" },
  "buzzer":  { "on":false },
  "led":     { "red_on":false, "green_on":false,
               "red_interval_ms":500, "green_interval_ms":500 }
}
```

---

## Error format

```json
{ "type":"error", "msg":"invalid sensor id" }
```

The HTTP layer returns `400` with `{"ok":false,"error":"..."}` for any
rejected command.
