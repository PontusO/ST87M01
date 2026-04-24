#pragma once

#include <Arduino.h>
#include "ST87M01Modem.h"

// HTTP client built on the ST87M01's native AT#HTTP* command family. Unlike
// raw TCP (ST87M01Client), the modem's HTTP stack chunks the response and
// fires a #HTTPRECV URC per chunk — AT#HTTPREAD then delivers ONE chunk per
// call, so large responses never overflow the library's RX buffer the way
// they do over raw TCP.
//
// Usage:
//     ST87M01HTTP http(modem);
//     http.begin("example.com", 80);
//     http.addHeader("User-Agent", "ST87M01/1.0");   // optional
//     if (http.get("/")) {
//       Serial.print("Status: "); Serial.println(http.statusCode());
//       while (!http.eof()) {
//         modem.poll();
//         while (http.available() > 0) Serial.write(http.read());
//       }
//     }
//     http.end();
//
// Only one HTTP session can be active at a time (the modem allows exactly
// one HTTP socket). Call end() before starting another.
class ST87M01HTTP {
public:
  explicit ST87M01HTTP(ST87M01Modem& modem, uint8_t cid = 0);

  void setCid(uint8_t cid) { _cid = cid; }
  uint8_t cid() const { return _cid ? _cid : _modem.defaultCid(); }

  // Opens a TCP socket to host:port and starts the HTTP stack. `host` is
  // kept for use as the HTTP Host header in subsequent requests. Returns
  // true on success; on failure, modem.at().lastCmeError() has details.
  bool begin(const char* host, uint16_t port = 80);

  // Tear down: AT#HTTPSTOP + AT#SOCKETCLOSE.
  void end();

  // Queue an optional request header. Call any time between begin() and the
  // request method — queued headers are flushed to the modem (via
  // AT#HTTPHEADER, which must follow AT#HTTPMETHOD) during get()/post()/etc.
  // The modem automatically adds Content-Length for requests with bodies —
  // do not add it manually. Headers stay queued only until the next request;
  // after a get()/post()/etc. completes, the queue is cleared.
  bool addHeader(const char* name, const char* value);

  // Perform an HTTP request. Sends the method/path via AT#HTTPMETHOD +
  // AT#HTTPSEND, then blocks until the first #HTTPRECV (header) URC fires
  // and the header chunk is read back. After the call returns true,
  // statusCode() / contentLength() / rawHeaders() are valid, and body
  // bytes can be pulled via available() / read().
  bool get(const char* path, unsigned long timeoutMs = 30000);
  bool post(const char* path, const uint8_t* body, size_t bodyLen,
            unsigned long timeoutMs = 30000);
  bool post(const char* path, const char* body,
            unsigned long timeoutMs = 30000) {
    return post(path, reinterpret_cast<const uint8_t*>(body), strlen(body), timeoutMs);
  }
  bool put(const char* path, const uint8_t* body, size_t bodyLen,
           unsigned long timeoutMs = 30000);
  bool head(const char* path, unsigned long timeoutMs = 30000);

  // Response metadata — valid after a successful request.
  int statusCode() const { return _statusCode; }
  size_t contentLength() const { return _contentLength; }
  const String& rawHeaders() const { return _rawHeaders; }

  // Body streaming. available() reports bytes ready right now (pumping a
  // new chunk from the modem if one is pending). read() pulls one byte,
  // read(buf,len) pulls up to len bytes. Returns -1 at EOF.
  int available();
  int read();
  int read(uint8_t* buf, size_t len);
  int peek();

  // True when no more body is expected — either Content-Length bytes have
  // been delivered or the server closed the connection (#HTTPDISC URC).
  bool eof();

  // Diagnostics: cumulative count of RX bytes dropped because a chunk
  // exceeded the local buffer. Should stay 0 for HTTP; non-zero means a
  // single modem chunk was larger than BUF_SIZE.
  size_t droppedBytes() const { return _modem.socketRxDropped(_socketId); }

private:
  static constexpr size_t BUF_SIZE = 1500;

  ST87M01Modem& _modem;
  uint8_t _cid;
  uint8_t _socketId;
  bool _socketOpen;
  bool _httpStarted;

  String _host;

  // Queued request headers (name\tvalue\n pairs), flushed during request.
  String _pendingHeaders;

  // Last timestamp a speculative (non-URC-driven) AT#HTTPREAD was issued.
  unsigned long _lastSpeculativeReadMs;

  // Response metadata
  int _statusCode;
  size_t _contentLength;          // 0 if server sent no Content-Length
  String _rawHeaders;
  size_t _bodyDelivered;          // body bytes already returned to caller

  // Current body chunk being drained
  uint8_t _buf[BUF_SIZE];
  size_t _bufLen;
  size_t _bufPos;

  // Pending-chunk counters, updated from URC handlers
  bool   _headerPending;
  size_t _headerPendingLen;
  size_t _bodyChunksPending;      // number of queued body chunks
  size_t _bodyBytesPending;       // sum of bytes across those queued chunks
  bool   _disconnected;

  // Internal helpers
  bool executeRequest(const char* method, const char* path,
                      const uint8_t* body, size_t bodyLen,
                      unsigned long timeoutMs);
  bool flushPendingHeaders();
  bool readOneChunk(uint8_t* dest, size_t destMax, size_t& got);
  bool waitForHeader(unsigned long timeoutMs);
  void parseHeaderMetadata();
  void resetResponseState();

  static void onHttpRecvUrc(const String& line, void* ctx);
  static void onHttpDiscUrc(const String& line, void* ctx);
};
