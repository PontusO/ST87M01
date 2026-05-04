// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

/*
  ST87M01 LowPowerLoop

  The complementary pairing of the modem's PSM and the RP2350's light-sleep:
  both go to sleep together, both come back together, the modem stays
  *registered* the whole time so each heartbeat costs almost nothing on the
  radio.

  Cycle (one iteration of loop()):
    1. Wake modem if needed, fetch time via NTP, print it.
    2. Wait briefly so the modem's hold-timer expires and it drops into PSM
       deep sleep on its own (configured via nbiot.setSleep).
    3. Arm two RP2350 wake sources — a timer for the next heartbeat AND the
       modem's ring pin so an unsolicited URC (e.g. a downlink during the
       modem's Active-Time window) can wake us early.
    4. WFI until either fires; report which.

  First-boot note: nbiot.setSleep() may take ~25 s the first time you run
  this on a device, because AT#SLEEPMODE has to be persisted to NVM and
  the modem rebooted for the change to take effect. The library detects
  the no-op case on subsequent boots and skips the reset.

  What to look for on the serial monitor:
    - Granted PSM TAU/Active values from the network (often different from
      what we requested).
    - Heartbeat seq counter ticking up at HEARTBEAT_INTERVAL_S spacing.
    - "Woke: timer" most cycles, occasional "Woke: ring pin" if the network
      pages us during a modem Active window.

  Power expectations (RP2350Power side; modem PSM is always sub-µA):
    - Depth::Light   — WFI only, all clocks unchanged: ~20-25 mA with USB
                       connected. Wake latency ~µs.
    - Depth::Medium  — sys_clk dropped to 48 MHz before WFI: ~10-15 mA.
                       Wake latency ~tens of µs. Modem UART baud rate is
                       restored via the post-wake callback.
    - Depth::Deep    — sys_clk dropped to 18 MHz: ~5-8 mA, more on
                       battery without USB. Wake latency ~ms.
    - Depth::Dormant — POWMAN-managed dormant: SWITCHED_CORE and XIP_CACHE
                       power-gated, SRAM retained, PLLs deinit'd, ROSC
                       disabled. Target ~150 µA on bare hardware. Wake
                       latency low ms. Both the ring pin AND the AON
                       timer can wake the chip; cadence below stays the
                       same as Deep. Read the caveats below before
                       selecting this.

  Caveats specific to Depth::Dormant on this sketch:
    * USB CDC (the Serial that you read on the IDE serial monitor) dies
      with pll_usb on the first sleep cycle and stays dark for the rest
      of the session — RP2350Power does NOT restore pll_usb on wake.
      Past the first sleep, the on-board LED is your only host-visible
      feedback. Use Light/Medium/Deep for interactive development; pick
      Dormant only when you're profiling on a battery or Otii.
    * The modem UART (Serial1) IS restored on wake via the post-wake
      callback below, so AT traffic continues to work normally.
    * The AON timer runs off LPOSC at nominal 32 kHz — uncalibrated, so
      HEARTBEAT_INTERVAL_S has ±10–20 % drift in Dormant. Acceptable for
      heartbeats; not a precision clock.

  Switch the depth via SLEEP_DEPTH below. Default is Medium — a good
  balance for an NB-IoT heartbeat where you don't need µs wake latency
  and you still want USB Serial for visibility.

  LED_BUILTIN is driven high during all "active" phases (boot setup,
  network bring-up, heartbeat exchange) and low while sleeping — useful
  visual feedback when the device is on a battery and you can't see
  serial output. The LED itself adds a few mA to the active-phase budget;
  comment out the LED_BUILTIN writes if you're profiling raw consumption.

  Hardware: any board with a ST87M01Boards.h preset and a ring pin wired
  (Challenger 2350 NB-IoT). On boards without a ring pin, only the timer
  wake source is armed and the example still works.

  Run NetworkAttach + PsmSleep first to confirm the basics work.
*/

#include <ST87M01Modem.h>
#include <ST87M01Network.h>
#include <ST87M01UDP.h>
#include <ST87M01NBIoT.h>
#include <ST87M01Boards.h>
#include <RP2350Power.h>
#include <time.h>

static const char* APN = "iot.1nce.net";

// Same NTP target as the UdpSend example (Google time1, IP literal so no DNS
// dependency).
static const IPAddress  NTP_SERVER(216, 239, 35, 0);
static constexpr uint16_t NTP_PORT   = 123;
static constexpr uint16_t LOCAL_PORT = 2390;

