# chest (nRF52840 Pro Micro + Ra-02 + MPU6050 + SOS + buzzer)

Production-direction chest firmware. Port of firmware/node main.c/transport.c.
`firmware/node/src/node_core.c` is compiled unchanged through `src/node_core_shared.c`.
HR/SpO2 come from firmware/wrist over BLE (`wrist_ble.h`). Battery = 65535 (not measured).

Pins: LoRa SCK P0.08, MISO P0.17, MOSI P0.06, NSS P0.20, RST P0.24, DIO0 P1.00;
MPU SDA P0.11, SCL P0.22 (bit-banged I2C); SOS P1.11 to GND (internal pull-up);
buzzer P1.06 via C1815; LED P0.15; 3.3 V rail switch P0.13 HIGH.

Serial log (USB, 115200): boot line, `SNAP ...` every 5 s, `EVENT ...`, `SOS pin ...`.

## Verified 2026-11-14 (desk, USB power)
- MPU6050: 250 samples per 5 s window, lost 0, IMU_VALID set.
- LoRa: snapshots received by the station and stored on the Pi (boot_id 1/2, consecutive sequences).
- Journal (LittleFS): boot_id increments across reboots (1 → 2).
- SOS: short taps (0.19 s, 0.34 s) are correctly ignored (hold 1 s rule). A 2.8 s
  hold produced MANUAL_SOS seq 25; a new 5.7 s hold produced LOCAL_CLEAR.
- Pi events table: `1:2:25 MANUAL_SOS ALARM local_cleared=1`, waiting for operator ack/resolve.
- The gateway forwarded the SOS frame once (it deduplicates repeats by design) and LOCAL_CLEAR 3 times.
- The snapshot covering the SOS lost 17 IMU samples (SENSOR_GAP). Probably the
  LittleFS journal write blocked the loop for about 340 ms. Not yet optimised.

## Not yet verified
- Fall detection on a real fall.
- Wrist HR/SpO2 end to end (the wrist board is not flashed yet).
- Battery-only operation, range, watchdog (not implemented).
