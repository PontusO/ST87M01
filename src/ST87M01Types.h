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