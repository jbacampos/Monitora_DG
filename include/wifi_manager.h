#pragma once

#include <Arduino.h>

class WiFiManager {
public:
   void begin();
   void update(uint32_t now);

   bool connected() const;
};
