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

  // Cumulative count of RX bytes that were silently dropped because a single
  // AT#IPREAD delivered more than the internal RX buffer could hold. Non-zero
  // after a session means the received stream is truncated — treat whatever
  // was read as incomplete. Zeroed on connect/stop.
  size_t droppedBytes() const { return _modem.socketRxDropped(_socketId); }

private:
  static constexpr size_t RX_BUF_SIZE = 1500;

  ST87M01Modem& _modem;
  uint8_t _cid;
  uint8_t _socketId;
  bool _opened;
  uint8_t _rxBuf[RX_BUF_SIZE];
  size_t _rxLen;
  size_t _rxPos;

  bool fillRx();
};
