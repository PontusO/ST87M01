// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#pragma once

#include <Arduino.h>

class ST87M01AT {
public:
  explicit ST87M01AT(Stream& serial);

  bool begin(unsigned long timeoutMs = 1000);
  void poll();

  bool send(const char* cmd);
  bool sendf(const __FlashStringHelper* fmt, ...);
  bool sendf(const char* fmt, ...);

  // Streaming interface for AT commands that carry a large hex-encoded
  // payload (AT#IPSENDTCP/UDP). Use this instead of sendf() when the
  // hex-expanded command would exceed sendf's 192-byte buffer. Call order:
  //   beginCommand("AT#IPSENDTCP=1,0,2,");
  //   writeHex(data, len);
  //   endCommand();
  // Then the usual expectOK(). Bytes stream directly to the underlying
  // serial port (and debug stream, if set) without any heap or fixed-size
  // intermediate buffer.
  void beginCommand(const char* prefix);
  void writeHex(const uint8_t* data, size_t len);
  void endCommand();

  bool expectOK(unsigned long timeoutMs = 1000);
  bool expectPrompt(char prompt = '>', unsigned long timeoutMs = 1000);

  bool readLine(String& line, unsigned long timeoutMs = 1000);
  bool waitLineStartsWith(const char* prefix, String& line, unsigned long timeoutMs = 1000);
  bool waitFinalResult(String* intermediate = nullptr, unsigned long timeoutMs = 1000);

  size_t readBytes(uint8_t* buf, size_t count, unsigned long timeoutMs);

  bool setVerboseErrors(bool enable);
  int lastCmeError() const { return _lastCmeError; }

  typedef void (*UrcHandler)(const String& line, void* ctx);
  bool registerUrcHandler(const char* prefix, UrcHandler cb, void* ctx);

  // Tee every line read from the modem and every command sent to the modem
  // into the given stream for debugging. Pass nullptr to disable. Very noisy;
  // normally only used to diagnose why a specific AT exchange is misbehaving.
  void setDebugStream(Stream* s) { _debug = s; }
  Stream* debugStream() const { return _debug; }

private:
  static constexpr size_t MAX_URC_HANDLERS = 8;

  struct Handler {
    String prefix;
    UrcHandler cb = nullptr;
    void* ctx = nullptr;
  };

  Stream& _serial;
  Stream* _debug = nullptr;
  unsigned long _defaultTimeoutMs;
  int _lastCmeError;
  String _rxLine;
  Handler _handlers[MAX_URC_HANDLERS];

  bool readRawLine(String& line, unsigned long timeoutMs);
  void dispatchIfUrc(const String& line);
  bool isFinalOk(const String& line) const;
  bool isFinalError(const String& line) const;
  void parseError(const String& line);
};