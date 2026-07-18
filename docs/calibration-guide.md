# Calibration Guide

This guide walks through calibrating every sensor in `wiring.md`. Each
calibration value is stored in NVS (Preferences) on ESP32 #1 and survives
power cycles.

You can apply calibration values from the **Sensors** tab of the dashboard.
The fields per sensor are:

- `slope` and `offset` — generic linear correction: `value = raw * slope + offset`.
- `us_off` — additive cm offset for the JSN-SR04T distance reading.
- `tds_k` — TDS cell constant.
- `mq_b` — MQ137 baseline (Rs/R0).

## 1. DS18B20 temperature sensors

The DS18B20 is factory-calibrated; only a small offset is usually needed.

1. Submerge both probes in an ice bath (0 °C) and a known warm bath.
2. In the dashboard, set the **slope = 1.0** and adjust **offset** so the
   displayed °C matches the reference thermometer.

## 2. pH sensor (PH4502C)

Use commercial pH buffer solutions (4.01, 6.86, 10.01).

1. Rinse the probe, place it in pH 6.86 buffer.
2. Note the **raw** voltage shown in the dashboard. Compute
   `slope = 6.86 / raw_v`.
3. Rinse and place the probe in pH 4.01 buffer. Compute
   `offset = 4.01 - slope * raw_v`.
4. Apply both values, then verify with the third buffer.

## 3. TDS sensors

TDS meters respond to temperature, so the firmware already applies a 2 %
per °C compensation using `DS18B20 #1`.

1. Submerge in a known TDS solution (e.g. 1413 µS/cm ˜ 707 ppm).
2. Adjust `tds_k` until the dashboard ppm reading matches the solution.
3. Repeat with a second solution to confirm linearity.

## 4. Dissolved oxygen (DFRobot)

The DO probe needs a 2-point calibration in the open air and in
zero-oxygen solution (sodium sulphite).

1. In air-saturated water, set the **slope = 1.0** and adjust **offset**
   so the dashboard reading matches 8.0 mg/L.
2. In zero-oxygen solution, adjust **slope** so the reading matches 0.0 mg/L.

## 5. MQ137 ammonia sensor

MQ137 needs a baseline captured in clean air.

1. Power the sensor for 24 h in clean air.
2. From the dashboard, set `slope = 1.0` and `offset = 0`. Note the **raw**
   voltage and enter it as `mq_b`.
3. Expose the sensor to a known gas concentration to fine-tune the slope
   if needed.

## 6. JSN-SR04T ultrasonic sensors

1. Place a flat target at a measured distance (e.g. 50 cm) in front of the
   sensor.
2. Adjust `us_off` so the dashboard `value` matches the measured distance.
3. Repeat at a second distance to confirm linearity.

## 7. Servos (ESP32 #4)

1. Use the Pan / Tilt sliders in the **Extras** tab.
2. Move each servo to the minimum mechanical angle and set `Min`.
3. Move to the maximum and set `Max`.
4. Set `Center`.
5. Click **Save cal**. The values are kept in RAM on the ESP32 #4 for the
   current session; future firmware revisions can persist them via
   Preferences.

## 8. Stepper motor (TB6600)

The stepper has no electronic calibration, but a "Calibration move" button
emits 200 forward steps so you can verify the wiring direction and
microstepping. After physical assembly, click **Reset position to 0** to
zero the internal counter.
