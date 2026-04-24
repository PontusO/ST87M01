// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

/*
  ST87M01 HttpGet

  Attaches to the cellular network and fetches http://ilabs.se/files/ilabs-logo.txt
  using the modem's native AT#HTTP* command family (wrapped by the
  ST87M01HTTP library class). Prints the status code, the server's
  response headers, and the full body.

  Why the native HTTP client instead of raw TCP: the modem's AT#IPREAD
  is a one-shot transaction — when it's called, the modem dumps the
  entire pending RX buffer over the UART in one go. If the response is
  larger than the library's local buffer, the tail is silently drained
  and lost (see ST87M01Client::droppedBytes() / socketRxDropped()).
  The HTTP stack, by contrast, delivers the response as discrete chunks
  via #HTTPRECV URCs, and AT#HTTPREAD pulls exactly one chunk per call,
  so arbitrary-sized responses work correctly.

  Exercises: AT#DNS, AT#SOCKETCREATE, AT#TCPCONNECT, AT#HTTPSTART,
  AT#HTTPMETHOD, AT#HTTPSEND, #HTTPRECV URC dispatch, AT#HTTPREAD,
  AT#HTTPSTOP, AT#SOCKETCLOSE.

  Hardware: any board with a ST87M01Boards.h preset. Run NetworkAttach
  first to verify the modem gets an IP address, and ConfigureDns once
  per device so AT#DNS has a server to query.
*/

#include <ST87M01Modem.h>
#include <ST87M01Network.h>
#include <ST87M01HTTP.h>
#include <ST87M01Boards.h>

static const char* APN  = "iot.1nce.net";
static const char* HOST = "ilabs.se";
static const char* PATH_ = "/files/ilabs-logo.txt";
static constexpr uint16_t PORT = 80;

static constexpr unsigned long REGISTRATION_TIMEOUT_MS = 120000;
static constexpr unsigned long RESPONSE_TIMEOUT_MS     = 30000;
static constexpr unsigned long READ_IDLE_TIMEOUT_MS    = 10000;

// Set to true to tee every AT command and modem response to Serial.
// Noisy — only useful when debugging the AT path.
static constexpr bool AT_DEBUG = false;

ST87M01Modem   modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);
ST87M01Network network(modem);
ST87M01HTTP    http(modem);

static const char* regStatusStr(ST87M01RegStatus r) {
  switch (r) {
    case ST87M01RegStatus::NotRegistered:      return "not registered";
    case ST87M01RegStatus::RegisteredHome:     return "registered (home)";
    case ST87M01RegStatus::Searching:          return "searching";
    case ST87M01RegStatus::RegistrationDenied: return "denied";
    case ST87M01RegStatus::RegisteredRoaming:  return "registered (roaming)";
    default:                                   return "unknown";
  }
}

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
      Serial.print(F("s] "));
      Serial.println(ok ? regStatusStr(cell.reg) : "(no response)");
    }
    delay(500);
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  ST87M01_SERIAL.begin(115200);

  Serial.println();
  Serial.println(F("=== ST87M01 HttpGet ==="));

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

  Serial.print(F("Network up. cid="));
  Serial.print(network.cid());
  Serial.print(F("  IP="));
  Serial.println(network.localIP());

  Serial.print(F("Opening HTTP session to "));
  Serial.print(HOST);
  Serial.print(':');
  Serial.println(PORT);

  if (!http.begin(HOST, PORT)) {
    Serial.print(F("http.begin() failed — CME "));
    Serial.println(modem.at().lastCmeError());
    return;
  }

  http.addHeader("User-Agent", "ST87M01/1.0");

  Serial.print(F("GET "));
  Serial.println(PATH_);

  if (!http.get(PATH_, RESPONSE_TIMEOUT_MS)) {
    Serial.print(F("http.get() failed — CME "));
    Serial.println(modem.at().lastCmeError());
    http.end();
    return;
  }

  Serial.print(F("Status: "));
  Serial.println(http.statusCode());
  Serial.print(F("Content-Length: "));
  Serial.println(http.contentLength());

  Serial.println(F("--- headers ---"));
  Serial.print(http.rawHeaders());
  Serial.println(F("--- body ---"));

  unsigned long lastByte = millis();
  size_t rxCount = 0;

  while (!http.eof()) {
    modem.poll();

    int n = http.available();
    if (n > 0) {
      while (http.available() > 0) {
        int b = http.read();
        if (b < 0) break;
        Serial.write(static_cast<uint8_t>(b));
        rxCount++;
        lastByte = millis();
      }
    } else {
      if ((millis() - lastByte) >= READ_IDLE_TIMEOUT_MS) {
        Serial.println(F("\n[idle timeout waiting for more body]"));
        break;
      }
      delay(10);
    }
  }

  Serial.println(F("--- end ---"));
  Serial.print(F("Received "));
  Serial.print(rxCount);
  Serial.println(F(" body bytes."));

  size_t dropped = http.droppedBytes();
  if (dropped) {
    Serial.print(F("WARNING: "));
    Serial.print(dropped);
    Serial.println(F(" bytes were silently dropped — a single chunk exceeded the local buffer."));
  }

  http.end();
  Serial.println(F("Done."));
}

void loop() {
  modem.poll();
}
