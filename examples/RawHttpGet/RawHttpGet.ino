// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

/*
  ST87M01 RawHttpGet

  Attaches to the cellular network, opens a raw TCP socket via ST87M01Client
  to httpbin.org:80, issues a bare HTTP/1.0 GET /bytes/4096, and drains the
  response. The endpoint returns a deterministic 4096 random bytes — enough
  to span multiple IP frames at the modem's default max_ip_frame_size=1500.

  Purpose: exercise ST87M01Modem::readSocket()'s one-frame-per-call
  behaviour and ST87M01Client::read(buf, size)'s multi-frame drain loop.
  After the 2026-05-11 IPREAD chunked-read fix, raw TCP should pull
  responses of arbitrary length without dropping bytes. Pre-fix, the same
  request would have reported ~2500+ bytes on client.droppedBytes() and
  truncated the body around the 1500-byte mark.

  What to look for in the output:
    - "Status: 200" with "Content-Length: 4096" in the headers.
    - "Received 4096 body bytes" (matches Content-Length).
    - "droppedBytes: 0" (no per-frame buffer overflow).
    - Read-loop diagnostics: each client.read(buf, N) call may return less
      than N when it crosses a frame boundary — expected, total still adds
      up to 4096.

  Exercises: AT#DNS, AT#SOCKETCREATE, AT#TCPCONNECT, AT#IPSENDTCP,
  multiple #IPRECV URC dispatches, multiple AT#IPREAD calls.

  Hardware: any board with a ST87M01Boards.h preset. Run NetworkAttach
  first to verify the modem attaches, and ConfigureDns once per device
  so AT#DNS has a server to query.
*/

#include <ST87M01Modem.h>
#include <ST87M01Network.h>
#include <ST87M01Client.h>
#include <ST87M01Boards.h>

static const char* APN   = "iot.1nce.net";
static const char* HOST  = "httpbin.org";
static const char* PATH_ = "/bytes/4096";
static constexpr uint16_t PORT = 80;
static constexpr size_t EXPECTED_BODY_BYTES = 4096;

static constexpr unsigned long REGISTRATION_TIMEOUT_MS = 120000;
static constexpr unsigned long READ_IDLE_TIMEOUT_MS    = 10000;
static constexpr unsigned long READ_HARD_TIMEOUT_MS    = 60000;

// Set to true to tee every AT command and modem response to Serial.
static constexpr bool AT_DEBUG = false;

ST87M01Modem   modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);
ST87M01Network network(modem);
ST87M01Client  client(modem);

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

// Send the bare HTTP/1.0 GET. Uses Connection: close so the server hangs
// up after the body — easy end-of-stream signal without parsing chunked
// encoding.
static bool sendRequest() {
  client.print(F("GET "));
  client.print(PATH_);
  client.print(F(" HTTP/1.0\r\n"));
  client.print(F("Host: "));
  client.print(HOST);
  client.print(F("\r\n"));
  client.print(F("User-Agent: ST87M01/raw\r\n"));
  client.print(F("Accept: */*\r\n"));
  client.print(F("Connection: close\r\n"));
  client.print(F("\r\n"));
  return true;
}

