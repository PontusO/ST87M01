// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#pragma once

#include <Arduino.h>
#include "ST87M01Types.h"

// Board presets for arduino-pico boards carrying the ST87M01 modem.
//
// Usage:
//   #include <ST87M01Modem.h>
//   #include <ST87M01Boards.h>
//   ST87M01Modem modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);
//
// Adding a new board: append an #elif branch below, keyed on the board's
// ARDUINO_* macro (from its boards.txt build.board= line).

#if defined(ARDUINO_CHALLENGER_2350_NBIOT_RP2350)
  constexpr ST87M01Pins ST87M01_DEFAULT_PINS = {
    /* reset  */ static_cast<int8_t>(PIN_ST87M01_RSTN),
    /* wakeup */ static_cast<int8_t>(PIN_ST87M01_WAKEUP),
    /* ring   */ static_cast<int8_t>(PIN_ST87M01_INT),
  };
#else
  #error "ST87M01Boards.h has no preset for the selected board. Either select a supported board in the IDE, add an #elif branch in ST87M01Boards.h, or skip including it and pass an ST87M01Pins{} literal to the modem constructor."
#endif
