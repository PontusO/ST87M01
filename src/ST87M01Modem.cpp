// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#include "ST87M01Modem.h"

ST87M01Modem::ST87M01Modem(Stream& serial, const ST87M01Pins& pins)
: _serial(serial), _at(serial), _pins(pins) {
}

bool ST87M01Modem::begin(unsigned long) {
  configurePins();
  _at.begin();
  _at.registerUrcHandler("+CEREG:", &ST87M01Modem::onCeregUrc, this);
  _at.registerUrcHandler("#IPRECV:", &ST87M01Modem::onIpRecvUrc, this);
  _at.registerUrcHandler("#SOCKETCLOSED:", &ST87M01Modem::onSocketClosedUrc, this);

  if (!isAlive()) {
    if (_pins.reset < 0 || !reset()) return false;
  }
  return setEcho(false) && setVerboseErrors(true);
}

void ST87M01Modem::poll() {
  _at.poll();
}

bool ST87M01Modem::reset(unsigned long holdMs, unsigned long bootMs) {
  if (_pins.reset < 0) return false;
  driveReset(true);
  delay(holdMs);
  driveReset(false);

  unsigned long start = millis();
  while ((millis() - start) < bootMs) {
    if (isAlive()) return true;
    delay(100);
  }
  return false;
}

bool ST87M01Modem::wake(unsigned long pulseMs, unsigned long settleMs) {
  if (_pins.wakeup < 0) return false;
  driveWakeup(true);
  delay(pulseMs);
  driveWakeup(false);
  delay(settleMs);
  return isAlive();
}

bool ST87M01Modem::ringAsserted() const {
  if (_pins.ring < 0) return false;
  int level = digitalRead(_pins.ring);
  return _pins.ringActiveLow ? (level == LOW) : (level == HIGH);
}

void ST87M01Modem::configurePins() {
  if (_pins.reset >= 0) {
    digitalWrite(_pins.reset, _pins.resetActiveLow ? HIGH : LOW);
    pinMode(_pins.reset, OUTPUT);
    digitalWrite(_pins.reset, _pins.resetActiveLow ? HIGH : LOW);
  }
  if (_pins.wakeup >= 0) {
    digitalWrite(_pins.wakeup, _pins.wakeupActiveLow ? HIGH : LOW);
    pinMode(_pins.wakeup, OUTPUT);
    digitalWrite(_pins.wakeup, _pins.wakeupActiveLow ? HIGH : LOW);
  }
  if (_pins.ring >= 0) {
    pinMode(_pins.ring, _pins.ringActiveLow ? INPUT_PULLUP : INPUT);
  }
}

void ST87M01Modem::driveReset(bool active) {
  if (_pins.reset < 0) return;
  bool low = (active == _pins.resetActiveLow);
  digitalWrite(_pins.reset, low ? LOW : HIGH);
}

void ST87M01Modem::driveWakeup(bool active) {
  if (_pins.wakeup < 0) return;
  bool low = (active == _pins.wakeupActiveLow);
  digitalWrite(_pins.wakeup, low ? LOW : HIGH);
}

bool ST87M01Modem::isAlive() {
  return runSimple("AT");
}

bool ST87M01Modem::setEcho(bool enable) {
  _at.sendf("ATE%d", enable ? 1 : 0);
  return _at.expectOK();
}

bool ST87M01Modem::setVerboseErrors(bool enable) {
  return _at.setVerboseErrors(enable);
}

bool ST87M01Modem::setFunctionality(uint8_t fun) {
  _at.sendf("AT+CFUN=%u", fun);
  return _at.expectOK(300000);
}

bool ST87M01Modem::getFunctionality(uint8_t& fun) {
  String line;
  _at.send("AT+CFUN?");
  if (!_at.waitLineStartsWith("+CFUN:", line, 2000)) return false;
  fun = static_cast<uint8_t>(line.substring(6).toInt());
  return _at.expectOK();
}

bool ST87M01Modem::getModel(String& model) {
  String line;
  _at.send("AT+CGMM");
  if (!_at.readLine(line, 1000)) return false;
  model = line;
  return _at.expectOK();
}

bool ST87M01Modem::getRevision(String& rev) {
  String line;
  _at.send("AT+CGMR");
  if (!_at.readLine(line, 1000)) return false;
  rev = line;
  return _at.expectOK();
}

bool ST87M01Modem::getImsi(String& imsi) {
  String line;
  _at.send("AT+CIMI");
  if (!_at.readLine(line, 1000)) return false;
  imsi = line;
  return _at.expectOK();
}

