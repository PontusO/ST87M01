#include "ST87M01HTTP.h"

ST87M01HTTP::ST87M01HTTP(ST87M01Modem& modem, uint8_t cid)
: _modem(modem), _cid(cid), _socketId(0xFF),
  _socketOpen(false), _httpStarted(false),
  _lastSpeculativeReadMs(0),
  _statusCode(0), _contentLength(0), _bodyDelivered(0),
  _bufLen(0), _bufPos(0),
  _headerPending(false), _headerPendingLen(0),
  _bodyChunksPending(0), _bodyBytesPending(0),
  _disconnected(false) {

  // Register URC handlers once. They live for the life of the modem; when
  // no HTTP session is active the callbacks just bump fields that nothing
  // else reads, which is harmless.
  _modem.at().registerUrcHandler("#HTTPRECV:", &ST87M01HTTP::onHttpRecvUrc, this);
  _modem.at().registerUrcHandler("#HTTPDISC",  &ST87M01HTTP::onHttpDiscUrc, this);
}

void ST87M01HTTP::resetResponseState() {
  _statusCode = 0;
  _contentLength = 0;
  _bodyDelivered = 0;
  _rawHeaders = "";
  _bufLen = _bufPos = 0;
  _headerPending = false;
  _headerPendingLen = 0;
  _bodyChunksPending = 0;
  _bodyBytesPending = 0;
  _disconnected = false;
}

bool ST87M01HTTP::begin(const char* host, uint16_t port) {
  if (!host || !*host) return false;

  if (_socketOpen) end();
  resetResponseState();

  _host = host;

  // Create TCP socket. frame_received_urc=1 is what the HTTP app note uses;
  // it enables the raw-IP URCs (which we ignore while HTTP is in control),
  // but the socket is still tracked by ST87M01Modem so droppedBytes() works.
  if (!_modem.createSocket(cid(), /*tcp=*/true, _socketId, /*localPort=*/0)) {
    return false;
  }
  _socketOpen = true;

  if (!_modem.connectTcp(cid(), _socketId, host, port)) {
    end();
    return false;
  }

  _modem.at().sendf("AT#HTTPSTART");
  if (!_modem.at().expectOK(10000)) {
    end();
    return false;
  }
  _httpStarted = true;
  return true;
}

void ST87M01HTTP::end() {
  if (_httpStarted) {
    _modem.at().sendf("AT#HTTPSTOP");
    _modem.at().expectOK(10000);
    _httpStarted = false;
  }
  if (_socketOpen && _socketId != 0xFF) {
    _modem.closeSocket(cid(), _socketId);
  }
  _socketOpen = false;
  _socketId = 0xFF;
  _host = "";
  resetResponseState();
}

bool ST87M01HTTP::addHeader(const char* name, const char* value) {
  if (!name || !value) return false;
  // AT#HTTPHEADER returns CME 2310 ("method not initialized") when issued
  // before AT#HTTPMETHOD, so we queue the pair here and flush it during
  // executeRequest() after HTTPMETHOD has run.
  _pendingHeaders += name;
  _pendingHeaders += '\t';
  _pendingHeaders += value;
  _pendingHeaders += '\n';
  return true;
}

bool ST87M01HTTP::flushPendingHeaders() {
  int start = 0;
  while (start < static_cast<int>(_pendingHeaders.length())) {
    int tab = _pendingHeaders.indexOf('\t', start);
    int nl  = _pendingHeaders.indexOf('\n', (tab >= 0) ? tab + 1 : start);
    if (tab < 0 || nl < 0) break;

    String name  = _pendingHeaders.substring(start, tab);
    String value = _pendingHeaders.substring(tab + 1, nl);
    _modem.at().sendf("AT#HTTPHEADER=%s,%s", name.c_str(), value.c_str());
    if (!_modem.at().expectOK(10000)) {
      _pendingHeaders = "";
      return false;
    }
    start = nl + 1;
  }
  _pendingHeaders = "";
  return true;
}

bool ST87M01HTTP::get(const char* path, unsigned long timeoutMs) {
  return executeRequest("GET", path, nullptr, 0, timeoutMs);
}

bool ST87M01HTTP::post(const char* path, const uint8_t* body, size_t bodyLen,
                      unsigned long timeoutMs) {
  return executeRequest("POST", path, body, bodyLen, timeoutMs);
}

bool ST87M01HTTP::put(const char* path, const uint8_t* body, size_t bodyLen,
                     unsigned long timeoutMs) {
  return executeRequest("PUT", path, body, bodyLen, timeoutMs);
}

bool ST87M01HTTP::head(const char* path, unsigned long timeoutMs) {
  return executeRequest("HEAD", path, nullptr, 0, timeoutMs);
}

