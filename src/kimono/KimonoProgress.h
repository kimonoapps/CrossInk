#pragma once

#include <cstdint>
#include <string>

class Epub;
namespace freeink {
class SecureHttpClient;
}

// Reading progress for Kimono Books.
//
// The reader saves its position on a debounce (every few page turns, or after a
// few minutes, and on leaving the book). Each of those saves calls note(), which
// keeps the book's newest fraction in a small SD file. The Wi-Fi radio is off
// while reading, so the queue is sent:
//   - when the device goes to sleep (a short Wi-Fi session on the last network,
//     only if something is queued and the reader is signed in to Kimono), and
//   - whenever the Kimono screen opens, and from its "Sync reading progress" row.
//
// A book is matched to Kimono Books by the key it was linked to (a shelf book
// downloaded from Kimono Drive), else by the EPUB's ISBN, else by its title on
// the person's shelf. A book with none of those is left alone.
namespace kimono::progress {

// The reader saved a position. Cheap: one small SD write, and only when signed in.
void note(const Epub& epub, float fraction);

// The local copy of a shelf book: progress on that file goes to `key`.
bool link(const std::string& path, const std::string& key);
// The Books key linked to a local file, or "".
std::string linkedKey(const std::string& path);
// The local file linked to a Books key, or "".
std::string linkedPath(const std::string& key);

bool hasPending();
// Sends everything queued. Returns how many books were sent, or -1 when Kimono
// could not be reached (the queue is kept).
int flush(freeink::SecureHttpClient* session);

// Joins a saved Wi-Fi network (the last one first) without asking anything.
// True when connected. Blocks for up to timeoutMs per network tried.
bool joinSavedWifi(uint32_t timeoutMs);

// Deep-sleep hook: joins the last Wi-Fi network for a few seconds when there is
// something to send, sends it, and turns the radio off again.
void flushAtSleep();

}  // namespace kimono::progress
