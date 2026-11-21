#include "button.h"

// The decision table is blogic::poll() in button_logic.h, which the host-side
// Unity suite compiles directly. This file only adapts it to Arduino's GPIO and
// millisecond sources.
void Button::begin(uint8_t pin, uint16_t debounceMs, uint16_t longMs) {
    pin_         = pin;
    debounceMs_  = debounceMs;
    longMs_      = longMs;
    pinMode(pin_, INPUT_PULLUP);
    btn_.lastRaw      = (digitalRead(pin_) != LOW);   // idle level is HIGH
    btn_.lastChangeMs = millis();
}

Button::Event Button::poll() {
    // blogic::Event and Button::Event share the 0/1/2 numbering by contract.
    // `raw` keeps Arduino's polarity: LOW (pressed) == false, HIGH (idle) == true,
    // exactly as the pre-extraction code stored it in lastRaw_.
    return (Button::Event)blogic::poll(millis(), (bool)digitalRead(pin_),
                                       btn_, debounceMs_, longMs_);
}
