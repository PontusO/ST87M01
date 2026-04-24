// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#include "ST87M01NBIoT.h"
#include <math.h>

// ---------------------------------------------------------------------------
// Timer codec tables.
//
// GPRS Timer 3 (T3412 ext, used for Periodic-TAU) — 3GPP TS 24.008 Table
// 10.5.163a. Unit code in bits 8..6, 5-bit value in bits 5..1.
//   000: 10 min   001: 1 hr     010: 10 hr    011: 2 s
//   100: 30 s     101: 1 min    110: 320 hr   111: deactivated (unsupported)
//
// GPRS Timer 2 (T3324, used for Active-Time) — 3GPP TS 24.008 Table 10.5.163.
// The CPSMS manual entry notes units 3..6 are not supported on NB-IoT; that
// leaves 000 (2 s), 001 (1 min), 010 (6 min), and 111 (deactivated).
// ---------------------------------------------------------------------------

namespace {

struct TimerUnit {
  uint8_t  code;   // 3-bit unit identifier
  uint32_t tick;   // seconds per count
};

// Ordered smallest-tick-first so the encoder picks the finest-precision unit
// that can represent a given duration exactly.
const TimerUnit kTauUnits[] = {
  {0b011,       2},
  {0b100,      30},
  {0b101,      60},
  {0b000,     600},
  {0b001,    3600},
  {0b010,   36000},
  {0b110, 1152000},
};

const TimerUnit kAtUnits[] = {
  {0b000,   2},
  {0b001,  60},
  {0b010, 360},
};

// Decode lookup, indexed by unit code (0..6). Code 7 = deactivated.
const uint32_t kTauTickByCode[8] = {
  600,      // 000
  3600,     // 001
  36000,    // 010
  2,        // 011
  30,       // 100
  60,       // 101
  1152000,  // 110
  0         // 111 (deactivated)
};

const uint32_t kAtTickByCode[8] = {
  2,   // 000
  60,  // 001
  360, // 010
  0,   // 011 unsupported
  0,   // 100 unsupported
  0,   // 101 unsupported
  0,   // 110 unsupported
  0    // 111 deactivated
};

// eDRX cycle values (3GPP TS 24.008 Table 10.5.5.32), stored as hundredths
// of a second so the lookup stays in integer math. Index is the 4-bit code.
const uint32_t kEdrxHundredths[16] = {
      512,    1024,    2048,    4096,
     6144,    8192,   10240,   12288,
    14336,   16384,   32768,   65536,
   131072,  262144,  524288, 1048576,
};

// NB-IoT-supported eDRX codes (per manual: "does not support 0,1,4,6,7,8").
const uint8_t kEdrxSupported[] = {2, 3, 5, 9, 10, 11, 12, 13, 14, 15};

String packTimerByte(uint32_t value5, uint8_t unit3) {
  char buf[9];
  for (int i = 0; i < 3; ++i) {
    buf[i] = ((unit3 >> (2 - i)) & 1) ? '1' : '0';
  }
  for (int i = 0; i < 5; ++i) {
    buf[3 + i] = ((value5 >> (4 - i)) & 1) ? '1' : '0';
  }
  buf[8] = '\0';
  return String(buf);
}

String encodeTimerByte(uint32_t seconds, const TimerUnit* units, size_t nUnits) {
  if (seconds == 0) return String("11111111");  // unit 111 = deactivated
  for (size_t i = 0; i < nUnits; ++i) {
    if (seconds <= 31u * units[i].tick && (seconds % units[i].tick) == 0) {
      return packTimerByte(seconds / units[i].tick, units[i].code);
    }
  }
  // No exact fit in a small unit; fall back to the coarsest and round.
  const TimerUnit& u = units[nUnits - 1];
  uint32_t v = (seconds + u.tick / 2) / u.tick;
  if (v == 0) v = 1;
  if (v > 31) v = 31;
  return packTimerByte(v, u.code);
}

bool parseTimerByte(const String& s, uint8_t& unitOut, uint8_t& valueOut) {
  if (s.length() != 8) return false;
  uint8_t unit = 0, value = 0;
  for (int i = 0; i < 3; ++i) {
    char c = s.charAt(i);
    if (c != '0' && c != '1') return false;
    unit = (unit << 1) | (c == '1' ? 1 : 0);
  }
  for (int i = 3; i < 8; ++i) {
    char c = s.charAt(i);
    if (c != '0' && c != '1') return false;
    value = (value << 1) | (c == '1' ? 1 : 0);
  }
  unitOut = unit;
  valueOut = value;
  return true;
}

String stripQuotes(const String& s) {
  if (s.length() >= 2 && s.charAt(0) == '"' && s.charAt(s.length() - 1) == '"') {
    return s.substring(1, s.length() - 1);
  }
  return s;
}

// Comma-tokenize a line, honoring double-quoted fields (quotes are stripped).
// Returns the number of tokens written into out[], up to maxTokens.
size_t tokenizeCsv(const String& line, String* out, size_t maxTokens) {
  size_t n = 0;
  String cur;
  bool inQuote = false;
  for (size_t i = 0; i < line.length() && n < maxTokens; ++i) {
    char c = line.charAt(i);
    if (c == '"') {
      inQuote = !inQuote;
      continue;
    }
    if (c == ',' && !inQuote) {
      out[n++] = cur;
      cur = "";
      continue;
    }
    cur += c;
  }
  if (n < maxTokens) out[n++] = cur;
  return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Construction / URC wiring.
// ---------------------------------------------------------------------------

ST87M01NBIoT::ST87M01NBIoT(ST87M01Modem& modem, uint8_t cid)
: _modem(modem), _cid(cid) {
  // Register URC handlers once. The modem emits these only when AT#SLEEPIND
  // has been configured non-zero and persisted via AT#RESET=1; otherwise
  // these handlers simply never fire.
  _modem.at().registerUrcHandler("#SLEEP",  &ST87M01NBIoT::onSleepUrc,  this);
  _modem.at().registerUrcHandler("#WAKEUP", &ST87M01NBIoT::onWakeupUrc, this);
  _modem.at().registerUrcHandler("#ENERGY", &ST87M01NBIoT::onEnergyUrc, this);
}

// ---------------------------------------------------------------------------
// PSM.
// ---------------------------------------------------------------------------

bool ST87M01NBIoT::requestPSM(uint32_t tauSeconds, uint32_t activeSeconds) {
  String tauStr = encodeTau(tauSeconds);
  String atStr  = encodeActiveTime(activeSeconds);

  _modem.at().sendf("AT+CPSMS=1,,,\"%s\",\"%s\"", tauStr.c_str(), atStr.c_str());
  if (!_modem.at().expectOK(5000)) return false;

  // Bump CEREG to mode 4 so AT+CEREG? returns network-granted Active-Time and
  // Periodic-TAU (the whole reason to request PSM is to learn what the
  // network actually gave you, not just what you asked for).
  _modem.setCeregMode(4);
  return true;
}

bool ST87M01NBIoT::disablePSM() {
  _modem.at().send("AT+CPSMS=0");
  return _modem.at().expectOK(5000);
}

bool ST87M01NBIoT::resetPSM() {
  _modem.at().send("AT+CPSMS=2");
  return _modem.at().expectOK(5000);
}

bool ST87M01NBIoT::getPSM(ST87M01PsmInfo& info) {
  info = ST87M01PsmInfo{};

  // --- Requested values, from AT+CPSMS? ---
  String line;
  _modem.at().send("AT+CPSMS?");
  if (!_modem.at().waitLineStartsWith("+CPSMS:", line, 2000)) return false;
  if (!_modem.at().expectOK(2000)) return false;

  {
    int colon = line.indexOf(':');
    if (colon < 0) return false;
    String rest = line.substring(colon + 1);
    rest.trim();

    String tok[5];
    size_t n = tokenizeCsv(rest, tok, 5);
    for (size_t i = 0; i < n; ++i) tok[i].trim();

    info.enabled = (n >= 1 && tok[0].toInt() == 1);
    if (n >= 4) {
      info.requestedTauRaw = stripQuotes(tok[3]);
      info.requestedTauSeconds = decodeTau(info.requestedTauRaw);
    }
    if (n >= 5) {
      info.requestedActiveRaw = stripQuotes(tok[4]);
      info.requestedActiveSeconds = decodeActiveTime(info.requestedActiveRaw);
    }
  }

  // --- Granted values, from AT+CEREG? (requires CEREG mode >= 4) ---
  _modem.at().send("AT+CEREG?");
  if (!_modem.at().waitLineStartsWith("+CEREG:", line, 2000)) return true;  // no grant yet is OK
  _modem.at().expectOK(2000);

  {
    int colon = line.indexOf(':');
    if (colon < 0) return true;
    String rest = line.substring(colon + 1);
    rest.trim();

    // Fields in AT+CEREG? with n=4:
    //   0:n  1:stat  2:tac  3:ci  4:AcT  5:cause_type  6:reject_cause
    //   7:Active-Time  8:Periodic-TAU
    String tok[9];
    size_t n = tokenizeCsv(rest, tok, 9);
    for (size_t i = 0; i < n; ++i) tok[i].trim();

    if (n >= 8) {
      info.grantedActiveRaw = stripQuotes(tok[7]);
      info.grantedActiveSeconds = decodeActiveTime(info.grantedActiveRaw);
    }
    if (n >= 9) {
      info.grantedTauRaw = stripQuotes(tok[8]);
      info.grantedTauSeconds = decodeTau(info.grantedTauRaw);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// eDRX.
// ---------------------------------------------------------------------------

bool ST87M01NBIoT::requestEDRX(uint32_t cycleSeconds, uint32_t pagingWindowSeconds) {
  uint8_t code = encodeEdrx(cycleSeconds);
  int8_t  ptw  = (pagingWindowSeconds > 0) ? (int8_t)encodePtw(pagingWindowSeconds) : -1;
  return requestEDRXRaw(code, ptw);
}

bool ST87M01NBIoT::requestEDRXRaw(uint8_t edrxCode, int8_t ptwCode) {
  // Both eDRX value and PTW are 4-bit fields, encoded on the wire as a
  // 4-char binary string MSB-first (manual: "Half a byte in a 4-bit format").
  char bits[5];
  for (int i = 0; i < 4; ++i) bits[i] = ((edrxCode >> (3 - i)) & 1) ? '1' : '0';
  bits[4] = '\0';

  // AcT-type 5 = E-UTRAN (NB-S1). mode=2 enables the +CEDRXP URC on
  // parameter changes — useful for catching network re-grants.
  _modem.at().sendf("AT+CEDRXS=2,5,\"%s\"", bits);
  if (!_modem.at().expectOK(5000)) return false;

  if (ptwCode >= 0) {
    for (int i = 0; i < 4; ++i) bits[i] = ((ptwCode >> (3 - i)) & 1) ? '1' : '0';
    _modem.at().sendf("AT#PTW=\"%s\"", bits);
    if (!_modem.at().expectOK(5000)) return false;
  }
  return true;
}

bool ST87M01NBIoT::disableEDRX() {
  _modem.at().send("AT+CEDRXS=0");
  return _modem.at().expectOK(5000);
}

bool ST87M01NBIoT::resetEDRX() {
  _modem.at().send("AT+CEDRXS=3");
  return _modem.at().expectOK(5000);
}

bool ST87M01NBIoT::getEDRX(ST87M01EdrxInfo& info) {
  info = ST87M01EdrxInfo{};

  String line;
  _modem.at().send("AT+CEDRXRDP");
  if (!_modem.at().waitLineStartsWith("+CEDRXRDP:", line, 2000)) {
    // Some firmwares return the fields without the +CEDRXRDP: prefix —
    // fall back to a bare-number line if the prefixed one didn't arrive.
    return _modem.at().expectOK(2000);  // at least consume the OK
  }
  _modem.at().expectOK(2000);

  int colon = line.indexOf(':');
  if (colon < 0) return false;
  String rest = line.substring(colon + 1);
  rest.trim();

  String tok[4];
  size_t n = tokenizeCsv(rest, tok, 4);
  for (size_t i = 0; i < n; ++i) tok[i].trim();

  if (n >= 1) info.actType = (uint8_t)tok[0].toInt();
  info.enabled = (info.actType != 0);
  auto parseFourBit = [](const String& s, uint8_t& out) -> bool {
    if (s.length() != 4) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
      char c = s.charAt(i);
      if (c != '0' && c != '1') return false;
      out = (out << 1) | (c == '1' ? 1 : 0);
    }
    return true;
  };

  if (n >= 2) {
    info.requestedRaw = stripQuotes(tok[1]);
    uint8_t code = 0;
    if (parseFourBit(info.requestedRaw, code)) info.requestedSeconds = decodeEdrx(code);
  }
  if (n >= 3) {
    info.grantedRaw = stripQuotes(tok[2]);
    uint8_t code = 0;
    if (parseFourBit(info.grantedRaw, code)) info.grantedSeconds = decodeEdrx(code);
  }
  if (n >= 4) {
    info.pagingWindowRaw = stripQuotes(tok[3]);
    uint8_t code = 0;
    if (parseFourBit(info.pagingWindowRaw, code)) info.pagingWindowSeconds = decodePtw(code);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Sleep mode.
// ---------------------------------------------------------------------------

bool ST87M01NBIoT::setSleep(bool enable, uint32_t holdSeconds, uint32_t awakeSeconds) {
  if (!enable) {
    _modem.at().send("AT#SLEEPMODE=0");
    return _modem.at().expectOK(2000);
  }
  if (awakeSeconds > 0) {
    _modem.at().sendf("AT#SLEEPMODE=1,%u,%u",
                      (unsigned)holdSeconds, (unsigned)awakeSeconds);
  } else if (holdSeconds > 0) {
    _modem.at().sendf("AT#SLEEPMODE=1,%u", (unsigned)holdSeconds);
  } else {
    _modem.at().send("AT#SLEEPMODE=1");
  }
  return _modem.at().expectOK(2000);
}

bool ST87M01NBIoT::sleepNow() {
  _modem.at().send("AT#SLEEPMODE");
  return _modem.at().expectOK(2000);
}

bool ST87M01NBIoT::setSleepIndications(uint8_t bitmap) {
  _modem.at().sendf("AT#SLEEPIND=%u", (unsigned)bitmap);
  return _modem.at().expectOK(2000);
}

bool ST87M01NBIoT::setWakeupEvent(bool pwrkeyEnable, bool pwrkeyActiveLow,
                                  bool pwrkeyPullEnable, bool pwrkeyPullUp,
                                  bool uartEnable, bool uartActiveLow,
                                  bool uartPullEnable, bool uartPullUp) {
  uint8_t pwr = (pwrkeyEnable     ? 0x01 : 0)
              | (pwrkeyActiveLow  ? 0x02 : 0)
              | (pwrkeyPullEnable ? 0x04 : 0)
              | (pwrkeyPullUp     ? 0x08 : 0);
  uint8_t ua  = (uartEnable       ? 0x01 : 0)
              | (uartActiveLow    ? 0x02 : 0)
              | (uartPullEnable   ? 0x04 : 0)
              | (uartPullUp       ? 0x08 : 0);
  _modem.at().sendf("AT#WAKEUPEVENT=%u,%u", (unsigned)pwr, (unsigned)ua);
  return _modem.at().expectOK(2000);
}

bool ST87M01NBIoT::wake() {
  return _modem.wake();
}

// ---------------------------------------------------------------------------
// URC handlers.
// ---------------------------------------------------------------------------

void ST87M01NBIoT::onSleepUrc(const String& line, void* ctx) {
  // Prefix "#SLEEP" also matches #SLEEPIND: and #SLEEPMODE: read-back lines.
  // Those are responses to our own commands, consumed by the issuing code,
  // but filter here defensively in case a URC-style one ever slips through.
  if (line.startsWith("#SLEEPIND") || line.startsWith("#SLEEPMODE")) return;

  auto* self = static_cast<ST87M01NBIoT*>(ctx);
  if (!self->_sleepCb) return;

  // Verbose form: "#SLEEP <KIND> <duration>s" (e.g. "#SLEEP PSM 3599.9s").
  // Bare form: "#SLEEP".
  String kind;
  float duration = 0.0f;
  int sp1 = line.indexOf(' ');
  if (sp1 > 0) {
    int sp2 = line.indexOf(' ', sp1 + 1);
    if (sp2 > sp1) {
      kind = line.substring(sp1 + 1, sp2);
      String d = line.substring(sp2 + 1);
      if (d.endsWith("s")) d = d.substring(0, d.length() - 1);
      duration = d.toFloat();
    } else {
      kind = line.substring(sp1 + 1);
    }
  }
  self->_sleepCb(kind, duration, self->_sleepCtx);
}

void ST87M01NBIoT::onWakeupUrc(const String& line, void* ctx) {
  // "#WAKEUP" as URC vs. "#WAKEUPEVENT:" as read-back of the config command.
  if (line.startsWith("#WAKEUPEVENT")) return;

  auto* self = static_cast<ST87M01NBIoT*>(ctx);
  if (self->_wakeCb) self->_wakeCb(self->_wakeCtx);
}

void ST87M01NBIoT::onEnergyUrc(const String& line, void* ctx) {
  auto* self = static_cast<ST87M01NBIoT*>(ctx);
  // "#ENERGY: 414.7" or "#ENERGY 414.7" — be lenient about the separator.
  int sep = line.indexOf(':');
  if (sep < 0) sep = line.indexOf(' ');
  if (sep < 0) return;
  String v = line.substring(sep + 1);
  v.trim();
  self->_lastEnergy = v.toFloat();
  if (self->_energyCb) self->_energyCb(self->_lastEnergy, self->_energyCtx);
}

// ---------------------------------------------------------------------------
// Existing pass-throughs (unchanged from the previous revision of this file).
// ---------------------------------------------------------------------------

bool ST87M01NBIoT::setCeregMode(uint8_t mode) {
  return _modem.setCeregMode(mode);
}

bool ST87M01NBIoT::getCellInfo(ST87M01CellInfo& cell) {
  return _modem.getCellInfo(cell);
}

bool ST87M01NBIoT::getSignal(ST87M01SignalInfo& sig) {
  return _modem.getSignal(sig);
}

bool ST87M01NBIoT::getOperator(ST87M01OperatorInfo& op) {
  return _modem.getOperator(op);
}

bool ST87M01NBIoT::getImsi(String& imsi) {
  return _modem.getImsi(imsi);
}

bool ST87M01NBIoT::ping(const char* host) {
  return _modem.ping(cid(), host);
}

bool ST87M01NBIoT::getCandidateFreqs(String& out) {
  String line;
  _modem.at().send("AT#CANDFREQ?");
  out = "";

  while (_modem.at().readLine(line, 1000)) {
    if (line == "OK") {
      return true;
    }
    if (line == "ERROR" || line.startsWith("+CME ERROR:")) {
      return false;
    }
    if (line.startsWith("#CANDFREQ")) {
      out += line;
      out += "\n";
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Codec helpers (static; exposed for diagnostics + unit tests).
// ---------------------------------------------------------------------------

String ST87M01NBIoT::encodeTau(uint32_t seconds) {
  return encodeTimerByte(seconds, kTauUnits, sizeof(kTauUnits) / sizeof(kTauUnits[0]));
}

String ST87M01NBIoT::encodeActiveTime(uint32_t seconds) {
  return encodeTimerByte(seconds, kAtUnits, sizeof(kAtUnits) / sizeof(kAtUnits[0]));
}

uint32_t ST87M01NBIoT::decodeTau(const String& s) {
  uint8_t unit = 0, value = 0;
  if (!parseTimerByte(s, unit, value)) return 0;
  if (unit == 7) return 0;  // deactivated
  return (uint32_t)value * kTauTickByCode[unit];
}

uint32_t ST87M01NBIoT::decodeActiveTime(const String& s) {
  uint8_t unit = 0, value = 0;
  if (!parseTimerByte(s, unit, value)) return 0;
  if (unit == 7) return 0;
  uint32_t tick = kAtTickByCode[unit];
  if (tick == 0) return 0;  // unsupported unit code
  return (uint32_t)value * tick;
}

uint8_t ST87M01NBIoT::encodeEdrx(uint32_t seconds) {
  // Pick the smallest NB-IoT-supported code whose value is >= seconds.
  uint64_t wantH = (uint64_t)seconds * 100;
  for (size_t i = 0; i < sizeof(kEdrxSupported) / sizeof(kEdrxSupported[0]); ++i) {
    uint8_t c = kEdrxSupported[i];
    if ((uint64_t)kEdrxHundredths[c] >= wantH) return c;
  }
  return kEdrxSupported[sizeof(kEdrxSupported) / sizeof(kEdrxSupported[0]) - 1];
}

uint32_t ST87M01NBIoT::decodeEdrx(uint8_t code) {
  if (code >= 16) return 0;
  return kEdrxHundredths[code] / 100;
}

uint8_t ST87M01NBIoT::encodePtw(uint32_t seconds) {
  // PTW = (code + 1) * 2.56 s, code in [0..15]. Round to nearest step.
  if (seconds == 0) return 0;
  uint32_t hundredths = seconds * 100;
  // (code + 1) = (hundredths + 128) / 256, so code = that - 1.
  uint32_t step = (hundredths + 128) / 256;
  if (step == 0) step = 1;
  if (step > 16) step = 16;
  return (uint8_t)(step - 1);
}

uint32_t ST87M01NBIoT::decodePtw(uint8_t code) {
  if (code >= 16) return 0;
  // (code + 1) * 256 hundredths → divide by 100 for whole seconds (floor).
  return ((uint32_t)(code + 1) * 256) / 100;
}
