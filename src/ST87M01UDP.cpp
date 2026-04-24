#include "ST87M01UDP.h"

ST87M01UDP::ST87M01UDP(ST87M01Modem& modem, uint8_t cid)
: _modem(modem), _cid(cid), _socketId(0xFF), _open(false), _port(0),
  _txLen(0), _rxLen(0), _rxPos(0) {
}

uint8_t ST87M01UDP::begin(uint16_t port) {
  _port = port;
  if (!_modem.createSocket(cid(), false, _socketId, port)) return 0;
  _open = true;
  return 1;
}

int ST87M01UDP::beginPacket(IPAddress ip, uint16_t port) {
  _host = ip.toString();
  _port = port;
  _txLen = 0;
  return 1;
}

int ST87M01UDP::beginPacket(const char* host, uint16_t port) {
  String ip;
  if (!_modem.resolveHost(cid(), host, ip) || !ip.length()) return 0;
  _host = ip;
  _port = port;
  _txLen = 0;
  return 1;
}

size_t ST87M01UDP::write(uint8_t b) {
  return write(&b, 1);
}

size_t ST87M01UDP::write(const uint8_t* buffer, size_t size) {
  size_t room = sizeof(_txBuf) - _txLen;
  size_t n = (size < room) ? size : room;
  memcpy(&_txBuf[_txLen], buffer, n);
  _txLen += n;
  return n;
}

int ST87M01UDP::endPacket() {
  if (!_open || !_txLen || !_host.length()) return 0;
  bool ok = _modem.sendUdp(cid(), _socketId, _host.c_str(), _port, _txBuf, _txLen);
  _txLen = 0;
  return ok ? 1 : 0;
}

int ST87M01UDP::parsePacket() {
  if (_rxPos < _rxLen) return static_cast<int>(_rxLen - _rxPos);

  _rxLen = _rxPos = 0;
  _modem.poll();
  if (!_open || _modem.socketRxPending(_socketId) == 0) return 0;

  int n = _modem.readSocket(cid(), _socketId, _rxBuf, sizeof(_rxBuf));
  if (n <= 0) return 0;
  _rxLen = static_cast<size_t>(n);
  return static_cast<int>(_rxLen);
}

int ST87M01UDP::available() {
  return (_rxPos < _rxLen) ? static_cast<int>(_rxLen - _rxPos) : 0;
}

int ST87M01UDP::read() {
  if (_rxPos >= _rxLen) return -1;
  return _rxBuf[_rxPos++];
}

int ST87M01UDP::read(unsigned char* buffer, size_t len) {
  if (_rxPos >= _rxLen) return -1;
  size_t n = min(len, _rxLen - _rxPos);
  memcpy(buffer, &_rxBuf[_rxPos], n);
  _rxPos += n;
  return static_cast<int>(n);
}

int ST87M01UDP::read(char* buffer, size_t len) {
  return read(reinterpret_cast<unsigned char*>(buffer), len);
}

int ST87M01UDP::peek() {
  if (_rxPos >= _rxLen) return -1;
  return _rxBuf[_rxPos];
}

void ST87M01UDP::flush() {
}

void ST87M01UDP::stop() {
  if (_open && _socketId != 0xFF) {
    _modem.closeSocket(cid(), _socketId);
  }
  _open = false;
  _socketId = 0xFF;
  _rxLen = _rxPos = 0;
  _txLen = 0;
}
