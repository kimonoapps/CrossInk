#include "KimonoActivity.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/kimono/KimonoSetupActivity.h"
#include "components/CatalogScreens.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "kimono/KimonoProgress.h"
#include "util/PluginHttp.h"
#include "util/StringUtils.h"

namespace fui = freeink::ui;

namespace {
// Whole JSON replies are buffered then filtered: the spaces list carries each
// space's SVG crest (~1 KB each), so these are generous. On the S3 boards large
// blocks come from PSRAM; a reply too big for RAM fails cleanly as an error.
constexpr size_t MAX_SHELF_RESPONSE = 96 * 1024;
constexpr size_t MAX_DRIVE_RESPONSE = 128 * 1024;
constexpr const char* DOWNLOAD_DIR = "/Kimono";

bool isEpubName(const std::string& name) {
  if (name.size() < 5) return false;
  std::string ext = name.substr(name.size() - 5);
  for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return ext == ".epub";
}

std::string percentLabel(const float fraction) {
  char text[8];
  snprintf(text, sizeof(text), "%d%%", static_cast<int>(fraction * 100.0f + 0.5f));
  return text;
}
}  // namespace

KimonoActivity::KimonoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : CatalogActivity("Kimono", renderer, mappedInput) {}

KimonoActivity::~KimonoActivity() = default;

void KimonoActivity::onEnter() {
  RenderLock lock;
  CatalogActivity::onEnter();
  sdFontSystem.releaseForNetwork(renderer);
  KIMONO_ACCOUNT.load();
  if (!KIMONO_ACCOUNT.paired() || !KIMONO_ACCOUNT.hasServer()) {
    launchSetup(false);
    return;
  }
  queue(Work::Connect);
}

void KimonoActivity::onExit() {
  session.reset();  // TLS before the radio goes
  CatalogActivity::onExit();
}

bool KimonoActivity::ensureSession() {
  if (session) return true;
  session = makeUniqueNoThrow<freeink::SecureHttpClient>();
  if (!session) {
    LOG_ERR("KIMONO", "OOM: HTTP session");
    fail(StrId::STR_MEMORY_ERROR);
    return false;
  }
  return true;
}

// Wi-Fi and pairing come from the phone (KimonoSetupActivity). pairOnly skips
// its hotspot when the reader is online and only the pairing is missing.
void KimonoActivity::launchSetup(const bool pairOnly) {
  session.reset();
  auto setup = makeUniqueNoThrow<KimonoSetupActivity>(renderer, mappedInput, pairOnly);
  if (!setup) {
    fail(StrId::STR_MEMORY_ERROR);
    return;
  }
  state = State::WIFI_SELECTION;  // a child flow owns the screen
  startActivityForResult(std::move(setup), [this](const ActivityResult& result) {
    RenderLock lock;
    if (result.isCancelled || !KIMONO_ACCOUNT.paired()) {
      finish();
      return;
    }
    queue(Work::Start);
  });
}

void KimonoActivity::queue(const Work work, const int argument, std::string text) {
  pendingWork = work;
  pendingArgument = argument;
  pendingText = std::move(text);
  state = State::LOADING;
  statusMessage = work == Work::Connect ? tr(STR_KIMONO_CONNECTING) : tr(STR_LOADING);
  requestUpdate();
}

// Input and view state share RenderLock. Network work is queued by input so
// the loading screen paints before any blocking request starts.
void KimonoActivity::loop() {
  Work work;
  int argument;
  std::string text;
  {
    RenderLock lock;
    UiListActivity::loop();
    work = pendingWork;
    argument = pendingArgument;
    text = std::move(pendingText);
    pendingWork = Work::None;
  }
  if (work == Work::None) return;
  requestUpdateAndWait();
  if (work == Work::Download) {
    runDownload(argument);  // progress callbacks take their own lock
    return;
  }
  if (work == Work::GetBook) {
    runGetBook();
    return;
  }
  RenderLock lock;
  if (work == Work::Connect) {
    runConnect();
    return;
  }
  if (!ensureSession()) return;
  switch (work) {
    case Work::Start:
      runStart();
      break;
    case Work::Shelf:
      runShelf();
      break;
    case Work::Spaces:
      runSpaces();
      break;
    case Work::Folder:
      runFolder(text);
      break;
    case Work::Search:
      runSearch(text);
      break;
    case Work::SetState:
      runSetState(text);
      break;
    case Work::Remove:
      runRemove();
      break;
    case Work::Sync:
      runSync();
      break;
    default:
      break;
  }
}

