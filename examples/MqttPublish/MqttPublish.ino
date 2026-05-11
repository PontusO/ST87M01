// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

/*
  ST87M01 MqttPublish — MQTT publish + subscribe via native modem commands

  Connects to an MQTT broker, subscribes to a command topic, and publishes
  a counter message every 30 seconds. Incoming messages are printed to
  Serial. Reconnects automatically if the connection drops.

  Uses the modem's native AT#MQTT* command family. The API mirrors
  PubSubClient (Nick O'Leary) so sketches can be ported with minimal
  changes. Key differences:
    - Constructor takes ST87M01Modem& (not Client&).
    - The modem handles MQTT protocol, QoS, and keep-alive internally.
    - Full QoS 0/1/2 support (PubSubClient only does QoS 0 publish).

  IMPORTANT — 50-character limit:
    The modem firmware limits topics AND payloads to 50 characters each.
    This is fine for sensor telemetry ("23.5", '{"t":25.3,"h":60}') but
    too small for rich JSON. For larger payloads, use PubSubClient with
    ST87M01Client (our Arduino Client implementation) instead — it runs
    MQTT over raw TCP with no payload size limit.

  For MQTTS (TLS-encrypted MQTT on port 8883): provision a CA certificate
  via ST87M01TLS, then call mqtt.setSecurityProfile(profileId) before
  connect(). See the HttpsGet / MtlsGet examples for TLS provisioning.

  Hardware: any board with a ST87M01Boards.h preset. Run ConfigureDns
  once per device so the modem can resolve broker hostnames.
*/

#include <ST87M01Modem.h>
#include <ST87M01Network.h>
#include <ST87M01MQTT.h>
#include <ST87M01Boards.h>

// ---- User configuration ------------------------------------------------
static const char* APN       = "iot.1nce.net";
static const char* BROKER    = "broker.hivemq.com";
static constexpr uint16_t PORT = 1883;
static const char* CLIENT_ID = "st87m01-demo";

static const char* PUB_TOPIC = "st87m01/data";
static const char* SUB_TOPIC = "st87m01/cmd";

static constexpr unsigned long PUBLISH_INTERVAL_MS     = 30000;
static constexpr unsigned long REGISTRATION_TIMEOUT_MS = 120000;
static constexpr bool AT_DEBUG = false;

// ---- Global objects ----------------------------------------------------
ST87M01Modem   modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);
ST87M01Network network(modem);
ST87M01MQTT    mqtt(modem);

static unsigned long lastPublish = 0;
static uint32_t msgCount = 0;

// ---- MQTT message callback (PubSubClient-compatible signature) ---------
void onMessage(const char* topic, const uint8_t* payload, unsigned int len) {
  Serial.print(F("Received ["));
  Serial.print(topic);
  Serial.print(F("]: "));
  for (unsigned int i = 0; i < len; i++) Serial.write(payload[i]);
  Serial.println();
}

// ---- Registration helpers (same as other examples) ---------------------
static bool isRegistered(ST87M01RegStatus r) {
  return r == ST87M01RegStatus::RegisteredHome ||
         r == ST87M01RegStatus::RegisteredRoaming;
}

static bool waitForRegistration(unsigned long timeoutMs) {
  unsigned long start = millis();
  unsigned long lastLog = 0;
  while ((millis() - start) < timeoutMs) {
    modem.poll();
    ST87M01CellInfo cell;
    bool ok = modem.getCellInfo(cell);
    if (ok && isRegistered(cell.reg)) return true;
    if (millis() - lastLog >= 5000) {
      lastLog = millis();
      Serial.print(F("  ["));
      Serial.print((millis() - start) / 1000);
      Serial.println(F("s] searching..."));
    }
    delay(500);
  }
  return false;
}

// ---- MQTT connect helper -----------------------------------------------
static bool mqttConnect() {
  Serial.print(F("Connecting to MQTT broker "));
  Serial.print(BROKER);
  Serial.print(':');
  Serial.println(PORT);

  if (!mqtt.connect(CLIENT_ID)) {
    Serial.print(F("MQTT connect failed — CME "));
    Serial.println(modem.at().lastCmeError());
    return false;
  }
  Serial.println(F("MQTT connected."));

  Serial.print(F("Subscribing to "));
  Serial.println(SUB_TOPIC);
  if (!mqtt.subscribe(SUB_TOPIC, 1)) {
    Serial.println(F("Subscribe failed."));
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  ST87M01_SERIAL.begin(115200);

  Serial.println();
  Serial.println(F("=== ST87M01 MqttPublish ==="));

  if (AT_DEBUG) modem.at().setDebugStream(&Serial);

  if (!modem.begin()) {
    Serial.println(F("modem.begin() failed."));
    return;
  }
  modem.setCeregMode(2);

  Serial.println(F("Waiting for registration..."));
  if (!waitForRegistration(REGISTRATION_TIMEOUT_MS)) {
    Serial.println(F("Not registered."));
    return;
  }

  if (!network.begin(APN)) {
    Serial.print(F("network.begin() failed — CME "));
    Serial.println(modem.at().lastCmeError());
    return;
  }

  Serial.print(F("Network up. IP="));
  Serial.println(network.localIP());

  mqtt.setServer(BROKER, PORT);
  mqtt.setCallback(onMessage);

  mqttConnect();
}

void loop() {
  if (!mqtt.loop()) {
    Serial.println(F("MQTT disconnected, reconnecting..."));
    delay(5000);
    mqttConnect();
    return;
  }

  if ((millis() - lastPublish) >= PUBLISH_INTERVAL_MS) {
    lastPublish = millis();
    msgCount++;

    char payload[51];
    snprintf(payload, sizeof(payload), "hello #%lu", (unsigned long)msgCount);

    Serial.print(F("Publish: "));
    Serial.println(payload);

    if (!mqtt.publish(PUB_TOPIC, payload)) {
      Serial.print(F("Publish failed — CME "));
      Serial.println(modem.at().lastCmeError());
    }
  }
}