bool ST87M01HTTP::executeRequest(const char* method, const char* path,
                                 const uint8_t* body, size_t bodyLen,
                                 unsigned long timeoutMs) {
  if (!_httpStarted || !path) return false;

  // Clear any stale response state from a prior call (headers, buffer).
  _statusCode = 0;
  _contentLength = 0;
  _bodyDelivered = 0;
  _rawHeaders = "";
  _bufLen = _bufPos = 0;
  _headerPending = false;
  _headerPendingLen = 0;
  _bodyChunksPending = 0;
  _bodyBytesPending = 0;
  _disconnected = false;

  // Optional body: stage it via AT#HTTPBODY before HTTPSEND. Use hex mode
  // (format=2) so arbitrary binary payloads work regardless of what bytes
  // the caller passes.
  if (body && bodyLen) {
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "AT#HTTPBODY=2,");
    _modem.at().beginCommand(prefix);
    _modem.at().writeHex(body, bodyLen);
    _modem.at().endCommand();
    if (!_modem.at().expectOK(10000)) return false;
  }

  // AT#HTTPMETHOD=<method>,<host>,<path>,<keep_alive>. keep_alive=0 = close
  // TCP after response, which pairs naturally with our one-session model.
  _modem.at().sendf("AT#HTTPMETHOD=%s,%s,%s,0", method, _host.c_str(), path);
  if (!_modem.at().expectOK(10000)) return false;

  // AT#HTTPHEADER only accepts pairs *after* AT#HTTPMETHOD has run, so flush
  // any queued headers now (see addHeader()).
  if (!flushPendingHeaders()) return false;

  // AT#HTTPSEND=<cid>,<socketId> — actually puts the request on the wire.
  _modem.at().sendf("AT#HTTPSEND=%u,%u", cid(), _socketId);
  if (!_modem.at().expectOK(10000)) return false;

  // Block until the header URC fires and the header chunk is read, so
  // statusCode() / contentLength() are populated before we return.
  if (!waitForHeader(timeoutMs)) return false;

  parseHeaderMetadata();
  return true;
}

bool ST87M01HTTP::waitForHeader(unsigned long timeoutMs) {
  unsigned long start = millis();
  while ((millis() - start) < timeoutMs) {
    _modem.poll();

    if (_headerPending) {
      // Pull the header chunk into _buf (its length was announced in the URC).
      size_t got = 0;
      if (!readOneChunk(_buf, sizeof(_buf), got)) return false;

      _headerPending = false;
      _headerPendingLen = 0;
      _rawHeaders = "";
      _rawHeaders.reserve(got);
      for (size_t i = 0; i < got; ++i) _rawHeaders += static_cast<char>(_buf[i]);
      _bufLen = _bufPos = 0;                  // header doesn't go to read()
      return true;
    }

    if (_disconnected) return false;
    delay(10);
  }
  return false;
}

// Reads one AT#HTTPREAD response: "#HTTPREAD: <len>" / <data> / "OK".
// Stores at most destMax bytes into dest; bytes beyond that are drained to
// keep the AT channel aligned (same trade-off as readSocket).
bool ST87M01HTTP::readOneChunk(uint8_t* dest, size_t destMax, size_t& got) {
  got = 0;
  String line;
  _modem.at().sendf("AT#HTTPREAD");
  if (!_modem.at().waitLineStartsWith("#HTTPREAD:", line, 30000)) return false;

  int colon = line.indexOf(':');
  if (colon < 0) return false;
  size_t declared = static_cast<size_t>(line.substring(colon + 1).toInt());
  if (declared == 0) {
    _modem.at().expectOK(5000);
    return true;                               // empty chunk, still valid
  }

  size_t toCopy = (declared < destMax) ? declared : destMax;
  got = _modem.at().readBytes(dest, toCopy, 30000);

  // Drain any excess — the modem ships the full chunk even if we undersized
  // our buffer. For HTTP this should be rare (chunks are typically <1 KB),
  // but surface it via the same socketRxDropped counter used by raw TCP so
  // diagnostics work consistently.
  size_t overflow = (declared > got) ? (declared - got) : 0;
  size_t dropped = 0;
  while (overflow--) {
    uint8_t scratch;
    if (_modem.at().readBytes(&scratch, 1, 30000) != 1) break;
    ++dropped;
  }
  if (dropped) {
    // The socket-slot dropped counter is bumped via the modem; we go at it
    // directly because readSocket() (which normally does this) isn't the
    // path used for HTTP reads. Simpler to also warn loudly if debug is on.
    if (Stream* dbg = _modem.at().debugStream()) {
      dbg->print(F("!!! HTTP chunk truncated: "));
      dbg->print(dropped);
      dbg->print(F(" bytes dropped (declared="));
      dbg->print(declared);
      dbg->print(F(", buffer="));
      dbg->print(destMax);
      dbg->println(F(")"));
    }
  }

  return _modem.at().expectOK(5000);
}