void KimonoActivity::showError(const kimono::Result result) {
  if (result == kimono::Result::Denied || !KIMONO_ACCOUNT.paired()) {
    // Unpaired from the phone: pair again, online already.
    launchSetup(true);
    return;
  }
  fail(StrId::STR_KIMONO_REQUEST_FAILED);
}

// No Wi-Fi picker: the reader joins a network it knows, or the phone gives it one.
void KimonoActivity::runConnect() {
  if (kimono::progress::joinSavedWifi(10000)) {
    queue(Work::Start);
    return;
  }
  LOG_INF("KIMONO", "No saved Wi-Fi answered; setting up from the phone");
  launchSetup(false);
}

// Online: send any queued reading progress, then show the shelf.
void KimonoActivity::runStart() {
  if (!KIMONO_ACCOUNT.paired()) {
    launchSetup(true);
    return;
  }
  kimono::progress::flush(session.get());
  runShelf();
}

void KimonoActivity::runShelf() {
  String body;
  kimono::Result result;
  const int status = KIMONO_ACCOUNT.api(session.get(), "GET", "/api/v1/books/shelf", "", body, MAX_SHELF_RESPONSE,
                                        &result);
  if (status != 200) {
    LOG_ERR("KIMONO", "Shelf: HTTP %d", status);
    showError(status == 401 || result == kimono::Result::Denied ? kimono::Result::Denied : result);
    return;
  }
  JsonDocument filter;
  for (const char* field : {"key", "title", "state", "fraction"}) filter["books"][0][field] = true;
  filter["books"][0]["authors"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    fail(StrId::STR_KIMONO_REQUEST_FAILED);
    return;
  }
  body = String();
  books.clear();
  const JsonArrayConst list = doc["books"].as<JsonArrayConst>();
  books.reserve(list.size());
  for (JsonVariantConst row : list) {
    ShelfBook book;
    book.key = row["key"] | "";
    book.title = row["title"] | "";
    book.state = row["state"] | "";
    book.fraction = row["fraction"] | 0.0f;
    for (JsonVariantConst author : row["authors"].as<JsonArrayConst>()) {
      const char* name = author.is<const char*>() ? author.as<const char*>() : (author["name"] | "");
      if (!name[0]) continue;
      if (!book.authors.empty()) book.authors += ", ";
      book.authors += name;
    }
    if (book.key.empty()) continue;
    book.localPath = kimono::progress::linkedPath(book.key);
    books.push_back(std::move(book));
  }
  if (view == View::Shelf && !shelfState.empty())
    showShelf(shelfState.c_str());
  else
    showMenu();
}

void KimonoActivity::runSpaces() {
  String body;
  kimono::Result result;
  const int status =
      KIMONO_ACCOUNT.api(session.get(), "GET", "/api/v1/drive/spaces", "", body, MAX_DRIVE_RESPONSE, &result);
  if (status != 200) {
    showError(status == 401 ? kimono::Result::Denied : result);
    return;
  }
  JsonDocument filter;
  for (const char* field : {"name", "rootId", "role"}) filter["spaces"][0][field] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    fail(StrId::STR_KIMONO_REQUEST_FAILED);
    return;
  }
  drive.clear();
  drivePath.clear();
  for (JsonVariantConst space : doc["spaces"].as<JsonArrayConst>()) {
    // Only the person's own spaces: what they keep, not what others share.
    if (std::strcmp(space["role"] | "", "owner") != 0) continue;
    DriveEntry entry;
    entry.id = space["rootId"] | "";
    entry.name = space["name"] | "";
    entry.folder = true;
    if (!entry.id.empty()) drive.push_back(std::move(entry));
  }
  driveTitle = tr(STR_KIMONO_DRIVE);
  view = View::Drive;
  state = State::BROWSING;
  rebuildRows();
}

