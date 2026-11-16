#pragma once
// HTTP transport interface for the sync engine. The engine never touches sockets or SD from the network side:
// it hands a request (with its body already in memory) to a Transport and later collects the response.
// Device: a worker task. Host tests: an in-process fake of the cloud.

#include <string>
#include <utility>
#include <vector>

namespace msync {

struct HttpRequest {
  std::string method;       // GET, POST, PUT, DELETE
  std::string url;          // always https://
  std::string bearer;       // empty = no Authorization header
  std::string contentType;  // empty = none
  std::vector<std::pair<std::string, std::string>> headers;  // extra headers (Dropbox passes its arguments in one)
  std::string body;
};

struct HttpResponse {
  int status = 0;           // <= 0: transport failure (no HTTP answer)
  std::string body;
  std::string location;     // Location header (redirects are never followed by the transport)
  std::string error;        // transport error text, for the log
};

class Transport {
 public:
  virtual ~Transport() {}
  // Starts a request. False if one is already in flight.
  virtual bool begin(const HttpRequest& req) = 0;
  // True exactly once when the request started by begin() has finished; `out` then holds the answer.
  virtual bool poll(HttpResponse& out) = 0;
  // Abandon the request in flight (the answer, if it arrives, is dropped).
  virtual void cancel() = 0;
};

}  // namespace msync
