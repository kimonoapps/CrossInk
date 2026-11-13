#include "KimonoProgress.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

#include "KimonoAccount.h"
#include "WifiCredentialStore.h"
#include "util/PluginHttp.h"

namespace kimono::progress {
namespace {
constexpr const char* DIR = "/.crosspoint/kimono";
constexpr const char* PENDING_PATH = "/.crosspoint/kimono/pending.json";
constexpr const char* LINKS_PATH = "/.crosspoint/kimono/links.json";
constexpr size_t MAX_FILE = 16 * 1024;
constexpr size_t MAX_SHELF = 96 * 1024;
constexpr size_t MAX_PENDING = 32;
constexpr size_t MAX_LINKS = 200;
constexpr uint32_t SLEEP_WIFI_MS = 10000;
constexpr uint32_t SLEEP_BUDGET_MS = 25000;

struct Pending {
  std::string path, title;
  float fraction = 0;
};

bool readJson(const char* path, JsonDocument& doc) {
  if (!Storage.exists(path)) return false;
  std::string raw;
  if (!Storage.readFileToString("KIMONO", path, MAX_FILE, raw)) return false;
  if (deserializeJson(doc, raw)) {
    LOG_ERR("KIMONO", "Unreadable %s", path);
    return false;
  }
  return true;
}

bool writeJson(const char* path, const JsonDocument& doc) {
  Storage.mkdir(DIR);
  String out;
  serializeJson(doc, out);
  // Through a temporary file, so a power cut never leaves half a queue.
  const std::string tmp = std::string(path) + ".tmp";
  if (!Storage.writeFile(tmp.c_str(), out)) {
    LOG_ERR("KIMONO", "Cannot write %s", tmp.c_str());
    return false;
  }
  Storage.remove(path);
  return Storage.rename(tmp.c_str(), path);
}

std::vector<Pending> loadPending() {
  std::vector<Pending> list;
  JsonDocument doc;
  if (!readJson(PENDING_PATH, doc)) return list;
  for (JsonVariantConst row : doc.as<JsonArrayConst>()) {
    Pending item;
    item.path = row["p"] | "";
    item.title = row["t"] | "";
    item.fraction = row["f"] | 0.0f;
    if (!item.path.empty()) list.push_back(std::move(item));
  }
  return list;
}

bool savePending(const std::vector<Pending>& list) {
  if (list.empty()) return !Storage.exists(PENDING_PATH) || Storage.remove(PENDING_PATH);
  JsonDocument doc;
  JsonArray rows = doc.to<JsonArray>();
  for (const auto& item : list) {
    JsonObject row = rows.add<JsonObject>();
    row["p"] = item.path;
    row["t"] = item.title;
    row["f"] = std::round(item.fraction * 10000.0f) / 10000.0f;
  }
  return writeJson(PENDING_PATH, doc);
}

std::string folded(const std::string& s) {
  std::string out;
  for (const char c : s)
    if (std::isalnum(static_cast<unsigned char>(c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string isbnOf(const std::string& path) {
  Epub epub(path, "/.crosspoint");
  if (!epub.load(false, true)) return {};
  Epub::SyncMetadata metadata;
  if (!epub.loadSyncMetadata(metadata)) return {};
  std::string digits;
  for (const char c : metadata.isbn)
    if (std::isdigit(static_cast<unsigned char>(c)) || c == 'X' || c == 'x') digits += c;
  return digits.size() == 10 || digits.size() == 13 ? digits : std::string{};
}

// The shelf's key for a title, when exactly one shelf book has that title.
std::string keyByTitle(const JsonDocument& shelf, const std::string& title) {
  const std::string want = folded(title);
  if (want.empty()) return {};
  std::string found;
  for (JsonVariantConst book : shelf["books"].as<JsonArrayConst>()) {
    if (folded(book["title"] | "") != want) continue;
    if (!found.empty()) return {};  // ambiguous
    found = book["key"] | "";
  }
  return found;
}
}  // namespace

void note(const Epub& epub, const float fraction) {
  KIMONO_ACCOUNT.load();
  if (!KIMONO_ACCOUNT.paired()) return;
  const float clamped = fraction < 0 ? 0 : (fraction > 1 ? 1 : fraction);
  // The same book at the same place needs no write (saves also run on exit).
  static std::string lastPath;
  static float lastFraction = -1;
  if (lastPath == epub.getPath() && std::fabs(lastFraction - clamped) < 0.0005f) return;

  auto list = loadPending();
  bool found = false;
  for (auto& item : list) {
    if (item.path != epub.getPath()) continue;
    item.fraction = clamped;
    found = true;
  }
  if (!found) {
    if (list.size() >= MAX_PENDING) list.erase(list.begin());
    list.push_back({epub.getPath(), epub.getTitle(), clamped});
  }
  if (savePending(list)) {
    lastPath = epub.getPath();
    lastFraction = clamped;
  }
}

bool link(const std::string& path, const std::string& key) {
  JsonDocument doc;
  readJson(LINKS_PATH, doc);
  if (!doc.is<JsonObject>()) doc.to<JsonObject>();
  JsonObject links = doc.as<JsonObject>();
  // One local file per key.
  for (JsonPair pair : links) {
    if (key == (pair.value() | "") && path != pair.key().c_str()) {
      links.remove(pair.key());
      break;
    }
  }
  if (links.size() >= MAX_LINKS && !links[path].is<const char*>()) {
    LOG_ERR("KIMONO", "Too many linked books");
    return false;
  }
  links[path] = key;
  return writeJson(LINKS_PATH, doc);
}

std::string linkedKey(const std::string& path) {
  JsonDocument doc;
  if (!readJson(LINKS_PATH, doc)) return {};
  return doc[path] | "";
}

std::string linkedPath(const std::string& key) {
  JsonDocument doc;
  if (!readJson(LINKS_PATH, doc)) return {};
  for (JsonPairConst pair : doc.as<JsonObjectConst>()) {
    if (key == (pair.value() | "") && Storage.exists(pair.key().c_str())) return pair.key().c_str();
  }
  return {};
}

bool hasPending() { return Storage.exists(PENDING_PATH); }

int flush(freeink::SecureHttpClient* session) {
  KIMONO_ACCOUNT.load();
  if (!KIMONO_ACCOUNT.paired()) return 0;
  auto list = loadPending();
  if (list.empty()) return 0;

  // The shelf, for books matched by title.
  JsonDocument shelf;
  {
    String body;
    Result result;
    const int status = KIMONO_ACCOUNT.api(session, "GET", "/api/v1/books/shelf", "", body, MAX_SHELF, &result);
    if (status < 0) return -1;
    if (status == 200) {
      JsonDocument filter;
      filter["books"][0]["key"] = true;
      filter["books"][0]["title"] = true;
      deserializeJson(shelf, body, DeserializationOption::Filter(filter));
    }
  }

  int sent = 0;
  std::vector<Pending> kept;
  for (size_t i = 0; i < list.size(); ++i) {
    const auto& item = list[i];
    JsonDocument payload;
    std::string key = linkedKey(item.path);
    std::string isbn;
    if (key.empty()) isbn = isbnOf(item.path);
    if (key.empty() && isbn.empty()) key = keyByTitle(shelf, item.title);
    if (key.empty() && isbn.empty()) {
      LOG_INF("KIMONO", "No Kimono match for %s; not syncing it", item.path.c_str());
      continue;  // dropped: nothing to match it with
    }
    if (!key.empty())
      payload["key"] = key;
    else
      payload["isbn"] = isbn;
    payload["fraction"] = std::round(item.fraction * 10000.0f) / 10000.0f;
    String body;
    serializeJson(payload, body);
    String reply;
    const int status =
        KIMONO_ACCOUNT.api(session, "POST", "/api/v1/books/progress", body.c_str(), reply, MAX_SHELF, nullptr);
    LOG_INF("KIMONO", "Progress %s %.3f: HTTP %d", item.path.c_str(), item.fraction, status);
    if (status < 0 || status == 401 || status >= 500) {
      // Kimono is away: keep this and everything after it for next time.
      kept.insert(kept.end(), list.begin() + static_cast<long>(i), list.end());
      savePending(kept);
      return sent > 0 ? sent : -1;
    }
    if (status >= 200 && status < 300) ++sent;
    // Any other 4xx is a book Kimono doesn't know: dropping it stops retrying forever.
  }
  savePending(kept);
  return sent;
}

bool joinSavedWifi(const uint32_t timeoutMs) {
#ifndef SIMULATOR
  if (WiFi.status() == WL_CONNECTED) return true;
  std::vector<std::string> order;
  const std::string last = WIFI_STORE.getLastConnectedSsid();
  if (!last.empty() && WIFI_STORE.findCredential(last)) order.push_back(last);
  for (size_t i = 0; i < WIFI_STORE.getCredentialCount() && order.size() < 3; ++i) {
    const auto ssid = WIFI_STORE.getSsidAt(i);
    if (ssid && *ssid != last) order.push_back(*ssid);
  }
  WiFi.mode(WIFI_STA);
  for (const auto& ssid : order) {
    const auto credential = WIFI_STORE.findCredential(ssid);
    if (!credential) continue;
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) delay(50);
    if (WiFi.status() == WL_CONNECTED) {
      WIFI_STORE.setLastConnectedSsid(ssid);
      return true;
    }
    WiFi.disconnect(false);
  }
  return false;
#else
  (void)timeoutMs;
  return WiFi.status() == WL_CONNECTED;
#endif
}

void flushAtSleep() {
#ifndef SIMULATOR
  KIMONO_ACCOUNT.load();
  if (!KIMONO_ACCOUNT.paired() || !hasPending()) return;
  const uint32_t start = millis();
  const bool wasConnected = WiFi.status() == WL_CONNECTED;
  if (!wasConnected) {
    // Only the last network: sleep should not wander through every saved one.
    const auto credential = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
    if (!credential) return;
    WiFi.mode(WIFI_STA);
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
    while (WiFi.status() != WL_CONNECTED && millis() - start < SLEEP_WIFI_MS) delay(50);
  }
  if (WiFi.status() == WL_CONNECTED && millis() - start < SLEEP_BUDGET_MS) {
    const int sent = flush(nullptr);
    LOG_INF("KIMONO", "Sleep sync: %d book(s) in %lu ms", sent, static_cast<unsigned long>(millis() - start));
  } else {
    LOG_INF("KIMONO", "Sleep sync skipped: no Wi-Fi");
  }
  if (!wasConnected) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
#endif
}

}  // namespace kimono::progress
