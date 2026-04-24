# iLabs ST87M01

Arduino library for the ST Microelectronics **ST87M01** cellular NB-IoT / LTE-M modem.

The library wraps the modem's AT command interface and exposes Arduino-style `Client` and `UDP` objects so sketches can talk TCP/UDP over the cellular PDP context. Helpers are included for NB-IoT power saving, cell and signal information, and network bring-up.

## Status

Early development. APIs may change, and the socket-layer AT commands are not yet verified against real hardware — treat them as provisional until validated.

## Usage

_Usage notes and example sketches will be added as the library matures._

## License

MIT — see [LICENSE](LICENSE).
