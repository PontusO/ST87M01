/*
  ST87M01 HttpGet

  Attaches to the cellular network, opens a plain TCP (port 80) connection
  to ifconfig.me, issues an HTTP/1.0 GET, and prints the entire response —
  status line, headers, blank line, body — until the server closes the
  connection. The response body is the modem's apparent public IP (the
  address the carrier's CGN NAT maps us to), which is genuinely useful
  on an IoT device and incidentally keeps the whole response comfortably
  small (~160 bytes total).

  Why small: raw TCP receive in this library is limited by ST87M01Client's
  1500-byte RX_BUF_SIZE combined with the modem's AT#IPREAD semantics
  (it dumps the entire pending buffer in a single transaction, with no
  per-read size cap). Responses larger than ~1500 bytes are silently
  truncated. For arbitrarily-sized HTTP responses, use the modem's
  dedicated HTTP client (AT#HTTPCFG / AT#HTTPMETHOD / AT#HTTPRECV —
  not yet wrapped by this library) instead of raw TCP sockets.

  HTTP/1.0 with "Connection: close" is used deliberately: the server
  terminates the socket at end-of-body, so there's no need to parse
  Content-Length or chunked transfer encoding. The #SOCKETCLOSED URC
  makes client.connected() return false, which is the natural end-of-
  stream signal.

  Exercises the full TCP + DNS path: AT#DNS, AT#SOCKETCREATE, AT#TCPCONNECT,
  AT#IPSENDTCP, #IPRECV URC dispatch, AT#IPREAD, #SOCKETCLOSED URC.

  Hardware: any board with a ST87M01Boards.h preset. Run NetworkAttach
  first to verify the modem gets an IP address, and ConfigureDns once
  per device so AT#DNS has a server to query (required — HOST is a
  name, not an IP).
*/

#include <ST87M01Modem.h>
#include <ST87M01Network.h>
#include <ST87M01Client.h>
#include <ST87M01Boards.h>

static const char* APN  = "iot.1nce.net";
static const char* HOST = "ifconfig.me";
// /ip always returns the plain-text IP (~12 bytes body, ~160 B total)
// regardless of User-Agent. The bare / path does User-Agent sniffing and
// serves a 10 KB HTML homepage to anything not recognised as curl/wget,
// which exceeds what we can cleanly receive over raw TCP on this modem.
static const char* PATH_ = "/ip";
static constexpr uint16_t PORT = 80;

static constexpr unsigned long REGISTRATION_TIMEOUT_MS = 120000;
static constexpr unsigned long READ_HARD_TIMEOUT_MS    = 30000;
static constexpr unsigned long READ_IDLE_TIMEOUT_MS    = 5000;

// Set to true to tee every AT command and modem response to Serial.
// Noisy — only useful when debugging the AT path.
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

  // Build and send the full request in a single write — one AT#IPSENDTCP
  // round-trip is cheaper than four println()s on NB-IoT.
  char req[192];
  int n = snprintf(req, sizeof(req),
                   "GET %s HTTP/1.0\r\n"
                   "Host: %s\r\n"
                   "User-Agent: ST87M01/1.0\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   PATH_, HOST);
  Serial.print(F("TX request ("));
  Serial.print(n);
  Serial.println(F(" B):"));
  Serial.print(req);     // show what we're sending
  client.write(reinterpret_cast<const uint8_t*>(req), (size_t)n);

  Serial.println(F("--- response ---"));

  unsigned long start = millis();
  unsigned long lastByte = start;
  size_t rxCount = 0;

  // Read until: (a) server closes and buffer is drained, (b) idle timeout
  // with nothing new arriving, (c) hard ceiling. HTTP/1.0 + Connection:
  // close means (a) is the normal path.
  while (true) {
    if ((millis() - start)    >= READ_HARD_TIMEOUT_MS) { Serial.println(F("\n[hard timeout]")); break; }
    if ((millis() - lastByte) >= READ_IDLE_TIMEOUT_MS) { Serial.println(F("\n[idle timeout]")); break; }
    if (!client.connected() && client.available() <= 0) break;

    modem.poll();
    while (client.available() > 0) {
      int b = client.read();
      if (b < 0) break;
      Serial.write(static_cast<uint8_t>(b));
      rxCount++;
      lastByte = millis();
    }
  }

  Serial.println(F("--- end ---"));
  Serial.print(F("Received "));
  Serial.print(rxCount);
  Serial.print(F(" bytes in "));
  Serial.print(millis() - start);
  Serial.println(F(" ms."));

  size_t dropped = client.droppedBytes();
  if (dropped) {
    Serial.print(F("WARNING: "));
    Serial.print(dropped);
    Serial.println(F(" bytes were silently dropped — response incomplete."));
    Serial.println(F("Pick a smaller endpoint, or wait until AT#HTTP* support lands."));
  }

  client.stop();
  Serial.println(F("Done."));
}

void loop() {
  modem.poll();
}
