// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// CfunOff — diagnostic for the "where does the deep-sleep floor come from"
// question on the Challenger 2350 NB-IoT carrier board.
//
// Reference points measured separately:
//   bare RP2350 (BConnect, no modem)  Deep sleep ~6.3 mA
//   Challenger 2350 NB-IoT  Deep sleep + attempted PSM ~16.2 mA
//
// This sketch isolates the modem's contribution: it brings the modem up far
// enough to talk to it, sends AT+CFUN=0 (radio fully off, baseband retained),
// then drops the RP2350 into Deep sleep on a long timer. The Otii floor here
// is "MCU Deep + modem at CFUN=0".
//
//   floor ~ 6.3 mA  → modem accounts for the full delta. PSM never engaged.
//   floor much higher → something else on the carrier (LDO, level shifters,
//                       pulled lines, ring/wakeup leakage) is contributing
//                       and PSM alone won't get us under that.
//
// No networking, no PSM, no eDRX. Just the radio off and the MCU asleep.

#include <ST87M01Modem.h>
#include <ST87M01Boards.h>
#include <RP2350Power.h>

ST87M01Modem modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);
RP2350Power  power;

static const uint32_t SLEEP_SECONDS = 30;

void setup() {
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && (millis() - t0) < 2000) {}

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);

  Serial.println();
  Serial.println(F("=== CfunOff ==="));

  ST87M01_SERIAL.begin(115200);
  if (!modem.begin()) {
    Serial.println(F("modem.begin() failed — is the modem powered?"));
  } else {
    Serial.println(F("modem alive, sending AT+CFUN=0..."));
    modem.at().send("AT+CFUN=0");
    if (modem.at().expectOK(30000)) {
      Serial.println(F("AT+CFUN=0 OK — radio off, baseband retained."));
    } else {
      Serial.print(F("AT+CFUN=0 failed, CME="));
      Serial.println(modem.at().lastCmeError());
    }
  }

  digitalWrite(LED_BUILTIN, LOW);
  power.begin();
}

void loop() {
  power.disarm();
  power.wakeOnTimer(SLEEP_SECONDS * 1000UL);

  Serial.print(F("[sleep Deep] for "));
  Serial.print(SLEEP_SECONDS);
  Serial.println(F(" s"));
  Serial.flush();

  power.sleep(PowerManager::Depth::Deep);

  // Brief blink so the trace shows we came back; loop will sleep again.
  digitalWrite(LED_BUILTIN, HIGH);
  delay(200);
  digitalWrite(LED_BUILTIN, LOW);
}
