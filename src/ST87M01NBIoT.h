// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#pragma once

#include <Arduino.h>
#include <math.h>
#include "ST87M01Modem.h"

// NB-IoT-specific extras: PSM (power saving mode), eDRX, sleep mode, and the
// wake-up-event pin configuration. The friendly entry points encode the GPRS
// timer bytes (T3412-extended for TAU, T3324 for Active Time) and the eDRX
// 4-bit codes from seconds, so sketches don't need to hand-compute them.
//
// Pair with ST87M01Network for the attach/PDP setup; this class adds the
// low-power knobs on top of the attached bearer. Constructed from an
// ST87M01Modem& (adapter pattern, same as ST87M01Client/UDP/HTTP).
//
// Sleep / wake URCs (#SLEEP, #WAKEUP, #ENERGY) are hooked on construction,
// but the modem only emits them if AT#SLEEPIND is configured with a
// non-zero bitmap and the setting is persisted via AT#RESET=1 — see
// setSleepIndications() for details.
class ST87M01NBIoT {
public:
  // Bitmap values for setSleepIndications() — see AT#SLEEPIND in the manual.
  static constexpr uint8_t SLEEP_IND_DRX      = 1 << 0;
  static constexpr uint8_t SLEEP_IND_EDRX     = 1 << 1;
  static constexpr uint8_t SLEEP_IND_PSM      = 1 << 2;
  static constexpr uint8_t SLEEP_IND_OOS      = 1 << 3;
  static constexpr uint8_t SLEEP_IND_VERBOSE  = 1 << 4;
  static constexpr uint8_t SLEEP_IND_LIB_BOOT = 1 << 5;
  static constexpr uint8_t SLEEP_IND_ENERGY   = 1 << 6;
  static constexpr uint8_t SLEEP_IND_ALL      = 0x7F;

  // Callback signatures. 'kind' is "PSM" / "DRX" / "eDRX" / "OOS" etc. when
  // SLEEP_IND_VERBOSE is set; empty otherwise. durationSeconds is reported
  // by the modem on the preceding sleep cycle's #SLEEP URC (precision 0.1 s);
  // 0 if not reported.
  typedef void (*SleepCb)(const String& kind, float durationSeconds, void* ctx);
  typedef void (*WakeCb)(void* ctx);
  typedef void (*EnergyCb)(float microWattH, void* ctx);

  // cid=0 (default) means "use whatever cid the modem's network layer resolved".
  explicit ST87M01NBIoT(ST87M01Modem& modem, uint8_t cid = 0);

  void setCid(uint8_t cid) { _cid = cid; }
  uint8_t cid() const { return _cid ? _cid : _modem.defaultCid(); }

  // ------------------------------------------------------------------ PSM
  //
  // Request PSM from the network with the given timers, in seconds:
  //   tauSeconds    -> requested Periodic-TAU (T3412 extended). This is the
  //                    time between tracking-area updates — roughly "how long
  //                    the modem will sleep between obligatory check-ins".
  //                    0 requests 'deactivated' (no periodic TAU).
  //   activeSeconds -> requested Active-Time (T3324). This is the time the
  //                    modem stays reachable after each TAU before dropping
  //                    into deep sleep. 0 requests 'deactivated'.
  //
  // The timers are encoded as GPRS Timer 3 / GPRS Timer 2 octets per
  // 3GPP TS 24.008; the encoder picks the smallest unit that represents the
  // requested value exactly, falling back to the coarsest unit (clamped to
  // the 5-bit value range) for very large values. Active Time on NB-IoT only
  // supports units of 2 s, 1 min, and 6 min, so values that don't fall on
  // one of those grids get rounded up to the next representable step.
  //
  // After a successful call, CEREG reporting is bumped to mode 4 so the
  // network-granted Active-Time and Periodic-TAU become visible via getPSM().
  bool requestPSM(uint32_t tauSeconds, uint32_t activeSeconds);

  // Disable PSM (AT+CPSMS=0). Previously-configured timer values are kept
  // on the modem side but not applied.
  bool disablePSM();

  // Disable PSM AND discard stored timer values, reverting to manufacturer
  // defaults (AT+CPSMS=2).
  bool resetPSM();

  // Read current PSM state. Requested values come from AT+CPSMS?; granted
  // values come from AT+CEREG? (requires CEREG mode >=4, which requestPSM()
  // sets automatically).
  bool getPSM(ST87M01PsmInfo& info);

  // ----------------------------------------------------------------- eDRX
  //
  // Request eDRX with a desired paging cycle and (optionally) paging-time
  // window, both in seconds. The cycle gets rounded up to the nearest value
  // supported by the NB-IoT eDRX table (minimum 20.48 s, maximum ~2.91 h).
  // Pass pagingWindowSeconds=0 to leave PTW untouched.
  bool requestEDRX(uint32_t cycleSeconds, uint32_t pagingWindowSeconds = 0);

  // Raw control — edrxCode is the 4-bit eDRX value from 3GPP TS 24.008
  // Table 10.5.5.32 (NB-IoT supports codes 2,3,5,9..15). ptwCode < 0 leaves
  // PTW untouched.
  bool requestEDRXRaw(uint8_t edrxCode, int8_t ptwCode = -1);

  // Disable eDRX (AT+CEDRXS=0). Previously-requested values are kept.
  bool disableEDRX();

  // Disable eDRX AND discard stored values (AT+CEDRXS=3).
  bool resetEDRX();

