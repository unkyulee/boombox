#pragma once
#include <Arduino.h>

#include <vector>

struct FsEntry {
  String name;
  bool dir;
  uint32_t size;
};

// Owns the SD card: mounting and the file operations used by the UI and the web page.
namespace storage {

bool begin();      // set up the bus and mount the card
bool mounted();
// Remounts the card if it dropped off the bus. False if it cannot be mounted.
bool ensureMounted();

// Paths are absolute, e.g. "/music/rock". Folders sort before files.
// `musicOnly` hides everything except folders and MP3s.
bool list(const String &path, std::vector<FsEntry> &out, bool musicOnly = false);
bool isDir(const String &path);
bool exists(const String &path);
bool mkdir(const String &path);
bool remove(const String &path);  // folders are removed with their contents
bool rename(const String &from, const String &to);
uint64_t totalBytes();
uint64_t usedBytes();

// Cleans a path that came from a browser: absolute, no "..", no trailing slash.
// Returns an empty string if the path is not acceptable.
String cleanPath(const String &path);
String join(const String &dir, const String &name);
String parentOf(const String &path);
bool isMp3(const String &name);

// Serialises file operations between the UI and web tasks.
void lock();
void unlock();
struct Guard {
  Guard() { lock(); }
  ~Guard() { unlock(); }
};

}  // namespace storage
