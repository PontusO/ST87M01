/*
  ST87M01 TcpEcho

  Attaches to the cellular network, opens a TCP connection to tcpbin.com:4242
  (a public bare-TCP echo server), sends one line, and prints whatever bytes
  come back during a short read window.

  Exercises the full socket stack: DNS via AT#DNS, AT#SOCKETCREATE, AT#TCPCONNECT,
  AT#IPSENDTCP, #IPRECV URC dispatch, and AT#IPREAD. If this works, the modem
  is ready for real TCP clients (MQTT, HTTP, etc.).

  Hardware: any board with a ST87M01Boards.h preset. Run NetworkAttach first
  to verify the modem gets an IP address.
*/

#include <ST87M01Modem.h>
#include <ST87M01Network.h>
#include <ST87M01Client.h>
#include <ST87M01Boards.h>

static const char* APN  = "iot.1nce.net";
static const char* HOST = "tcpbin.com";
static constexpr uint16_t PORT = 4242;
static const char* MESSAGE = "hello from ST87M01";

static constexpr unsigned long REGISTRATION_TIMEOUT_MS = 120000;
static constexpr unsigned long READ_HARD_TIMEOUT_MS    = 5000;
static constexpr unsigned long READ_IDLE_TIMEOUT_MS    = 1500;

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
  Serial.println(F("=== ST87M01 TcpEcho ==="));

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

  Serial.print(F("TX: "));
  Serial.println(MESSAGE);
  client.println(MESSAGE);

  // Expected echo = the message + the "\r\n" println appended. We exit the
  // read loop on the first of: (a) all expected bytes received, (b) idle
  // timeout after the last byte, (c) hard ceiling, (d) remote closed.
  const size_t EXPECTED_BYTES = strlen(MESSAGE) + 2;

  Serial.println(F("RX:"));
  Serial.print(F("  "));

  unsigned long start = millis();
  unsigned long lastByte = start;
  size_t rxCount = 0;

  while (rxCount < EXPECTED_BYTES) {
    if ((millis() - start) >= READ_HARD_TIMEOUT_MS) break;
    if ((millis() - lastByte) >= READ_IDLE_TIMEOUT_MS) break;
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

  Serial.println();
  Serial.print(F("Received "));
  Serial.print(rxCount);
  Serial.print(F("/"));
  Serial.print(EXPECTED_BYTES);
  Serial.print(F(" bytes in "));
  Serial.print(millis() - start);
  Serial.println(F(" ms. Closing."));

  client.stop();
  Serial.println(F("Done."));
}

void loop() {
  modem.poll();
}