void KimonoActivity::runFolder(const std::string& id) {
  String body;
  kimono::Result result;
  const int status = KIMONO_ACCOUNT.api(session.get(), "GET", "/api/v1/drive/nodes/" + id, "", body,
                                        MAX_DRIVE_RESPONSE, &result);
  if (status != 200) {
    showError(status == 401 ? kimono::Result::Denied : result);
    return;
  }
  JsonDocument filter;
  filter["folder"]["name"] = true;
  for (const char* field : {"id", "name", "kind", "size"}) filter["children"][0][field] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    fail(StrId::STR_KIMONO_REQUEST_FAILED);
    return;
  }
  drive.clear();
  for (JsonVariantConst child : doc["children"].as<JsonArrayConst>()) {
    DriveEntry entry;
    entry.id = child["id"] | "";
    entry.name = child["name"] | "";
    entry.folder = std::strcmp(child["kind"] | "", "folder") == 0;
    entry.size = child["size"] | 0u;
    if (entry.id.empty() || (!entry.folder && !isEpubName(entry.name))) continue;
    drive.push_back(std::move(entry));
  }
  driveTitle = doc["folder"]["name"] | tr(STR_KIMONO_DRIVE);
  view = View::Drive;
  state = State::BROWSING;
  rebuildRows();
}

// EPUBs in the person's own spaces whose name holds the book's title.
void KimonoActivity::runSearch(const std::string& title) {
  String body;
  kimono::Result result;
  const int status = KIMONO_ACCOUNT.api(session.get(), "GET",
                                        "/api/v1/drive/search?q=" + pluginhttp::urlEncodeQuery(title), "", body,
                                        MAX_DRIVE_RESPONSE, &result);
  if (status != 200) {
    showError(status == 401 ? kimono::Result::Denied : result);
    return;
  }
  JsonDocument filter;
  for (const char* field : {"id", "name", "kind", "size", "role"}) filter["items"][0][field] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    fail(StrId::STR_KIMONO_REQUEST_FAILED);
    return;
  }
  drive.clear();
  for (JsonVariantConst item : doc["items"].as<JsonArrayConst>()) {
    DriveEntry entry;
    entry.id = item["id"] | "";
    entry.name = item["name"] | "";
    entry.size = item["size"] | 0u;
    const char* role = item["role"] | "owner";
    if (entry.id.empty() || std::strcmp(item["kind"] | "", "file") != 0 || !isEpubName(entry.name)) continue;
    if (std::strcmp(role, "owner") != 0 && std::strcmp(role, "editor") != 0) continue;
    drive.push_back(std::move(entry));
  }
  if (drive.empty()) {
    fail(StrId::STR_KIMONO_NO_DRIVE_MATCH);
    return;
  }
  drivePath.clear();
  drivePath.emplace_back("", title);  // Back from results returns to the book
  driveTitle = title;
  view = View::Drive;
  state = State::BROWSING;
  rebuildRows();
}

void KimonoActivity::runDownload(const int index) {
  DriveEntry entry;
  std::string linkKey;
  {
    RenderLock lock;
    if (index < 0 || index >= static_cast<int>(drive.size())) return;
    entry = drive[index];
    linkKey = linkKeyForDownload;
  }
  saveToCard(KIMONO_ACCOUNT.server() + "/api/v1/drive/nodes/" + entry.id + "/content", entry.name, linkKey);
}

