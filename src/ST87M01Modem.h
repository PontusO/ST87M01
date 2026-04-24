// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#pragma once

#include <Arduino.h>
#include "ST87M01AT.h"
#include "ST87M01Types.h"

class ST87M01Modem {
public:
  static constexpr size_t MAX_SOCKETS = 3;

  explicit ST87M01Modem(Stream& serial, const ST87M01Pins& pins = {});

  bool begin(unsigned long baud = 115200);
  void poll();

  bool reset(unsigned long holdMs = 100, unsigned long bootMs = 5000);
  bool wake(unsigned long pulseMs = 100, unsigned long settleMs = 200);
  int  ringPin() const { return _pins.ring; }
  bool ringAsserted() const;
  const ST87M01Pins& pins() const { return _pins; }

  bool isAlive();
  bool setEcho(bool enable);
  bool setVerboseErrors(bool enable);
  bool setFunctionality(uint8_t fun);
  bool getFunctionality(uint8_t& fun);

  bool getModel(String& model);
  bool getRevision(String& rev);
  bool getImsi(String& imsi);
  bool getOperator(ST87M01OperatorInfo& info);

  bool attach(bool enable);
  bool isAttached(bool& attached);

  bool definePdpContext(uint8_t cid, const char* apn, const char* pdpType = "IP");
  bool activatePdp(uint8_t cid, bool enable);
  bool getActiveContext(uint8_t& cid);
  bool getLocalAddress(uint8_t cid, String& ip);
  bool configureIpStack(uint8_t cid, const char* ip);
  bool isIpStackActive(uint8_t cid);
  bool startModem();
  bool stopModem();
  bool setDns(const char* ipv4Dns);
  bool softReset(unsigned long bootMs = 20000);
  bool configureDns(const char* ipv4Dns);

  bool setCeregMode(uint8_t mode);
  bool getCellInfo(ST87M01CellInfo& cell);
  bool getSignal(ST87M01SignalInfo& sig);

  bool resolveHost(uint8_t cid, const char* host, String& ip);
  bool ping(uint8_t cid, const char* hostOrIp);

  bool createSocket(uint8_t cid, bool tcp, uint8_t& socketId, uint16_t localPort = 0);
  bool connectTcp(uint8_t cid, uint8_t socketId, const char* ip, uint16_t port);
  bool sendTcp(uint8_t cid, uint8_t socketId, const uint8_t* data, size_t len);
  bool sendUdp(uint8_t cid, uint8_t socketId, const char* ip, uint16_t port,
               const uint8_t* data, size_t len);
  int  readSocket(uint8_t cid, uint8_t socketId, uint8_t* data, size_t maxLen);
  bool closeSocket(uint8_t cid, uint8_t socketId);

  bool socketConnected(uint8_t socketId) const;
  size_t socketRxPending(uint8_t socketId) const;

  // Cumulative number of RX bytes that were dropped because the caller's
  // buffer was smaller than what the modem delivered in a single AT#IPREAD
  // transaction. Non-zero means the socket's data stream is incomplete and
  // should be treated as corrupted. Reset on the next createSocket() call.
  size_t socketRxDropped(uint8_t socketId) const;

  ST87M01AT& at() { return _at; }

  void setDefaultCid(uint8_t cid) { _defaultCid = cid; }
  uint8_t defaultCid() const { return _defaultCid; }

private:
  struct SocketSlot {
    bool inUse = false;
    bool connected = false;
    size_t rxPending = 0;
    size_t rxDropped = 0;
  };

  Stream& _serial;
  ST87M01AT _at;
  ST87M01Pins _pins;
  ST87M01CellInfo _lastCell;
  SocketSlot _sockets[MAX_SOCKETS];
  uint8_t _defaultCid = 1;

  static void onCeregUrc(const String& line, void* ctx);
  static void onIpRecvUrc(const String& line, void* ctx);
  static void onSocketClosedUrc(const String& line, void* ctx);
  void parseCereg(const String& line, ST87M01CellInfo& cell, bool isReadResponse);

  bool runSimple(const char* cmd, unsigned long timeoutMs = 2000);
  bool isIpLiteral(const char* s) const;

  void configurePins();
  void driveReset(bool active);
  void driveWakeup(bool active);
};
