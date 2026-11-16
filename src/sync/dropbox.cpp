#include "dropbox.h"

#include "sync_json.h"
#include "sync_names.h"
#include "sync_pkce.h"
#include "sync_web.h"

#include <cstdio>
#include <cstdlib>

namespace msync {

static const char* const kApi = "https://api.dropboxapi.com";
static const char* const kContent = "https://content.dropboxapi.com";

static HttpRequest rpc(const std::string& path, const std::string& json) {
  HttpRequest q = webRequest("POST", std::string(kApi) + path);
  q.contentType = "application/json";
  q.body = json;
  return q;
}

// Dropbox carries its arguments in an HTTP header, and a header must be plain ASCII: anything else goes as \uXXXX.
static std::string asciiJson(const std::string& s) {
  const std::string esc = jsonEscape(s);
  std::string out;
  for (size_t i = 0; i < esc.size();) {
    const unsigned char c = (unsigned char)esc[i];
    if (c < 0x80) { out += (char)c; i++; continue; }
    uint32_t cp = 0;
    int len = 1;
    if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { cp = 0xFFFD; }
    for (int k = 1; k < len && i + k < esc.size(); k++) cp = (cp << 6) | ((unsigned char)esc[i + k] & 0x3F);
    i += len;
    char buf[16];
    if (cp >= 0x10000) {
      cp -= 0x10000;
      snprintf(buf, sizeof(buf), "\\u%04x\\u%04x", 0xD800 + (unsigned)(cp >> 10), 0xDC00 + (unsigned)(cp & 0x3FF));
    } else {
      snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)cp);
    }
    out += buf;
  }
  return out;
}

static HttpRequest content(const std::string& path, const std::string& arg) {
  HttpRequest q = webRequest("POST", std::string(kContent) + path);
  q.contentType = "application/octet-stream";
  q.headers.push_back({"Dropbox-API-Arg", arg});
  return q;
}

std::string DropboxProvider::authorizeUrl(const std::string& clientId, const std::string& verifier) const {
  return "https://www.dropbox.com/oauth2/authorize?client_id=" + urlEncode(clientId) +
         "&response_type=code&code_challenge=" + pkceChallenge(verifier) +
         "&code_challenge_method=S256&token_access_type=offline";
}

HttpRequest DropboxProvider::codeExchangeRequest(const std::string& clientId, const std::string& code,
                                                 const std::string& verifier, bool viaRelay) const {
  std::string body = "code=" + urlEncode(code) + "&grant_type=authorization_code&code_verifier=" +
                     urlEncode(verifier) + "&client_id=" + urlEncode(clientId);
  if (viaRelay && !relay_.empty()) body += "&redirect_uri=" + urlEncode(relay_ + "/cb");   // must match the approval
  return formPost(std::string(kApi) + "/oauth2/token", body);
}

// The QR code carries only this short address; the relay forwards the phone to Dropbox with the full request.
std::string DropboxProvider::relayAuthorizeUrl(const std::string& session, const std::string& verifier) const {
  return relay_ + "/a?s=" + session + "&c=" + pkceChallenge(verifier);
}

HttpRequest DropboxProvider::relayPollRequest(const std::string& session) const {
  return webRequest("GET", relay_ + "/p?s=" + session);
}

bool DropboxProvider::parseRelayPoll(int status, const std::string& body, std::string& code) const {
  if (status != 200) return false;
  Json j;
  if (!jsonParse(body, j) || !j.isObject()) return false;
  code = j.str("code");
  return !code.empty() && code.size() <= 256;
}

HttpRequest DropboxProvider::refreshRequest(const std::string& clientId, const std::string& refreshToken) const {
  return formPost(std::string(kApi) + "/oauth2/token",
                  "grant_type=refresh_token&refresh_token=" + urlEncode(refreshToken) +
                  "&client_id=" + urlEncode(clientId));
}

TokenReply DropboxProvider::parseToken(int status, const std::string& body) const { return parseTokenReply(status, body); }

