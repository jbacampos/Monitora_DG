#include "input_monitor.h"

#include "config.h"

void InputMonitor::begin() {
   inputs_[0] = {PIN_REDE_DISP, INPUT_REDE_DISPONIVEL_INVERTED, false, false, 0};
   inputs_[1] = {PIN_ALIM_REDE, INPUT_ALIMENTACAO_REDE_INVERTED, false, false, 0};
   inputs_[2] = {PIN_ALIM_OFFGRID, INPUT_ALIMENTACAO_OFFGRID_INVERTED, false, false, 0};
   inputs_[3] = {PIN_ALIM_GERADOR, INPUT_ALIMENTACAO_GERADOR_INVERTED, false, false, 0};

   for (auto &input : inputs_) {
      pinMode(input.pin, INPUT);
   }

   // Stabilise the debounce before the first confirmed reading is trusted.
   const uint32_t start = millis();

   while (millis() - start < INPUT_STABILIZE_MS) {
      const uint32_t now = millis();

      for (auto &input : inputs_) {
         sample(input, now);
      }

      delay(5);
   }

   rebuild();
   changed_ = false;
}

void InputMonitor::sample(Input &input, uint32_t now) {
   const bool level = digitalRead(input.pin) == HIGH;
   const bool logical = input.inverted ? !level : level;

   if (logical != input.raw) {
      input.raw = logical;
      input.changedAt = now;
      return;
   }

   if (logical != input.stable && (now - input.changedAt) >= INPUT_DEBOUNCE_MS) {
      input.stable = logical;
      changed_ = true;
   }
}

void InputMonitor::rebuild() {
   state_.redeDisponivel = inputs_[0].stable;
   state_.alimentacaoRede = inputs_[1].stable;
   state_.alimentacaoOffgrid = inputs_[2].stable;
   state_.alimentacaoGerador = inputs_[3].stable;
}

void InputMonitor::update(uint32_t now) {
   for (auto &input : inputs_) {
      sample(input, now);
   }

   if (changed_) {
      rebuild();
   }
}