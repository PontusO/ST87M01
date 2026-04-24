/*
  ST87M01 UdpSend

  Attaches to the cellular network and polls time.google.com (an NTP
  server) every SEND_INTERVAL_MS. Sends a 48-byte NTP v3 client request,
  waits for the 48-byte server reply, and prints the stratum and the
  decoded UTC time.

  Why NTP instead of a generic UDP echo: most public UDP echo endpoints
  are long gone (tcpbin's UDP echo on 34.230.40.69:40000 doesn't respond
  as of 2026-04). NTP, by contrast, is universally reachable, replies
  deterministically on port 123, and is genuinely useful on IoT devices
  that need wall-clock time without RTC hardware. Server source port is
  always 123, so the reply traverses even symmetric CGN NATs cleanly.

  Target is an IP literal so no AT#DNS lookup is needed — useful on a
  fresh board before ConfigureDns has been run.

  Exercises: AT#SOCKETCREATE (UDP), AT#IPSENDUDP, #IPRECV URC dispatch,
  AT#IPREAD.

  Hardware: any board with a ST87M01Boards.h preset. Run NetworkAttach
  first to verify the modem gets an IP address.
*/

#include <ST87M01Modem.h>
#include <ST87M01Network.h>
#include <ST87M01UDP.h>
#include <ST87M01Boards.h>
#include <time.h>

static const char* APN = "iot.1nce.net";

// time1.google.com. Google's public NTP hosts live on 216.239.35.{0,4,8,12}
// and these addresses have been stable for years. Using an IP literal avoids
// the DNS dependency (and thus any need for ConfigureDns to have been run).
static const IPAddress NTP_SERVER(216, 239, 35, 0);
static constexpr uint16_t NTP_PORT   = 123;
static constexpr uint16_t LOCAL_PORT = 2390;  // arbitrary non-reserved

static constexpr unsigned long REGISTRATION_TIMEOUT_MS = 120000;
static constexpr unsigned long SEND_INTERVAL_MS        = 20000;
static constexpr unsigned long REPLY_WAIT_MS           = 15000;

// Set to true to tee every AT command and modem response to Serial.
// Very noisy — normally leave false once you've verified the link works.
static constexpr bool AT_DEBUG = false;

// NTP epoch (1900-01-01) vs Unix epoch (1970-01-01): 70 years + 17 leap days.
static constexpr uint32_t NTP_UNIX_OFFSET = 2208988800UL;

ST87M01Modem   modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);
ST87M01Network network(modem);
ST87M01UDP     udp(modem);

static uint32_t seq = 0;
static unsigned long lastSend = 0;
static bool ready = false;

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

// Build a minimal NTP v3 client-mode request. Byte 0 = LI(0) | VN(3) | Mode(3)
// = 0x1B; all other bytes zero. Servers reply with a full 48-byte response
// regardless of which optional fields we populate, so keeping the request
// blank is the conventional SNTP client pattern.
static void buildNtpRequest(uint8_t* buf) {
  memset(buf, 0, 48);
  buf[0] = 0x1B;
}

// Extract the transmit-timestamp seconds field (bytes 40..43, big-endian uint32,
// seconds since 1900-01-01 UTC) and convert to Unix epoch. Fractional seconds
// in bytes 44..47 are ignored here.
static uint32_t ntpToUnix(const uint8_t* buf) {
  uint32_t ntpSecs = ((uint32_t)buf[40] << 24) |
                     ((uint32_t)buf[41] << 16) |
                     ((uint32_t)buf[42] <<  8) |
                     ((uint32_t)buf[43]);
  return ntpSecs - NTP_UNIX_OFFSET;
}

// Wait up to waitMs for a single NTP reply on the open socket. Returns true
// if a plausible 48-byte packet was received and parsed.
static bool waitForNtpReply(unsigned long waitMs) {
  unsigned long start = millis();

  while ((millis() - start) < waitMs) {
    modem.poll();

    int n = udp.parsePacket();
    if (n <= 0) {
      delay(50);
      continue;
    }

    uint8_t reply[48];
    size_t got = 0;
    while (udp.available() > 0 && got < sizeof(reply)) {
      int b = udp.read();
      if (b < 0) break;
      reply[got++] = (uint8_t)b;
    }

    if (got != 48) {
      Serial.print(F("  unexpected reply size: "));
      Serial.print(got);
      Serial.println(F(" B (ignored)"));
      continue;
    }

    uint8_t mode    = reply[0] & 0x07;
    uint8_t stratum = reply[1];
    uint32_t unixTs = ntpToUnix(reply);

    time_t t = (time_t)unixTs;
    struct tm g;
    gmtime_r(&t, &g);
    char iso[24];
    strftime(iso, sizeof(iso), "%Y-%m-%d %H:%M:%S", &g);

    Serial.print(F("RX 48 B: mode="));
    Serial.print(mode);                // server reply should be 4
    Serial.print(F(" stratum="));
    Serial.print(stratum);
    Serial.print(F(" UTC="));
    Serial.println(iso);
    return true;
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  ST87M01_SERIAL.begin(115200);

  Serial.println();
  Serial.println(F("=== ST87M01 UdpSend (NTP) ==="));

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

  if (!udp.begin(LOCAL_PORT)) {
    Serial.print(F("udp.begin() failed — CME "));
    Serial.println(modem.at().lastCmeError());
    return;
  }

  Serial.print(F("UDP socket open on local port "));
  Serial.println(LOCAL_PORT);
  Serial.print(F("NTP target "));
  Serial.print(NTP_SERVER);
  Serial.print(':');
  Serial.print(NTP_PORT);
  Serial.print(F(", every "));
  Serial.print(SEND_INTERVAL_MS / 1000);
  Serial.println(F(" s"));

  ready = true;
  lastSend = millis() - SEND_INTERVAL_MS;   // send one immediately
}

void loop() {
  modem.poll();

  if (!ready) return;

  if ((millis() - lastSend) < SEND_INTERVAL_MS) return;
  lastSend = millis();

  uint8_t req[48];
  buildNtpRequest(req);

  if (!udp.beginPacket(NTP_SERVER, NTP_PORT)) {
    Serial.println(F("beginPacket() failed."));
    return;
  }
  udp.write(req, sizeof(req));
  int rc = udp.endPacket();

  ++seq;
  Serial.print(F("TX seq="));
  Serial.print(seq);
  Serial.print(F(" (48 B NTP request) "));
  if (rc != 1) {
    Serial.print(F("FAIL — CME "));
    Serial.println(modem.at().lastCmeError());
    return;
  }
  Serial.println(F("OK, waiting for reply..."));

  if (!waitForNtpReply(REPLY_WAIT_MS)) {
    Serial.println(F("  (no reply within timeout)"));
  }
}
