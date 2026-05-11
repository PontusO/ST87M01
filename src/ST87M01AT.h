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

  // Push raw bytes directly to the modem. Used for AT commands that accept a
  // binary payload AFTER an OK response (e.g. AT#TLSCERTADD without inline
  // <data>) — the host first sends the command + \r, waits for OK, then
  // streams exactly <data_length> bytes via this method. No framing or
  // escaping is applied.
  void writeRaw(const uint8_t* data, size_t len);

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

  // Pre-send hook fired before every send()/beginCommand(). The modem layer
  // sets this to a function that drains pending URCs (so the sleep flag is
  // current) and, if the modem is sleeping, performs the wake sequence
  // before the real command is written. Recursion is guarded internally —
  // the hook may safely re-enter send() to issue probe commands like AT.
  typedef void (*PreSendHook)(void* ctx);
  void setPreSendHook(PreSendHook hook, void* ctx) { _preSendHook = hook; _preSendCtx = ctx; }

  // Tee every line read from the modem and every command sent to the modem
  // into the given stream for debugging. Pass nullptr to disable. Very noisy;
  // normally only used to diagnose why a specific AT exchange is misbehaving.
  void setDebugStream(Stream* s) { _debug = s; }
  Stream* debugStream() const { return _debug; }

private:
  static constexpr size_t MAX_URC_HANDLERS = 12;

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
  PreSendHook _preSendHook = nullptr;
  void* _preSendCtx = nullptr;
  bool _inPreSend = false;

  void runPreSendHook();

  bool readRawLine(String& line, unsigned long timeoutMs);
  void dispatchIfUrc(const String& line);
  bool isFinalOk(const String& line) const;
  bool isFinalError(const String& line) const;
  void parseError(const String& line);
};