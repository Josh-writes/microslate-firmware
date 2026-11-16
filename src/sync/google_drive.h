#pragma once
// Google Drive (Drive API v3, scope drive.file) as a Provider. Pure functions plus one cached folder id.
//
// drive.file means the app only sees files it created itself: notes made on MicroSlate devices (all devices share one
// Google app, so they see each other's), and notes edited from a phone or PC stay visible. A brand-new note created
// on a PC inside the folder is invisible to the device.
//
// Notes live in a folder called "MicroSlate" in My Drive that the app finds or creates. Files are addressed by id.

#include "provider.h"

#include <string>
#include <vector>

namespace msync {

class GoogleDriveProvider : public Provider {
 public:
  // The client secret of a "TVs and Limited Input devices" client is not confidential (Google's own wording); it is
  // still kept out of the public repository (src/sync/secrets.local.h).
  explicit GoogleDriveProvider(const std::string& clientSecret) : secret_(clientSecret) {}

  const char* id() const override { return "gdrive"; }
  const char* name() const override { return "Google Drive"; }

  HttpRequest deviceCodeRequest(const std::string& clientId) const override;
  DeviceCode parseDeviceCode(int status, const std::string& body) const override;
  HttpRequest devicePollRequest(const std::string& clientId, const std::string& deviceCode) const override;
  HttpRequest refreshRequest(const std::string& clientId, const std::string& refreshToken) const override;
  TokenReply parseToken(int status, const std::string& body) const override;
  HttpRequest accountRequest() const override;
  std::string parseAccount(const std::string& body) const override;

  bool setupRequest(int step, HttpRequest& out) override;
  bool setupReply(int step, int status, const std::string& body, std::string& error) override;
  void forgetSession() override { folderId_.clear(); }

  HttpRequest listRequest(const std::string& pageLink) const override;
  bool parseList(const std::string& body, std::vector<RemoteFile>& files, std::string& nextLink,
                 int& skipped) const override;
  bool pagingLinkAllowed(const std::string& url) const override;
  HttpRequest downloadRequest(const std::string& name, const std::string& id) const override;
  bool redirectAllowed(const std::string& url) const override;
  HttpRequest uploadRequest(const std::string& name, const std::string& id, std::string body, uint32_t modified) const override;
  bool parseUpload(const std::string& body, RemoteFile& f) const override;
  HttpRequest deleteRequest(const std::string& name, const std::string& id) const override;

 private:
  std::string listUrl(const std::string& pageToken) const;
  std::string secret_;
  std::string folderId_;   // the MicroSlate folder, found or created at the start of each run
};

}  // namespace msync
