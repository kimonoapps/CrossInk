#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/CatalogActivity.h"
#include "kimono/KimonoAccount.h"

namespace freeink {
class SecureHttpClient;
}

// Kimono Read: the person's Kimono Books shelf and Kimono Drive on the reader.
//
// Opening it joins a saved Wi-Fi network by itself, sends any queued reading
// progress and pulls the shelf. With no pairing (or no Wi-Fi it can join) it
// hands over to KimonoSetupActivity, where the phone does all the typing.
// EPUBs from Drive download to /Kimono on the card. Runs from a minimal network
// boot like the other network screens.
class KimonoActivity final : public CatalogActivity {
 public:
  explicit KimonoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  ~KimonoActivity() override;

  void onEnter() override;
  void loop() override;
  void onExit() override;

 private:
  enum class View : uint8_t { Menu, Shelf, Book, Drive };
  enum class Work : uint8_t {
    None, Connect, Start, Shelf, Spaces, Folder, Search, Download, GetBook, SetState, Remove, Sync
  };
  enum class Row : uint8_t {
    ShelfReading,
    ShelfWant,
    ShelfRead,
    Drive,
    Sync,
    SetUpAgain,
    Unpair,
    Book,
    Open,
    GetBook,
    FindInDrive,
    MarkReading,
    MarkWant,
    MarkRead,
    Remove,
    Space,
    Folder,
    File
  };
  struct ListRow {
    std::string label, subtitle, value;
    Row kind;
    int arg = 0;
  };
  struct ShelfBook {
    std::string key, title, authors, state, localPath;
    float fraction = 0;
  };
  struct DriveEntry {
    std::string id, name;
    bool folder = false;
    uint32_t size = 0;
  };

  std::unique_ptr<freeink::SecureHttpClient> session;

  View view = View::Menu;
  std::string shelfState;  // "reading" | "want" | "read" while viewing a shelf
  int bookIndex = -1;      // the shelf book whose actions are showing
  std::vector<ShelfBook> books;
  std::vector<DriveEntry> drive;
  // Drive folders opened, outermost first: {id, name}. Empty = the list of spaces.
  std::vector<std::pair<std::string, std::string>> drivePath;
  std::string driveTitle;
  std::string linkKeyForDownload;  // shelf book a Drive download belongs to
  std::vector<ListRow> rows;
  bool unpairPending = false;  // Unpair asks once more before it acts

  Work pendingWork = Work::None;
  int pendingArgument = 0;
  std::string pendingText;

  // CatalogActivity
  void startBrowse() override { queue(Work::Start); }
  void retryBrowse() override { queue(Work::Connect); }
  bool hasSearch() const override { return false; }
  void performSearch(const std::string&) override {}
  void downloadFinished(bool cancelled) override;

  // UiListActivity
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  void onBackButton() override;
  void drawFooter() override;

  void queue(Work work, int argument = 0, std::string text = {});
  void launchSetup(bool pairOnly);
  void runConnect();
  void runStart();
  void runShelf();
  void runSpaces();
  void runFolder(const std::string& id);
  void runSearch(const std::string& title);
  void runDownload(int index);
  void runGetBook();
  void saveToCard(const std::string& url, const std::string& name, const std::string& key);
  void runSetState(const std::string& state);
  void runRemove();
  void runSync();
  void showError(kimono::Result result);
  void showMenu();
  void showShelf(const char* state);
  void showBook(int index);
  void rebuildRows();
  void openLocalBook(const std::string& path);
  bool ensureSession();
  int count(const char* state) const;
  std::string syncedLabel() const;
};
