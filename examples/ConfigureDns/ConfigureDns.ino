// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

/*
  ST87M01 ConfigureDns — one-time per-device setup.

  The ST87M01 ships with dns_to_use=0 (use NVM) and an empty NVM DNSv4, so
  AT#DNS fails with CME 2112 until a DNS server is committed. This sketch
  writes a DNS address to NVM and reboots the modem to commit. Run it ONCE
  per device. After a successful run the setting persists across power
  cycles and sketch flashes — you don't need to run it again.

  Why this is needed: the ST87M01's IP stack does not automatically ingest
  DNS pushed via PCO during PDP activation (see §9.1, §9.4 of the AT manual).
  Modems like u-blox SARA or Würth Calypso do, which is why 1NCE Just Works
  there but needs this step here.

  Default is Google's 8.8.8.8, which works through 1NCE's CGN.

  After this sketch reports success, flash NetworkAttach or TcpEcho.
*/

#include <ST87M01Modem.h>
#include <ST87M01Boards.h>

static const char* DNS_IPV4 = "8.8.8.8";

ST87M01Modem modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  ST87M01_SERIAL.begin(115200);

  Serial.println();
  Serial.println(F("=== ST87M01 ConfigureDns ==="));
  Serial.println(F("One-time per-device NVM setup."));

  if (!modem.begin()) {
    Serial.println(F("modem.begin() failed — check wiring."));
    return;
  }
  Serial.println(F("Modem alive."));

  Serial.print(F("Writing DNSv4="));
  Serial.print(DNS_IPV4);
  Serial.println(F(" to NVM and rebooting..."));

  if (!modem.configureDns(DNS_IPV4)) {
    Serial.println(F("configureDns() failed — modem did not come back in time."));
    return;
  }

  Serial.println(F("Modem is back. DNS is committed."));
  Serial.println(F("You can now run NetworkAttach or TcpEcho. This sketch does"));
  Serial.println(F("not need to be re-flashed on subsequent power cycles."));
}

void loop() {
  modem.poll();
}
