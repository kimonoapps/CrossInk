#include "KimonoSetupActivity.h"

#include <DNSServer.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <WebServer.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <vector>

#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

namespace {
constexpr uint8_t DNS_PORT = 53;
constexpr uint32_t JOIN_TIMEOUT_MS = 20000;
constexpr uint32_t SUBMIT_SETTLE_MS = 1500;  // let the phone receive "Done" before the hotspot goes
constexpr size_t MAX_NETWORKS = 12;

std::string htmlEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (const char c : in) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

// The page is small and self-contained: the phone has no internet while it is
// on the reader's hotspot.
constexpr const char* PAGE_HEAD = R"(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Set up Kimono Read</title><style>
body{font:17px/1.45 -apple-system,system-ui,sans-serif;margin:0;padding:24px 18px;background:#f7f1ec;color:#2b2422}
h1{font-size:24px;margin:0 0 4px}p{color:#6b5f5a;margin:0 0 18px}label{display:block;font-weight:600;margin:14px 0 6px}
select,input{width:100%;box-sizing:border-box;font:inherit;padding:12px;border:1px solid #cbbdb5;border-radius:12px;background:#fff}
button{margin-top:22px;width:100%;padding:14px;border:0;border-radius:999px;background:#a84d63;color:#fff;font:600 17px system-ui}
.err{background:#fbe4e4;color:#8a2b2b;padding:10px 12px;border-radius:10px}.hint{font-size:14px;margin-top:6px}
</style></head><body>)";
}  // namespace

KimonoSetupActivity::KimonoSetupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                         const bool skipHotspot)
    : Activity("KimonoSetup", renderer, mappedInput), skipHotspot(skipHotspot) {}

KimonoSetupActivity::~KimonoSetupActivity() = default;

void KimonoSetupActivity::onEnter() {
  Activity::onEnter();
  KIMONO_ACCOUNT.load();
  if (skipHotspot) {
    // Online already, with an address: only the pairing is missing.
    beginPairing();
    return;
  }
  startHotspot();
}

void KimonoSetupActivity::onExit() {
  stopHotspot();
  session.reset();
  Activity::onExit();
}

void KimonoSetupActivity::startHotspot() {
  state = State::Starting;
  requestUpdateAndWait();
  // Scan first, so the page can offer the networks around (the phone can't
  // tell a web page which network it was on).
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  const int found = WiFi.scanNetworks();
  std::vector<std::pair<int, std::string>> seen;
  for (int i = 0; i < found; ++i) {
    std::string name = WiFi.SSID(i).c_str();
    if (name.empty()) continue;
    auto same = std::find_if(seen.begin(), seen.end(), [&](const auto& s) { return s.second == name; });
    if (same == seen.end())
      seen.emplace_back(WiFi.RSSI(i), name);
    else
      same->first = std::max(same->first, static_cast<int>(WiFi.RSSI(i)));
  }
  WiFi.scanDelete();
  std::sort(seen.begin(), seen.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  if (seen.size() > MAX_NETWORKS) seen.resize(MAX_NETWORKS);
  networksHtml.clear();
  for (const auto& network : seen) {
    const std::string name = htmlEscape(network.second);
    networksHtml += "<option value=\"" + name + "\">" + name + "</option>";
  }

  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(kimono::Account::hotspotName())) {
    fail(tr(STR_KIMONO_SETUP_FAILED));
    return;
  }
  delay(100);
  dns = makeUniqueNoThrow<DNSServer>();
  web = makeUniqueNoThrow<WebServer>(80);
  if (!dns || !web) {
    LOG_ERR("KIMONO", "OOM: setup hotspot");
    fail(tr(STR_MEMORY_ERROR));
    return;
  }
  // Every name answers with the reader, so phones show the setup page as a sign-in sheet.
  dns->setErrorReplyCode(DNSReplyCode::NoError);
  dns->start(DNS_PORT, "*", WiFi.softAPIP());
  web->on("/kimono", HTTP_GET, [this] { serveSetupPage(); });
  web->on("/kimono", HTTP_POST, [this] { receiveSetup(); });
  web->onNotFound([this] {
    web->sendHeader("Location", "http://192.168.4.1/kimono", true);
    web->send(302, "text/plain", "");
  });
  web->begin();
  LOG_INF("KIMONO", "Setup hotspot %s up, %u networks", kimono::Account::hotspotName(),
          static_cast<unsigned>(seen.size()));
  state = State::Hotspot;
  requestUpdate();
}

void KimonoSetupActivity::stopHotspot() {
  if (web) {
    web->stop();
    web.reset();
  }
  if (dns) {
    dns->stop();
    dns.reset();
  }
  if (WiFi.getMode() & WIFI_AP) WiFi.softAPdisconnect(true);
}

void KimonoSetupActivity::serveSetupPage() {
  // Filled in by the phone's Kimono (Account > Set up Kimono Read).
  if (web->hasArg("server") && !web->arg("server").isEmpty()) server = web->arg("server").c_str();
  if (web->hasArg("ticket") && !web->arg("ticket").isEmpty()) ticket = web->arg("ticket").c_str();
  if (server.empty() && KIMONO_ACCOUNT.hasServer()) server = KIMONO_ACCOUNT.server();

  std::string page = PAGE_HEAD;
  page += "<h1>Set up Kimono Read</h1><p>Pick your Wi-Fi. The reader joins it and pairs itself with your Kimono.</p>";
  if (!message.empty()) page += "<p class=err>" + htmlEscape(message) + "</p>";
  page += "<form method=post action=/kimono><label for=s>Wi-Fi</label><select id=s name=ssid>" + networksHtml +
          "<option value=\"\">Another network…</option></select>"
          "<input name=other placeholder=\"Network name\" style=\"margin-top:8px\" autocapitalize=off>"
          "<label for=p>Password</label><input id=p name=password type=password autocomplete=current-password>";
  if (server.empty()) {
    page +=
        "<label for=k>Your Kimono</label><input id=k name=server placeholder=\"kimono.example.com\" "
        "autocapitalize=off autocorrect=off inputmode=url required>"
        "<p class=hint>Its address, as in your browser. Next time, start from Kimono on your phone "
        "(You › Set up Kimono Read) and this is filled in.</p>";
  } else {
    page += "<input type=hidden name=server value=\"" + htmlEscape(server) + "\"><p class=hint>Kimono: " +
            htmlEscape(server) + "</p>";
  }
  page += "<input type=hidden name=ticket value=\"" + htmlEscape(ticket) + "\"><button>Connect</button></form></body></html>";
  web->send(200, "text/html; charset=utf-8", page.c_str());
}

void KimonoSetupActivity::receiveSetup() {
  std::string name = web->arg("ssid").c_str();
  if (name.empty()) name = web->arg("other").c_str();
  const std::string sentServer = web->arg("server").c_str();
  if (name.empty() || name.size() > 32 || web->arg("password").length() > 64 || sentServer.empty()) {
    message = "Pick a Wi-Fi network and give your Kimono's address.";
    serveSetupPage();
    return;
  }
  ssid = name;
  password = web->arg("password").c_str();
  server = sentServer;
  if (!web->arg("ticket").isEmpty()) ticket = web->arg("ticket").c_str();
  std::string page = PAGE_HEAD;
  page += "<h1>Done</h1><p>The reader is joining " + htmlEscape(ssid) +
          ". Switch your phone back to your usual Wi-Fi and look at the reader.</p></body></html>";
  web->send(200, "text/html; charset=utf-8", page.c_str());
  message.clear();
  submitted = true;
  submittedAtMs = millis();
}

void KimonoSetupActivity::joinAndPair() {
  state = State::Joining;
  requestUpdateAndWait();
  stopHotspot();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < JOIN_TIMEOUT_MS) delay(100);
  if (WiFi.status() != WL_CONNECTED) {
    LOG_ERR("KIMONO", "Could not join %s", ssid.c_str());
    WiFi.disconnect(false);
    message = "The reader couldn't join " + ssid + ". Check the password and try again.";
    startHotspot();
    return;
  }
  WIFI_STORE.addCredential(ssid, password);
  WIFI_STORE.setLastConnectedSsid(ssid);
  password.clear();
  if (!KIMONO_ACCOUNT.setServer(server)) {
    fail(tr(STR_KIMONO_REQUEST_FAILED));
    return;
  }
  if (!ticket.empty()) {
    session = makeUniqueNoThrow<freeink::SecureHttpClient>();
    const auto result = KIMONO_ACCOUNT.redeemTicket(session.get(), ticket);
    ticket.clear();
    if (result == kimono::Result::Ok) {
      state = State::Paired;
      requestUpdate();
      return;
    }
    LOG_ERR("KIMONO", "Setup ticket not accepted; pairing with a code instead");
  }
  beginPairing();
}

void KimonoSetupActivity::beginPairing() {
  if (!session) session = makeUniqueNoThrow<freeink::SecureHttpClient>();
  const auto result = KIMONO_ACCOUNT.startPairing(session.get(), pairing);
  if (result != kimono::Result::Ok) {
    fail(result == kimono::Result::Network ? tr(STR_KIMONO_REQUEST_FAILED) : tr(STR_KIMONO_SETUP_FAILED));
    return;
  }
  nextPollMs = millis() + pairing.intervalMs;
  state = State::Pairing;
  requestUpdate();
}

void KimonoSetupActivity::pollPairing() {
  if (static_cast<int32_t>(millis() - pairing.expiresAtMs) >= 0) {
    beginPairing();  // a fresh code; the old one ran out
    return;
  }
  nextPollMs = millis() + pairing.intervalMs;
  switch (KIMONO_ACCOUNT.claimPairing(session.get(), pairing)) {
    case kimono::Result::Ok:
      state = State::Paired;
      requestUpdate();
      return;
    case kimono::Result::Denied:
      beginPairing();
      return;
    default:
      return;  // not yet, or no answer: keep waiting
  }
}

void KimonoSetupActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
}

void KimonoSetupActivity::fail(const char* text) {
  stopHotspot();
  message = text;
  state = State::Failed;
  requestUpdate();
}

void KimonoSetupActivity::loop() {
  if (dns) dns->processNextRequest();
  if (web) web->handleClient();

  const bool back = mappedInput.wasReleased(MappedInputManager::Button::Back) ||
                    TouchHeaderBackButton::wasTapped(mappedInput, renderer);
  switch (state) {
    case State::Hotspot:
      if (submitted && millis() - submittedAtMs >= SUBMIT_SETTLE_MS) {
        submitted = false;
        joinAndPair();
        return;
      }
      if (back) {
        cancel();
        finish();
      }
      return;
    case State::Pairing:
      if (back) {
        cancel();
        finish();
        return;
      }
      if (static_cast<int32_t>(millis() - nextPollMs) >= 0) pollPairing();
      return;
    case State::Paired:
      // A moment to read "Paired", then on to the shelf.
      finish();
      return;
    case State::Failed:
      if (back) {
        cancel();
        finish();
      } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        message.clear();
        startHotspot();
      }
      return;
    default:
      return;
  }
}

void KimonoSetupActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware())
    TouchHeaderBackButton::draw(renderer, header, tr(STR_KIMONO), false);
  else
    GUI.drawHeader(renderer, header, tr(STR_KIMONO));

  const int lh = renderer.getLineHeight(UI_10_FONT_ID);
  const int big = renderer.getLineHeight(UI_12_FONT_ID);
  const Rect text{safe.x + metrics.contentSidePadding, safe.y, safe.width - metrics.contentSidePadding * 2,
                  safe.height};
  int y = TouchHeaderBackButton::contentTop(renderer, mappedInput) + metrics.verticalSpacing * 2;
  const int qrSize = std::min(safe.width - 80, 300);
  const Rect qr{safe.x + (safe.width - qrSize) / 2, 0, qrSize, qrSize};

  switch (state) {
    case State::Starting:
    case State::Joining:
      renderer.drawCenteredText(UI_12_FONT_ID, safe.y + safe.height / 2 - big,
                                state == State::Starting ? tr(STR_KIMONO_SETUP_STARTING) : tr(STR_KIMONO_JOINING),
                                true, EpdFontFamily::BOLD);
      if (state == State::Joining) renderer.drawCenteredText(UI_10_FONT_ID, safe.y + safe.height / 2 + 10, ssid.c_str());
      break;
    case State::Hotspot: {
      renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_KIMONO_SETUP_TITLE), true, EpdFontFamily::BOLD);
      y += big + metrics.verticalSpacing;
      y = UITheme::drawCenteredWrappedText(renderer, text, UI_10_FONT_ID, y, tr(STR_KIMONO_SETUP_SCAN), 3, true,
                                           EpdFontFamily::REGULAR, 4) +
          metrics.verticalSpacing;
      char join[64];
      snprintf(join, sizeof(join), "WIFI:T:nopass;S:%s;;", kimono::Account::hotspotName());
      QrUtils::drawQrCode(renderer, Rect{qr.x, y, qr.width, qr.height}, join);
      y += qrSize + metrics.verticalSpacing;
      renderer.drawCenteredText(UI_12_FONT_ID, y, kimono::Account::hotspotName(), true, EpdFontFamily::BOLD);
      y += big + metrics.verticalSpacing;
      UITheme::drawCenteredWrappedText(renderer, text, UI_10_FONT_ID, y,
                                       message.empty() ? tr(STR_KIMONO_SETUP_PHONE) : message.c_str(), 4, true,
                                       EpdFontFamily::REGULAR, 4);
      break;
    }
    case State::Pairing: {
      renderer.drawCenteredText(UI_12_FONT_ID, y, tr(STR_KIMONO_PAIR_TITLE), true, EpdFontFamily::BOLD);
      y += big + metrics.verticalSpacing;
      y = UITheme::drawCenteredWrappedText(renderer, text, UI_10_FONT_ID, y, tr(STR_KIMONO_PAIR_SCAN), 3, true,
                                           EpdFontFamily::REGULAR, 4) +
          metrics.verticalSpacing;
      QrUtils::drawQrCode(renderer, Rect{qr.x, y, qr.width, qr.height}, pairing.url);
      y += qrSize + metrics.verticalSpacing;
      std::string spaced;
      for (const char c : pairing.code) {
        if (!spaced.empty()) spaced += ' ';
        spaced += c;
      }
      renderer.drawCenteredText(UI_12_FONT_ID, y, spaced.c_str(), true, EpdFontFamily::BOLD);
      y += big + metrics.verticalSpacing;
      UITheme::drawCenteredWrappedText(renderer, text, UI_10_FONT_ID, y, KIMONO_ACCOUNT.server().c_str(), 2, true,
                                       EpdFontFamily::REGULAR, 4);
      break;
    }
    case State::Paired:
      renderer.drawCenteredText(UI_12_FONT_ID, safe.y + safe.height / 2 - big, tr(STR_KIMONO_PAIRED), true,
                                EpdFontFamily::BOLD);
      break;
    case State::Failed:
      renderer.drawCenteredText(UI_12_FONT_ID, safe.y + safe.height / 2 - big * 2, tr(STR_ERROR_MSG), true,
                                EpdFontFamily::BOLD);
      UITheme::drawCenteredWrappedText(renderer, text, UI_10_FONT_ID, safe.y + safe.height / 2, message.c_str(), 4,
                                       true, EpdFontFamily::REGULAR, 4);
      break;
  }
  (void)lh;
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)),
                                            state == State::Failed ? tr(STR_RETRY) : "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