bool ST87M01Modem::getOperator(ST87M01OperatorInfo& info) {
  info = ST87M01OperatorInfo{};

  String line;
  _at.send("AT+COPS?");
  if (!_at.waitLineStartsWith("+COPS:", line, 2000)) return false;

  int colon = line.indexOf(':');
  String rest = line.substring(colon + 1);
  rest.trim();

  String parts[4];
  int count = 0;
  bool inQuote = false;
  String cur;
  for (size_t i = 0; i < rest.length(); ++i) {
    char c = rest[i];
    if (c == '"') { inQuote = !inQuote; continue; }
    if (c == ',' && !inQuote) {
      if (count < 4) parts[count++] = cur;
      cur = "";
      continue;
    }
    cur += c;
  }
  if (count < 4) parts[count++] = cur;

  if (count >= 1) info.mode = parts[0];
  if (count >= 2) info.format = parts[1];
  if (count >= 3) info.oper = parts[2];
  if (count >= 4) info.act = parts[3].toInt();

  return _at.expectOK();
}

bool ST87M01Modem::attach(bool enable) {
  _at.sendf("AT+CGATT=%d", enable ? 1 : 0);
  return _at.expectOK(120000);
}

bool ST87M01Modem::isAttached(bool& attached) {
  String line;
  _at.send("AT+CGATT?");
  if (!_at.waitLineStartsWith("+CGATT:", line, 2000)) return false;
  attached = line.substring(7).toInt() == 1;
  return _at.expectOK();
}

bool ST87M01Modem::definePdpContext(uint8_t cid, const char* apn, const char* pdpType) {
  _at.sendf("AT+CGDCONT=%u,\"%s\",\"%s\"", cid, pdpType, apn);
  return _at.expectOK();
}

bool ST87M01Modem::activatePdp(uint8_t cid, bool enable) {
  _at.sendf("AT+CGACT=%d,%u", enable ? 1 : 0, cid);
  return _at.expectOK(120000);
}

bool ST87M01Modem::getActiveContext(uint8_t& cid) {
  _at.send("AT+CGACT?");
  String line;
  bool found = false;

  while (_at.readLine(line, 2000)) {
    if (line == "OK") return found;
    if (line == "ERROR" || line.startsWith("+CME ERROR:")) return false;
    if (line.startsWith("+CGACT:")) {
      int colon = line.indexOf(':');
      String rest = line.substring(colon + 1);
      rest.trim();
      int comma = rest.indexOf(',');
      if (comma < 0) continue;
      int foundCid = rest.substring(0, comma).toInt();
      int state = rest.substring(comma + 1).toInt();
      if (state == 1 && !found) {
        cid = static_cast<uint8_t>(foundCid);
        found = true;
      }
    }
  }
  return found;
}

bool ST87M01Modem::getLocalAddress(uint8_t cid, String& ip) {
  String line;
  _at.sendf("AT+CGPADDR=%u", cid);
  if (!_at.waitLineStartsWith("+CGPADDR:", line, 2000)) return false;

  int comma = line.indexOf(',');
  if (comma >= 0) {
    ip = line.substring(comma + 1);
    ip.replace("\"", "");
    ip.trim();
  }
  return _at.expectOK();
}

bool ST87M01Modem::configureIpStack(uint8_t cid, const char* ip) {
  _at.sendf("AT#IPCFG=%u,0,\"%s\"", cid, ip);
  return _at.expectOK(10000);
}

bool ST87M01Modem::isIpStackActive(uint8_t cid) {
  _at.send("AT#IPCFG?");

  String line;
  bool active = false;

  while (true) {
    if (!_at.readLine(line, 3000)) return false;
    if (line == "OK") return active;
    if (line == "ERROR" || line.startsWith("+CME ERROR:")) return false;
    if (!line.startsWith("#IPCFG:")) continue;

    int colon = line.indexOf(':');
    String rest = line.substring(colon + 1);
    rest.trim();

    int c1 = rest.indexOf(',');
    if (c1 < 0) continue;
    int thisCid = rest.substring(0, c1).toInt();
    if (static_cast<uint8_t>(thisCid) != cid) continue;

    int c2 = rest.indexOf(',', c1 + 1);
    String statusStr = (c2 >= 0) ? rest.substring(c1 + 1, c2) : rest.substring(c1 + 1);
    if (statusStr.toInt() == 1) active = true;
  }
}

