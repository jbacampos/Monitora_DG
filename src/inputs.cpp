#include "inputs.h"

#include "config.h"

void InputMonitor::begin() {
   inputs_[0] = {PIN_REDE_DISP, false, false, 0};
   inputs_[1] = {PIN_ALIM_REDE, false, false, 0};
   inputs_[2] = {PIN_ALIM_OFFGRID, false, false, 0};
   inputs_[3] = {PIN_ALIM_GERADOR, false, false, 0};

   for (auto &input : inputs_) {
      pinMode(input.pin, INPUT);

      const bool value = digitalRead(input.pin) == HIGH;
      input.raw = value;
      input.stable = value;
      input.changedAt = millis();
   }

   rebuildState();
   changed_ = false;
}

void InputMonitor::updateInput(Input &input, uint32_t now) {
   const bool raw = digitalRead(input.pin) == HIGH;

   if (raw != input.raw) {
      input.raw = raw;
      input.changedAt = now;
      return;
   }

   if (raw != input.stable &&
       static_cast<uint32_t>(now - input.changedAt) >= INPUT_DEBOUNCE_MS) {
      input.stable = raw;
      changed_ = true;
   }
}

void InputMonitor::rebuildState() {
   state_.redeDisponivel = inputs_[0].stable;
   state_.alimentacaoRede = inputs_[1].stable;
   state_.alimentacaoOffgrid = inputs_[2].stable;
   state_.alimentacaoGerador = inputs_[3].stable;
}

void InputMonitor::update(uint32_t now) {
   for (auto &input : inputs_) {
      updateInput(input, now);
   }

   if (changed_) {
      rebuildState();
   }
}

bool InputMonitor::changed() const {
   return changed_;
}

void InputMonitor::clearChanged() {
   changed_ = false;
}

const PowerState &InputMonitor::state() const {
   return state_;
}
