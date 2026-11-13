#include "KimonoAccount.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Logging.h>
#include <esp_random.h>
#include <time.h>

#include <cstring>
#ifndef SIMULATOR
#include <Preferences.h>
#else
#include <HalStorage.h>
#endif

#include "util/PluginHttp.h"

namespace kimono {
namespace {
#ifdef SIMULATOR
// The desktop simulator has no NVS: the same keys in a JSON file on its card.
class Preferences {
 public:
  bool begin(const char*, bool readOnly) {
    readOnly_ = readOnly;
    std::string raw;
    if (Storage.exists(PATH)) Storage.readFileToString("KIMONO", PATH, 8192, raw);
    if (deserializeJson(doc_, raw)) doc_.to<JsonObject>();
    return true;
  }
  String getString(const char* key, const char* fallback) { return String(doc_[key] | fallback); }
  size_t putString(const char* key, const char* value) {
    doc_[key] = value;
    return strlen(value);
  }
  void remove(const char* key) { doc_.remove(key); }
  void end() {
    if (readOnly_) return;
    Storage.mkdir("/.crosspoint/kimono");
    String out;
    serializeJson(doc_, out);
    Storage.writeFile(PATH, out);
  }

 private:
  static constexpr const char* PATH = "/.crosspoint/kimono/account.json";
  JsonDocument doc_;
  bool readOnly_ = true;
};
#endif
constexpr const char* NVS_NAMESPACE = "kimono";
constexpr size_t MAX_PAIR_RESPONSE = 4 * 1024;

std::string trimSlashes(std::string url) {
  while (!url.empty() && url.back() == '/') url.pop_back();
  return url;
}

pluginhttp::Headers jsonHeaders() {
  return {{"Content-Type", "application/json"}, {"Accept", "application/json"}, {"X-Kimono-Device", Account::deviceHeader()}};
}

std::string randomSecret() {
  char out[33];
  for (int i = 0; i < 4; ++i) snprintf(out + i * 8, 9, "%08lx", static_cast<unsigned long>(esp_random()));
  return out;
}

// A clock set from the network or the RTC; before that, time() counts from 1970.
bool clockKnown() { return time(nullptr) > 1700000000; }
}  // namespace

Account& Account::instance() {
  static Account account;
  return account;
}

const char* Account::deviceId() {
  static char id[13] = "";
  if (!id[0]) {
#ifndef SIMULATOR
    const uint64_t mac = ESP.getEfuseMac();
#else
    const uint64_t mac = 0x5ced5ced5cedULL;
#endif
    // The MAC's bytes in their usual order (the eFuse word holds them reversed).
    snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x", static_cast<unsigned>(mac & 0xff),
             static_cast<unsigned>((mac >> 8) & 0xff), static_cast<unsigned>((mac >> 16) & 0xff),
             static_cast<unsigned>((mac >> 24) & 0xff), static_cast<unsigned>((mac >> 32) & 0xff),
             static_cast<unsigned>((mac >> 40) & 0xff));
  }
  return id;
}

const char* Account::hotspotName() {
  static char name[24] = "";
  if (!name[0]) {
    const char* id = deviceId();
    snprintf(name, sizeof(name), "Kimono Read-%c%c%c%c", toupper(id[8]), toupper(id[9]), toupper(id[10]),
             toupper(id[11]));
  }
  return name;
}

std::string Account::deviceHeader() { return std::string("kimono-read/") + model() + "; id=" + deviceId(); }

void Account::load() {
  if (loaded_) return;
  loaded_ = true;
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return;  // nothing saved yet
  server_ = prefs.getString("server", "").c_str();
  token_ = prefs.getString("token", "").c_str();
  username_ = prefs.getString("user", "").c_str();
  lastSync_ = atoll(prefs.getString("synced", "0").c_str());
  prefs.end();
}

bool Account::save() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {
    LOG_ERR("KIMONO", "Cannot open NVS");
    return false;
  }
  bool ok = prefs.putString("server", server_.c_str()) == server_.size();
  if (token_.empty()) {
    prefs.remove("token");
  } else {
    ok = prefs.putString("token", token_.c_str()) == token_.size() && ok;
  }
  prefs.putString("user", username_.c_str());
  char synced[24];
  snprintf(synced, sizeof(synced), "%lld", static_cast<long long>(lastSync_));
  prefs.putString("synced", synced);
  prefs.end();
  if (!ok) LOG_ERR("KIMONO", "Could not save the pairing to NVS");
  return ok;
}

