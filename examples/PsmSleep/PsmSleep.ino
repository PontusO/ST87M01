// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

/*
  ST87M01 PSM / Sleep demo

  Attaches to the network and then negotiates PSM with the carrier. Reports
  both what the sketch requested and what the network actually granted —
  the two differ in practice, often by a lot, so always check the granted
  values before building a power-budget around them.

  Order of operations (order matters):
    1. modem.begin() — bring AT up.
    2. nbiot.resetPSM() — wipe any stale PSM state left in NVM from prior
       sessions. A modem that boots up already in PSM is unreliable during
       the first attach because it may doze off mid-AT-exchange.
    3. Wait for network registration explicitly (attach needs the modem to
       already be registered — network.begin() does not wait on its own).
    4. network.begin() — CGATT / CGDCONT / CGACT / IPCFG.
    5. Request PSM with the target timers.
    6. Read back granted values from the network.
    7. Optional: setSleepIndications() + softReset() so #SLEEP / #WAKEUP /
       #ENERGY URCs fire (this saves SLEEPIND to NVM and reboots the modem;
       after the reboot we reattach). Disabled by default — set
       CONFIGURE_SLEEP_URCS below to enable.

  Hardware: any board with a ST87M01Boards.h preset (e.g. iLabs Challenger
  2350 NB-IoT). Flash NetworkAttach first to confirm the SIM/APN work before
  layering PSM on top.
*/

#include <ST87M01Modem.h>
#include <ST87M01Network.h>
#include <ST87M01NBIoT.h>
#include <ST87M01Boards.h>

static const char* APN = "iot.1nce.net";

// Target timers. PSM "sleep between TAUs" ≈ tauSeconds; Active-Time is the
// window after each TAU during which the modem is reachable before deep
// sleep. These are REQUESTS; the carrier picks the real values.
static constexpr uint32_t REQUESTED_TAU_SECONDS    = 3600;   // 1 hour
static constexpr uint32_t REQUESTED_ACTIVE_SECONDS = 60;     // 1 minute

// Set to true to also configure #SLEEP / #WAKEUP / #ENERGY URC visibility.
// This requires a modem reset (AT#SLEEPIND is SAVED to NVM and takes effect
// after a reboot) and a subsequent reattach, so it adds ~30 s to startup.
// Once configured on a device it stays configured; you can set this back to
// false for future flashes.
static constexpr bool CONFIGURE_SLEEP_URCS = false;

static constexpr unsigned long REGISTRATION_TIMEOUT_MS = 120000;

// Turn on to stream every AT exchange to Serial — invaluable when something
// doesn't attach. Very noisy; leave off for production.
#define DEBUG_AT 0

ST87M01Modem   modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);
ST87M01Network network(modem);
ST87M01NBIoT   nbiot(modem);

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

static void onSleepUrc(const String& kind, float durationSeconds, void* /*ctx*/) {
  Serial.print(F("[URC] #SLEEP"));
  if (kind.length()) {
    Serial.print(' ');
    Serial.print(kind);
    Serial.print(' ');
    Serial.print(durationSeconds, 1);
    Serial.print('s');
  }
  Serial.println();
}

static void onWakeUrc(void* /*ctx*/) {
  Serial.println(F("[URC] #WAKEUP"));
}

static void onEnergyUrc(float microWattH, void* /*ctx*/) {
  Serial.print(F("[URC] #ENERGY "));
  Serial.print(microWattH, 1);
  Serial.println(F(" uWh"));
}

static void dumpPsm(const ST87M01PsmInfo& p) {
  Serial.print(F("  enabled="));        Serial.println(p.enabled ? F("yes") : F("no"));
  Serial.print(F("  requested TAU="));  Serial.print(p.requestedTauSeconds);
  Serial.print(F("s  Active="));        Serial.print(p.requestedActiveSeconds);
  Serial.println(F("s"));
  if (p.grantedTauRaw.length() || p.grantedActiveRaw.length()) {
    Serial.print(F("  granted   TAU="));  Serial.print(p.grantedTauSeconds);
    Serial.print(F("s  Active="));         Serial.print(p.grantedActiveSeconds);
    Serial.println(F("s"));
  } else {
    Serial.println(F("  granted   (no grant yet — network hasn't confirmed)"));
  }
}

