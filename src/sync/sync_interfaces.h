#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace msync {

// Local note storage as the engine sees it. Device: SD card /notes. Host tests: an in-memory fake.
// All calls run on the thread that owns SD access (the main loop); the engine never blocks them for long.
class SyncStorage {
 public:
  virtual ~SyncStorage() {}
  virtual bool listNotes(std::vector<std::string>& names) = 0;  // plain file names in the notes folder
  virtual bool readNote(const std::string& name, std::string& out, size_t maxBytes) = 0;  // false: missing or larger
  virtual bool noteExists(const std::string& name) = 0;
  virtual bool writeNote(const std::string& name, const std::string& data) = 0;  // atomic (tmp, verify, rename)
  virtual bool trashNote(const std::string& name) = 0;           // move to the trash folder, never a hard delete
  virtual bool readState(std::string& out) = 0;                  // false: no state file
  virtual bool writeState(const std::string& data) = 0;
  virtual bool isOpenNote(const std::string& name) = 0;          // the editor currently has this note open
};

// The refresh token lives only here (device: NVS, never the SD card).
class TokenStore {
 public:
  virtual ~TokenStore() {}
  virtual std::string load() = 0;                    // empty if none
  virtual bool save(const std::string& refresh) = 0;
  virtual void clear() = 0;
};

class Clock {
 public:
  virtual ~Clock() {}
  virtual uint32_t millisNow() = 0;
  virtual uint32_t epochNow() = 0;  // seconds since 1970; 0 if the date is not known
  // Unpredictable 32 bits for sign-in challenges. The device overrides this with the hardware generator; this default
  // is only good enough for tests.
  virtual uint32_t random32() { return millisNow() * 2654435761u + 12345u; }
};

}  // namespace msync
