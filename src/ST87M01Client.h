// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#pragma once

#include <Arduino.h>
#include <Client.h>
#include "ST87M01Modem.h"

class ST87M01Client : public Client {
public:
  // cid=0 (default) means "use whatever cid the modem's network layer resolved".
  // Pass a non-zero cid to pin this client to a specific PDP context.
  explicit ST87M01Client(ST87M01Modem& modem, uint8_t cid = 0);

  int connect(IPAddress ip, uint16_t port) override;
  int connect(const char* host, uint16_t port) override;
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buf, size_t size) override;
  int available() override;
  int read() override;
  int read(uint8_t* buf, size_t size);
  int peek() override;
  void flush() override;
  void stop() override;
  uint8_t connected() override;
  operator bool() override;

  void setCid(uint8_t cid) { _cid = cid; }
  uint8_t cid() const { return _cid ? _cid : _modem.defaultCid(); }

  // Cumulative count of RX bytes that were dropped because a single
  // AT#IPREAD frame was larger than the internal RX buffer. The buffer is
  // sized to the documented upper bound of AT#IPPARAMS max_ip_frame_size
  // (1600 bytes), so this should stay at zero. Multi-frame responses are
  // NOT a source of drops — read() loops through them via #IPRECV-driven
  // refills. Zeroed on connect/stop.
  size_t droppedBytes() const { return _modem.socketRxDropped(_socketId); }

private:
  // Sized to the documented upper bound of AT#IPPARAMS max_ip_frame_size
  // (per ST: 512..1600 bytes). The library sets 1514 in setIpStack, but
  // empirically the modem can deliver frames up to ~1540 bytes regardless,
  // so cover the full documented ceiling.
  static constexpr size_t RX_BUF_SIZE = 1600;

  ST87M01Modem& _modem;
  uint8_t _cid;
  uint8_t _socketId;
  bool _opened;
  uint8_t _rxBuf[RX_BUF_SIZE];
  size_t _rxLen;
  size_t _rxPos;

  bool fillRx();
};
