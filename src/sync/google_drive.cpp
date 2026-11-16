#include "google_drive.h"

#include "sync_json.h"
#include "sync_names.h"
#include "sync_web.h"

#include <cstdlib>
#include <set>

namespace msync {

static const char* const kAuth = "https://oauth2.googleapis.com";
static const char* const kApi = "https://www.googleapis.com/drive/v3";
static const char* const kUpload = "https://www.googleapis.com/upload/drive/v3";
static const char* const kScope = "https://www.googleapis.com/auth/drive.file";
static const char* const kFolderName = "MicroSlate";
static const char* const kFolderMime = "application/vnd.google-apps.folder";
static const char* const kFileFields = "id,name,size,md5Checksum";

HttpRequest GoogleDriveProvider::deviceCodeRequest(const std::string& clientId) const {
  return formPost(std::string(kAuth) + "/device/code",
              "client_id=" + urlEncode(clientId) + "&scope=" + urlEncode(kScope));
}

DeviceCode GoogleDriveProvider::parseDeviceCode(int status, const std::string& body) const {
  return parseDeviceCodeReply(status, body);   // Google names the address verification_url
}

HttpRequest GoogleDriveProvider::devicePollRequest(const std::string& clientId, const std::string& deviceCode) const {
  return formPost(std::string(kAuth) + "/token",
              "client_id=" + urlEncode(clientId) + "&client_secret=" + urlEncode(secret_) +
                  "&device_code=" + urlEncode(deviceCode) +
                  "&grant_type=urn:ietf:params:oauth:grant-type:device_code");
}

HttpRequest GoogleDriveProvider::refreshRequest(const std::string& clientId, const std::string& refreshToken) const {
  return formPost(std::string(kAuth) + "/token",
              "client_id=" + urlEncode(clientId) + "&client_secret=" + urlEncode(secret_) +
                  "&refresh_token=" + urlEncode(refreshToken) + "&grant_type=refresh_token");
}

TokenReply GoogleDriveProvider::parseToken(int status, const std::string& body) const {
  TokenReply t = parseTokenReply(status, body);
  // The engine speaks Microsoft's names for the user saying no.
  if (t.error == "access_denied") t.error = "authorization_declined";
  return t;
}

HttpRequest GoogleDriveProvider::accountRequest() const {
  return webRequest("GET", std::string(kApi) + "/about?fields=" + urlEncode("user(displayName,emailAddress)"));
}

std::string GoogleDriveProvider::parseAccount(const std::string& body) const {
  Json j;
  if (!jsonParse(body, j) || !j.isObject()) return "";
  const Json* u = j.get("user");
  if (!u || !u->isObject()) return "";
  std::string n = u->str("emailAddress");
  return n.empty() ? u->str("displayName") : n;
}

// ---- the MicroSlate folder ---------------------------------------------------------------------------------------

bool GoogleDriveProvider::setupRequest(int step, HttpRequest& out) {
  if (!folderId_.empty()) return false;
  if (step == 0) {
    const std::string q = std::string("name='") + kFolderName + "' and mimeType='" + kFolderMime +
                          "' and trashed=false and 'root' in parents";
    out = webRequest("GET", std::string(kApi) + "/files?q=" + urlEncode(q) + "&fields=" +
                           urlEncode("files(id,name)") + "&spaces=drive&pageSize=10");
    return true;
  }
  if (step == 1) {
    out = webRequest("POST", std::string(kApi) + "/files?fields=id");
    out.contentType = "application/json";
    out.body = std::string("{\"name\":\"") + kFolderName + "\",\"mimeType\":\"" + kFolderMime + "\"}";
    return true;
  }
  return false;
}

bool GoogleDriveProvider::setupReply(int step, int status, const std::string& body, std::string& error) {
  if (status != 200) {
    error = step == 0 ? "Could not look for the MicroSlate folder in Google Drive"
                      : "Could not create the MicroSlate folder in Google Drive";
    return false;
  }
  Json j;
  if (!jsonParse(body, j) || !j.isObject()) { error = "Unreadable reply from Google Drive"; return false; }
  if (step == 0) {
    const Json* arr = j.get("files");
    if (arr && arr->isArray() && !arr->a.empty() && arr->a[0].isObject()) folderId_ = arr->a[0].str("id");
    return true;   // none found: the next step creates it
  }
  folderId_ = j.str("id");
  if (folderId_.empty()) { error = "Google Drive did not return the folder"; return false; }
  return true;
}

// ---- notes -------------------------------------------------------------------------------------------------------

std::string GoogleDriveProvider::listUrl(const std::string& pageToken) const {
  std::string u = std::string(kApi) + "/files?q=" + urlEncode("'" + folderId_ + "' in parents and trashed=false") +
                  "&fields=" + urlEncode("nextPageToken,files(id,name,size,md5Checksum,mimeType)") +
                  "&pageSize=200&spaces=drive";
  if (!pageToken.empty()) u += "&pageToken=" + urlEncode(pageToken);
  return u;
}

HttpRequest GoogleDriveProvider::listRequest(const std::string& pageLink) const {
  return webRequest("GET", pageLink.empty() ? listUrl("") : pageLink);
}

bool GoogleDriveProvider::parseList(const std::string& body, std::vector<RemoteFile>& files, std::string& nextLink,
                                    int& skipped) const {
  Json j;
  if (!jsonParse(body, j) || !j.isObject()) return false;
  const Json* arr = j.get("files");
  if (!arr || !arr->isArray()) return false;
  std::set<std::string> seen;
  for (const RemoteFile& f : files) seen.insert(f.name);
  for (const Json& item : arr->a) {
    if (!item.isObject()) { skipped++; continue; }
    RemoteFile f;
    f.name = item.str("name");
    f.id = item.str("id");
    f.rev = item.str("md5Checksum");   // changes whenever the content changes; Google's own documents have none
    f.size = (uint32_t)strtoul(item.str("size").c_str(), nullptr, 10);
    if (item.str("mimeType") == kFolderMime || f.id.empty() || f.rev.empty() || !noteNameValid(f.name)) { skipped++; continue; }
    // Drive allows two files with one name; the device cannot. Keep the first, ignore the rest.
    if (!seen.insert(f.name).second) { skipped++; continue; }
    files.push_back(f);
  }
  const std::string token = j.str("nextPageToken");
  nextLink = token.empty() ? "" : listUrl(token);
  return true;
}

bool GoogleDriveProvider::pagingLinkAllowed(const std::string& url) const {
  return url.compare(0, 36, "https://www.googleapis.com/drive/v3/") == 0;
}

HttpRequest GoogleDriveProvider::downloadRequest(const std::string& name, const std::string& id) const {
  (void)name;
  return webRequest("GET", std::string(kApi) + "/files/" + urlEncode(id) + "?alt=media");
}

bool GoogleDriveProvider::redirectAllowed(const std::string& url) const {
  return url.size() > 8 && url.compare(0, 8, "https://") == 0;
}

// `modified` is not sent: Drive sets modifiedTime to the time of the upload by itself.
HttpRequest GoogleDriveProvider::uploadRequest(const std::string& name, const std::string& id, std::string body,
                                               uint32_t) const {
  if (!id.empty()) {   // the note exists: replace its content
    HttpRequest q = webRequest("PATCH", std::string(kUpload) + "/files/" + urlEncode(id) + "?uploadType=media&fields=" +
                                       urlEncode(kFileFields));
    q.contentType = "text/plain";
    q.body = std::move(body);
    return q;
  }
  // A new note: one multipart request carries the name, the folder and the text.
  static const char* const kBoundary = "microslate_boundary_7e1f3c";
  HttpRequest q = webRequest("POST", std::string(kUpload) + "/files?uploadType=multipart&fields=" + urlEncode(kFileFields));
  q.contentType = std::string("multipart/related; boundary=") + kBoundary;
  const std::string head = std::string("--") + kBoundary + "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n{\"name\":\"" +
                           jsonEscape(name) + "\",\"parents\":[\"" + folderId_ + "\"],\"mimeType\":\"text/plain\"}\r\n--" +
                           kBoundary + "\r\nContent-Type: text/plain\r\n\r\n";
  const std::string tail = std::string("\r\n--") + kBoundary + "--";
  body.insert(0, head);   // reuse the note's own buffer: a 4 MB note is not copied twice
  body += tail;
  q.body = std::move(body);
  return q;
}

bool GoogleDriveProvider::parseUpload(const std::string& body, RemoteFile& f) const {
  return parseFileObject(body, "md5Checksum", f);
}

HttpRequest GoogleDriveProvider::deleteRequest(const std::string& name, const std::string& id) const {
  (void)name;
  // Move to the Drive trash instead of deleting for good, so a mistake can be undone from Drive.
  HttpRequest q = webRequest("PATCH", std::string(kApi) + "/files/" + urlEncode(id) + "?fields=id");
  q.contentType = "application/json";
  q.body = "{\"trashed\":true}";
  return q;
}

}  // namespace msync
