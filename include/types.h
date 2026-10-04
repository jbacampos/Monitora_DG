#pragma once

#include <Arduino.h>

struct PowerState {
   bool redeDisponivel;
   bool alimentacaoRede;
   bool alimentacaoOffgrid;
   bool alimentacaoGerador;
};

struct PersistedState {
   uint32_t magic;
   uint16_t version;
   uint16_t reserved;
   uint32_t sequence;

   PowerState power;

   int32_t lastFallStickerId;

   uint32_t crc32;
};

struct RuntimeState {
   PowerState power;
   bool initialized;
};