// Heartbeat cadence. Keep it short for the demo so you see several cycles
// per session; in production you'd typically pick something close to (but
// not larger than) the granted PSM TAU.
static constexpr uint32_t HEARTBEAT_INTERVAL_S = 120;   // 2 min

// PSM request. Most carriers will round these down — check the granted
// values printed at startup.
static constexpr uint32_t PSM_TAU_S    = 600;           // 10 min
static constexpr uint32_t PSM_ACTIVE_S = 30;            // 30 s

// Seconds the modem stays awake after our last AT command before slipping
// into PSM. Short = save more power; too short = slows down back-to-back
// command sequences.
static constexpr uint32_t MODEM_IDLE_HOLD_S = 5;

// MCU sleep depth — see RP2350Power.h, plus the dormant-specific caveats
// in the header comment above (USB Serial dies, ring pin required).
static constexpr PowerManager::Depth SLEEP_DEPTH = PowerManager::Depth::Medium;

static constexpr unsigned long REGISTRATION_TIMEOUT_MS = 120000;
static constexpr unsigned long REPLY_WAIT_MS           = 8000;

ST87M01Modem   modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);
ST87M01Network network(modem);
ST87M01UDP     udp(modem);
ST87M01NBIoT   nbiot(modem);
RP2350Power    pwr;

static uint32_t seq = 0;

// Post-wake hook: called by RP2350Power AFTER it restores sys_clk on wake
// from a Medium/Deep/Dormant sleep, BEFORE pwr.sleep() returns. Reinitialise
// any peripherals whose baud / clock derivation gets disturbed by the
// dropped-clock period — for this sketch that's just the modem UART.
// Light depth never invokes this hook. Dormant additionally takes USB CDC
// down with pll_usb and we don't bring USB back here, so Serial.println
// from the main loop simply goes nowhere after the first dormant cycle.
static void onPostWake(void* /*ctx*/) {
  ST87M01_SERIAL.begin(115200);
}

