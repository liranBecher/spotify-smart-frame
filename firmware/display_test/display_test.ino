#include <JC3248W535.h>

JC3248W535_Display display;

void setup() {
  Serial.begin(115200);
  if (!display.begin()) {
    Serial.println("Display init failed; check PSRAM setting");
    return;
  }

  display.setRotation(ROTATION_90);
  auto screen = display.getCanvas();
  screen->fillScreen(0x0841);
  screen->setTextColor(0xFFFF);
  screen->setTextSize(3);
  screen->setCursor(35, 120);
  screen->print("Spotify Frame");
  screen->setTextSize(2);
  screen->setCursor(35, 170);
  screen->print("Display test OK");
  display.flush();
  Serial.println("Display test OK");
}

void loop() {}