// Kimono Books' own copy of a shelf book: the best readable EPUB it has.
void KimonoActivity::runGetBook() {
  std::string key, title;
  {
    RenderLock lock;
    if (bookIndex < 0 || bookIndex >= static_cast<int>(books.size())) return;
    key = books[bookIndex].key;
    title = books[bookIndex].title;
    if (!ensureSession()) return;
  }
  String body;
  kimono::Result result;
  const int status = KIMONO_ACCOUNT.api(session.get(), "GET", "/api/v1/books/files?key=" + pluginhttp::urlEncodeQuery(key),
                                        "", body, 32 * 1024, &result);
  if (status != 200) {
    RenderLock lock;
    showError(status == 401 ? kimono::Result::Denied : result);
    return;
  }
  std::string address;
  bool preparing = false;
  {
    JsonDocument filter;
    for (const char* field : {"address", "format", "readable", "sample", "preparing"}) filter["files"][0][field] = true;
    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
      RenderLock lock;
      fail(StrId::STR_KIMONO_REQUEST_FAILED);
      return;
    }
    // Best first, as Kimono lists them; a whole book before a sample.
    std::string sample;
    for (JsonVariantConst file : doc["files"].as<JsonArrayConst>()) {
      if (std::strcmp(file["format"] | "", "epub") != 0 || !(file["readable"] | true)) continue;
      if (file["preparing"] | false) {
        preparing = true;  // an EPUB being made (from a PDF, say): ask again later
        continue;
      }
      const char* where = file["address"] | "";
      if (!where[0]) continue;
      if (file["sample"] | false) {
        if (sample.empty()) sample = where;
        continue;
      }
      address = where;
      break;
    }
    if (address.empty()) address = sample;
  }
  if (address.empty()) {
    // Said plainly, not as an error: there's nothing wrong, there's no copy (yet).
    RenderLock lock;
    statusMessage = preparing ? tr(STR_KIMONO_PREPARING) : tr(STR_KIMONO_NO_COPY);
    state = State::DONE;
    requestUpdate();
    return;
  }
  saveToCard(KIMONO_ACCOUNT.server() + "/api/v1/books/file?f=" + pluginhttp::urlEncodeQuery(address) + "&format=epub",
             title, key);
}

// Saves a book to /Kimono on the card. It lands as "<name>.part" first and is
// renamed when whole; a dropped connection keeps the part, and the next try
// asks only for the rest (Range). Linked to its shelf book when there is one.
void KimonoActivity::saveToCard(const std::string& url, const std::string& name, const std::string& key) {
  {
    RenderLock lock;
    downloadHeading = tr(STR_KIMONO_GETTING_BOOK);
    beginDownload(name);
  }
  const std::string auth = KIMONO_ACCOUNT.bearer();
  session.reset();  // the transfer makes its own connection; free this TLS first
  Storage.mkdir(DOWNLOAD_DIR);
  std::string file = StringUtils::sanitizeFilename(name);
  if (!isEpubName(file)) file += ".epub";
  const std::string dest = std::string(DOWNLOAD_DIR) + "/" + file;
  const std::vector<HttpDownloader::Header> headers = {{"Authorization", auth},
                                                       {"X-Kimono-Device", kimono::Account::deviceHeader()}};
  const auto outcome = downloadFile(url, dest, {}, {}, headers, HttpDownloader::Transport::WOLFSSL, true);
  RenderLock lock;
  if (outcome == HttpDownloader::OK) {
    statusMessage = dest;
    if (!key.empty()) kimono::progress::link(dest, key);
  }
  finishDownload(outcome);
}

void KimonoActivity::downloadFinished(const bool cancelled) {
  if (cancelled) {
    state = State::BROWSING;
    rebuildRows();
    return;
  }
  // Refresh "on device" marks on the shelf.
  for (auto& book : books) book.localPath = kimono::progress::linkedPath(book.key);
  state = State::DONE;
  requestUpdate();
}

void KimonoActivity::runSetState(const std::string& newState) {
  if (bookIndex < 0 || bookIndex >= static_cast<int>(books.size())) return;
  JsonDocument payload;
  payload["key"] = books[bookIndex].key;
  payload["state"] = newState;
  String body, reply;
  serializeJson(payload, body);
  kimono::Result result;
  const int status = KIMONO_ACCOUNT.api(session.get(), "PUT", "/api/v1/books/shelf", body.c_str(), reply, MAX_SHELF_RESPONSE, &result);
  if (status < 200 || status >= 300) {
    showError(status == 401 ? kimono::Result::Denied : result);
    return;
  }
  books[bookIndex].state = newState;
  showShelf(newState.c_str());
}

