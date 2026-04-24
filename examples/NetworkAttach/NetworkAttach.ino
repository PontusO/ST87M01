// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

/*
  ST87M01 NetworkAttach

  Registers on the cellular network and brings up an IP connection via the
  ST87M01 modem. Runs the full bring-up: CFUN → wait for registration →
  CGATT → CGDCONT → CGACT → CGPADDR → AT#IPCFG. When done, the modem has
  an IP address and is ready for sockets.

  APN is set to 1NCE's IoT APN by default. Change APN below to match your SIM.

  Hardware: any board with a ST87M01Boards.h preset (e.g. iLabs Challenger
  2350 NB-IoT). Run ModemInfo first to verify basic AT communication.
*/

#include <ST87M01Modem.h>
#include <ST87M01Network.h>
#include <ST87M01Boards.h>

static const char* APN = "iot.1nce.net";
static constexpr unsigned long REGISTRATION_TIMEOUT_MS = 120000;

ST87M01Modem   modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);
ST87M01Network network(modem);

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

static bool isRegistered(ST87M01RegStatus r) {
  return r == ST87M01RegStatus::RegisteredHome ||
         r == ST87M01RegStatus::RegisteredRoaming;
}

static bool waitForRegistration(unsigned long timeoutMs) {
  unsigned long start = millis();
  unsigned long lastLog = 0;

  while ((millis() - start) < timeoutMs) {
    modem.poll();

    ST87M01CellInfo cell;
    bool ok = modem.getCellInfo(cell);
    if (ok && isRegistered(cell.reg)) return true;

    if (millis() - lastLog >= 5000) {
      lastLog = millis();
      Serial.print(F("  ["));
      Serial.print((millis() - start) / 1000);
      Serial.print(F("s] "));
      Serial.println(ok ? regStatusStr(cell.reg) : "(no response)");
    }
    delay(500);
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  ST87M01_SERIAL.begin(115200);

  Serial.println();
  Serial.println(F("=== ST87M01 NetworkAttach ==="));

  if (!modem.begin()) {
    Serial.println(F("modem.begin() failed."));
    return;
  }
  Serial.println(F("Modem alive."));

  // +CEREG=2 enables location info (tac/ci/act) in CEREG reports and URCs.
  modem.setCeregMode(2);

  Serial.println(F("Waiting for network registration..."));
  if (!waitForRegistration(REGISTRATION_TIMEOUT_MS)) {
    Serial.println(F("Not registered — check antenna, SIM, coverage."));
    return;
  }

  ST87M01CellInfo cell;
  modem.getCellInfo(cell);
  Serial.print(F("Registered: "));
  Serial.println(regStatusStr(cell.reg));

  Serial.print(F("Attaching PDP context with APN: "));
  Serial.println(APN);

  if (!network.begin(APN)) {
    Serial.print(F("network.begin() failed — last CME error: "));
    Serial.println(modem.at().lastCmeError());
    return;
  }

  Serial.print(F("Attached on cid="));
  Serial.print(network.cid());
  Serial.print(F(".  Local IP: "));
  String ip = network.localIP();
  Serial.println(ip.length() ? ip : String("(none)"));

  ST87M01SignalInfo sig;
  if (modem.getSignal(sig) && sig.rsrp >= 0 && sig.rsrp != 255) {
    Serial.print(F("RSRP: "));
    Serial.print(sig.rsrp - 140);
    Serial.println(F(" dBm"));
  }

  Serial.println(F("\nReady. Polling for URCs..."));
}

void loop() {
  modem.poll();

  static unsigned long lastReport = 0;
  if (millis() - lastReport >= 30000) {
    lastReport = millis();

    bool attached = false;
    modem.isAttached(attached);

    Serial.print(F("[+"));
    Serial.print(millis() / 1000);
    Serial.print(F("s] attached="));
    Serial.print(attached ? "yes" : "no");

    ST87M01CellInfo cell;
    if (modem.getCellInfo(cell)) {
      Serial.print(F("  "));
      Serial.print(regStatusStr(cell.reg));
    }
    Serial.println();
  }
}
