// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#pragma once

#include <Arduino.h>
#include "ST87M01Modem.h"

class ST87M01NBIoT {
public:
  // cid=0 (default) means "use whatever cid the modem's network layer resolved".
  explicit ST87M01NBIoT(ST87M01Modem& modem, uint8_t cid = 0);

  void setCid(uint8_t cid) { _cid = cid; }
  uint8_t cid() const { return _cid ? _cid : _modem.defaultCid(); }

  bool setSleepMode(bool enable);
  bool setPSM(bool enable);
  bool setCeregMode(uint8_t mode);

  bool getCellInfo(ST87M01CellInfo& cell);
  bool getSignal(ST87M01SignalInfo& sig);
  bool getOperator(ST87M01OperatorInfo& op);
  bool getImsi(String& imsi);

  bool ping(const char* host);
  bool getCandidateFreqs(String& out);

private:
  ST87M01Modem& _modem;
  uint8_t _cid;
};