bool ST87M01Modem::startModem() {
  _at.send("AT#MODEMSTART");
  return _at.expectOK(30000);
}

bool ST87M01Modem::stopModem() {
  _at.send("AT#MODEMSTOP");
  return _at.expectOK(10000);
}

bool ST87M01Modem::setDns(const char* ipv4Dns) {
  // Writes to NVM; takes effect only after AT#RESET=1 + reboot.
  _at.sendf("AT#IPPARAMS=1,0,65535,60,0,\"%s\",\"\",1514,0", ipv4Dns ? ipv4Dns : "");
  return _at.expectOK(5000);
}

bool ST87M01Modem::softReset(unsigned long bootMs) {
  _at.send("AT#RESET=1");
  _at.expectOK(2000);

  // All sockets are gone after a reset.
  for (size_t i = 0; i < MAX_SOCKETS; ++i) {
    _sockets[i] = SocketSlot{};
  }

  delay(2000);
  unsigned long start = millis();
  while ((millis() - start) < bootMs) {
    if (isAlive()) return true;
    delay(500);
  }
  return false;
}

bool ST87M01Modem::configureDns(const char* ipv4Dns) {
  return setDns(ipv4Dns) && softReset();
}

bool ST87M01Modem::setCeregMode(uint8_t mode) {
  _at.sendf("AT+CEREG=%u", mode);
  return _at.expectOK();
}

bool ST87M01Modem::getCellInfo(ST87M01CellInfo& cell) {
  String line;
  _at.send("AT+CEREG?");
  if (!_at.waitLineStartsWith("+CEREG:", line, 2000)) return false;
  parseCereg(line, cell, /*isReadResponse=*/true);
  _lastCell = cell;
  return _at.expectOK();
}

bool ST87M01Modem::getSignal(ST87M01SignalInfo& sig) {
  String line;
  _at.send("AT+CESQ");
  if (!_at.waitLineStartsWith("+CESQ:", line, 2000)) return false;

  int vals[6] = {-1,-1,-1,-1,-1,-1};
  int idx = line.indexOf(':');
  String rest = line.substring(idx + 1);
  rest.trim();

  for (int i = 0; i < 6; ++i) {
    int comma = rest.indexOf(',');
    String token = (comma >= 0) ? rest.substring(0, comma) : rest;
    token.trim();
    vals[i] = token.toInt();
    if (comma < 0) break;
    rest = rest.substring(comma + 1);
  }

  sig.rxlev = vals[0];
  sig.ber   = vals[1];
  sig.rsrq  = vals[4];
  sig.rsrp  = vals[5];
  return _at.expectOK();
}

bool ST87M01Modem::resolveHost(uint8_t cid, const char* host, String& ip) {
  String line;
  _at.sendf("AT#DNS=%u,0,\"%s\"", cid, host);
  if (!_at.waitLineStartsWith("#DNS:", line, 30000)) return false;

  int colon = line.indexOf(':');
  String rest = line.substring(colon + 1);
  rest.trim();
  int comma = rest.indexOf(',');
  ip = (comma >= 0) ? rest.substring(0, comma) : rest;
  ip.replace("\"", "");
  ip.trim();
  return _at.expectOK(30000);
}

bool ST87M01Modem::ping(uint8_t cid, const char* hostOrIp) {
  String ip;
  if (isIpLiteral(hostOrIp)) {
    ip = hostOrIp;
  } else if (!resolveHost(cid, hostOrIp, ip)) {
    return false;
  }
  _at.sendf("AT#IPPING=%u,\"%s\"", cid, ip.c_str());
  return _at.expectOK(120000);
}

bool ST87M01Modem::createSocket(uint8_t cid, bool tcp, uint8_t& socketId, uint16_t localPort) {
  String line;
  // ip_version=0 (IPv4), type="TCP"/"UDP", local_port (empty = random),
  // send_timeout=10s, receive_timeout=10s, frame_received_urc=2 (fires #IPRECV with length).
  if (localPort) {
    _at.sendf("AT#SOCKETCREATE=%u,0,\"%s\",%u,10,10,2",
              cid, tcp ? "TCP" : "UDP", localPort);
  } else {
    _at.sendf("AT#SOCKETCREATE=%u,0,\"%s\",,10,10,2",
              cid, tcp ? "TCP" : "UDP");
  }
  if (!_at.waitLineStartsWith("#SOCKETCREATE:", line, 5000)) return false;
  int colon = line.indexOf(':');
  socketId = static_cast<uint8_t>(line.substring(colon + 1).toInt());
  if (!_at.expectOK()) return false;

  if (socketId < MAX_SOCKETS) {
    _sockets[socketId] = SocketSlot{};
    _sockets[socketId].inUse = true;
  }
  return true;
}