void ST87M01HTTP::parseHeaderMetadata() {
  // Walk the raw header text for the HTTP status line and Content-Length.
  // Simple manual parse — no regex, no malloc beyond String.
  int lineStart = 0;
  while (lineStart < static_cast<int>(_rawHeaders.length())) {
    int lineEnd = _rawHeaders.indexOf('\n', lineStart);
    if (lineEnd < 0) lineEnd = _rawHeaders.length();

    String line = _rawHeaders.substring(lineStart, lineEnd);
    // Trim trailing \r
    while (line.length() && (line.charAt(line.length() - 1) == '\r' ||
                             line.charAt(line.length() - 1) == '\n')) {
      line.remove(line.length() - 1);
    }

    if (line.startsWith("HTTP/") && _statusCode == 0) {
      int sp1 = line.indexOf(' ');
      if (sp1 >= 0) {
        int sp2 = line.indexOf(' ', sp1 + 1);
        String code = (sp2 >= 0) ? line.substring(sp1 + 1, sp2)
                                 : line.substring(sp1 + 1);
        _statusCode = code.toInt();
      }
    } else {
      // Case-insensitive match on "content-length:"
      String lower = line;
      lower.toLowerCase();
      if (lower.startsWith("content-length:")) {
        int colon = line.indexOf(':');
        if (colon >= 0) {
          String v = line.substring(colon + 1);
          v.trim();
          _contentLength = static_cast<size_t>(v.toInt());
        }
      }
    }

    lineStart = lineEnd + 1;
  }
}

int ST87M01HTTP::available() {
  if (_bufPos < _bufLen) return static_cast<int>(_bufLen - _bufPos);

  _modem.poll();

  // Check for obvious end-of-stream conditions first.
  if (_contentLength > 0 && _bodyDelivered >= _contentLength) return 0;
  if (_disconnected && _bodyChunksPending == 0) return 0;

  // Observed modem behaviour: body `#HTTPRECV: 1,...` URCs are unreliable
  // (they can arrive corrupted or not at all on some responses). So we
  // fall back to pulling — issue AT#HTTPREAD speculatively whenever more
  // body bytes are expected. URC-driven reads go through immediately; pure
  // speculative reads are rate-limited to ~10/s so we don't flood the AT
  // channel with empty polls.
  bool urcSignal = _bodyChunksPending > 0;
  if (!urcSignal) {
    unsigned long now = millis();
    if ((now - _lastSpeculativeReadMs) < 100) return 0;
    _lastSpeculativeReadMs = now;
  }

  size_t got = 0;
  if (!readOneChunk(_buf, sizeof(_buf), got)) return 0;

  if (_bodyChunksPending > 0) _bodyChunksPending--;
  if (_bodyBytesPending >= got) _bodyBytesPending -= got;
  else _bodyBytesPending = 0;

  if (got == 0) return 0;

  _bufPos = 0;
  _bufLen = got;
  return static_cast<int>(got);
}

int ST87M01HTTP::read() {
  if (available() <= 0) return -1;
  int b = _buf[_bufPos++];
  _bodyDelivered++;
  return b;
}

int ST87M01HTTP::read(uint8_t* buf, size_t len) {
  if (!buf || !len) return 0;
  if (available() <= 0) return -1;
  size_t n = _bufLen - _bufPos;
  if (n > len) n = len;
  memcpy(buf, &_buf[_bufPos], n);
  _bufPos += n;
  _bodyDelivered += n;
  return static_cast<int>(n);
}

int ST87M01HTTP::peek() {
  if (available() <= 0) return -1;
  return _buf[_bufPos];
}

bool ST87M01HTTP::eof() {
  // More bytes in the current chunk? Not done.
  if (_bufPos < _bufLen) return false;
  _modem.poll();
  // More chunks queued by URC? Not done.
  if (_bodyChunksPending > 0) return false;
  // Content-Length satisfied? Done (even if we haven't seen #HTTPDISC yet).
  if (_contentLength > 0 && _bodyDelivered >= _contentLength) return true;
  // Server closed the HTTP session? Done.
  if (_disconnected) return true;
  return false;
}

// ---------------------------------------------------------------------------
// URC handlers. Invoked from modem.poll() — not an ISR, so no volatile needed.
// ---------------------------------------------------------------------------
void ST87M01HTTP::onHttpRecvUrc(const String& line, void* ctx) {
  auto* self = static_cast<ST87M01HTTP*>(ctx);

  int colon = line.indexOf(':');
  if (colon < 0) return;
  String rest = line.substring(colon + 1);
  rest.trim();

  // Fields are comma-separated: <type>,<chunk_len>[,<status>,<content_length>]
  int c1 = rest.indexOf(',');
  if (c1 < 0) return;

  int type = rest.substring(0, c1).toInt();
  int c2 = rest.indexOf(',', c1 + 1);
  size_t chunkLen = static_cast<size_t>(
      rest.substring(c1 + 1, (c2 >= 0) ? c2 : rest.length()).toInt());

  switch (type) {
    case 0:  // Header
      self->_headerPending = true;
      self->_headerPendingLen = chunkLen;
      // status + content_length fields, if present, are redundant with what
      // we'll pull out of the raw headers — ignore them here to keep the
      // parser single-sourced.
      break;

    case 1:  // Body chunk
      self->_bodyChunksPending++;
      self->_bodyBytesPending += chunkLen;
      break;

    case 2:  // Partial header — not currently handled; would need merging.
    case 3:  // Invalid — signal end of session.
    default:
      self->_disconnected = true;
      break;
  }
}

void ST87M01HTTP::onHttpDiscUrc(const String& /*line*/, void* ctx) {
  auto* self = static_cast<ST87M01HTTP*>(ctx);
  self->_disconnected = true;
}