  // Read current eDRX state via AT+CEDRXRDP.
  bool getEDRX(ST87M01EdrxInfo& info);

  // --------------------------------------------------------------- Sleep
  //
  // Configure the modem's sleep mode. When enabled the modem drops into its
  // lowest power state once it's been idle for holdSeconds and no other
  // low-power condition (PSM window, eDRX slot, OOS) is active.
  //
  //   holdSeconds  -> seconds after the last AT command before the modem
  //                   is allowed to sleep. 0 = immediately.
  //   awakeSeconds -> the module-side awake-watchdog (see manual): 0 disables
  //                   it; values above 600 enable it, and the modem will
  //                   reset itself if AT activity stays quiet longer than
  //                   this after a wake. Values 1..600 are rejected by the
  //                   modem itself — don't pass them.
  //
  // NOTE: AT#SLEEPMODE is saved to NVM via AT#RESET=1; the library does NOT
  // automatically reset. Call modem.softReset() after all sleep/PSM/eDRX
  // configuration is in place if you want settings to survive a power cycle.
  bool setSleep(bool enable, uint32_t holdSeconds = 0, uint32_t awakeSeconds = 0);

  // Execute AT#SLEEPMODE with no parameters — cancels any pending hold-timer
  // and puts the modem to sleep immediately.
  bool sleepNow();

  // Configure which sleep-related URCs the modem emits. Pass 0 to suppress
  // them all; SLEEP_IND_ALL to enable the lot. Takes effect after a modem
  // reset (this setting is saved to NVM).
  bool setSleepIndications(uint8_t bitmap);

  // Configure the modem's wakeup-event policy (AT#WAKEUPEVENT). The two
  // inputs are the power-key pin and the UART RX line; each can be
  // independently enabled, with configurable polarity and internal pull.
  // Defaults match ST's recommended "wake on either" setup: pwrkey enabled
  // active-low with pull-up, UART enabled active-low with no pull.
  bool setWakeupEvent(bool pwrkeyEnable,
                      bool pwrkeyActiveLow  = true,
                      bool pwrkeyPullEnable = true,
                      bool pwrkeyPullUp     = true,
                      bool uartEnable       = true,
                      bool uartActiveLow    = true,
                      bool uartPullEnable   = false,
                      bool uartPullUp       = false);

  // Drive the modem's wakeup pin (if wired) and confirm AT responds. Thin
  // alias for ST87M01Modem::wake() — exposed here so sleep-related code in a
  // sketch can stay in one place.
  bool wake();

  // Event hooks. Pass nullptr as cb to clear. The callbacks run in URC
  // dispatch context (inside modem.poll() or inside another AT exchange that
  // happened to drain the URC), so they must not issue AT commands
  // themselves — follow the same rule as the other URC handlers in this
  // library.
  void onSleep(SleepCb cb, void* ctx = nullptr)   { _sleepCb  = cb; _sleepCtx  = ctx; }
  void onWake(WakeCb cb, void* ctx = nullptr)     { _wakeCb   = cb; _wakeCtx   = ctx; }
  void onEnergy(EnergyCb cb, void* ctx = nullptr) { _energyCb = cb; _energyCtx = ctx; }

  // Most recent #ENERGY URC value (consumption in µWh since the last such
  // URC per the manual). NAN until the first URC has been seen.
  float lastEnergyMicroWattH() const { return _lastEnergy; }

  // ------------------------------------------------- existing pass-throughs
  bool setCeregMode(uint8_t mode);
  bool getCellInfo(ST87M01CellInfo& cell);
  bool getSignal(ST87M01SignalInfo& sig);
  bool getOperator(ST87M01OperatorInfo& op);
  bool getImsi(String& imsi);
  bool ping(const char* host);
  bool getCandidateFreqs(String& out);

  // --------------------------- timer codec helpers (public for diagnostics)
  //
  // These take / return the 8-char binary string (MSB first) that the modem
  // speaks on the wire for CPSMS/CEREG. decodeTau/decodeActiveTime return 0
  // both when the timer is 'deactivated' and when the input is malformed —
  // use the raw string if you need to distinguish.
  static String   encodeTau(uint32_t seconds);
  static String   encodeActiveTime(uint32_t seconds);
  static uint32_t decodeTau(const String& s);
  static uint32_t decodeActiveTime(const String& s);

  // 4-bit eDRX code (3GPP TS 24.008 Table 10.5.5.32). encodeEdrx picks the
  // smallest NB-IoT-supported code whose value is >= seconds.
  static uint8_t  encodeEdrx(uint32_t seconds);
  static uint32_t decodeEdrx(uint8_t code);

  // 4-bit PTW code. PTW on NB-IoT is (code+1) * 2.56 s.
  static uint8_t  encodePtw(uint32_t seconds);
  static uint32_t decodePtw(uint8_t code);

private:
  ST87M01Modem& _modem;
  uint8_t _cid;

  SleepCb  _sleepCb  = nullptr; void* _sleepCtx  = nullptr;
  WakeCb   _wakeCb   = nullptr; void* _wakeCtx   = nullptr;
  EnergyCb _energyCb = nullptr; void* _energyCtx = nullptr;
  float    _lastEnergy = NAN;

  static void onSleepUrc(const String& line, void* ctx);
  static void onWakeupUrc(const String& line, void* ctx);
  static void onEnergyUrc(const String& line, void* ctx);
};