bool ST87M01Modem::connectTcp(uint8_t cid, uint8_t socketId, const char* ip, uint16_t port) {
  _at.sendf("AT#TCPCONNECT=%u,%u,\"%s\",%u", cid, socketId, ip, port);
  if (!_at.expectOK(120000)) return false;
  if (socketId < MAX_SOCKETS) {
    _sockets[socketId].connected = true;
  }
  return true;
}

bool ST87M01Modem::sendTcp(uint8_t cid, uint8_t socketId, const uint8_t* data, size_t len) {
  if (!data || !len) return false;

  // Stream prefix + hex + terminator directly so arbitrarily large payloads
  // bypass the 192-byte sendf buffer. No heap allocation for the payload.
  char prefix[48];
  snprintf(prefix, sizeof(prefix), "AT#IPSENDTCP=%u,%u,2,", cid, socketId);
  _at.beginCommand(prefix);
  _at.writeHex(data, len);
  _at.endCommand();
  return _at.expectOK(10000);
}

bool ST87M01Modem::sendUdp(uint8_t cid, uint8_t socketId, const char* ip, uint16_t port,
                           const uint8_t* data, size_t len) {
  if (!data || !len) return false;

  // <rai> is intentionally empty (no RAI hint); <data_type>=2 (hex text).
  // Same streaming pattern as sendTcp to bypass sendf's 192-byte ceiling.
  char prefix[80];
  snprintf(prefix, sizeof(prefix), "AT#IPSENDUDP=%u,%u,\"%s\",%u,,2,",
           cid, socketId, ip, port);
  _at.beginCommand(prefix);
  _at.writeHex(data, len);
  _at.endCommand();
  return _at.expectOK(10000);
}

int ST87M01Modem::readSocket(uint8_t cid, uint8_t socketId, uint8_t* data, size_t maxLen) {
  if (!data || !maxLen) return -1;

  String line;
  _at.sendf("AT#IPREAD=%u,%u", cid, socketId);
  if (!_at.waitLineStartsWith("#IPREAD:", line, 30000)) return -1;

  // Parse "#IPREAD: <cid>,<sock>,<len>"
  int colon = line.indexOf(':');
  String rest = line.substring(colon + 1);
  rest.trim();

  size_t declared = 0;
  int c1 = rest.indexOf(',');
  int c2 = (c1 >= 0) ? rest.indexOf(',', c1 + 1) : -1;
  if (c2 > 0) {
    declared = static_cast<size_t>(rest.substring(c2 + 1).toInt());
  }
  if (declared == 0) {
    // Still need to consume OK.
    _at.expectOK(5000);
    if (socketId < MAX_SOCKETS) _sockets[socketId].rxPending = 0;
    return 0;
  }

  size_t toCopy = (declared < maxLen) ? declared : maxLen;
  size_t got = _at.readBytes(data, toCopy, 30000);

  // AT#IPREAD is one-shot: the modem has already queued `declared` bytes on
  // the UART before we see the #IPREAD: header, so any excess beyond `maxLen`
  // MUST be drained — otherwise stray bytes corrupt the next AT exchange.
  // But dropping data silently is dangerous (the caller thinks they got a
  // complete stream), so record the drop count on the socket slot and shout
  // about it on the debug stream. The caller can query via socketRxDropped().
  size_t overflow = (declared > got) ? (declared - got) : 0;
  size_t dropped = 0;
  while (overflow--) {
    uint8_t scratch;
    if (_at.readBytes(&scratch, 1, 30000) != 1) break;
    ++dropped;
  }
  if (dropped && socketId < MAX_SOCKETS) {
    _sockets[socketId].rxDropped += dropped;
  }
  if (dropped) {
    if (Stream* dbg = _at.debugStream()) {
      dbg->print(F("!!! readSocket truncated: "));
      dbg->print(dropped);
      dbg->print(F(" bytes dropped (declared="));
      dbg->print(declared);
      dbg->print(F(", buffer="));
      dbg->print(maxLen);
      dbg->println(F(")"));
    }
  }

  if (!_at.expectOK(5000)) {
    if (socketId < MAX_SOCKETS) _sockets[socketId].rxPending = 0;
    return static_cast<int>(got);
  }

  if (socketId < MAX_SOCKETS) {
    _sockets[socketId].rxPending = 0;
  }
  return static_cast<int>(got);
}