void KimonoActivity::runRemove() {
  if (bookIndex < 0 || bookIndex >= static_cast<int>(books.size())) return;
  String reply;
  kimono::Result result;
  const int status = KIMONO_ACCOUNT.api(session.get(), "DELETE",
                                        "/api/v1/books/shelf?key=" + pluginhttp::urlEncodeQuery(books[bookIndex].key),
                                        "", reply, MAX_SHELF_RESPONSE, &result);
  if (status < 200 || status >= 300) {
    showError(status == 401 ? kimono::Result::Denied : result);
    return;
  }
  books.erase(books.begin() + bookIndex);
  bookIndex = -1;
  showShelf(shelfState.c_str());
}

void KimonoActivity::runSync() {
  const bool had = kimono::progress::hasPending();
  const int sent = kimono::progress::flush(session.get());
  if (sent < 0) {
    fail(StrId::STR_KIMONO_REQUEST_FAILED);
    return;
  }
  char text[64];
  if (!had || sent == 0)
    snprintf(text, sizeof(text), "%s", tr(STR_KIMONO_NOTHING_TO_SYNC));
  else
    snprintf(text, sizeof(text), tr(STR_KIMONO_SYNCED_FORMAT), sent);
  statusMessage = text;
  // Progress moved books between shelves: show the fresh shelf behind the message.
  runShelf();
  state = State::DONE;
  requestUpdate();
}

int KimonoActivity::count(const char* bookState) const {
  int n = 0;
  for (const auto& book : books)
    if (book.state == bookState) ++n;
  return n;
}

std::string KimonoActivity::syncedLabel() const {
  const int64_t last = KIMONO_ACCOUNT.lastSync();
  const int64_t now = time(nullptr);
  if (last <= 0 || now < last) return {};
  const long minutes = static_cast<long>((now - last) / 60);
  char text[48];
  if (minutes < 1)
    snprintf(text, sizeof(text), "%s", tr(STR_KIMONO_SYNCED_NOW));
  else if (minutes < 120)
    snprintf(text, sizeof(text), tr(STR_KIMONO_SYNCED_MIN_FORMAT), minutes);
  else if (minutes < 48 * 60)
    snprintf(text, sizeof(text), tr(STR_KIMONO_SYNCED_HOURS_FORMAT), minutes / 60);
  else
    snprintf(text, sizeof(text), tr(STR_KIMONO_SYNCED_DAYS_FORMAT), minutes / (24 * 60));
  return text;
}

void KimonoActivity::showMenu() {
  view = View::Menu;
  shelfState.clear();
  bookIndex = -1;
  state = State::BROWSING;
  rebuildRows();
}

void KimonoActivity::showShelf(const char* bookState) {
  view = View::Shelf;
  shelfState = bookState;
  bookIndex = -1;
  state = State::BROWSING;
  rebuildRows();
}

void KimonoActivity::showBook(const int index) {
  view = View::Book;
  bookIndex = index;
  state = State::BROWSING;
  rebuildRows();
}

