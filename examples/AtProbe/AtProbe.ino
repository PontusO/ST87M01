// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

/*
  ST87M01 AtProbe

  Diagnostic tool. Dumps the modem's current state by running a battery of
  status queries, then drops into an interactive AT console where any command
  typed into the USB serial monitor is forwarded to the modem and the response
  printed back.

  Use this when a higher-level example fails and you need to see what the
  modem actually thinks its state is.
*/

#include <ST87M01Modem.h>
#include <ST87M01Boards.h>

ST87M01Modem modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);

static void dumpAt(const char* cmd, unsigned long timeoutMs = 5000) {
  Serial.println();
  Serial.print(F(">>> "));
  Serial.println(cmd);
  modem.at().send(cmd);

  String line;
  unsigned long start = millis();
  while ((millis() - start) < timeoutMs) {
    if (!modem.at().readLine(line, 300)) continue;
    Serial.print(F("<<< "));
    Serial.println(line);
    if (line == "OK" || line == "ERROR" || line.startsWith("+CME ERROR:")) break;
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  ST87M01_SERIAL.begin(115200);

  Serial.println();
  Serial.println(F("=== ST87M01 AtProbe ==="));

  if (!modem.begin()) {
    Serial.println(F("modem.begin() failed."));
    return;
  }

  // State snapshot.
  dumpAt("AT+CFUN?");
  dumpAt("AT+CEREG?");
  dumpAt("AT+CGATT?");
  dumpAt("AT+CGACT?");
  dumpAt("AT+CGDCONT?");
  dumpAt("AT+CGPADDR=5");
  dumpAt("AT#IPCFG?");
  dumpAt("AT#IPPARAMS?");
  dumpAt("AT+CESQ");

  Serial.println();
  Serial.println(F("=== Interactive AT console ==="));
  Serial.println(F("Type an AT command + Enter. Try: AT#MODEMSTART,  AT#DNS=5,0,\"tcpbin.com\""));
}

void loop() {
  modem.poll();

  static String buffer;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      buffer.trim();
      if (buffer.length()) {
        dumpAt(buffer.c_str(), 15000);
      }
      buffer = "";
    } else {
      buffer += c;
    }
  }
}
