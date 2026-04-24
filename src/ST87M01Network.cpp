// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#include "ST87M01Network.h"

ST87M01Network::ST87M01Network(ST87M01Modem& modem)
: _modem(modem), _cid(1) {
}

bool ST87M01Network::begin(const char* apn, uint8_t cid) {
  _cid = cid;

  if (!_modem.setFunctionality(1)) return false;

  // NB-IoT SIMs that carry a pre-provisioned APN (1NCE, most MVNOs) auto-activate
  // the initial default EPS bearer on CFUN=1. Trying to define a second context
  // on top of that fails with CME 2105; trying to AT#IPCFG on the auto-activated
  // context fails with CME 2100 (the IP stack is already up internally and the
  // modem won't let user IPCFG touch that cid). So: adopt an active context if
  // one exists, and skip IPCFG for it. Only define/activate + IPCFG if nothing
  // is up yet (SIMs without auto-provisioning, or a custom APN flow).
  uint8_t activeCid = 0;
  bool adopted = _modem.getActiveContext(activeCid);
  if (adopted) {
    _cid = activeCid;
  } else {
    if (!_modem.attach(true)) return false;
    if (!_modem.definePdpContext(_cid, apn)) return false;
    if (!_modem.activatePdp(_cid, true)) return false;
  }

  // Even when the PDP context shows active, the modem's user-visible IP stack
  // may be down — commonly after a sleep cycle or stale state from a previous
  // session. AT#DNS / AT#SOCKETCREATE would return CME 2106 "network is down"
  // in that case. Cycle the modem core to bring the stack back up.
  if (!_modem.isIpStackActive(_cid)) {
    _modem.stopModem();
    if (!_modem.startModem()) return false;

    unsigned long start = millis();
    bool up = false;
    while ((millis() - start) < 20000) {
      _modem.poll();
      if (_modem.isIpStackActive(_cid)) { up = true; break; }
      delay(500);
    }
    if (!up) return false;
  }

  String ip;
  if (!_modem.getLocalAddress(_cid, ip) || !ip.length()) return false;

  // Publish the resolved cid so Client/UDP/NBIoT pick it up by default.
  _modem.setDefaultCid(_cid);

  if (adopted) return true;
  return _modem.configureIpStack(_cid, ip.c_str());
}

bool ST87M01Network::connected() {
  bool attached = false;
  return _modem.isAttached(attached) && attached;
}

String ST87M01Network::localIP() {
  String ip;
  if (_modem.getLocalAddress(_cid, ip)) {
    return ip;
  }
  return "";
}
