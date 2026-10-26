// Battery + Button probe for the Guition JC2432W328 board.
//
// Goal: discover (1) whether the LiPo voltage is readable on any ADC pin, and
// (2) whether SW1 is wired to a readable GPIO. Reads only; drives nothing.
//
// Serial Monitor @ 115200. See README.md for the steps to run.

#include <Arduino.h>

namespace {

// Input-only ADC-capable pins on the ESP32-WROOM. These are the usual spots a
// board routes a battery-sense divider to, and they cannot be outputs, so
// reading them is safe. We measure these as ANALOG.
constexpr int kAdcPins[] = {34, 35, 36, 39};
constexpr size_t kAdcPinCount = sizeof(kAdcPins) / sizeof(kAdcPins[0]);

// Candidate GPIOs that SW1 might be wired to. We deliberately EXCLUDE pins the
// real firmware uses: TFT 2/12/13/14/15, touch 21/25/32/33, backlight 27, and
// the SPI/flash pins. We read these as DIGITAL INPUT with pullups and watch for
// a press to pull one LOW (0). GPIO0 is the BOOT button and is included because
// it is a common "spare" button line, but do not hold it at reset.
constexpr int kButtonPins[] = {0, 4, 5, 16, 17, 22, 26, 35, 34, 36, 39};
constexpr size_t kButtonPinCount =
    sizeof(kButtonPins) / sizeof(kButtonPins[0]);

// For divider-based sensing, the reading is proportional to battery voltage. We
// convert the raw ADC count to an approximate pin voltage for convenience. The
// ESP32 ADC is ~0..4095 over ~0..3.3V with 11dB attenuation.
float approxPinVolts(int raw) {
  return (static_cast<float>(raw) / 4095.0f) * 3.3f;
}

uint32_t lastPrintMs = 0;

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.println("=== Guition JC2432W328 battery + button probe ===");
  Serial.println("Reads ADC pins (analog) and candidate button pins (digital).");
  Serial.println("Nothing is driven. Safe to run on USB or battery.");
  Serial.println();

  // Full-range analog reads.
  analogReadResolution(12);
  for (size_t i = 0; i < kAdcPinCount; ++i) {
    analogSetPinAttenuation(kAdcPins[i], ADC_11db);
  }

  // Button candidates: input with internal pullup. A press that connects the
  // pin to GND will read 0. GPIO 34/35/36/39 have NO internal pullup, so they
  // float unless externally driven; included only to catch a board that pulls
  // them for a button.
  for (size_t i = 0; i < kButtonPinCount; ++i) {
    const int pin = kButtonPins[i];
    if (pin >= 34) {
      pinMode(pin, INPUT);
    } else {
      pinMode(pin, INPUT_PULLUP);
    }
  }

  Serial.print("ADC pins (analog): ");
  for (size_t i = 0; i < kAdcPinCount; ++i) {
    Serial.printf("GPIO%d ", kAdcPins[i]);
  }
  Serial.println();
  Serial.print("Button pins (digital, press should show 0): ");
  for (size_t i = 0; i < kButtonPinCount; ++i) {
    Serial.printf("GPIO%d ", kButtonPins[i]);
  }
  Serial.println();
  Serial.println("--------------------------------------------------");
}

void loop() {
  const uint32_t now = millis();
  if (now - lastPrintMs < 500) {
    return;
  }
  lastPrintMs = now;

  // --- Analog section: look for a pin whose value tracks battery voltage. ---
  Serial.print("ADC  ");
  for (size_t i = 0; i < kAdcPinCount; ++i) {
    const int raw = analogRead(kAdcPins[i]);
    Serial.printf("G%d=%4d(%.2fVpin) ", kAdcPins[i], raw, approxPinVolts(raw));
  }
  Serial.println();

  // --- Digital section: look for a pin that flips when SW1 is pressed. ---
  Serial.print("BTN  ");
  for (size_t i = 0; i < kButtonPinCount; ++i) {
    Serial.printf("G%d=%d ", kButtonPins[i], digitalRead(kButtonPins[i]));
  }
  Serial.println();
  Serial.println();
}
