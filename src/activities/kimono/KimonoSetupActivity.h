#pragma once

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "kimono/KimonoAccount.h"

class DNSServer;
class WebServer;
namespace freeink {
class SecureHttpClient;
}

// Setting up Kimono Read with nothing typed on the reader.
//
// The reader opens a hotspot ("Kimono Read-1A2B", no password) with a captive
// setup page. The phone joins it (the QR code on screen joins it in one scan),
// and the page asks there, on the phone, for the home Wi-Fi and its password,
// and for the Kimono address unless the phone's Kimono already filled it in
// (Account > Set up Kimono Read opens the page with the address and a one-time
// ticket). The reader then joins that Wi-Fi and pairs itself: with the ticket
// at once, or else by showing a code and QR code that the person approves on
// their phone. It also starts straight at that last step when it already has
// Wi-Fi and an address but no pairing.
class KimonoSetupActivity final : public Activity {
 public:
  explicit KimonoSetupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool skipHotspot = false);
  ~KimonoSetupActivity() override;

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return state == State::Hotspot; }

 private:
  enum class State : uint8_t { Starting, Hotspot, Joining, Pairing, Paired, Failed };

  State state = State::Starting;
  const bool skipHotspot;
  std::unique_ptr<DNSServer> dns;
  std::unique_ptr<WebServer> web;
  std::unique_ptr<freeink::SecureHttpClient> session;
  std::string networksHtml;  // <option>s from the scan, built once
  std::string message;       // shown on screen and on the setup page

  // What the phone sent.
  std::string ssid, password, server, ticket;
  bool submitted = false;
  uint32_t submittedAtMs = 0;

  kimono::Pairing pairing;
  uint32_t nextPollMs = 0;

  void startHotspot();
  void stopHotspot();
  void serveSetupPage();
  void receiveSetup();
  void joinAndPair();
  void beginPairing();
  void pollPairing();
  void fail(const char* text);
  void cancel();
};
