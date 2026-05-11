// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

#pragma once

#include <Arduino.h>
#include "ST87M01Modem.h"

// MQTT client built on the ST87M01's native AT#MQTT* command family.
//
// IMPORTANT: The modem's native MQTT firmware imposes a **50-character
// limit** on topics AND message payloads. This is a hard constraint that
// cannot be worked around at the library level. For payloads exceeding
// 50 bytes, use PubSubClient (or any other MQTT library that takes an
// Arduino Client&) together with ST87M01Client — our TCP client already
// implements the standard Arduino Client interface.
//
// API intentionally mirrors PubSubClient (Nick O'Leary) where possible:
//   - setServer() / setCallback() / connect() / publish() / subscribe()
//   - loop() must be called regularly to pump incoming messages
//   - Callback signature: void cb(const char* topic, const uint8_t* payload,
//     unsigned int length)
//
// Key differences from PubSubClient:
//   - Constructor takes ST87M01Modem& (not Client&) — the modem handles
//     MQTT natively at the AT command level.
//   - setSecurityProfile() enables MQTTS (TLS) using a profile provisioned
//     via ST87M01TLS.
//   - 50-character topic and payload limit.
//   - Full QoS 0/1/2 support (PubSubClient only supports QoS 0 publish).
//
// Usage (plain MQTT):
//     ST87M01MQTT mqtt(modem);
//     mqtt.setServer("broker.example.com", 1883);
//     mqtt.setCallback(onMessage);
//     mqtt.connect("myDeviceId");
//     mqtt.subscribe("cmd/device1", 1);
//     mqtt.publish("data/device1", "23.5");
//
// Usage (MQTTS with TLS):
//     ST87M01TLS tls(modem);
//     tls.addCaCertPem(1, brokerCaPem);
//     tls.saveToNvm();
//     ST87M01MQTT mqtt(modem);
//     mqtt.setServer("broker.example.com", 8883);
//     mqtt.setSecurityProfile(1);
//     mqtt.connect("myDeviceId");
//
// Only one MQTT session at a time. The modem has 3 socket slots; MQTT
// claims one, leaving 2 for other uses (TCP, HTTP, UDP).
class ST87M01MQTT {
public:
  static constexpr size_t MAX_TOPIC_LEN     = 50;
  static constexpr size_t MAX_MESSAGE_LEN   = 50;
  static constexpr size_t MAX_CLIENT_ID_LEN = 25;
  static constexpr size_t MAX_USERNAME_LEN  = 25;
  static constexpr size_t MAX_PASSWORD_LEN  = 50;

  typedef void (*Callback)(const char* topic, const uint8_t* payload,
                           unsigned int length);

  explicit ST87M01MQTT(ST87M01Modem& modem, uint8_t cid = 0);

  void setCid(uint8_t cid) { _cid = cid; }

  // --- Configuration (call before connect) ---

  ST87M01MQTT& setServer(const char* host, uint16_t port = 1883);
  ST87M01MQTT& setSecurityProfile(uint8_t profile);
  ST87M01MQTT& setCallback(Callback cb);

  // --- Connection (PubSubClient-compatible signatures) ---

  bool connect(const char* clientId);
  bool connect(const char* clientId, const char* user, const char* pass);
  bool connect(const char* clientId, const char* user, const char* pass,
               const char* willTopic, uint8_t willQos, bool willRetain,
               const char* willMessage);

  void disconnect();
  bool connected();

  // --- Publish ---

  bool publish(const char* topic, const char* payload,
               uint8_t qos = 0, bool retained = false);

  // --- Subscribe / Unsubscribe ---

  bool subscribe(const char* topic, uint8_t qos = 0);
  bool unsubscribe(const char* topic);

  // --- Main loop (must call regularly) ---

  bool loop();

  // --- Engine configuration (AT#MQTTCFG) ---
  // Sets MQTT engine timeouts. These are also sent automatically by
  // connect() with the clientId, so calling configure() is optional.

  void configure(uint8_t connectionTimeout = 20,
                 uint8_t protocolTimeout = 20,
                 uint8_t publishRetry = 10,
                 uint8_t keepAlive = 10);

  // --- Diagnostics ---

  int lastTlsError() const { return _lastTlsError; }
  uint8_t securityProfile() const { return _securityProfile; }

private:
  static constexpr size_t MSG_QUEUE_SIZE = 4;

  struct MqttMessage {
    char topic[MAX_TOPIC_LEN + 1];
    char payload[MAX_MESSAGE_LEN + 1];
    uint8_t payloadLen;
    bool valid;
  };

  ST87M01Modem& _modem;
  uint8_t  _cid;
  uint8_t  _socketId;
  uint8_t  _securityProfile;
  bool     _socketOpen;
  bool     _mqttConnected;
  int      _lastTlsError;

  String   _host;
  uint16_t _port;

  uint8_t  _connTimeout;
  uint8_t  _protoTimeout;
  uint8_t  _pubRetry;
  uint8_t  _keepAlive;

  Callback _callback;

  MqttMessage _msgQueue[MSG_QUEUE_SIZE];
  volatile uint8_t _msgHead;
  volatile uint8_t _msgTail;

  uint8_t cid() const { return _cid ? _cid : _modem.defaultCid(); }

  bool connectInternal(const char* clientId, const char* user,
                       const char* pass, const char* willTopic,
                       uint8_t willQos, bool willRetain,
                       const char* willMessage);
  void resetState();

  static void onMqtRecvUrc(const String& line, void* ctx);
  static void onMqttDiscUrc(const String& line, void* ctx);
};
