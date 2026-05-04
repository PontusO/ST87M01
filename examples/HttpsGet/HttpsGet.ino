// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Pontus Oldberg / iLabs (https://ilabs.se)
//
// Part of the iLabs ST87M01 Arduino library.
// See LICENSE at the library root for full terms.

/*
  ST87M01 HttpsGet

  Same flow as HttpGet, but over TLS. Provisions a CA root certificate into
  modem security profile 1, then opens an HTTPS session to a public Let's
  Encrypt-signed test endpoint and prints the response.

  Provisioning lives in RAM by default — uncomment SAVE_TO_NVM to persist
  the CA across power cycle (issues AT#RESET=1, which reboots the modem
  and forces another network registration).

  On TLS failure, AT#TCPCONNECT returns ERROR and the modem reports the
  failure via #HTTPDISC: <tls_error_code> — readable here through
  https.lastTlsError().

  HOST/CA: by default this targets https://valid.rootca3.demo.amazontrust.com,
  which Amazon operates specifically as a TLS demonstration endpoint signed
  by Amazon Root CA 3 (ECDSA P-256). The reason a single-cert import works
  here is that Amazon's leaf for this hostname is signed DIRECTLY by Root
  CA 3 — there's no intermediate to walk. See the chain-validation note
  below before swapping in a different host.

  Constraints to know before swapping HOST + CA:

   * Certificate chain validation is NOT supported (TLS app note §3.3.2).
     The modem only verifies that the server's leaf cert is signed by
     EXACTLY the cert imported into the profile — it will not walk a
     multi-level chain. So for a typical "server → intermediate → root"
     deployment, import the INTERMEDIATE that signs the leaf, not the
     root above it. The Amazon demo endpoint used here works with the
     root imported only because there is no intermediate.

   * The CA-cert parser is ECDSA-only and curve-restricted to P-256 /
     Brainpool-P256 (empirical, not in the doc). RSA roots (Amazon Root
     CA 1 / 2048-bit, ISRG Root X1 / 4096-bit) and ECDSA P-384 roots
     (ISRG Root X2) are rejected at AT#TLSCERTADD time. The documented
     TLS 1.2 cipher list (Table 6, all TLS_ECDHE_ECDSA_WITH_*) implies
     this for handshakes, but the parser is stricter still.

   * The AT command line caps near 1.5 KB; CA certs have to fit there
     after hex-encoding. ECDSA P-256 roots (~250-500 bytes DER) are well
     within budget; RSA-4096 (1391 bytes / 2782 hex chars) overflows.

   * AT#TLSCERTADD requires the modem in AT+CFUN=4 (RF off, empirical).
     The ST87M01TLS class auto-toggles this internally.

  Practical implication: most public Let's Encrypt-signed endpoints can
  NOT be talked to by this modem. The two LE roots (X1 RSA-4096, X2
  P-384) are both rejected, and every LE intermediate (R10/R11/E1/E5/E6)
  is RSA-2048 or P-384 — also rejected. For real deployments, host your
  backend behind AWS IoT / a CloudFront ECC distribution, or stand up
  your own ACME server issuing P-256 chains.

  Hardware: any board with a ST87M01Boards.h preset. Run NetworkAttach
  first to verify the modem gets an IP address, and ConfigureDns once per
  device so AT#DNS has a server to query.
*/

#include <ST87M01Modem.h>
#include <ST87M01Network.h>
#include <ST87M01HTTP.h>
#include <ST87M01TLS.h>
#include <ST87M01Boards.h>

static const char* APN  = "iot.1nce.net";
static const char* HOST = "valid.rootca3.demo.amazontrust.com";
static const char* PATH_ = "/";
static constexpr uint16_t PORT = 443;

// Profile id is arbitrary in 1..9 — only requirement is that the same id
// is used for provisioning and for begin(secProfile=...).
static constexpr uint8_t SEC_PROFILE = 1;

// Set to true to commit the CA cert to NVM after upload. Reboots the modem;
// the sketch then waits for re-registration before the HTTPS request runs.
static constexpr bool SAVE_TO_NVM = false;

static constexpr unsigned long REGISTRATION_TIMEOUT_MS = 120000;
static constexpr unsigned long RESPONSE_TIMEOUT_MS     = 30000;
static constexpr unsigned long READ_IDLE_TIMEOUT_MS    = 10000;

static constexpr bool AT_DEBUG = false;

ST87M01Modem   modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);
ST87M01Network network(modem);
ST87M01TLS     tls(modem);
ST87M01HTTP    https(modem);

