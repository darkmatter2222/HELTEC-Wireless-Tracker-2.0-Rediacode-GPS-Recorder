// =============================================================================
// PRG-button and UI navigation decision helpers — header-only, zero deps.
//
// Same pattern as wifi_gate.h: the firmware compiles these exact functions and
// the host-side Unity suite (`test/test_button_native`) includes this header
// directly, so the debounce/press-length table cannot drift from the device.
//
// Why the engine is written this way (it was the v0.3.4 bug): `pressStartMs` is
// stamped when the *debounced* level first reads low, not when the physical
// transition happened, and the long-press fires while the button is still held
// so the UI can react to a hold without waiting for the release. A blocked main
// loop therefore shifts the whole window instead of losing the event.
// =============================================================================
#pragma once
#include <stdint.h>

namespace blogic {

// ---------------------------------------------------------------------------
// Debounce + press-length engine (exact port of Button::poll)
// ---------------------------------------------------------------------------
struct State {
    bool     lastRaw      = true;   // active LOW, so the idle level is HIGH
    bool     pressed      = false;
    uint32_t lastChangeMs = 0;
    uint32_t pressStartMs = 0;
    bool     longFired    = false;
};

enum Event : uint8_t {
    None  = 0,
    Short = 1,
    Long  = 2,
};

// `raw` follows the Arduino convention: false == LOW == button pulled down.
// Every branch mirrors button.cpp one-for-one.
inline Event poll(uint32_t now, bool raw, State& s, uint16_t debounceMs, uint16_t longMs) {
    if (raw != s.lastRaw) {              // level changed: restart the debounce timer
        s.lastRaw      = raw;
        s.lastChangeMs = now;
        return None;
    }
    if ((uint32_t)(now - s.lastChangeMs) < debounceMs) return None;   // not stable yet

    const bool downStable = (raw == false);

    if (downStable && !s.pressed) {      // first stable read of a press
        s.pressed      = true;
        s.pressStartMs = now;
        s.longFired    = false;
        return None;
    }

    // Long press fires while still held, exactly once per hold.
    if (s.pressed && downStable && !s.longFired &&
        (uint32_t)(now - s.pressStartMs) >= longMs) {
        s.longFired = true;
        return Long;
    }

    // Released: a short press is reported only if the long one never fired.
    if (!downStable && s.pressed) {
        s.pressed = false;
        const bool wasLong = s.longFired;
        s.longFired = false;
        return wasLong ? None : Short;
    }

    return None;
}

// ---------------------------------------------------------------------------
// Screen / picker cursor arithmetic (ui.cpp onShortPress / onLongPress)
// ---------------------------------------------------------------------------
// Short press cycles the NORMAL screens only; PICKER is outside the modulus.
inline int nextScreen(int cur, int count) {
    return (count <= 0) ? cur : (cur + 1) % count;
}

// The picker list has nDevices entries plus one "Cancel" row, so the modulus is
// nDevices + 1 and the Cancel row is the last index.
inline int nextPickerCursor(int cur, int nDevices) {
    const int n = nDevices + 1;
    return (n <= 1) ? 0 : (cur + 1) % n;
}
inline bool pickerCancelSlot(int cursor, int nDevices) { return cursor >= nDevices; }

// ---------------------------------------------------------------------------
// Dose display scaling (ui.cpp renderDose)
// ---------------------------------------------------------------------------
// The 5-char value field switches to millisieverts at 1 000 uSv, and the number
// of decimals follows the magnitude so the string always fits 5 characters.
inline bool useMillisievert(float microSv)  { return microSv >= 1000.0f; }
inline int  mantissaDecimals(float value) {
    if (value < 10.0f)   return 3;
    if (value < 100.0f)  return 2;
    return 1;
}

} // namespace blogic
