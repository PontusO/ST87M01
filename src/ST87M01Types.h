// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#pragma once

#include <Arduino.h>

enum class ST87M01RegStatus : uint8_t {
  NotRegistered = 0,
  RegisteredHome = 1,
  Searching = 2,
  RegistrationDenied = 3,
  Unknown = 4,
  RegisteredRoaming = 5
};

struct ST87M01CellInfo {
  bool valid = false;
  ST87M01RegStatus reg = ST87M01RegStatus::Unknown;
  String tac;
  String ci;
  int act = -1;
};

struct ST87M01SignalInfo {
  int rxlev = -1;
  int ber = -1;
  int rsrq = -1;
  int rsrp = -1;
};

struct ST87M01OperatorInfo {
  String mode;
  String format;
  String oper;
  int act = -1;
};

struct ST87M01SocketState {
  bool inUse = false;
  bool connected = false;
  uint8_t contextId = 0;
  uint8_t socketId = 0;
  bool tcp = true;
};

struct ST87M01Pins {
  int8_t reset = -1;
  int8_t wakeup = -1;
  int8_t ring = -1;
  bool resetActiveLow = true;
  bool wakeupActiveLow = true;
  bool ringActiveLow = true;
};

// Populated by ST87M01NBIoT::getPSM(). Requested values come from AT+CPSMS?;
// granted values come from AT+CEREG? with CEREG mode >=4 (requestPSM() bumps
// the mode automatically, so after a successful request grants show up here).
// A seconds field of 0 means either "not set" or "timer deactivated" per
// 3GPP; the raw 8-bit binary string is kept alongside so callers can tell.
struct ST87M01PsmInfo {
  bool enabled = false;
  uint32_t requestedTauSeconds = 0;
  uint32_t requestedActiveSeconds = 0;
  uint32_t grantedTauSeconds = 0;
  uint32_t grantedActiveSeconds = 0;
  String requestedTauRaw;
  String requestedActiveRaw;
  String grantedTauRaw;
  String grantedActiveRaw;
};

// Populated by ST87M01NBIoT::getEDRX() from AT+CEDRXRDP. actType=0 means the
// serving cell is not using eDRX (the other fields are then unset).
struct ST87M01EdrxInfo {
  bool enabled = false;
  uint8_t actType = 0;
  uint32_t requestedSeconds = 0;
  uint32_t grantedSeconds = 0;
  uint32_t pagingWindowSeconds = 0;
  String requestedRaw;
  String grantedRaw;
  String pagingWindowRaw;
};