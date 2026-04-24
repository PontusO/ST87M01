// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#include "ST87M01AT.h"
#include <stdarg.h>

ST87M01AT::ST87M01AT(Stream& serial)
: _serial(serial), _defaultTimeoutMs(1000), _lastCmeError(0) {
}

bool ST87M01AT::begin(unsigned long timeoutMs) {
  _defaultTimeoutMs = timeoutMs;
  return true;
}

void ST87M01AT::poll() {
  String line;
  while (readRawLine(line, 0)) {
    if (line.length()) {
      dispatchIfUrc(line);
    }
  }
}

bool ST87M01AT::send(const char* cmd) {
  if (_debug) {
    _debug->print(">>> ");
    _debug->println(cmd);
  }
  _serial.print(cmd);
  _serial.print("\r");
  return true;
}

bool ST87M01AT::sendf(const __FlashStringHelper* fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf_P(buf, sizeof(buf), reinterpret_cast<PGM_P>(fmt), ap);
  va_end(ap);
  return send(buf);
}

bool ST87M01AT::sendf(const char* fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return send(buf);
}

void ST87M01AT::beginCommand(const char* prefix) {
  if (_debug) {
    _debug->print(">>> ");
    _debug->print(prefix);
  }
  _serial.print(prefix);
}

void ST87M01AT::writeHex(const uint8_t* data, size_t len) {
  static const char digits[] = "0123456789ABCDEF";
  for (size_t i = 0; i < len; ++i) {
    char pair[2] = { digits[(data[i] >> 4) & 0x0F], digits[data[i] & 0x0F] };
    _serial.write(reinterpret_cast<const uint8_t*>(pair), 2);
    if (_debug) {
      _debug->write(reinterpret_cast<const uint8_t*>(pair), 2);
    }
  }
}

void ST87M01AT::endCommand() {
  _serial.print("\r");
  if (_debug) _debug->println();
}

bool ST87M01AT::expectOK(unsigned long timeoutMs) {
  String line;
  return waitFinalResult(&line, timeoutMs);
}

bool ST87M01AT::expectPrompt(char prompt, unsigned long timeoutMs) {
  unsigned long start = millis();
  while ((millis() - start) < timeoutMs) {
    if (_serial.available()) {
      int c = _serial.read();
      if (c == prompt) {
        return true;
      }
    }
    yield();
  }
  return false;
}

bool ST87M01AT::readLine(String& line, unsigned long timeoutMs) {
  while (true) {
    if (!readRawLine(line, timeoutMs)) {
      return false;
    }
    if (!line.length()) {
      continue;
    }
    if (line.startsWith("+") || line.startsWith("#")) {
      // Could be URC or response. Caller decides.
      return true;
    }
    if (line == "OK" || line == "ERROR" || line.startsWith("+CME ERROR:")) {
      return true;
    }
    return true;
  }
}

bool ST87M01AT::waitLineStartsWith(const char* prefix, String& line, unsigned long timeoutMs) {
  unsigned long start = millis();
  while ((millis() - start) < timeoutMs) {
    if (!readLine(line, timeoutMs)) {
      return false;
    }
    if (line.startsWith(prefix)) {
      return true;
    }
    if (isFinalError(line)) {
      parseError(line);
      return false;
    }
  }
  return false;
}

bool ST87M01AT::waitFinalResult(String* intermediate, unsigned long timeoutMs) {
  unsigned long start = millis();
  String line;
  while ((millis() - start) < timeoutMs) {
    if (!readRawLine(line, timeoutMs)) {
      return false;
    }
    if (!line.length()) {
      continue;
    }
    if (isFinalOk(line)) {
      return true;
    }
    if (isFinalError(line)) {
      parseError(line);
      return false;
    }
    if (line.startsWith("+") || line.startsWith("#")) {
      if (intermediate) {
        *intermediate = line;
      } else {
        dispatchIfUrc(line);
      }
    }
  }
  return false;
}

bool ST87M01AT::setVerboseErrors(bool enable) {
  sendf("AT+CMEE=%d", enable ? 2 : 0);
  return expectOK();
}

bool ST87M01AT::registerUrcHandler(const char* prefix, UrcHandler cb, void* ctx) {
  for (size_t i = 0; i < MAX_URC_HANDLERS; ++i) {
    if (_handlers[i].cb == nullptr) {
      _handlers[i].prefix = prefix;
      _handlers[i].cb = cb;
      _handlers[i].ctx = ctx;
      return true;
    }
  }
  return false;
}

size_t ST87M01AT::readBytes(uint8_t* buf, size_t count, unsigned long timeoutMs) {
  size_t got = 0;
  unsigned long start = millis();
  while (got < count) {
    if (_serial.available()) {
      buf[got++] = static_cast<uint8_t>(_serial.read());
      continue;
    }
    if ((millis() - start) >= timeoutMs) break;
    yield();
  }
  return got;
}

bool ST87M01AT::readRawLine(String& line, unsigned long timeoutMs) {
  // Accumulate into _rxLine across calls so a line that arrives slowly
  // (URCs coming out of deep sleep drip-feed char-by-char) doesn't get
  // chopped into useless single-char fragments.
  unsigned long start = millis();

  while (true) {
    while (_serial.available()) {
      char c = static_cast<char>(_serial.read());
      if (c == '\r') continue;
      if (c == '\n') {
        if (_rxLine.length()) {
          line = _rxLine;
          _rxLine = "";
          if (_debug) {
            _debug->print("<<< ");
            _debug->println(line);
          }
          return true;
        }
        continue;  // blank line
      }
      _rxLine += c;
    }

    if (timeoutMs == 0) {
      line = "";
      return false;  // no more data, partial stays in _rxLine
    }
    if ((millis() - start) >= timeoutMs) {
      line = "";
      return false;  // timed out; keep partial for a future call
    }
    yield();
  }
}

void ST87M01AT::dispatchIfUrc(const String& line) {
  for (size_t i = 0; i < MAX_URC_HANDLERS; ++i) {
    if (_handlers[i].cb && line.startsWith(_handlers[i].prefix)) {
      _handlers[i].cb(line, _handlers[i].ctx);
      return;
    }
  }
}

bool ST87M01AT::isFinalOk(const String& line) const {
  return line == "OK";
}

bool ST87M01AT::isFinalError(const String& line) const {
  return line == "ERROR" || line.startsWith("+CME ERROR:");
}

void ST87M01AT::parseError(const String& line) {
  _lastCmeError = 0;
  if (line.startsWith("+CME ERROR:")) {
    _lastCmeError = line.substring(11).toInt();
  }
}