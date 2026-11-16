#pragma once
// Dropbox (API v2, "App folder" access) as a Provider. Pure functions; no state.
//
// Sign-in is the authorization code flow with PKCE (no client secret): the device shows a QR code for the Dropbox
// authorization page, the user approves there, Dropbox shows a code, and the user types it on the device.
// Notes live in the app folder (Dropbox/Apps/<app name>) and are addressed by path.

#include "provider.h"

#include <string>
#include <vector>

namespace msync {

class DropboxProvider : public Provider {
 public:
  // relayBase: address of the sign-in relay Worker ("https://....workers.dev"), or empty for typed codes only.
  explicit DropboxProvider(const std::string& relayBase = "") : relay_(relayBase) {}

  const char* id() const override { return "dropbox"; }
  const char* name() const override { return "Dropbox"; }

  HttpRequest deviceCodeRequest(const std::string&) const override { return HttpRequest(); }
  DeviceCode parseDeviceCode(int, const std::string&) const override { return DeviceCode(); }
  HttpRequest devicePollRequest(const std::string&, const std::string&) const override { return HttpRequest(); }
  bool usesCodeEntry() const override { return true; }
  std::string authorizeUrl(const std::string& clientId, const std::string& verifier) const override;
  HttpRequest codeExchangeRequest(const std::string& clientId, const std::string& code, const std::string& verifier,
                                  bool viaRelay) const override;
  bool hasRelay() const override { return !relay_.empty(); }
  std::string relayAuthorizeUrl(const std::string& session, const std::string& verifier) const override;
  HttpRequest relayPollRequest(const std::string& session) const override;
  bool parseRelayPoll(int status, const std::string& body, std::string& code) const override;
  HttpRequest refreshRequest(const std::string& clientId, const std::string& refreshToken) const override;
  TokenReply parseToken(int status, const std::string& body) const override;
  HttpRequest accountRequest() const override;
  std::string parseAccount(const std::string& body) const override;

  HttpRequest listRequest(const std::string& pageLink) const override;
  bool parseList(const std::string& body, std::vector<RemoteFile>& files, std::string& nextLink,
                 int& skipped) const override;
  bool pagingLinkAllowed(const std::string& cursor) const override;
  HttpRequest downloadRequest(const std::string& name, const std::string& id) const override;
  bool redirectAllowed(const std::string&) const override { return false; }
  HttpRequest uploadRequest(const std::string& name, const std::string& id, std::string body, uint32_t modified) const override;
  bool parseUpload(const std::string& body, RemoteFile& f) const override;
  HttpRequest deleteRequest(const std::string& name, const std::string& id) const override;

 private:
  std::string relay_;
};

}  // namespace msync
