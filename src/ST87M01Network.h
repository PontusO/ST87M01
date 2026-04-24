#pragma once

#include <Arduino.h>
#include "ST87M01Modem.h"

class ST87M01Network {
public:
  explicit ST87M01Network(ST87M01Modem& modem);

  bool begin(const char* apn, uint8_t cid = 1);
  bool connected();
  String localIP();
  uint8_t cid() const { return _cid; }

private:
  ST87M01Modem& _modem;
  uint8_t _cid;
};