// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#pragma once

#include <Arduino.h>
#include <Udp.h>
#include "ST87M01Modem.h"

class ST87M01UDP : public UDP {
public:
  // cid=0 (default) means "use whatever cid the modem's network layer resolved".
  // Pass a non-zero cid to pin this UDP object to a specific PDP context.
  explicit ST87M01UDP(ST87M01Modem& modem, uint8_t cid = 0);

  void setCid(uint8_t cid) { _cid = cid; }
  uint8_t cid() const { return _cid ? _cid : _modem.defaultCid(); }

  // Cumulative count of RX bytes silently dropped because a single AT#IPREAD
  // delivered more than the internal RX buffer could hold. Non-zero means
  // some datagrams were truncated. Zeroed on begin()/stop().
  size_t droppedBytes() const { return _modem.socketRxDropped(_socketId); }

  uint8_t begin(uint16_t port) override;
  int beginPacket(IPAddress ip, uint16_t port) override;
  int beginPacket(const char* host, uint16_t port) override;
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  int endPacket() override;
  int parsePacket() override;
  int available() override;
  int read() override;
  int read(unsigned char* buffer, size_t len) override;
  int read(char* buffer, size_t len) override;
  int peek() override;
  void flush() override;
  void stop() override;
  // The ST87M01's #IPREAD response doesn't expose the remote address of the
  // last received UDP datagram, so these return 0 / 0.0.0.0 until the modem
  // layer is extended to capture it.
  IPAddress remoteIP() override { return IPAddress(0, 0, 0, 0); }
  uint16_t remotePort() override { return 0; }

private:
  static constexpr size_t BUF_SIZE = 1500;

  ST87M01Modem& _modem;
  uint8_t _cid;
  uint8_t _socketId;
  bool _open;
  String _host;
  uint16_t _port;
  uint8_t _txBuf[BUF_SIZE];
  size_t _txLen;
  uint8_t _rxBuf[BUF_SIZE];
  size_t _rxLen;
  size_t _rxPos;
};
