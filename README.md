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

- **`ST87M01HTTP`** — HTTP client built on the modem's native `AT#HTTP*` command family. Use this for HTTP instead of raw TCP via `ST87M01Client` — the modem delivers HTTP responses in discrete chunks via `AT#HTTPREAD`, so arbitrary response sizes work.
- **`ST87M01NBIoT`** — NB-IoT-specific extras: PSM, eDRX, sleep mode, wake-up-event configuration, cell/signal/operator info, IMSI, ping.

## Which class for what

| Goal | Use |
|---|---|
| Bring the radio up and get an IP | `ST87M01Modem` + `ST87M01Network` |
| Raw TCP, small request/response | `ST87M01Client` |
| Raw UDP | `ST87M01UDP` |
| HTTP GET/POST of arbitrary size | `ST87M01HTTP` |
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

## Examples

Located in [`examples/`](examples), in rough order of complexity:

- **AtProbe** — minimal AT round-trip. Good for verifying wiring.
- **ModemInfo** — read model, revision, IMSI, operator.
- **ConfigureDns** — one-shot NVM setup for the modem's DNS server (see quirks below).
- **NetworkAttach** — register and bring up the PDP context.
- **UdpSend** — NTP round-trip.
- **TcpEcho** — raw TCP fetch against `ifconfig.me/ip`.
- **HttpGet** — HTTP GET via `ST87M01HTTP`, large-body handling.
- **PsmSleep** — negotiate PSM with the network, read requested vs. granted timers, optionally enable sleep URCs.

## Known quirks

- **Configure DNS once per device.** The ST87M01's IP stack does not ingest DNS pushed via PCO during PDP activation. Factory default is no DNS, so `AT#DNS` returns `CME 2112` until you commit a DNS server to NVM. Run the `ConfigureDns` example once per device — it persists across power cycles.
- **Raw TCP RX is capped at 1500 bytes per read.** `AT#IPREAD` is one-shot and dumps the whole pending buffer in one go. If the modem has more than 1500 bytes pending, the excess is drained and dropped (drop count is reported via `client.droppedBytes()`). For HTTP specifically, use `ST87M01HTTP` — it uses `AT#HTTPREAD`, which is chunked.
- **`AT#HTTPHEADER` must follow `AT#HTTPMETHOD`.** The library queues user-supplied headers and flushes them in the right order — don't "refactor" that away.
- **HTTP body URCs (`#HTTPRECV: type=1`) are not reliably fired.** `ST87M01HTTP` uses speculative `AT#HTTPREAD` polling while there's body outstanding; don't rely on URCs alone to drain the body.
- **Stale PSM state.** `AT+CPSMS` settings are saved to NVM via `AT#RESET=1`. A modem that boots already in PSM mode can misbehave during the first attach — `nbiot.resetPSM()` early in setup clears the stored values. The `PsmSleep` example does this automatically.
- **`AT#SLEEPIND` and `AT#SLEEPMODE` settings persist only after `AT#RESET=1`.** The `modem.softReset()` helper takes care of the reset and re-probe; the library does not call it automatically.
- **Only one in-flight AT transaction.** `ST87M01Modem`, `ST87M01Network`, `ST87M01Client`, `ST87M01UDP`, `ST87M01NBIoT`, and `ST87M01HTTP` all share the same AT channel. Do not issue AT commands from a URC handler.
- **URC-handler budget: 8.** `ST87M01Modem::begin` registers 3 (CEREG, IPRECV, SOCKETCLOSED). `ST87M01HTTP` registers 2 if instantiated; `ST87M01NBIoT` registers 3. All four stacks together = the full 8.

## Reference

The authoritative AT command reference is the **ST87MXX UM AT Commands description** PDF (v3.1, 2025-10-06) from ST.

## License

MIT — see [LICENSE](LICENSE).
