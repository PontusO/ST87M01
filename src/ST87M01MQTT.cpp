// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#include "ST87M01MQTT.h"

ST87M01MQTT::ST87M01MQTT(ST87M01Modem& modem, uint8_t cid)
: _modem(modem), _cid(cid), _socketId(0xFF),
  _securityProfile(0), _socketOpen(false), _mqttConnected(false),
  _lastTlsError(0), _port(1883),
  _connTimeout(20), _protoTimeout(20), _pubRetry(10), _keepAlive(60),
  _callback(nullptr), _msgHead(0), _msgTail(0) {
  memset(_msgQueue, 0, sizeof(_msgQueue));
  _modem.at().registerUrcHandler("#MQTRECV:", &ST87M01MQTT::onMqtRecvUrc, this);
  _modem.at().registerUrcHandler("#MQTTDISC", &ST87M01MQTT::onMqttDiscUrc, this);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
ST87M01MQTT& ST87M01MQTT::setServer(const char* host, uint16_t port) {
  _host = host ? host : "";
  _port = port;
  return *this;
}

ST87M01MQTT& ST87M01MQTT::setSecurityProfile(uint8_t profile) {
  _securityProfile = profile;
  return *this;
}

ST87M01MQTT& ST87M01MQTT::setCallback(Callback cb) {
  _callback = cb;
  return *this;
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------
bool ST87M01MQTT::connect(const char* clientId) {
  return connectInternal(clientId, nullptr, nullptr,
                         nullptr, 0, false, nullptr);
}

bool ST87M01MQTT::connect(const char* clientId,
                          const char* user, const char* pass) {
  return connectInternal(clientId, user, pass,
                         nullptr, 0, false, nullptr);
}

bool ST87M01MQTT::connect(const char* clientId,
                          const char* user, const char* pass,
                          const char* willTopic, uint8_t willQos,
                          bool willRetain, const char* willMessage) {
  return connectInternal(clientId, user, pass,
                         willTopic, willQos, willRetain, willMessage);
}

bool ST87M01MQTT::connectInternal(const char* clientId,
                                  const char* user, const char* pass,
                                  const char* willTopic, uint8_t willQos,
                                  bool willRetain, const char* willMessage) {
  if (_host.length() == 0) return false;
  if (!clientId || !*clientId) return false;

  // Force-clean any stale MQTT/socket state before connecting. Two cases:
  //   1. Local reconnect — our flags say something is open, tear it down.
  //   2. Fresh boot — the host MCU has reset but the modem firmware may
  //      still hold an MQTT session and its socket from the previous run.
  //      Until that session times out (~20 s), AT#SOCKETCREATE returns
  //      CME 2159 (socket in use). Sending AT#MQTTDISC up-front cleans
  //      both ends in well under a second.
  _modem.at().sendf("AT#MQTTDISC");
  _modem.at().expectOK(5000);
  _modem.at().sendf("AT#SOCKETCLOSE=%u,0", cid());
  _modem.at().expectOK(2000);
  _mqttConnected = false;
  _socketOpen = false;
  _socketId = 0xFF;
  resetState();

  // AT#MQTTCFG must precede AT#MQTTCONNECT (sets client name + timeouts).
  _modem.at().sendf("AT#MQTTCFG=%s,%u,%u,%u,%u",
                    clientId,
                    static_cast<unsigned>(_connTimeout),
                    static_cast<unsigned>(_protoTimeout),
                    static_cast<unsigned>(_pubRetry),
                    static_cast<unsigned>(_keepAlive));
  if (!_modem.at().expectOK(5000)) return false;

  if (!_modem.createSocket(cid(), true, _socketId, _port, _securityProfile, 0)) {
    return false;
  }
  _socketOpen = true;

  // MQTTCONNECT handles TCP + MQTT handshake internally (no TCPCONNECT).
  _modem.at().sendf("AT#MQTTCONNECT=%u,%u,%s,%u",
                    cid(), _socketId, _host.c_str(), _port);

  if (!_modem.at().expectOK(25000)) {
    _lastTlsError = _modem.at().lastCmeError();
    disconnect();
    return false;
  }

  _mqttConnected = true;
  return true;
}

void ST87M01MQTT::disconnect() {
  if (_mqttConnected) {
    _modem.at().sendf("AT#MQTTDISC");
    _modem.at().expectOK(20000);
    _mqttConnected = false;
  }
  // MQTTDISC closes the socket internally; only attempt explicit close
  // if disconnect wasn't clean (ignore errors — the socket may already
  // be gone).
  if (_socketOpen && _socketId != 0xFF) {
    _modem.at().sendf("AT#SOCKETCLOSE=%u,%u", cid(), _socketId);
    _modem.at().expectOK(5000);
  }
  _socketOpen = false;
  _socketId = 0xFF;
  _lastTlsError = 0;
}

bool ST87M01MQTT::connected() {
  _modem.poll();
  return _mqttConnected;
}

// ---------------------------------------------------------------------------
// Publish
// ---------------------------------------------------------------------------
bool ST87M01MQTT::publish(const char* topic, const char* payload,
                          uint8_t qos, bool retained) {
  if (!_mqttConnected) return false;
  if (!topic || strlen(topic) > MAX_TOPIC_LEN) return false;
  if (payload && strlen(payload) > MAX_MESSAGE_LEN) return false;

  // Topic and payload are double-quoted so they may contain spaces, commas,
  // and other characters the AT parser treats as field separators. Caller
  // is responsible for escaping any embedded double quotes in the payload
  // — the library does not (yet) do that for you.
  _modem.at().sendf("AT#MQTTPUB=\"%s\",\"%s\",%u,%u,%u",
                    topic, payload ? payload : "",
                    static_cast<unsigned>(_pubRetry),
                    static_cast<unsigned>(qos),
                    retained ? 1u : 0u);
  return _modem.at().expectOK(5000);
}

// ---------------------------------------------------------------------------
// Subscribe / Unsubscribe
// ---------------------------------------------------------------------------
bool ST87M01MQTT::subscribe(const char* topic, uint8_t qos) {
  if (!_mqttConnected) return false;
  if (!topic || strlen(topic) > MAX_TOPIC_LEN) return false;

  _modem.at().sendf("AT#MQTTSUB=\"%s\",%u", topic, static_cast<unsigned>(qos));
  return _modem.at().expectOK(10000);
}

bool ST87M01MQTT::unsubscribe(const char* topic) {
  if (!_mqttConnected) return false;
  if (!topic) return false;

  _modem.at().sendf("AT#MQTTUNSUB=\"%s\"", topic);
  return _modem.at().expectOK(10000);
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
bool ST87M01MQTT::loop() {
  if (!connected()) return false;

  while (_msgTail != _msgHead) {
    MqttMessage& msg = _msgQueue[_msgTail];
    if (msg.valid && _callback) {
      _callback(msg.topic,
                reinterpret_cast<const uint8_t*>(msg.payload),
                msg.payloadLen);
    }
    msg.valid = false;
    _msgTail = (_msgTail + 1) % MSG_QUEUE_SIZE;
  }

  return true;
}

// ---------------------------------------------------------------------------
// Engine configuration (AT#MQTTCFG)
// ---------------------------------------------------------------------------
void ST87M01MQTT::configure(uint8_t connectionTimeout,
                            uint8_t protocolTimeout,
                            uint8_t publishRetry,
                            uint8_t keepAlive) {
  _connTimeout  = connectionTimeout;
  _protoTimeout = protocolTimeout;
  _pubRetry     = publishRetry;
  _keepAlive    = keepAlive;
}

// ---------------------------------------------------------------------------
// URC handler
// ---------------------------------------------------------------------------
void ST87M01MQTT::onMqtRecvUrc(const String& line, void* ctx) {
  auto* self = static_cast<ST87M01MQTT*>(ctx);

  // URC format: #MQTRECV: <topic>,<message>
  // Both fields may be double-quoted (consistent with the quoted form we
  // send in publish/subscribe). When quoted, the comma between fields is
  // the one between a closing and an opening quote.
  int colon = line.indexOf(':');
  if (colon < 0) return;

  int pos = colon + 1;
  while (pos < (int)line.length() && line.charAt(pos) == ' ') ++pos;

  int comma;
  if (pos < (int)line.length() && line.charAt(pos) == '"') {
    // Quoted topic — find the closing quote, then the following comma.
    int closeQuote = line.indexOf('"', pos + 1);
    if (closeQuote < 0) return;
    comma = line.indexOf(',', closeQuote + 1);
  } else {
    comma = line.indexOf(',', pos);
  }
  if (comma < 0) return;

  String topic = line.substring(pos, comma);
  String message = line.substring(comma + 1);

  // Strip surrounding double quotes if present.
  auto unquote = [](String& s) {
    if (s.length() >= 2 && s.charAt(0) == '"' && s.charAt(s.length() - 1) == '"') {
      s = s.substring(1, s.length() - 1);
    }
  };
  unquote(topic);
  unquote(message);

  uint8_t next = (self->_msgHead + 1) % MSG_QUEUE_SIZE;
  if (next == self->_msgTail) {
    // Ring full — drop oldest to make room
    self->_msgTail = (self->_msgTail + 1) % MSG_QUEUE_SIZE;
  }

  MqttMessage& msg = self->_msgQueue[self->_msgHead];
  strncpy(msg.topic, topic.c_str(), MAX_TOPIC_LEN);
  msg.topic[MAX_TOPIC_LEN] = '\0';
  strncpy(msg.payload, message.c_str(), MAX_MESSAGE_LEN);
  msg.payload[MAX_MESSAGE_LEN] = '\0';
  msg.payloadLen = min(static_cast<size_t>(message.length()), MAX_MESSAGE_LEN);
  msg.valid = true;

  self->_msgHead = next;
}

void ST87M01MQTT::onMqttDiscUrc(const String& /*line*/, void* ctx) {
  auto* self = static_cast<ST87M01MQTT*>(ctx);
  self->_mqttConnected = false;
  self->_socketOpen = false;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
void ST87M01MQTT::resetState() {
  _msgHead = 0;
  _msgTail = 0;
  memset(_msgQueue, 0, sizeof(_msgQueue));
  _lastTlsError = 0;
}