bool Account::setServer(const std::string& typed) {
  std::string url;
  for (const char c : typed)
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') url += c;
  if (url.empty() || url.size() > 200) return false;
  if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) url = "https://" + url;
  url = trimSlashes(url);
  if (url == server_) return true;
  // A different Kimono: a pairing with the old one means nothing there.
  server_ = url;
  token_.clear();
  username_.clear();
  lastSync_ = 0;
  return save();
}

void Account::unpair() {
  token_.clear();
  username_.clear();
  lastSync_ = 0;
  save();
}

void Account::noteSynced() {
  if (!clockKnown()) return;
  const int64_t now = time(nullptr);
  // "Synced 2 min ago" needs minutes, not every call: spare the flash.
  if (now - lastSync_ < 60) return;
  lastSync_ = now;
  save();
}

Result Account::takeToken(const int status, const String& body) {
  if (status < 0) return Result::Network;
  if (status == 202) return Result::Pending;
  if (status == 404 || status == 400) return Result::Denied;
  if (status != 200) return Result::Server;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return Result::Server;
  const std::string token = doc["token"] | "";
  if (token.rfind("kr_", 0) != 0) return Result::Server;
  token_ = token;
  username_ = doc["username"] | "";
  return save() ? Result::Ok : Result::Server;
}

Result Account::redeemTicket(freeink::SecureHttpClient* session, const std::string& ticket) {
  JsonDocument payload;
  payload["ticket"] = ticket;
  payload["deviceId"] = deviceId();
  payload["model"] = model();
  String body, reply;
  serializeJson(payload, body);
  const int status = pluginhttp::request(session, server_ + "/api/v1/devices/tickets/redeem", "POST", body.c_str(),
                                         jsonHeaders(), reply, MAX_PAIR_RESPONSE);
  LOG_INF("KIMONO", "Ticket: HTTP %d", status);
  return takeToken(status, reply);
}

Result Account::startPairing(freeink::SecureHttpClient* session, Pairing& out) {
  out = {};
  out.secret = randomSecret();
  JsonDocument payload;
  payload["deviceId"] = deviceId();
  payload["model"] = model();
  payload["secret"] = out.secret;
  String body, reply;
  serializeJson(payload, body);
  const int status = pluginhttp::request(session, server_ + "/api/v1/devices/pair/start", "POST", body.c_str(),
                                         jsonHeaders(), reply, MAX_PAIR_RESPONSE);
  if (status < 0) return Result::Network;
  if (status != 201) {
    LOG_ERR("KIMONO", "Pairing refused: HTTP %d", status);
    return status == 404 ? Result::Denied : Result::Server;
  }
  JsonDocument doc;
  if (deserializeJson(doc, reply)) return Result::Server;
  out.code = doc["code"] | "";
  out.url = doc["url"] | "";
  const long interval = doc["interval"] | 3L;
  const long expires = doc["expiresIn"] | 600L;
  out.intervalMs = static_cast<uint32_t>(interval < 2 ? 2 : interval) * 1000UL;
  out.expiresAtMs = millis() + static_cast<uint32_t>(expires) * 1000UL;
  return out.code.empty() || out.url.empty() ? Result::Server : Result::Ok;
}

Result Account::claimPairing(freeink::SecureHttpClient* session, const Pairing& pairing) {
  JsonDocument payload;
  payload["deviceId"] = deviceId();
  payload["secret"] = pairing.secret;
  String body, reply;
  serializeJson(payload, body);
  const int status = pluginhttp::request(session, server_ + "/api/v1/devices/pair/claim", "POST", body.c_str(),
                                         jsonHeaders(), reply, MAX_PAIR_RESPONSE);
  return takeToken(status, reply);
}

int Account::api(freeink::SecureHttpClient* session, const char* method, const std::string& path,
                 const std::string& body, String& out, const size_t maxResponse, Result* result) {
  load();
  if (token_.empty()) {
    if (result) *result = Result::Denied;
    return -1;
  }
  pluginhttp::Headers headers = {
      {"Authorization", bearer()}, {"Accept", "application/json"}, {"X-Kimono-Device", deviceHeader()}};
  if (!body.empty()) headers.emplace_back("Content-Type", "application/json");
  const int status = pluginhttp::request(session, server_ + path, method, body, headers, out, maxResponse);
  if (status == 401) {
    // Unpaired from the phone (or signed out everywhere): forget the token.
    LOG_ERR("KIMONO", "Device token refused; unpairing");
    unpair();
  } else if (status >= 200 && status < 300) {
    noteSynced();
  }
  if (result) *result = status < 0 ? Result::Network : (status == 401 ? Result::Denied : Result::Ok);
  return status;
}

}  // namespace kimono
