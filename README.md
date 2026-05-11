# iLabs ST87M01

Arduino library for the ST Microelectronics **ST87M01** cellular NB-IoT / LTE-M modem.

The library wraps the modem's AT command interface and exposes Arduino-style `Client` and `UDP` objects so sketches can talk TCP/UDP over the cellular PDP context. Higher-level helpers cover HTTP, network bring-up, cell and signal information, and NB-IoT low-power features (PSM, eDRX, sleep mode, wake-up events).

## Status

Validated on real hardware against 1NCE NB-IoT as of 2026-04-24:

- UDP send/receive end-to-end (tested with NTP).
- TCP end-to-end (tested with raw `ifconfig.me`).
- HTTP GET for arbitrary response sizes via `ST87M01HTTP` (tested with a 1457-byte body over multiple `AT#HTTPREAD` chunks).
- DNS resolution (`AT#DNS`) and ping (`AT#IPPING`).
- PSM negotiated with the network; requested and granted timer values are both readable.

HTTPS support (server-authenticated TLS 1.2 via `ST87M01HTTP` + `ST87M01TLS`) is implemented and hardware-validated against `valid.rootca3.demo.amazontrust.com` over a 1NCE NB-IoT link. Mutual TLS (client certificate + private key) is implemented — see [HTTPS / TLS](#https--tls) below. PSK cipher suites are not yet exposed.

Known limits and workarounds are covered in [Known quirks](#known-quirks) below. All AT commands are cross-checked against the ST87MXX UM AT Commands manual v3.1 (2025-10-06).

## Supported boards

Out-of-the-box pin presets live in [`src/ST87M01Boards.h`](src/ST87M01Boards.h), keyed on the `ARDUINO_*` board macro emitted by the core. Currently:

- **iLabs Challenger 2350 NB-IoT** (`rp2040:rp2040:challenger_2350_nbiot`).

Any other board works too — just pass an explicit [`ST87M01Pins`](src/ST87M01Types.h) struct to the modem constructor with your reset / wakeup / ring pins. See [Custom boards](#custom-boards) below.

## Quick start

```cpp
#include <ST87M01Modem.h>
#include <ST87M01Network.h>
#include <ST87M01Client.h>
#include <ST87M01Boards.h>

ST87M01Modem   modem(ST87M01_SERIAL, ST87M01_DEFAULT_PINS);
ST87M01Network network(modem);
ST87M01Client  client(modem);

void setup() {
  Serial.begin(115200);
  ST87M01_SERIAL.begin(115200);

  modem.begin();
  modem.setCeregMode(2);
  // ...wait for registration (see NetworkAttach example)...
  network.begin("iot.1nce.net");

  if (client.connect("example.com", 80)) {
    client.print(F("GET / HTTP/1.1\r\nHost: example.com\r\n\r\n"));
  }
}

void loop() {
  modem.poll();
  while (client.available()) Serial.write(client.read());
}
```

`ST87M01Client` and `ST87M01UDP` implement the standard Arduino `Client` / `UDP` interfaces, so anything written against those (MQTT, NTP, HTTP clients, etc.) can talk through the modem with no modification.

## Architecture

Five layers, bottom-up. Each layer holds a reference to the one below it — nothing owns a copy.

1. **`ST87M01AT`** — AT transport over a `Stream&` (normally a `HardwareSerial`). Handles line framing, `OK` / `ERROR` / `+CME ERROR:` final results, and a URC (unsolicited result code) dispatcher that can fan out to up to 8 prefix-matched callbacks.

2. **`ST87M01Modem`** — Semantic modem API on top of `ST87M01AT`. Identity (model, IMSI, operator), attach and PDP context control, IP stack activation (`AT#IPCFG`), registration and signal (`AT+CEREG` / `AT+CESQ`), DNS/ping, and the socket primitives used by `ST87M01Client` and `ST87M01UDP`. Tracks up to 3 socket slots (one TCP + two UDP per manual). You normally keep one `ST87M01Modem` per sketch and call `modem.poll()` from `loop()` to drain URCs.

3. **`ST87M01Network`** — Thin orchestration on top of `ST87M01Modem`. `begin(apn)` runs CFUN → (adopt active context **or** CGATT → CGDCONT → CGACT → CGPADDR → `AT#IPCFG`) → IP-stack-alive check. NB-IoT SIMs with pre-provisioned APNs (1NCE, most MVNOs) auto-activate the initial EPS bearer on `CFUN=1`; `Network::begin()` detects that and adopts the cid instead of trying to redefine it.

4. **`ST87M01Client`** — Arduino `Client` over a TCP socket. `connect(const char*)` resolves DNS automatically. Uses the modem's URC-driven RX indication so `AT#IPREAD` is only issued when bytes are actually waiting. 1500-byte RX buffer; excess is drained (required) and reported via `client.droppedBytes()`.

5. **`ST87M01UDP`** — Arduino `UDP` over a UDP socket. Send via `sendUdp`; RX mirrors Client.

Two more specialist adapters sit alongside the socket layer:

- **`ST87M01HTTP`** — HTTP and HTTPS client built on the modem's native `AT#HTTP*` command family. Use this for HTTP instead of raw TCP via `ST87M01Client` — the modem delivers HTTP responses in discrete chunks via `AT#HTTPREAD`, so arbitrary response sizes work. For HTTPS, pass a TLS profile id (provisioned via `ST87M01TLS`) as the third arg to `begin()`.
- **`ST87M01TLS`** — TLS profile / credential provisioning. Wraps `AT#TLSCERT*` and `AT#TLSKEY*` to upload CA root certs, client certificates, and ECC P-256 private keys into the modem's secure storage. The resulting profile id is what `ST87M01HTTP::begin()` consumes for HTTPS. Supports server-authenticated TLS and mutual TLS (mTLS).
- **`ST87M01NBIoT`** — NB-IoT-specific extras: PSM, eDRX, sleep mode, wake-up-event configuration, cell/signal/operator info, IMSI, ping.

## Which class for what

| Goal | Use |
|---|---|
| Bring the radio up and get an IP | `ST87M01Modem` + `ST87M01Network` |
| Raw TCP, small request/response | `ST87M01Client` |
| Raw UDP | `ST87M01UDP` |
| HTTP GET/POST of arbitrary size | `ST87M01HTTP` |
| HTTPS (server-auth TLS 1.2) | `ST87M01TLS` (provision CA) + `ST87M01HTTP` (`begin(host, 443, profileId)`) |
| HTTPS with mutual TLS (mTLS) | `ST87M01TLS` (CA + private key + client cert) + `ST87M01HTTP` |
| MQTT / NTP / any `Client`-based library | `ST87M01Client` / `ST87M01UDP` |
| Register on the network and monitor signal | `ST87M01Modem::getCellInfo()` / `getSignal()` + `ST87M01NBIoT` |
| PSM / eDRX / deep sleep / low-power wake | `ST87M01NBIoT` |

## Custom boards

If your board isn't in `ST87M01Boards.h`, skip that header and pass your own pin struct:

```cpp
ST87M01Modem modem(Serial1, ST87M01Pins{
  .reset  = 22,
  .wakeup = 24,
  .ring   = 25,
});
```

Each pin defaults to `-1` (= not wired) and each has a polarity flag (`resetActiveLow` / `wakeupActiveLow` / `ringActiveLow`), all defaulting to active-low.

## HTTPS / TLS

HTTPS rides on top of `ST87M01HTTP` — same API, same lifecycle, same workarounds for the modem's HTTP quirks. The only difference is at the socket layer: `AT#SOCKETCREATE` takes an extra `<security_profile_id>` argument that selects a TLS profile already provisioned on the modem, and the subsequent `AT#TCPCONNECT` performs a TLS handshake transparently.

**Server-authenticated TLS** (CA root cert only):

```cpp
#include <ST87M01TLS.h>
#include <ST87M01HTTP.h>

ST87M01TLS  tls(modem);
ST87M01HTTP https(modem);

// Once per device: provision the cert that DIRECTLY signs your server's leaf
// into a security profile (1-9). For a "server → intermediate → root" chain
// that is the INTERMEDIATE, not the root — see chain-validation note below.
tls.addCaCertPem(/*profileId=*/1, amazonRootCA3Pem);
tls.saveToNvm();   // optional — persist across power cycle (issues AT#RESET=1)

// Per session:
https.begin("api.example.com", 443, /*secProfile=*/1);
https.get("/v1/health");
```

**Mutual TLS** (client cert + private key for client authentication):

```cpp
// Provisioning order matters — import key before client cert.
tls.addCaCertPem(1, caCertPem);                  // CA that signed server's leaf
tls.addPrivateKeyPem(1, clientKeyPem);            // ECC P-256, PEM or raw 32-byte
tls.addClientCertPem(1, clientCertPem);           // client cert (modem validates match)
tls.saveToNvm();                                  // optional

https.begin("iot.example.com", 8443, /*secProfile=*/1);
```

The modem validates key-certificate correspondence at client cert import time — a mismatch returns `CME ERROR`. Import the private key *before* the client cert. Both PEM and DER forms are accepted for certs; private keys accept PEM (PKCS#8 or SEC1 `EC PRIVATE KEY`) or the raw 32-byte scalar via `addPrivateKey()`.

On TLS failure, `AT#TCPCONNECT` returns `ERROR` and the modem reports a `#HTTPDISC: <tls_error_code>` URC — read it via `https.lastTlsError()`. SNI is taken automatically from the host argument.

Storage limits: up to 9 security profiles (ids 1-9), each holding a CA cert, optionally a client cert + private key. ITS holds at most 10 files in total across all profiles (TLS app note §3.3). A full mTLS profile uses 4 ITS files (header + CA + key + client cert).

**Caveats — documented in the TLS app note (v2.0):**

- **Certificate chain validation is NOT supported** (TLS app note §3.3.2). The modem only verifies that the server's leaf cert is signed by *exactly the cert you imported* — it will not walk a multi-level chain. So for any "server → intermediate → root" deployment you must import the **intermediate** that issues the leaf, not the root. Endpoints whose leaf is signed directly by the root (e.g. `valid.rootca3.demo.amazontrust.com` → Amazon Root CA 3) work with the root imported because there is no intermediate.
- **DER-only on the wire.** `addCaCertPem()` decodes the standard PEM form (between `-----BEGIN CERTIFICATE-----` markers) host-side before upload.
- **Cipher suites** (TLS app note Table 6) are TLS 1.3 (`AES_128_{GCM,CCM,CCM_8}_SHA256`) and TLS/DTLS 1.2 `ECDHE_ECDSA_WITH_AES_*` + a PSK family. **No RSA cipher suites in TLS 1.2.**

**Caveats — empirical, not in the doc** (observed against an ST87M0 1NCE board, 2026-05-04):

- **The CA-cert parser is ECDSA-only and curve-restricted to P-256 / Brainpool-P256.** RSA roots (Amazon Root CA 1 / 2048-bit; ISRG Root X1 / 4096-bit) and ECDSA P-384 roots (ISRG Root X2) are rejected at `AT#TLSCERTADD` time with a bare `+CME ERROR`, before any handshake. The TLS 1.2 cipher list above implies this for handshakes, but the parser is stricter still.
- **AT command line caps near 1.5 KB**, so the cert plus framing has to fit there hex-encoded. ECDSA P-256 roots (~250-500 bytes DER) are well within budget; RSA-4096 (1391 bytes / 2782 hex chars) overflows and the upload silently hangs.
- **`AT#TLSCERTADD` requires `AT+CFUN=4`** (RF off / minimum functionality). The library auto-toggles this internally around each upload and restores the prior CFUN level on the way out — a sketch that's already attached doesn't need to do anything special.
- **`+CME ERROR` may arrive without a code or text** even with `AT+CMEE=2`. The AT layer recognizes the bare form as a final result so the call returns immediately rather than waiting for the timeout.

**Practical implication: most public Let's Encrypt-signed endpoints can NOT be talked to by this modem.** LE chains via either ISRG Root X1 (RSA-4096, too big) or ISRG Root X2 (ECDSA P-384, wrong curve), and every LE intermediate (R10/R11/E1/E5/E6) is RSA-2048 or P-384 — none of which the parser accepts. For real deployments, host your backend behind AWS IoT (which issues ECDSA P-256 chains via Amazon Root CA 3), a CloudFront ECC distribution, or your own ACME server issuing P-256, and remember to import the intermediate that signs your leaf, not the root above it. The `HttpsGet` example provisions **Amazon Root CA 3** and targets `valid.rootca3.demo.amazontrust.com`, which Amazon operates specifically to demonstrate Root CA 3 trust — its leaf is signed by the root directly, so a single-cert import works.

See [`examples/HttpsGet`](examples/HttpsGet) for the full flow.

## Low-power: PSM, eDRX, sleep mode

`ST87M01NBIoT` provides a friendly API for the low-power features. For PSM:

```cpp
#include <ST87M01NBIoT.h>

ST87M01NBIoT nbiot(modem);

// Once attached:
nbiot.requestPSM(/*tauSeconds=*/3600, /*activeSeconds=*/60);

ST87M01PsmInfo psm;
nbiot.getPSM(psm);
// psm.grantedTauSeconds / grantedActiveSeconds — what the network actually
// gave you (often different from what you requested).
```

The API handles the unpleasant parts for you: the T3412-extended / T3324 GPRS timer octets are encoded from seconds, `AT+CEREG` is bumped to mode 4 so the network-granted values are visible, and the readback struct keeps both decoded seconds and raw 8-bit strings. eDRX and wake-up-event configuration follow the same shape (`requestEDRX`, `setWakeupEvent`, `setSleep`, `setSleepIndications`). See the [`PsmSleep`](examples/PsmSleep) example for the full flow.

Coordinating MCU sleep with modem PSM: the modem's PSM puts the **modem** into deep sleep; putting the MCU to sleep alongside it is your application's job. The ring pin (routed by the board header) is the signal for "modem has something to tell the host" — wire it to a GPIO interrupt so the MCU can wake from deep sleep when the modem has data or a URC for you.

For MCU-side sleep, install the companion **[`iLabs_RPi_PowerManager`](../iLabs_RPi_PowerManager/)** library — it provides `RP2350Power` (resume model, `Light`/`Medium`/`Deep`/`Dormant`) and `RP2350ColdBootPower` (POWMAN P1.x cold-boot via bootrom warm-restart). For sub-mA host-side floors on the iLabs Challenger 2350 NB-IoT carrier, also install `iLabs_PMC` — it provides a separate `PMCPowerManager` adapter that plugs into the same `PowerManager&` interface and cuts MCU power entirely via the on-board PMC. The `LowPowerLoop` and `CfunOff` examples in this library `#include <RP2350Power.h>` from `iLabs_RPi_PowerManager`.

## Examples

Located in [`examples/`](examples), in rough order of complexity:

- **AtProbe** — minimal AT round-trip. Good for verifying wiring.
- **ModemInfo** — read model, revision, IMSI, operator.
- **ConfigureDns** — one-shot NVM setup for the modem's DNS server (see quirks below).
- **NetworkAttach** — register and bring up the PDP context.
- **UdpSend** — NTP round-trip.
- **TcpEcho** — raw TCP fetch against `ifconfig.me/ip`.
- **HttpGet** — HTTP GET via `ST87M01HTTP`, large-body handling.
- **HttpsGet** — HTTPS GET. Provisions Amazon Root CA 3 (ECDSA P-256) into a TLS profile then opens a secure session to Amazon's `valid.rootca3.demo.amazontrust.com` via `ST87M01TLS` + `ST87M01HTTP::begin(host, 443, profileId)`.
- **MtlsGet** — mutual TLS (mTLS). Provisions a CA cert, ECC P-256 client private key, and client certificate into a profile, then performs an HTTPS GET with client authentication. Template sketch — replace the placeholder certs/key with your own.
- **MtlsProvisionTest** — exercises the full mTLS provisioning flow (CA cert → private key → client cert → list → cleanup) without needing a server. Uses embedded test credentials to validate the modem's `AT#TLSKEYADD` and `AT#TLSCERTADD` acceptance on hardware.
- **PsmSleep** — negotiate PSM with the network, read requested vs. granted timers, optionally enable sleep URCs.
- **LowPowerLoop** — UDP/NTP heartbeat with both modem PSM and RP2350 light-sleep; demonstrates the `RP2350Power` + `ST87M01NBIoT` coordination pattern with timer + ring-pin wake.

## Known quirks

- **Configure DNS once per device.** The ST87M01's IP stack does not ingest DNS pushed via PCO during PDP activation. Factory default is no DNS, so `AT#DNS` returns `CME 2112` until you commit a DNS server to NVM. Run the `ConfigureDns` example once per device — it persists across power cycles.
- **Raw TCP RX is capped at 1500 bytes per read.** `AT#IPREAD` is one-shot and dumps the whole pending buffer in one go. If the modem has more than 1500 bytes pending, the excess is drained and dropped (drop count is reported via `client.droppedBytes()`). For HTTP specifically, use `ST87M01HTTP` — it uses `AT#HTTPREAD`, which is chunked.
- **`AT#HTTPHEADER` must follow `AT#HTTPMETHOD`.** The library queues user-supplied headers and flushes them in the right order — don't "refactor" that away.
- **HTTP body URCs (`#HTTPRECV: type=1`) are not reliably fired.** `ST87M01HTTP` uses speculative `AT#HTTPREAD` polling while there's body outstanding; don't rely on URCs alone to drain the body.
- **Stale PSM state.** `AT+CPSMS` settings are saved to NVM via `AT#RESET=1`. A modem that boots already in PSM mode can misbehave during the first attach — `nbiot.resetPSM()` early in setup clears the stored values. The `PsmSleep` example does this automatically.
- **`AT#SLEEPIND` and `AT#SLEEPMODE` settings persist only after `AT#RESET=1`.** The `modem.softReset()` helper takes care of the reset and re-probe; the library does not call it automatically.
- **Only one in-flight AT transaction.** `ST87M01Modem`, `ST87M01Network`, `ST87M01Client`, `ST87M01UDP`, `ST87M01NBIoT`, and `ST87M01HTTP` all share the same AT channel. Do not issue AT commands from a URC handler.
- **URC-handler budget: 8.** `ST87M01Modem::begin` registers 3 (CEREG, IPRECV, SOCKETCLOSED). `ST87M01HTTP` registers 2 if instantiated; `ST87M01NBIoT` registers 3. All four stacks together = the full 8. HTTPS reuses the same `#HTTPRECV` / `#HTTPDISC` handlers — `ST87M01TLS` registers no URCs of its own.
- **TLS credentials are RAM-only by default.** `AT#TLSCERTADD` and `AT#TLSKEYADD` take effect immediately but don't persist across power cycle until `AT#RESET=1`. Call `ST87M01TLS::saveToNvm()` once after provisioning; the modem reboots and you'll need to re-attach.

## Reference

The authoritative AT command reference is the **ST87MXX UM AT Commands description** PDF (v3.1, 2025-10-06) from ST.

## License

MIT — see [LICENSE](LICENSE).