static const char* regStatusStr(ST87M01RegStatus r) {
  switch (r) {
    case ST87M01RegStatus::RegisteredHome:    return "registered (home)";
    case ST87M01RegStatus::RegisteredRoaming: return "registered (roaming)";
    case ST87M01RegStatus::Searching:         return "searching";
    case ST87M01RegStatus::NotRegistered:     return "not registered";
    case ST87M01RegStatus::RegistrationDenied:return "denied";
    default:                                  return "unknown";
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

// Send one NTP request, wait for reply, print decoded UTC. Returns false on
// any failure path so the caller can decide whether to log/retry.
static bool sendOneHeartbeat() {
  // Pulse the wake pin and probe AT. If the modem is currently in PSM deep
  // sleep this brings it back; if it's already awake this is a cheap no-op.
  modem.wake();

  uint8_t req[48];
  memset(req, 0, sizeof(req));
  req[0] = 0x1B;  // NTP v3 client request

  if (!udp.beginPacket(NTP_SERVER, NTP_PORT)) return false;
  udp.write(req, sizeof(req));
  if (udp.endPacket() != 1) return false;

  unsigned long start = millis();
  while ((millis() - start) < REPLY_WAIT_MS) {
    modem.poll();
    if (udp.parsePacket() <= 0) {
      delay(50);
      continue;
    }
    uint8_t reply[48];
    size_t got = 0;
    while (udp.available() > 0 && got < sizeof(reply)) {
      int b = udp.read();
      if (b < 0) break;
      reply[got++] = (uint8_t)b;
    }
    if (got != 48) continue;

    uint32_t ntpSecs = ((uint32_t)reply[40] << 24) | ((uint32_t)reply[41] << 16) |
                       ((uint32_t)reply[42] <<  8) |  (uint32_t)reply[43];
    time_t t = (time_t)(ntpSecs - 2208988800UL);
    struct tm g;
    gmtime_r(&t, &g);
    char iso[24];
    strftime(iso, sizeof(iso), "%Y-%m-%d %H:%M:%S", &g);
    Serial.print(F("  NTP UTC="));
    Serial.println(iso);
    return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  ST87M01_SERIAL.begin(115200);

  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // active during boot + network bring-up

  Serial.println(F("\n=== ST87M01 LowPowerLoop ==="));

  if (!modem.begin()) {
    Serial.println(F("modem.begin() failed."));
    return;
  }

  // Configure deep-sleep behaviour. The library handles persistence: if
  // the values are already in NVM this is a fast no-op; otherwise it
  // writes them and reboots the modem to activate (~25 s, once per
  // device).
  if (!nbiot.setSleep(true, MODEM_IDLE_HOLD_S)) {
    Serial.println(F("setSleep failed; deep PSM may not activate."));
    // Don't bail — sketch still works at higher sleep current.
  }

  // Wipe stale PSM state from prior runs — see PsmSleep example for why
  // a modem that boots already in PSM mode can disrupt the initial attach.
  nbiot.resetPSM();
  modem.setCeregMode(2);

  Serial.println(F("Waiting for registration..."));
  if (!waitForRegistration(REGISTRATION_TIMEOUT_MS)) {
    Serial.println(F("Not registered."));
    return;
  }

  if (!network.begin(APN)) {
    Serial.print(F("network.begin() failed — CME "));
    Serial.println(modem.at().lastCmeError());
    return;
  }
  Serial.print(F("Attached on cid="));
  Serial.print(network.cid());
  Serial.print(F("  IP="));
  Serial.println(network.localIP());

  if (!udp.begin(LOCAL_PORT)) {
    Serial.println(F("udp.begin() failed."));
    return;
  }

  Serial.print(F("Requesting PSM: TAU="));
  Serial.print(PSM_TAU_S);
  Serial.print(F("s  Active="));
  Serial.print(PSM_ACTIVE_S);
  Serial.println(F("s"));

  if (!nbiot.requestPSM(PSM_TAU_S, PSM_ACTIVE_S)) {
    Serial.print(F("requestPSM failed — CME "));
    Serial.println(modem.at().lastCmeError());
    return;
  }

  // AT#SLEEPMODE was already persisted via the one-time NVM setup above
  // (or was already in NVM from a previous run). No need to re-set here —
  // the value is loaded from NVM at every modem boot.

  // Give the network a moment to acknowledge before reading grants.
  delay(2000);
  ST87M01PsmInfo psm;
  if (nbiot.getPSM(psm)) {
    Serial.print(F("Granted: TAU="));
    Serial.print(psm.grantedTauSeconds);
    Serial.print(F("s  Active="));
    Serial.print(psm.grantedActiveSeconds);
    Serial.println(F("s"));
  }

  pwr.begin();
  pwr.setPostWakeCallback(onPostWake);

  Serial.print(F("Heartbeat every "));
  Serial.print(HEARTBEAT_INTERVAL_S);
  Serial.println(F(" s. Sleeping between cycles."));
  if (modem.ringPin() >= 0) {
    Serial.print(F("Ring-pin wake armed on GPIO "));
    Serial.println(modem.ringPin());
  }
  Serial.println();
}

void loop() {
  digitalWrite(LED_BUILTIN, HIGH);  // active phase begins
  modem.poll();

  ++seq;
  Serial.print(F("[seq="));
  Serial.print(seq);
  Serial.println(F("] heartbeat..."));
  if (!sendOneHeartbeat()) {
    Serial.println(F("  (send/reply failed)"));
  }

  // Idle for a touch longer than MODEM_IDLE_HOLD_S so the modem actually
  // crosses into PSM before we sleep the MCU. Keeps poll() running so any
  // late URCs (e.g. #SLEEP if SLEEPIND is configured) get logged.
  unsigned long settleStart = millis();
  while ((millis() - settleStart) < (MODEM_IDLE_HOLD_S + 1) * 1000UL) {
    modem.poll();
    delay(100);
  }

  // Arm the wake sources and sleep.
  pwr.disarm();
  pwr.wakeOnTimer(HEARTBEAT_INTERVAL_S * 1000UL);
  if (modem.ringPin() >= 0) {
    // Ring polarity comes straight from the board pin descriptor — most
    // carrier boards use active-low, but ringActiveLow lets a custom board
    // override.
    pwr.wakeOnPin((uint8_t)modem.ringPin(),
                  modem.pins().ringActiveLow,
                  /*pullUp=*/true);
  }

  Serial.print(F("Sleeping up to "));
  Serial.print(HEARTBEAT_INTERVAL_S);
  Serial.println(F(" s..."));
  Serial.flush();                   // make sure the line lands before USB stalls

  digitalWrite(LED_BUILTIN, LOW);   // dark during sleep
  PowerManager::WakeReason reason = pwr.sleep(SLEEP_DEPTH);
  digitalWrite(LED_BUILTIN, HIGH);  // back active immediately on wake

  Serial.print(F("Woke: "));
  switch (reason) {
    case PowerManager::WakeReason::Timer:
      Serial.println(F("timer (next heartbeat)"));
      break;
    case PowerManager::WakeReason::Pin:
      Serial.println(F("ring pin (modem URC pending)"));
      break;
    default:
      Serial.println(F("unknown"));
      break;
  }
  Serial.println();
}
