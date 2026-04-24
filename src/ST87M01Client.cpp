// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#include "ST87M01Client.h"

ST87M01Client::ST87M01Client(ST87M01Modem& modem, uint8_t cid)
: _modem(modem), _cid(cid), _socketId(0xFF), _opened(false), _rxLen(0), _rxPos(0) {
}

int ST87M01Client::connect(IPAddress ip, uint16_t port) {
  String host = ip.toString();
  if (!_modem.createSocket(cid(), true, _socketId)) return 0;
  _opened = true;
  if (!_modem.connectTcp(cid(), _socketId, host.c_str(), port)) {
    stop();
    return 0;
  }
  return 1;
}

int ST87M01Client::connect(const char* host, uint16_t port) {
  String ip;
  if (!_modem.resolveHost(cid(), host, ip) || !ip.length()) return 0;

  if (!_modem.createSocket(cid(), true, _socketId)) return 0;
  _opened = true;
  if (!_modem.connectTcp(cid(), _socketId, ip.c_str(), port)) {
    stop();
    return 0;
  }
  return 1;
}

size_t ST87M01Client::write(uint8_t b) {
  return write(&b, 1);
}

size_t ST87M01Client::write(const uint8_t* buf, size_t size) {
  if (!connected()) return 0;
  return _modem.sendTcp(cid(), _socketId, buf, size) ? size : 0;
}

int ST87M01Client::available() {
  if (_rxPos < _rxLen) return static_cast<int>(_rxLen - _rxPos);
  _modem.poll();
  if (_modem.socketRxPending(_socketId) == 0) return 0;
  return fillRx() ? static_cast<int>(_rxLen - _rxPos) : 0;
}

int ST87M01Client::read() {
  uint8_t b;
  int n = read(&b, 1);
  return (n == 1) ? b : -1;
}

int ST87M01Client::read(uint8_t* buf, size_t size) {
  if (available() <= 0) return -1;
  size_t n = min(size, _rxLen - _rxPos);
  memcpy(buf, &_rxBuf[_rxPos], n);
  _rxPos += n;
  return static_cast<int>(n);
}

int ST87M01Client::peek() {
  if (available() <= 0) return -1;
  return _rxBuf[_rxPos];
}

void ST87M01Client::flush() {
}

void ST87M01Client::stop() {
  if (_opened && _socketId != 0xFF) {
    _modem.closeSocket(cid(), _socketId);
  }
  _opened = false;
  _socketId = 0xFF;
  _rxLen = _rxPos = 0;
}

uint8_t ST87M01Client::connected() {
  if (!_opened || _socketId == 0xFF) return 0;
  _modem.poll();
  // Stay "connected" as long as either the modem still reports the link up,
  // or we have unread bytes buffered locally.
  if (_modem.socketConnected(_socketId)) return 1;
  return (_rxPos < _rxLen || _modem.socketRxPending(_socketId) > 0) ? 1 : 0;
}

ST87M01Client::operator bool() {
  return connected() != 0;
}

bool ST87M01Client::fillRx() {
  _rxLen = _rxPos = 0;
  int n = _modem.readSocket(cid(), _socketId, _rxBuf, sizeof(_rxBuf));
  if (n <= 0) return false;
  _rxLen = static_cast<size_t>(n);
  return true;
}