void KimonoActivity::rebuildRows() {
  closeRouting();
  rows.clear();
  const auto add = [this](std::string label, std::string subtitle, std::string value, Row kind, int arg = 0) {
    rows.push_back({std::move(label), std::move(subtitle), std::move(value), kind, arg});
  };
  switch (view) {
    case View::Menu: {
      add(tr(STR_KIMONO_READING), "", std::to_string(count("reading")), Row::ShelfReading);
      add(tr(STR_KIMONO_WANT), "", std::to_string(count("want")), Row::ShelfWant);
      add(tr(STR_KIMONO_READ), "", std::to_string(count("read")), Row::ShelfRead);
      add(tr(STR_KIMONO_DRIVE), "", ">", Row::Drive);
      add(tr(STR_KIMONO_SYNC_NOW), syncedLabel(), "", Row::Sync);
      add(tr(STR_KIMONO_SET_UP_AGAIN), KIMONO_ACCOUNT.server(), "", Row::SetUpAgain);
      add(unpairPending ? tr(STR_KIMONO_UNPAIR_CONFIRM) : tr(STR_KIMONO_UNPAIR), KIMONO_ACCOUNT.username(), "",
          Row::Unpair);
      break;
    }
    case View::Shelf:
      for (size_t i = 0; i < books.size(); ++i) {
        const auto& book = books[i];
        if (book.state != shelfState) continue;
        std::string value = book.state == "reading" ? percentLabel(book.fraction) : std::string{};
        if (!book.localPath.empty()) value = value.empty() ? "*" : value + " *";
        add(book.title, book.authors, value, Row::Book, static_cast<int>(i));
      }
      break;
    case View::Book: {
      if (bookIndex < 0 || bookIndex >= static_cast<int>(books.size())) break;
      const auto& book = books[bookIndex];
      if (!book.localPath.empty())
        add(tr(STR_OPEN), book.localPath, "", Row::Open);
      else
        add(tr(STR_KIMONO_GET_BOOK), "", "", Row::GetBook);
      add(tr(STR_KIMONO_FIND_IN_DRIVE), "", ">", Row::FindInDrive);
      if (book.state != "reading") add(tr(STR_KIMONO_MARK_READING), "", "", Row::MarkReading);
      if (book.state != "want") add(tr(STR_KIMONO_MARK_WANT), "", "", Row::MarkWant);
      if (book.state != "read") add(tr(STR_KIMONO_MARK_READ), "", "", Row::MarkRead);
      add(tr(STR_KIMONO_REMOVE), "", "", Row::Remove);
      break;
    }
    case View::Drive:
      for (size_t i = 0; i < drive.size(); ++i) {
        const auto& entry = drive[i];
        if (entry.folder) {
          add(entry.name, "", ">", drivePath.empty() ? Row::Space : Row::Folder, static_cast<int>(i));
        } else {
          char size[16];
          snprintf(size, sizeof(size), "%.1f MB", static_cast<double>(entry.size) / (1024.0 * 1024.0));
          add(entry.name, size, "", Row::File, static_cast<int>(i));
        }
      }
      break;
  }
  nav.reset();
  requestUpdate();
}

int KimonoActivity::listCount() const { return state == State::BROWSING ? static_cast<int>(rows.size()) : 0; }

void KimonoActivity::activateIndex(const int index) {
  if (state != State::BROWSING || index < 0 || index >= static_cast<int>(rows.size())) return;
  app.clearTapFlash();
  const ListRow row = rows[index];
  switch (row.kind) {
    case Row::ShelfReading:
      showShelf("reading");
      return;
    case Row::ShelfWant:
      showShelf("want");
      return;
    case Row::ShelfRead:
      showShelf("read");
      return;
    case Row::Drive:
      linkKeyForDownload.clear();
      queue(Work::Spaces);
      return;
    case Row::Sync:
      queue(Work::Sync);
      return;
    case Row::SetUpAgain:
      launchSetup(false);
      return;
    case Row::Unpair:
      if (!unpairPending) {
        unpairPending = true;
        rebuildRows();
        moveSelectionTo(index);
        return;
      }
      KIMONO_ACCOUNT.unpair();
      unpairPending = false;
      launchSetup(true);
      return;
    case Row::Book:
      showBook(row.arg);
      return;
    case Row::Open:
      if (bookIndex >= 0) openLocalBook(books[bookIndex].localPath);
      return;
    case Row::GetBook:
      queue(Work::GetBook);
      return;
    case Row::FindInDrive:
      if (bookIndex < 0) return;
      linkKeyForDownload = books[bookIndex].key;
      queue(Work::Search, 0, books[bookIndex].title);
      return;
    case Row::MarkReading:
      queue(Work::SetState, 0, "reading");
      return;
    case Row::MarkWant:
      queue(Work::SetState, 0, "want");
      return;
    case Row::MarkRead:
      queue(Work::SetState, 0, "read");
      return;
    case Row::Remove:
      queue(Work::Remove);
      return;
    case Row::Space:
    case Row::Folder: {
      const auto& entry = drive[row.arg];
      drivePath.emplace_back(entry.id, entry.name);
      queue(Work::Folder, 0, entry.id);
      return;
    }
    case Row::File:
      queue(Work::Download, row.arg);
      return;
  }
}

