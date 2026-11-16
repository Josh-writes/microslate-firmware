#pragma once
// OneDrive (Microsoft Graph, app folder) request building and reply parsing. Pure functions.

#include "provider.h"
#include "sync_types.h"
#include "sync_web.h"

#include <string>
#include <vector>

namespace msync {
namespace od {

extern const char* const kAuthBase;   // https://login.microsoftonline.com/consumers/oauth2/v2.0
extern const char* const kGraph;      // https://graph.microsoft.com/v1.0
extern const char* const kScope;      // Files.ReadWrite.AppFolder User.Read offline_access

std::string listUrl();                                      // first page of the app folder
std::string contentUrl(const std::string& name);            // GET download (redirects) / PUT upload
std::string itemUrl(const std::string& name);               // DELETE
std::string meUrl();

// Only Graph itself may be followed for paging; the bearer token is never sent anywhere else.
bool graphUrl(const std::string& url);
// Redirect targets must be https (the transport sends no token to them).
bool httpsUrl(const std::string& url);

// Reads one page of the folder listing. Folders, bad names and non-notes are counted in `skipped`.
bool parseList(const std::string& body, std::vector<RemoteFile>& files, std::string& nextLink, int& skipped);
// Reads the item returned by an upload (id, cTag, size).
bool parseItem(const std::string& body, RemoteFile& f);

std::string formDeviceCode(const std::string& clientId);
std::string formDevicePoll(const std::string& clientId, const std::string& deviceCode);
std::string formRefresh(const std::string& clientId, const std::string& refreshToken);

// Account display name from /me, or empty.
std::string parseDisplayName(const std::string& body);

}  // namespace od

// OneDrive (Microsoft Graph, app folder) as a Provider.
class OneDriveProvider : public Provider {
 public:
  // The id and name can be overridden to register a second Microsoft account as its own provider (test builds).
  explicit OneDriveProvider(const char* id = "onedrive", const char* name = "OneDrive") : id_(id), name_(name) {}
  const char* id() const override { return id_.c_str(); }
  const char* name() const override { return name_.c_str(); }
  HttpRequest deviceCodeRequest(const std::string& clientId) const override;
  DeviceCode parseDeviceCode(int status, const std::string& body) const override;
  HttpRequest devicePollRequest(const std::string& clientId, const std::string& deviceCode) const override;
  HttpRequest refreshRequest(const std::string& clientId, const std::string& refreshToken) const override;
  TokenReply parseToken(int status, const std::string& body) const override;
  HttpRequest accountRequest() const override;
  std::string parseAccount(const std::string& body) const override;
  HttpRequest listRequest(const std::string& pageLink) const override;
  bool parseList(const std::string& body, std::vector<RemoteFile>& files, std::string& nextLink,
                 int& skipped) const override;
  bool pagingLinkAllowed(const std::string& url) const override;
  HttpRequest downloadRequest(const std::string& name, const std::string& id) const override;
  bool redirectAllowed(const std::string& url) const override;
  HttpRequest uploadRequest(const std::string& name, const std::string& id, std::string body, uint32_t modified) const override;
  bool dateRequest(const std::string& name, const RemoteFile& uploaded, uint32_t modified,
                   HttpRequest& out) const override;
  bool parseUpload(const std::string& body, RemoteFile& f) const override;
  HttpRequest deleteRequest(const std::string& name, const std::string& id) const override;

 private:
  std::string id_, name_;
};

}  // namespace msync
