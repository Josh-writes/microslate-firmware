#pragma once
// Web helpers shared by the cloud providers: URL encoding, dates, request builders and the standard OAuth replies
// (RFC 6749 token reply, RFC 8628 device code reply). Pure functions.

#include "provider.h"
#include "sync_http.h"
#include "sync_types.h"

#include <cstdint>
#include <string>

namespace msync {

std::string urlEncode(const std::string& s);   // RFC 3986 unreserved kept, everything else %XX
std::string isoTime(uint32_t epoch);           // "2026-11-02T06:26:15Z" (UTC)

HttpRequest webRequest(const char* method, const std::string& url);
HttpRequest formPost(const std::string& url, const std::string& body);   // application/x-www-form-urlencoded

// The device code reply; the verification address may be named verification_uri (the standard) or
// verification_url (Google).
DeviceCode parseDeviceCodeReply(int status, const std::string& body);
// The token reply of every provider's /token endpoint.
TokenReply parseTokenReply(int status, const std::string& body);

// A file object with id, name, a string size and the given revision key (Dropbox "rev", Google "md5Checksum").
bool parseFileObject(const std::string& body, const char* revKey, RemoteFile& f);

}  // namespace msync
