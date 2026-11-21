#pragma once
#include <Arduino.h>
#include "button_logic.h"

// Single PRG-button helper with short/long-press semantics. The decision table
// lives in button_logic.h (namespace blogic) so the host-side Unity suite
// compiles the exact same code that runs here.
class Button {
public:
    // Event codes alias blogic::Event so callers keep the 0/1/2 numbering.
    enum Event : uint8_t { NONE = 0, SHORT_PRESS = 1, LONG_PRESS = 2 };

    void begin(uint8_t pin, uint16_t debounceMs, uint16_t longMs);
    Event poll();   // call from loop()

private:
    uint8_t  pin_ = 0;
    uint16_t debounceMs_ = 30;
    uint16_t longMs_     = 800;

    blogic::State btn_;   // lastRaw starts HIGH: the pin is active LOW
};
