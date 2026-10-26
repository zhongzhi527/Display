# Battery + Button Probe (temporary diagnostic)

This is a **throwaway diagnostic sketch** for the Guition JC2432W328 board. It
does NOT replace the main firmware. Its only job is to tell us two things we
cannot know from the code alone:

1. **Is the LiPo battery voltage readable** by the ESP32 on any ADC pin?
2. **Is the SW1 push-button wired to a GPIO** the ESP32 can read?

Once we have the answers, the real battery gauge and power-button behaviour can
be built against the correct pins.

## How to run it

It is a separate PlatformIO project so it won't disturb the main firmware.

1. Open **this** folder (`diagnostics/battery_button_probe`) in VS Code as its
   own PlatformIO project (File -> Open Folder -> pick this folder), OR copy its
   `src/main.cpp` over temporarily. Opening it as its own folder is cleanest.
2. Build and Upload it to the board.
3. Open the Serial Monitor at **115200 baud**.

## What to do while it runs (and report back)

The sketch prints a table about twice a second. Do each of these and note what
changes:

- **Step A — USB powered, battery connected:** let it print for ~10 seconds.
  Copy a few lines.
- **Step B — unplug USB, run on battery only** (if safe to do so): copy a few
  lines. (If the board powers off when USB is removed, that itself is an
  important answer — tell me.)
- **Step C — press and release SW1 a few times**, slowly. Watch whether any of
  the `GPIOxx` button columns flip between 1 and 0 when you press it.

Paste the printed lines back and I'll interpret them.

## Safety

- The sketch only **reads** pins (plus internal pullups on digital inputs). It
  does not drive battery/charger pins.
- It does not touch the TFT (GPIO 2/12/13/14/15), touch (GPIO 21/25/32/33) or
  backlight (GPIO 27) pins used by the real firmware.
