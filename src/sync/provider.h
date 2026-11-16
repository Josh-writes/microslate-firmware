#pragma once
// One cloud storage service (OneDrive now; Google Drive and Dropbox next) as the sync engine sees it: it builds every
// request and reads every reply. Pure functions, no I/O, so each service is tested on a PC against a fake cloud.
// The engine adds the bearer token and does the transfers; a provider never touches the network, the SD card or
// the tokens.

#include "sync_http.h"
#include "sync_types.h"

#include <string>
#include <vector>

namespace msync {

struct DeviceCode {
  bool ok = false;
  std::string deviceCode, userCode, verificationUri, error;
  int expiresIn = 0, interval = 0;
};

struct TokenReply {
  bool ok = false;
  std::string access, refresh, error, errorDesc;
  int expiresIn = 0;
};

class Provider {
 public:
  virtual ~Provider() {}

  virtual const char* id() const = 0;    // short stable key ("onedrive"): names this service's tokens and sync state
  virtual const char* name() const = 0;  // shown to the user ("OneDrive")

  // Sign-in (device code) and tokens.
  virtual HttpRequest deviceCodeRequest(const std::string& clientId) const = 0;
  virtual DeviceCode parseDeviceCode(int status, const std::string& body) const = 0;
  virtual HttpRequest devicePollRequest(const std::string& clientId, const std::string& deviceCode) const = 0;
  virtual HttpRequest refreshRequest(const std::string& clientId, const std::string& refreshToken) const = 0;
  virtual TokenReply parseToken(int status, const std::string& body) const = 0;
  virtual HttpRequest accountRequest() const = 0;
  virtual std::string parseAccount(const std::string& body) const = 0;  // display name, or empty

  // Sign-in with a code the user types (Dropbox) instead of a device code. The engine makes a random `verifier`, shows
  // authorizeUrl() as a QR code, and sends the code the user typed to codeExchangeRequest(); the reply is read by
  // parseToken(). Providers that use the device code flow leave these alone.
  virtual bool usesCodeEntry() const { return false; }
  virtual std::string authorizeUrl(const std::string& clientId, const std::string& verifier) const {
    (void)clientId; (void)verifier;
    return "";
  }
  // viaRelay: the code came from the relay flow, which has a redirect address the token request must repeat.
  virtual HttpRequest codeExchangeRequest(const std::string& clientId, const std::string& code,
                                          const std::string& verifier, bool viaRelay) const {
    (void)clientId; (void)code; (void)verifier; (void)viaRelay;
    return HttpRequest();
  }
  // Optional relay (a small web service the project runs): the QR code opens a short address there, and the device
  // collects the one-time code by itself, so the user types nothing. Providers without one leave these alone.
  virtual bool hasRelay() const { return false; }
  virtual std::string relayAuthorizeUrl(const std::string& session, const std::string& verifier) const {
    (void)session; (void)verifier;
    return "";
  }
  virtual HttpRequest relayPollRequest(const std::string& session) const { (void)session; return HttpRequest(); }
  virtual bool parseRelayPoll(int status, const std::string& body, std::string& code) const {
    (void)status; (void)body; (void)code;
    return false;   // false: nothing there yet
  }

  // One-time preparation before the first listing of a sync (Google Drive: find or create the MicroSlate folder).
  // The engine asks for step 0, 1, 2... until setupRequest returns false; setupReply returns false to stop the sync
  // (with `error` in plain words). The default does nothing.
  virtual bool setupRequest(int step, HttpRequest& out) { (void)step; (void)out; return false; }
  virtual bool setupReply(int step, int status, const std::string& body, std::string& error) {
    (void)step; (void)status; (void)body; (void)error;
    return true;
  }
  // Signing out or signing in as someone else: drop anything learned about the old account (a cached folder id).
  virtual void forgetSession() {}

  // The notes folder in the cloud. `id` is what the last listing returned for the note ("" when unknown).
  virtual HttpRequest listRequest(const std::string& pageLink) const = 0;  // empty link: first page
  virtual bool parseList(const std::string& body, std::vector<RemoteFile>& files, std::string& nextLink,
                         int& skipped) const = 0;
  virtual bool pagingLinkAllowed(const std::string& url) const = 0;  // the token is only ever sent to the service itself
  virtual HttpRequest downloadRequest(const std::string& name, const std::string& id) const = 0;
  virtual bool redirectAllowed(const std::string& url) const = 0;    // download links are fetched without the token
  // `modified`: when the note was written, in seconds since 1970 (0 when the date is not known). The cloud copy, the
  // PC copy and Scrivener's External Folder Sync go by that date, so it must be today's, never an older one.
  virtual HttpRequest uploadRequest(const std::string& name, const std::string& id, std::string body,
                                    uint32_t modified) const = 0;
  virtual bool parseUpload(const std::string& body, RemoteFile& f) const = 0;
  // A second request that sets the modified date, for a service whose upload cannot carry it (OneDrive keeps the
  // date the file had before). Its answer is parsed like an upload's. False: nothing more to send.
  virtual bool dateRequest(const std::string& name, const RemoteFile& uploaded, uint32_t modified,
                           HttpRequest& out) const {
    (void)name; (void)uploaded; (void)modified; (void)out;
    return false;
  }
  virtual HttpRequest deleteRequest(const std::string& name, const std::string& id) const = 0;
};

}  // namespace msync