// Read response headers line-by-line into a single String, stopping at the
// blank line that separates headers from body. Captures Content-Length
// along the way.
static bool readHeaders(String& headers, long& contentLength) {
  contentLength = -1;
  headers = "";
  unsigned long start = millis();
  String line;

  while ((millis() - start) < READ_HARD_TIMEOUT_MS) {
    if (!client.connected() && client.available() <= 0) return false;
    modem.poll();

    if (client.available() <= 0) {
      delay(10);
      continue;
    }

    int b = client.read();
    if (b < 0) continue;
    char c = static_cast<char>(b);
    headers += c;

    if (c == '\n') {
      // Trim trailing \r for parsing.
      String trimmed = line;
      trimmed.trim();
      if (trimmed.length() == 0) return true;  // blank line — body next

      // case-insensitive prefix match for "Content-Length:"
      String lower = trimmed;
      lower.toLowerCase();
      if (lower.startsWith("content-length:")) {
        contentLength = trimmed.substring(15).toInt();
      }
      line = "";
    } else if (c != '\r') {
      line += c;
    }
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  ST87M01_SERIAL.begin(115200);

  Serial.println();
  Serial.println(F("=== ST87M01 RawHttpGet ==="));

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

  Serial.print(F("Connecting to "));
  Serial.print(HOST);
  Serial.print(':');
  Serial.println(PORT);

  if (!client.connect(HOST, PORT)) {
    Serial.print(F("connect() failed — CME "));
    Serial.println(modem.at().lastCmeError());
    return;
  }
  Serial.println(F("Connected."));

  Serial.print(F("GET "));
  Serial.println(PATH_);
  sendRequest();

  String headers;
  long contentLength = -1;
  if (!readHeaders(headers, contentLength)) {
    Serial.println(F("Failed to read headers."));
    client.stop();
    return;
  }

  Serial.println(F("--- headers ---"));
  Serial.print(headers);
  Serial.print(F("Content-Length parsed: "));
  Serial.println(contentLength);

  // Drain the body using the chunked-read path: client.read(buf, N).
  // Each call may span multiple IP frames or end mid-frame; the loop
  // continues until the connection closes or we hit the idle timeout.
  Serial.println(F("--- draining body via client.read(buf, N) ---"));

  uint8_t buf[1024];
  size_t rxCount = 0;
  unsigned long start = millis();
  unsigned long lastByte = start;
  unsigned int reads = 0;
  unsigned int shortReads = 0;

  while ((millis() - start) < READ_HARD_TIMEOUT_MS) {
    modem.poll();

    int n = client.read(buf, sizeof(buf));
    if (n > 0) {
      reads++;
      if (static_cast<size_t>(n) < sizeof(buf)) shortReads++;
      rxCount += static_cast<size_t>(n);
      lastByte = millis();
      Serial.print(F("  read #"));
      Serial.print(reads);
      Serial.print(F(": "));
      Serial.print(n);
      Serial.print(F(" bytes (total "));
      Serial.print(rxCount);
      Serial.println(F(")"));
      continue;
    }

    // n <= 0: nothing this tick. Decide whether to wait or bail.
    if (!client.connected() && client.available() <= 0) break;
    if ((millis() - lastByte) >= READ_IDLE_TIMEOUT_MS) {
      Serial.println(F("[idle timeout]"));
      break;
    }
    delay(10);
  }

  unsigned long elapsed = millis() - start;

  Serial.println(F("--- summary ---"));
  Serial.print(F("Received body bytes: "));
  Serial.println(rxCount);
  Serial.print(F("Expected (Content-Length): "));
  Serial.println(contentLength);
  Serial.print(F("Expected (hardcoded /bytes/4096): "));
  Serial.println(EXPECTED_BODY_BYTES);
  Serial.print(F("client.read() calls: "));
  Serial.println(reads);
  Serial.print(F("  short reads (n < buffer): "));
  Serial.println(shortReads);
  Serial.print(F("Elapsed: "));
  Serial.print(elapsed);
  Serial.println(F(" ms"));

  size_t dropped = client.droppedBytes();
  Serial.print(F("droppedBytes: "));
  Serial.println(dropped);
  if (dropped) {
    Serial.println(F("WARNING: a single IP frame exceeded the 1600-byte buffer."));
    Serial.println(F("  Frame-overflow drops, not multi-frame drops — the modem"));
    Serial.println(F("  emitted a frame larger than the documented max_ip_frame_size"));
    Serial.println(F("  ceiling. Check AT#IPPARAMS and ST forum for firmware quirks."));
  }

  bool ok = (rxCount == EXPECTED_BODY_BYTES) && (dropped == 0) &&
            (contentLength < 0 || static_cast<size_t>(contentLength) == rxCount);
  Serial.println(ok ? F("PASS — multi-frame raw-TCP drain works.")
                    : F("FAIL — check the numbers above."));

  client.stop();
  Serial.println(F("Done."));
}

void loop() {
  modem.poll();
}
