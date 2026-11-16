#include "onedrive.h"

#include "sync_json.h"
#include "sync_names.h"

namespace msync {
namespace od {

const char* const kAuthBase = "https://login.microsoftonline.com/consumers/oauth2/v2.0";
const char* const kGraph = "https://graph.microsoft.com/v1.0";
const char* const kScope = "Files.ReadWrite.AppFolder User.Read offline_access";

std::string listUrl() {
  return std::string(kGraph) + "/me/drive/special/approot/children?$select=id,name,size,cTag,file&$top=200";
}
std::string contentUrl(const std::string& name) {
  return std::string(kGraph) + "/me/drive/special/approot:/" + urlEncode(name) + ":/content";
}
std::string itemUrl(const std::string& name) {
  return std::string(kGraph) + "/me/drive/special/approot:/" + urlEncode(name);
}
std::string meUrl() { return std::string(kGraph) + "/me?$select=displayName"; }

bool graphUrl(const std::string& url) { return url.compare(0, 30, "https://graph.microsoft.com/v1") == 0; }
bool httpsUrl(const std::string& url) { return url.size() > 8 && url.compare(0, 8, "https://") == 0; }

static bool fillFile(const Json& item, RemoteFile& f) {
  f.name = item.str("name");
  f.id = item.str("id");
  f.rev = item.str("cTag");
  f.size = (uint32_t)item.num("size");
  return !f.name.empty() && !f.id.empty();
}

bool parseList(const std::string& body, std::vector<RemoteFile>& files, std::string& nextLink, int& skipped) {
  Json j;
  if (!jsonParse(body, j) || !j.isObject()) return false;
  const Json* arr = j.get("value");
  if (!arr || !arr->isArray()) return false;
  for (const Json& item : arr->a) {
    if (!item.isObject()) { skipped++; continue; }
    RemoteFile f;
    if (!item.get("file") || !fillFile(item, f) || !noteNameValid(f.name) || f.rev.empty()) { skipped++; continue; }
    files.push_back(f);
  }
  nextLink = j.str("@odata.nextLink");
  return true;
}

bool parseItem(const std::string& body, RemoteFile& f) {
  Json j;
  if (!jsonParse(body, j) || !j.isObject()) return false;
  return fillFile(j, f) && !f.rev.empty();
}

std::string formDeviceCode(const std::string& clientId) {
  return "client_id=" + clientId + "&scope=" + urlEncode(kScope);
}
std::string formDevicePoll(const std::string& clientId, const std::string& deviceCode) {
  return "grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=" + clientId +
         "&device_code=" + urlEncode(deviceCode);
}
std::string formRefresh(const std::string& clientId, const std::string& refreshToken) {
  return "grant_type=refresh_token&client_id=" + clientId + "&refresh_token=" + urlEncode(refreshToken) +
         "&scope=" + urlEncode(kScope);
}

std::string parseDisplayName(const std::string& body) {
  Json j;
  if (!jsonParse(body, j) || !j.isObject()) return "";
  return j.str("displayName");
}

}  // namespace od

HttpRequest OneDriveProvider::deviceCodeRequest(const std::string& clientId) const {
  return formPost(std::string(od::kAuthBase) + "/devicecode", od::formDeviceCode(clientId));
}
DeviceCode OneDriveProvider::parseDeviceCode(int status, const std::string& body) const {
  return parseDeviceCodeReply(status, body);
}
HttpRequest OneDriveProvider::devicePollRequest(const std::string& clientId, const std::string& deviceCode) const {
  return formPost(std::string(od::kAuthBase) + "/token", od::formDevicePoll(clientId, deviceCode));
}
HttpRequest OneDriveProvider::refreshRequest(const std::string& clientId, const std::string& refreshToken) const {
  return formPost(std::string(od::kAuthBase) + "/token", od::formRefresh(clientId, refreshToken));
}
TokenReply OneDriveProvider::parseToken(int status, const std::string& body) const { return parseTokenReply(status, body); }
HttpRequest OneDriveProvider::accountRequest() const { return webRequest("GET", od::meUrl()); }
std::string OneDriveProvider::parseAccount(const std::string& body) const { return od::parseDisplayName(body); }
HttpRequest OneDriveProvider::listRequest(const std::string& pageLink) const {
  return webRequest("GET", pageLink.empty() ? od::listUrl() : pageLink);
}
bool OneDriveProvider::parseList(const std::string& body, std::vector<RemoteFile>& files, std::string& nextLink,
                                 int& skipped) const {
  return od::parseList(body, files, nextLink, skipped);
}
bool OneDriveProvider::pagingLinkAllowed(const std::string& url) const { return od::graphUrl(url); }
HttpRequest OneDriveProvider::downloadRequest(const std::string& name, const std::string&) const {
  return webRequest("GET", od::contentUrl(name));
}
bool OneDriveProvider::redirectAllowed(const std::string& url) const { return od::httpsUrl(url); }
HttpRequest OneDriveProvider::uploadRequest(const std::string& name, const std::string&, std::string body,
                                            uint32_t) const {
  HttpRequest q = webRequest("PUT", od::contentUrl(name));
  q.contentType = "text/plain";
  q.body = std::move(body);
  return q;
}
bool OneDriveProvider::parseUpload(const std::string& body, RemoteFile& f) const { return od::parseItem(body, f); }
// A content upload keeps the file's old "modified" date (seen 2026-11-02: an edit uploaded today still showed the
// end of March, so the PC copy and Scrivener did not take it as new). Setting fileSystemInfo puts today's on it.
bool OneDriveProvider::dateRequest(const std::string& name, const RemoteFile&, uint32_t modified,
                                   HttpRequest& out) const {
  if (modified == 0) return false;
  out = webRequest("PATCH", od::itemUrl(name) + "?$select=id,name,size,cTag,file");
  out.contentType = "application/json";
  out.body = "{\"fileSystemInfo\":{\"lastModifiedDateTime\":\"" + isoTime(modified) + "\"}}";
  return true;
}
HttpRequest OneDriveProvider::deleteRequest(const std::string& name, const std::string&) const {
  return webRequest("DELETE", od::itemUrl(name));
}

}  // namespace msync