// Opening a book leaves the network session: the radio goes off and the
// device restarts straight into the reader, as after any Wi-Fi screen.
void KimonoActivity::openLocalBook(const std::string& path) {
  if (path.empty() || !Storage.exists(path.c_str())) return;
  session.reset();
#ifndef SIMULATOR
  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
  silentRestartToReader();
#else
  activityManager.goToReader(path);
#endif
}

void KimonoActivity::onBackButton() {
  if (state == State::BROWSING || state == State::DONE) {
    switch (view) {
      case View::Menu:
        break;  // leaves Kimono
      case View::Shelf:
        showMenu();
        return;
      case View::Book:
        showShelf(shelfState.c_str());
        return;
      case View::Drive:
        if (drivePath.empty()) {
          showMenu();
          return;
        }
        drivePath.pop_back();
        if (!drivePath.empty() && drivePath.back().first.empty()) {
          // Search results: back to the book they were for.
          drivePath.clear();
          if (bookIndex >= 0)
            showBook(bookIndex);
          else
            showMenu();
          return;
        }
        if (drivePath.empty() && !linkKeyForDownload.empty() && bookIndex >= 0) {
          showBook(bookIndex);
          return;
        }
        if (drivePath.empty())
          queue(Work::Spaces);
        else
          queue(Work::Folder, 0, drivePath.back().first);
        return;
    }
  }
  finish();
}

bool KimonoActivity::handleCustomInput() {
  if (state == State::DONE) {
    int x = 0, y = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      state = State::BROWSING;
      rebuildRows();
    }
    return true;
  }
  if (unpairPending && (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
                        mappedInput.wasPressed(MappedInputManager::Button::Up) ||
                        mappedInput.wasPressed(MappedInputManager::Button::Down))) {
    unpairPending = false;  // anything but a second Confirm keeps the pairing
    rebuildRows();
  }
  return CatalogActivity::handleCustomInput();
}

void KimonoActivity::buildScreen(UiScreen& screen) {
  const char* title = tr(STR_KIMONO);
  std::string heading;
  if (state == State::BROWSING) {
    switch (view) {
      case View::Shelf:
        title = shelfState == "reading" ? tr(STR_KIMONO_READING)
                                        : (shelfState == "want" ? tr(STR_KIMONO_WANT) : tr(STR_KIMONO_READ));
        break;
      case View::Book:
        if (bookIndex >= 0) heading = books[bookIndex].title;
        title = heading.c_str();
        break;
      case View::Drive:
        title = driveTitle.c_str();
        break;
      default:
        break;
    }
  }
  screenHeader(screen, title);
  if (buildStatusScreen(screen)) return;

  switch (state) {
    case State::DONE:
      catalogCenteredBlock(screen, {{statusMessage.c_str(), true}});
      return;
    case State::BROWSING:
      break;
    default:
      return;
  }

  if (rows.empty()) {
    screen.centeredText(view == View::Drive ? tr(STR_KIMONO_EMPTY_FOLDER) : tr(STR_KIMONO_EMPTY_SHELF),
                        screen.theme().bodyText);
    return;
  }
  std::vector<fui::ListItem> items;
  items.reserve(rows.size());
  for (size_t i = 0; i < rows.size(); ++i) {
    fui::ListItem item;
    item.label = rows[i].label.c_str();
    item.subtitle = rows[i].subtitle.empty() ? nullptr : rows[i].subtitle.c_str();
    item.value = rows[i].value.empty() ? nullptr : rows[i].value.c_str();
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }
  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  syncListViewport(screen, props);
  screen.list(props);
}

void KimonoActivity::drawFooter() {
  MappedInputManager::Labels labels;
  switch (state) {
    case State::BROWSING: {
      const bool many = rows.size() > 1;
      labels = mappedInput.mapLabels(tr(STR_BACK), rows.empty() ? "" : tr(STR_SELECT), many ? tr(STR_DIR_UP) : "",
                                     many ? tr(STR_DIR_DOWN) : "");
      break;
    }
    case State::ERROR:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
      break;
    case State::DONE:
      labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), "", "");
      break;
    case State::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    default:
      labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
