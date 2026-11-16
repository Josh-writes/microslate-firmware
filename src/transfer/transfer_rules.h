#pragma once
// Transfer: the pure rules behind the page the device serves (no Arduino headers, host-tested in test/transfer).
// Addresses, the access code in them, download file names, JSON text and the Wi-Fi QR text.

#include <cstddef>
#include <cstdint>
#include <string>

namespace transfer {

// Characters for the access code and the hotspot password: lower case and digits without the ones that are easy to
// misread on a small screen (0/o, 1/l/i).
extern const char kCodeAlphabet[];
constexpr int kCodeLen = 4;         // in the address: microslate.local/k7m2
constexpr int kHotspotPassLen = 8;  // WPA2 needs at least 8

// A code of `len` characters from kCodeAlphabet, drawn from `rnd` (one call per character).
std::string makeCode(int len, uint32_t (*rnd)());
// True when `s` is a code this device could have made (length and alphabet).
bool codeLooksValid(const std::string& s, int len);

// What a request asks for. Every address starts with the access code: /<code>/...
enum class Route : uint8_t {
  Page,      // /<code>/           the page
  List,      // /<code>/list       the notes on offer (JSON)
  Note,      // /<code>/n/<i>      note i as text (?dl=1 asks for a download)
  NoCode,    // /<code>  without the slash: send to /<code>/
  Unknown    // anything else, including a wrong code
};
struct Request {
  Route route = Route::Unknown;
  int index = -1;   // Note: which entry of the list
};
Request parse(const std::string& path, const std::string& code);

// The name a download is saved as: the note's file name with anything a browser or file system would choke on
// replaced, never empty, always ending in .txt.
std::string downloadName(const std::string& fileName);
// Content-Disposition value for that name: an ASCII fallback plus the UTF-8 name (RFC 6266 / 5987).
std::string contentDisposition(const std::string& fileName);

// A string as a JSON string literal, quotes included.
std::string jsonString(const std::string& s);

// Wi-Fi join text for a QR code (the format phone cameras read): WIFI:T:WPA;S:<ssid>;P:<password>;;
std::string wifiQrText(const std::string& ssid, const std::string& password);

}  // namespace transfer