// Fresh attach: reset stale PSM state, wait for registration, then
// network.begin(). Called from setup() and again after the optional
// softReset().
static bool attachCleanly() {
  // Wipe any PSM config left in NVM from a prior sketch. A modem that boots
  // already in PSM mode can misbehave during the initial attach.
  nbiot.resetPSM();

  modem.setCeregMode(2);

  Serial.println(F("Waiting for registration..."));
  if (!waitForRegistration(REGISTRATION_TIMEOUT_MS)) {
    Serial.println(F("Not registered — check antenna, SIM, coverage."));
    return false;
  }

  if (!network.begin(APN)) {
    Serial.print(F("network.begin() failed — CME="));
    Serial.println(modem.at().lastCmeError());
    return false;
  }
  Serial.print(F("Attached on cid="));
  Serial.print(network.cid());
  Serial.print(F("  IP="));
  Serial.println(network.localIP());
  return true;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  ST87M01_SERIAL.begin(115200);

  Serial.println(F("\n=== ST87M01 PSM / Sleep demo ==="));

#if DEBUG_AT
  modem.at().setDebugStream(&Serial);
#endif

  if (!modem.begin()) {
    Serial.println(F("modem.begin() failed."));
    return;
  }

  if (!attachCleanly()) return;

  nbiot.onSleep(onSleepUrc);
  nbiot.onWake(onWakeUrc);
  nbiot.onEnergy(onEnergyUrc);

  Serial.print(F("Requesting PSM: TAU="));
  Serial.print(REQUESTED_TAU_SECONDS);
  Serial.print(F("s  Active="));
  Serial.print(REQUESTED_ACTIVE_SECONDS);
  Serial.println(F("s"));

  if (!nbiot.requestPSM(REQUESTED_TAU_SECONDS, REQUESTED_ACTIVE_SECONDS)) {
    Serial.print(F("requestPSM failed — CME="));
    Serial.println(modem.at().lastCmeError());
    return;
  }

  // Give the network a moment to ack the request so getPSM() can see the
  // granted values on the first read. It's fine if this first read shows
  // "no grant yet"; loop() re-reads below.
  delay(2000);

  ST87M01PsmInfo psm;
  if (nbiot.getPSM(psm)) {
    Serial.println(F("PSM state after request:"));
    dumpPsm(psm);
  }

  if (CONFIGURE_SLEEP_URCS) {
    Serial.println(F("Enabling verbose sleep URCs (AT#SLEEPIND)..."));
    nbiot.setSleepIndications(ST87M01NBIoT::SLEEP_IND_PSM
                            | ST87M01NBIoT::SLEEP_IND_VERBOSE
                            | ST87M01NBIoT::SLEEP_IND_ENERGY);

    Serial.println(F("Resetting modem to persist SLEEPIND..."));
    if (!modem.softReset(30000)) {
      Serial.println(F("softReset failed; continuing without URCs."));
    } else {
      Serial.println(F("Reattaching after reset..."));
      if (!attachCleanly()) return;
      // PSM NVM state was wiped by attachCleanly's resetPSM, so re-apply.
      nbiot.requestPSM(REQUESTED_TAU_SECONDS, REQUESTED_ACTIVE_SECONDS);
    }
  }

  Serial.println(F("\nReady. Watching for PSM cycles...\n"));
}

void loop() {
  modem.poll();

  static unsigned long lastReport = 0;
  if (millis() - lastReport >= 60000) {
    lastReport = millis();

    Serial.print(F("[+"));
    Serial.print(millis() / 1000);
    Serial.println(F("s] PSM state:"));

    ST87M01PsmInfo psm;
    if (nbiot.getPSM(psm)) dumpPsm(psm);

    float e = nbiot.lastEnergyMicroWattH();
    if (!isnan(e)) {
      Serial.print(F("  last #ENERGY: "));
      Serial.print(e, 1);
      Serial.println(F(" uWh"));
    }
  }
}
