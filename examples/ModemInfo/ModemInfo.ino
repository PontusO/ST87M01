// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

/*
  ST87M01 ModemInfo

  Prints identity, registration, signal and operator information for the
  attached ST87M01 cellular modem. Does NOT attach to a cellular network
  (no APN needed) — use this first to verify wiring, UART, and basic AT
  communication before moving on to NetworkAttach.

  Hardware: any board with a ST87M01Boards.h preset (e.g. iLabs Challenger
  2350 NB-IoT). For a custom board, replace ST87M01_SERIAL and
  ST87M01_DEFAULT_PINS with your own values.
*/

#include <ST87M01Modem.h>
#include <ST87M01Boards.h>

ST87M01Modem modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);

static const char* regStatusStr(ST87M01RegStatus r) {
  switch (r) {
    case ST87M01RegStatus::NotRegistered:      return "not registered";
    case ST87M01RegStatus::RegisteredHome:     return "registered (home)";
    case ST87M01RegStatus::Searching:          return "searching";
    case ST87M01RegStatus::RegistrationDenied: return "denied";
    case ST87M01RegStatus::RegisteredRoaming:  return "registered (roaming)";
    default:                                   return "unknown";
  }
}

static void printSignal(const ST87M01SignalInfo& s) {
  // CESQ rsrp: 0 = <-140 dBm, 97 = >=-44 dBm, step 1 dB; 255 = not detected.
  Serial.print(F("  RSRP: "));
  if (s.rsrp < 0 || s.rsrp == 255) {
    Serial.println(F("n/a"));
  } else {
    Serial.print(s.rsrp - 140);
    Serial.println(F(" dBm"));
  }
  Serial.print(F("  RSRQ idx: "));
  Serial.println(s.rsrq);
}

static void printCell(const ST87M01CellInfo& c) {
  Serial.print(F("  Status: "));
  Serial.println(regStatusStr(c.reg));
  if (c.tac.length()) { Serial.print(F("  TAC:    ")); Serial.println(c.tac); }
  if (c.ci.length())  { Serial.print(F("  CI:     ")); Serial.println(c.ci); }
  if (c.act >= 0)     { Serial.print(F("  AcT:    ")); Serial.println(c.act); }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  ST87M01_SERIAL.begin(115200);

  Serial.println();
  Serial.println(F("=== ST87M01 ModemInfo ==="));

  if (!modem.begin()) {
    Serial.println(F("modem.begin() failed — check wiring and power."));
    return;
  }
  Serial.println(F("Modem is alive."));

  // Enable registration URCs with location info so CEREG reports stat+tac+ci+act.
  modem.setCeregMode(2);

  String s;
  if (modem.getModel(s))    { Serial.print(F("Model:    ")); Serial.println(s); }
  if (modem.getRevision(s)) { Serial.print(F("Revision: ")); Serial.println(s); }
  if (modem.getImsi(s))     { Serial.print(F("IMSI:     ")); Serial.println(s); }
  else                      { Serial.println(F("IMSI:     (no SIM?)")); }

  Serial.println(F("\nRegistration:"));
  ST87M01CellInfo cell;
  if (modem.getCellInfo(cell)) printCell(cell);

  Serial.println(F("\nSignal:"));
  ST87M01SignalInfo sig;
  if (modem.getSignal(sig)) printSignal(sig);

  Serial.println(F("\nOperator:"));
  ST87M01OperatorInfo op;
  if (modem.getOperator(op)) {
    Serial.print(F("  Mode:   ")); Serial.println(op.mode);
    Serial.print(F("  Oper:   ")); Serial.println(op.oper.length() ? op.oper : String("(none)"));
    if (op.act >= 0) { Serial.print(F("  AcT:    ")); Serial.println(op.act); }
  }

  Serial.println(F("\nPolling for URCs (status refreshes every 10s)..."));
}

void loop() {
  modem.poll();

  static unsigned long lastReport = 0;
  if (millis() - lastReport >= 10000) {
    lastReport = millis();

    ST87M01CellInfo cell;
    ST87M01SignalInfo sig;
    modem.getCellInfo(cell);
    modem.getSignal(sig);

    Serial.print(F("[+")); Serial.print(millis() / 1000); Serial.print(F("s] "));
    Serial.print(regStatusStr(cell.reg));
    if (sig.rsrp >= 0 && sig.rsrp != 255) {
      Serial.print(F("  RSRP=")); Serial.print(sig.rsrp - 140); Serial.print(F(" dBm"));
    }
    Serial.println();
  }
}
