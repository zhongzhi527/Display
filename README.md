# Display-Load-A-B — DUAL SCALE handheld display

Handheld touchscreen display for the **DUAL SCALE** system. It shows the live
weight of **Load Cell A** and **Load Cell B**, plus their combined total, and
sends tare / calibrate commands to the two load-cell nodes over **Bluetooth
Low Energy (BLE)**.

This is the **display** half of the DUAL system. It pairs with the load-cell
firmware **[ESP32-Load-A-B](https://github.com/zhongzhi527/ESP32-Load-A-B)**.

> There are two load-cell systems. This repo is the handheld for the **2-cell
> Bluetooth** one. The separate **4-cell ESP-NOW** system lives in
> `Display-Load-A-B-C-D` (display) + `ESP32-Load-A-B-C-D` (scale).

## Hardware

- **Guition JC2432W328C** — ESP32 board with a 240×320 ST7789 TFT and a CST820
  capacitive touch panel.
- Single-cell LiPo, sensed through an on-board divider on GPIO39 for the
  battery gauge.

## What it does

- **Dashboard** — live weight of Load Cell A and B and the A+B total.
- **Connection status** — each cell shows a clear lifecycle:
  **OFFLINE** (red) → **SYNCING** (yellow) → **CONNECTED** (green), with a
  matching pulsing panel border.
- **LINK page** — per-scale Bluetooth / sensor / data health.
- **CALIBRATE** — guided tare + known-weight calibration per cell.
- **SETTINGS** — display brightness and ESP32 CPU clock.
- **Auto weight-lock** — freezes a stable combined reading; tap UNLOCK to
  re-arm.
- **Idle standby (snooze)** — after 5 min with no touch the screen sleeps;
  the last few seconds show a SNOOZING countdown. Any touch wakes it.

## Build & flash (PlatformIO)

One environment, `jc2432w328`:

```sh
pio run -e jc2432w328 -t upload
```

Or in the VS Code PlatformIO sidebar: `jc2432w328` → **Upload**.

### Platform version is pinned on purpose

`platformio.ini` pins `platform = espressif32@6.5.0` (Arduino-ESP32 2.0.x).
Leaving it unpinned pulls the newest platform (8.0.0), whose ESP-IDF is
incompatible with `NimBLE-Arduino@2.3.2` and fails to build. If a build fails
with `syscfg.h` / `nimconfig.h` errors, confirm the pin and do a clean build
(delete the `.pio` folder, then rebuild).

## Pairing

The display scans for the two load-cell nodes by BLE name (`LOAD_CELL_1` =
Load Cell A, `LOAD_CELL_2` = Load Cell B). Keep this display and the scale
firmware on matching firmware — they share the `DualScaleBleProtocol.h` packet
definitions, so flashing only one side can leave them unable to connect. After
flashing, power-cycle both and allow ~20 s to link.