HttpRequest DropboxProvider::accountRequest() const {
  return webRequest("POST", std::string(kApi) + "/2/users/get_current_account");
}

std::string DropboxProvider::parseAccount(const std::string& body) const {
  Json j;
  if (!jsonParse(body, j) || !j.isObject()) return "";
  const std::string email = j.str("email");
  if (!email.empty()) return email;
  const Json* n = j.get("name");
  return n && n->isObject() ? n->str("display_name") : "";
}

// ---- notes: the app folder is the root, paths are "/<name>" ----------------------------------------------------------

HttpRequest DropboxProvider::listRequest(const std::string& cursor) const {
  if (!cursor.empty()) return rpc("/2/files/list_folder/continue", "{\"cursor\":\"" + jsonEscape(cursor) + "\"}");
  return rpc("/2/files/list_folder", "{\"path\":\"\",\"recursive\":false,\"include_deleted\":false,\"limit\":2000}");
}

bool DropboxProvider::parseList(const std::string& body, std::vector<RemoteFile>& files, std::string& nextLink,
                                int& skipped) const {
  Json j;
  if (!jsonParse(body, j) || !j.isObject()) return false;
  const Json* arr = j.get("entries");
  if (!arr || !arr->isArray()) return false;
  for (const Json& item : arr->a) {
    if (!item.isObject()) { skipped++; continue; }
    RemoteFile f;
    f.name = item.str("name");
    f.id = item.str("id");
    f.rev = item.str("rev");   // changes whenever the content changes
    f.size = (uint32_t)strtoul(item.str("size").c_str(), nullptr, 10);
    const Json* dl = item.get("is_downloadable");
    const bool notDownloadable = dl && dl->type == Json::Bool && !dl->b;   // Dropbox Paper documents and the like
    if (item.str(".tag") != "file" || notDownloadable || f.id.empty() || f.rev.empty() || !noteNameValid(f.name)) {
      skipped++;
      continue;
    }
    files.push_back(f);   // paths are case-insensitive in Dropbox, so two entries can never share a name
  }
  const Json* more = j.get("has_more");
  const bool hasMore = more && more->type == Json::Bool && more->b;
  nextLink = hasMore ? j.str("cursor") : "";
  if (hasMore && nextLink.empty()) return false;
  return true;
}

bool DropboxProvider::pagingLinkAllowed(const std::string& cursor) const {
  if (cursor.empty() || cursor.size() > 2048) return false;
  for (char c : cursor) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                    c == '=' || c == '+' || c == '/';
    if (!ok) return false;
  }
  return true;
}

HttpRequest DropboxProvider::downloadRequest(const std::string& name, const std::string& id) const {
  (void)id;
  return content("/2/files/download", "{\"path\":\"/" + asciiJson(name) + "\"}");
}

HttpRequest DropboxProvider::uploadRequest(const std::string& name, const std::string& id, std::string body,
                                           uint32_t modified) const {
  (void)id;
  // client_modified is the date the Dropbox app gives the PC copy (Scrivener's External Folder Sync reads it).
  const std::string when = modified ? ",\"client_modified\":\"" + isoTime(modified) + "\"" : std::string();
  HttpRequest q = content("/2/files/upload", "{\"path\":\"/" + asciiJson(name) +
                                                  "\",\"mode\":\"overwrite\",\"autorename\":false,\"mute\":true" + when +
                                                  "}");
  q.body = std::move(body);
  return q;
}

bool DropboxProvider::parseUpload(const std::string& body, RemoteFile& f) const {
  return parseFileObject(body, "rev", f);
}

HttpRequest DropboxProvider::deleteRequest(const std::string& name, const std::string& id) const {
  (void)id;
  // Dropbox keeps deleted files recoverable for a while, so a mistake can be undone from the website.
  return rpc("/2/files/delete_v2", "{\"path\":\"/" + jsonEscape(name) + "\"}");
}

}  // namespace msync
