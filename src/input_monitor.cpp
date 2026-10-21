#include "input_monitor.h"

#include <Arduino.h>

#include "config.h"

void InputMonitor::begin() {
   pins_[0] = {PIN_REDE_DISP, INPUT_REDE_DISPONIVEL_INVERTED};
   pins_[1] = {PIN_ALIM_REDE, INPUT_ALIMENTACAO_REDE_INVERTED};
   pins_[2] = {PIN_ALIM_OFFGRID, INPUT_ALIMENTACAO_OFFGRID_INVERTED};
   pins_[3] = {PIN_ALIM_GERADOR, INPUT_ALIMENTACAO_GERADOR_INVERTED};

   for (const auto &pin : pins_) {
      pinMode(pin.pin, INPUT);
   }

   // Stabilise the debounce before the first confirmed reading is trusted. The whole group must
   // be settled before the composite is trusted: a half-transfer snapshot must never become the
   // initial state (or the boot snapshot).
   bool levels[InputGroup::COUNT];
   readLevels(levels);

   const uint32_t start = millis();

   group_.begin(levels, start, INPUT_DEBOUNCE_MS);

   while ((millis() - start) < INPUT_STABILIZE_MS || !group_.settled()) {
      const uint32_t now = millis();

      readLevels(levels);
      group_.sample(levels, now);

      if ((now - start) >= INPUT_STABILIZE_MAX_MS) {
         Serial.println("AVISO: entradas oscilando no boot; estado inicial provisorio.");
         break;
      }

      delay(5);
   }

   rebuild();

   // If the bounded wait ended while a channel was still debouncing, the first settled update()
   // must still commit the corrected composite. Nothing is dropped.
   changed_ = !group_.settled();
}

void InputMonitor::readLevels(bool levels[InputGroup::COUNT]) const {
   for (uint8_t i = 0; i < InputGroup::COUNT; ++i) {
      const bool level = digitalRead(pins_[i].pin) == HIGH;
      levels[i] = pins_[i].inverted ? !level : level;
   }
}

void InputMonitor::rebuild() {
   state_ = group_.state();
}

void InputMonitor::update(uint32_t now) {
   bool levels[InputGroup::COUNT];
   readLevels(levels);

   if (group_.sample(levels, now)) {
      changed_ = true;
   }

   if (changed_) {
      rebuild();
   }
}
