#pragma once
// PKCE (RFC 7636) helpers for providers that sign in with a code the user types (Dropbox). Pure C++.

#include <cstddef>
#include <cstdint>
#include <string>

namespace msync {

// SHA-256 of `data`, 32 raw bytes.
std::string sha256(const std::string& data);

// Base64 with the URL-safe alphabet and no padding.
std::string base64Url(const std::string& raw);

// The S256 code challenge for a verifier: base64url(sha256(verifier)).
std::string pkceChallenge(const std::string& verifier);

// `len` characters from `alphabet` (N characters), drawn from `random32`.
template <typename Rng, size_t N>
std::string randomString(Rng&& random32, int len, const char (&alphabet)[N]) {
  std::string v;
  for (int i = 0; i < len; i++) v += alphabet[(random32() >> 8) % (N - 1)];
  return v;
}

// A verifier of `len` (43..128) characters from the unreserved alphabet.
template <typename Rng>
std::string makeVerifier(Rng&& random32, int len = 64) {
  return randomString(random32, len, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~");
}

// A session id for the sign-in relay: letters, digits, '-' and '_' only.
template <typename Rng>
std::string makeSession(Rng&& random32, int len = 24) {
  return randomString(random32, len, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
}

}  // namespace msync
