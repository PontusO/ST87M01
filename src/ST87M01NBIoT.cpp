#include "ST87M01NBIoT.h"

ST87M01NBIoT::ST87M01NBIoT(ST87M01Modem& modem, uint8_t cid)
: _modem(modem), _cid(cid) {
}

bool ST87M01NBIoT::setSleepMode(bool enable) {
  _modem.at().sendf("AT#SLEEPMODE=%d", enable ? 1 : 0);
  return _modem.at().expectOK();
}

bool ST87M01NBIoT::setPSM(bool enable) {
  _modem.at().sendf("AT+CPSMS=%d", enable ? 1 : 0);
  return _modem.at().expectOK();
}

bool ST87M01NBIoT::setCeregMode(uint8_t mode) {
  return _modem.setCeregMode(mode);
}

bool ST87M01NBIoT::getCellInfo(ST87M01CellInfo& cell) {
  return _modem.getCellInfo(cell);
}

bool ST87M01NBIoT::getSignal(ST87M01SignalInfo& sig) {
  return _modem.getSignal(sig);
}

bool ST87M01NBIoT::getOperator(ST87M01OperatorInfo& op) {
  return _modem.getOperator(op);
}

bool ST87M01NBIoT::getImsi(String& imsi) {
  return _modem.getImsi(imsi);
}

bool ST87M01NBIoT::ping(const char* host) {
  return _modem.ping(cid(), host);
}

bool ST87M01NBIoT::getCandidateFreqs(String& out) {
  String line;
  _modem.at().send("AT#CANDFREQ?");
  out = "";

  while (_modem.at().readLine(line, 1000)) {
    if (line == "OK") {
      return true;
    }
    if (line == "ERROR" || line.startsWith("+CME ERROR:")) {
      return false;
    }
    if (line.startsWith("#CANDFREQ")) {
      out += line;
      out += "\n";
    }
  }
  return false;
}