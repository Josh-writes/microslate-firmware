#pragma once
// Device side of cloud sync (ESP32): SD storage, NVS token store, clock and the HTTPS worker task that
// implement the engine's interfaces. Pure engine code lives in sync_*.cpp and onedrive.cpp.

#include "sync_http.h"
#include "sync_interfaces.h"

namespace msync {

// OneDrive app registration (public client, no secret).
extern const char* const kOneDriveClientId;

class DeviceClock : public Clock {
 public:
  uint32_t millisNow() override;
  uint32_t epochNow() override;  // 0 until the date is known (RTC or NTP)
  uint32_t random32() override;  // hardware random generator
};

// The refresh token lives in NVS only. It is never written to the SD card or to any JSON backup.
// Each provider has its own key. OneDrive keeps the key it always had, so signing in again is never needed.
class DeviceTokens : public TokenStore {
 public:
  explicit DeviceTokens(const char* providerId = "onedrive");
  std::string load() override;
  bool save(const std::string& refresh) override;
  void clear() override;

 private:
  std::string key_;
};

// /notes on the SD card. Each provider has its own sync record: OneDrive keeps /microslate/sync/state.json,
// the others use /microslate/sync/<id>/state.json, so switching provider can never look like a mass delete.
class DeviceStorage : public SyncStorage {
 public:
  explicit DeviceStorage(const char* providerId = "onedrive");
  bool listNotes(std::vector<std::string>& names) override;
  bool readNote(const std::string& name, std::string& out, size_t maxBytes) override;
  bool noteExists(const std::string& name) override;
  bool writeNote(const std::string& name, const std::string& data) override;
  bool trashNote(const std::string& name) override;
  bool readState(std::string& out) override;
  bool writeState(const std::string& data) override;
  bool isOpenNote(const std::string& name) override;

 private:
  std::string stateDir_, statePath_;
};

// HTTPS client on its own task. It only handles memory buffers: it never touches the SD card, so the SD/display
// SPI bus stays with the main loop. Keeps the connection to a host alive between requests.
class DeviceTransport : public Transport {
 public:
  DeviceTransport();
  bool begin(const HttpRequest& req) override;
  bool poll(HttpResponse& out) override;
  void cancel() override;
  // Memory policy for TLS: while a sync runs, allocations of 256 B and up (and all of mbedTLS) come from PSRAM.
  static void psramBegin();
  static void psramEnd();
  // Drop the kept-alive TLS connection (frees its memory). Call when no request is in flight.
  static void closeConnection();
};

}  // namespace msync