// Amazon Root CA 3 — Amazon Trust Services' ECDSA P-256 root, valid
// through 2040. 442 bytes DER → ~880 hex chars on the AT line. P-256 is
// the curve the ST87M0's CA-cert parser actually accepts. Replace with
// the CA root that signed YOUR server's chain when targeting a different
// host (must also be ECDSA P-256 or Brainpool P-256 — see file header).
static const char AMAZON_ROOT_CA3_PEM[] =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIBtjCCAVugAwIBAgITBmyf1XSXNmY/Owua2eiedgPySjAKBggqhkjOPQQDAjA5\n"
  "MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6b24g\n"
  "Um9vdCBDQSAzMB4XDTE1MDUyNjAwMDAwMFoXDTQwMDUyNjAwMDAwMFowOTELMAkG\n"
  "A1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJvb3Qg\n"
  "Q0EgMzBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABCmXp8ZBf8ANm+gBG1bG8lKl\n"
  "ui2yEujSLtf6ycXYqm0fc4E7O5hrOXwzpcVOho6AF2hiRVd9RFgdszflZwjrZt6j\n"
  "QjBAMA8GA1UdEwEB/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgGGMB0GA1UdDgQWBBSr\n"
  "ttvXBp43rDCGB5Fwx5zEGbF4wDAKBggqhkjOPQQDAgNJADBGAiEA4IWSoxe3jfkr\n"
  "BqWTrBqYaGFy+uGh0PsceGCmQ5nFuMQCIQCcAu/xlJyzlvnrxir4tiz+OpAUFteM\n"
  "YyRIHN8wfdVoOw==\n"
  "-----END CERTIFICATE-----\n";

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
  Serial.println(F("=== ST87M01 HttpsGet ==="));

  if (AT_DEBUG) modem.at().setDebugStream(&Serial);

  if (!modem.begin()) {
    Serial.println(F("modem.begin() failed."));
    return;
  }
  modem.setCeregMode(2);

  // Provisioning happens before network attach so we don't burn registration
  // time on a configuration step. AT#TLSCERTADD doesn't need an active PDP.
  Serial.print(F("Provisioning Amazon Root CA 3 into profile "));
  Serial.print(SEC_PROFILE);
  Serial.println(F("..."));
  if (!tls.addCaCertPem(SEC_PROFILE, AMAZON_ROOT_CA3_PEM)) {
    Serial.print(F("addCaCertPem failed — CME "));
    Serial.println(modem.at().lastCmeError());
    return;
  }

  if (SAVE_TO_NVM) {
    Serial.println(F("Saving to NVM (modem will reboot)..."));
    if (!tls.saveToNvm()) {
      Serial.println(F("saveToNvm failed."));
      return;
    }
    modem.setCeregMode(2);
  }

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

  Serial.print(F("Opening HTTPS session to "));
  Serial.print(HOST);
  Serial.print(':');
  Serial.println(PORT);

  if (!https.begin(HOST, PORT, SEC_PROFILE)) {
    Serial.print(F("https.begin() failed — CME "));
    Serial.print(modem.at().lastCmeError());
    Serial.print(F("  TLS err="));
    Serial.println(https.lastTlsError());
    return;
  }

  https.addHeader("User-Agent", "ST87M01/1.0");

  Serial.print(F("GET "));
  Serial.println(PATH_);

  if (!https.get(PATH_, RESPONSE_TIMEOUT_MS)) {
    Serial.print(F("https.get() failed — CME "));
    Serial.print(modem.at().lastCmeError());
    Serial.print(F("  TLS err="));
    Serial.println(https.lastTlsError());
    https.end();
    return;
  }

  Serial.print(F("Status: "));
  Serial.println(https.statusCode());
  Serial.print(F("Content-Length: "));
  Serial.println(https.contentLength());

  Serial.println(F("--- headers ---"));
  Serial.print(https.rawHeaders());
  Serial.println(F("--- body ---"));

  unsigned long lastByte = millis();
  size_t rxCount = 0;

  while (!https.eof()) {
    modem.poll();

    int n = https.available();
    if (n > 0) {
      while (https.available() > 0) {
        int b = https.read();
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

  size_t dropped = https.droppedBytes();
  if (dropped) {
    Serial.print(F("WARNING: "));
    Serial.print(dropped);
    Serial.println(F(" bytes were silently dropped — a single chunk exceeded the local buffer."));
  }

  https.end();
  Serial.println(F("Done."));
}

void loop() {
  modem.poll();
}