bool ST87M01Modem::closeSocket(uint8_t cid, uint8_t socketId) {
  _at.sendf("AT#SOCKETCLOSE=%u,%u", cid, socketId);
  bool ok = _at.expectOK(30000);
  if (socketId < MAX_SOCKETS) {
    _sockets[socketId] = SocketSlot{};
  }
  return ok;
}

bool ST87M01Modem::socketConnected(uint8_t socketId) const {
  if (socketId >= MAX_SOCKETS) return false;
  return _sockets[socketId].connected;
}

size_t ST87M01Modem::socketRxPending(uint8_t socketId) const {
  if (socketId >= MAX_SOCKETS) return 0;
  return _sockets[socketId].rxPending;
}

size_t ST87M01Modem::socketRxDropped(uint8_t socketId) const {
  if (socketId >= MAX_SOCKETS) return 0;
  return _sockets[socketId].rxDropped;
}

void ST87M01Modem::onCeregUrc(const String& line, void* ctx) {
  auto* self = static_cast<ST87M01Modem*>(ctx);
  self->parseCereg(line, self->_lastCell, /*isReadResponse=*/false);
}

void ST87M01Modem::onIpRecvUrc(const String& line, void* ctx) {
  auto* self = static_cast<ST87M01Modem*>(ctx);
  int colon = line.indexOf(':');
  if (colon < 0) return;
  String rest = line.substring(colon + 1);
  rest.trim();

  int c1 = rest.indexOf(',');
  if (c1 < 0) return;
  int c2 = rest.indexOf(',', c1 + 1);

  int sockId = rest.substring(c1 + 1, (c2 >= 0) ? c2 : rest.length()).toInt();
  size_t len = (c2 >= 0) ? rest.substring(c2 + 1).toInt() : 0;

  if (sockId >= 0 && sockId < static_cast<int>(MAX_SOCKETS)) {
    self->_sockets[sockId].rxPending = len ? len : self->_sockets[sockId].rxPending + 1;
  }
}

void ST87M01Modem::onSocketClosedUrc(const String& line, void* ctx) {
  auto* self = static_cast<ST87M01Modem*>(ctx);
  int colon = line.indexOf(':');
  if (colon < 0) return;
  String rest = line.substring(colon + 1);
  rest.trim();

  int c1 = rest.indexOf(',');
  if (c1 < 0) return;
  int c2 = rest.indexOf(',', c1 + 1);
  int sockId = rest.substring(c1 + 1, (c2 >= 0) ? c2 : rest.length()).toInt();

  if (sockId >= 0 && sockId < static_cast<int>(MAX_SOCKETS)) {
    self->_sockets[sockId].connected = false;
  }
}

void ST87M01Modem::parseCereg(const String& line, ST87M01CellInfo& cell, bool isReadResponse) {
  cell = ST87M01CellInfo{};
  cell.valid = true;

  int colon = line.indexOf(':');
  String rest = line.substring(colon + 1);
  rest.trim();

  String parts[8];
  int count = 0;
  bool inQuote = false;
  String cur;

  for (size_t i = 0; i < rest.length(); ++i) {
    char c = rest[i];
    if (c == '"') {
      inQuote = !inQuote;
      continue;
    }
    if (c == ',' && !inQuote) {
      if (count < 8) parts[count++] = cur;
      cur = "";
      continue;
    }
    cur += c;
  }
  if (count < 8) parts[count++] = cur;

  // AT+CEREG? read response prepends <n> before <stat>; URC form does not.
  int base = isReadResponse ? 1 : 0;
  if (count > base)     cell.reg = static_cast<ST87M01RegStatus>(parts[base].toInt());
  if (count > base + 1) cell.tac = parts[base + 1];
  if (count > base + 2) cell.ci  = parts[base + 2];
  if (count > base + 3) cell.act = parts[base + 3].toInt();
}

bool ST87M01Modem::runSimple(const char* cmd, unsigned long timeoutMs) {
  _at.send(cmd);
  return _at.expectOK(timeoutMs);
}

bool ST87M01Modem::isIpLiteral(const char* s) const {
  if (!s || !*s) return false;
  for (const char* p = s; *p; ++p) {
    if (!(isdigit(static_cast<unsigned char>(*p)) || *p == '.' || *p == ':'
          || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
      return false;
    }
  }
  return true;
}
