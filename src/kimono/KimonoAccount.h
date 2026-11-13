#pragma once

#include <WString.h>

#include <cstdint>
#include <string>

namespace freeink {
class SecureHttpClient;
}

// Kimono Read: this reader's pairing with a person's Kimono.
//
// Nothing is typed on the reader. It learns its Kimono's address and its Wi-Fi
// from a page it serves on its own hotspot (KimonoSetupActivity), filled in on
// the phone, and is then paired one of two ways:
//   - with a one-time setup ticket the phone handed over (redeemTicket), or
//   - by showing a pairing code / QR code that the person approves on their
//     phone, while the reader polls for its token (startPairing, claimPairing).
// The result is a long-lived device token from the Portal, kept in NVS with the
// server address. Every Kimono call carries it, and the
// `X-Kimono-Device: kimono-read/<model>; id=<eFuse MAC>` header the profile's
// Kimono Read badge comes from.
namespace kimono {

enum class Result : uint8_t {
  Ok,
  Pending,  // the pairing hasn't been approved yet
  Denied,   // unpaired, revoked, or the code/ticket ran out
  Network,  // no answer
  Server,   // an answer that isn't what Kimono sends
};

struct Pairing {
  std::string code;  // six characters, shown large
  std::string url;   // the page that pairs it (QR code)
  std::string secret;
  uint32_t intervalMs = 3000;
  uint32_t expiresAtMs = 0;
};

class Account {
 public:
  static Account& instance();

  void load();
  bool hasServer() const { return !server_.empty(); }
  bool paired() const { return !token_.empty(); }
  const std::string& server() const { return server_; }
  const std::string& username() const { return username_; }
  // "https://kimono.example" from what the phone sent; https:// is assumed when no scheme is given.
  bool setServer(const std::string& typed);
  void unpair();

  // The eFuse MAC as 12 hex digits, and the hotspot's name ("Kimono Read-1A2B").
  static const char* deviceId();
  static const char* hotspotName();
  static const char* model() { return "silkscreen"; }

  Result redeemTicket(freeink::SecureHttpClient* session, const std::string& ticket);
  Result startPairing(freeink::SecureHttpClient* session, Pairing& out);
  Result claimPairing(freeink::SecureHttpClient* session, const Pairing& pairing);

  // One call to the Kimono API (path like "/api/v1/books/shelf"). Returns the
  // HTTP status, or -1 with no answer. A 401 means the reader was unpaired: the
  // token is forgotten and `result` says Denied.
  int api(freeink::SecureHttpClient* session, const char* method, const std::string& path, const std::string& body,
          String& out, size_t maxResponse, Result* result = nullptr);
  // Headers for a download from Kimono (authorization and device).
  std::string bearer() const { return "Bearer " + token_; }
  static std::string deviceHeader();

  // When the shelf or progress last went through, as a Unix time (0 = never, or no clock).
  int64_t lastSync() const { return lastSync_; }
  void noteSynced();

 private:
  Account() = default;
  Result takeToken(int status, const String& body);
  bool save();

  bool loaded_ = false;
  std::string server_;
  std::string token_;
  std::string username_;
  int64_t lastSync_ = 0;
};

}  // namespace kimono

#define KIMONO_ACCOUNT kimono::Account::instance()
