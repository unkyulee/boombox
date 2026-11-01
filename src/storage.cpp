#include "storage.h"

#include <SD.h>
#include <SPI.h>
#include <algorithm>

#include "config.h"

namespace {

// The display's QSPI driver owns SPI2 (the default `SPI` object) and reprograms it on every
// redraw, so the card gets the other general-purpose peripheral, SPI3.
SPIClass sdSpi(HSPI);

SemaphoreHandle_t mutex;
bool isMounted = false;

const char *MOUNT_POINT = "/sd";

bool mountFs() { return SD.begin(PIN_SD_CS, sdSpi, SD_SPI_FREQ, MOUNT_POINT, 10); }


bool removeTree(const String &path) {
  if (!storage::isDir(path)) return SD.remove(path);

  std::vector<String> children;
  File dir = SD.open(path);
  if (!dir) return false;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) children.push_back(String(f.path()));
  dir.close();

  for (const String &child : children) {
    if (!removeTree(child)) return false;
  }
  return SD.rmdir(path);
}

}  // namespace

namespace storage {

bool begin() {
  if (!mutex) mutex = xSemaphoreCreateMutex();
  sdSpi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  isMounted = mountFs();
  return isMounted;
}

bool mounted() { return isMounted; }

bool ensureMounted() {
  if (isMounted) return true;
  Guard guard;
  SD.end();
  isMounted = mountFs();
  Serial.printf("[storage] remount: %s\n", isMounted ? "ok" : "failed");
  return isMounted;
}

void lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
void unlock() { xSemaphoreGive(mutex); }

bool isMp3(const String &name) {
  return name.length() > 4 && name.substring(name.length() - 4).equalsIgnoreCase(".mp3");
}

String cleanPath(const String &raw) {
  String p = raw;
  p.trim();
  p.replace("\\", "/");
  if (!p.startsWith("/")) p = "/" + p;
  while (p.indexOf("//") >= 0) p.replace("//", "/");
  if (p.length() > 1 && p.endsWith("/")) p.remove(p.length() - 1);
  if (p.indexOf("..") >= 0 || p.length() > 250) return "";
  for (size_t i = 0; i < p.length(); i++) {
    if ((uint8_t)p[i] < 0x20 || strchr(":*?\"<>|", p[i])) return "";
  }
  return p;
}

String join(const String &dir, const String &name) {
  return cleanPath(dir == "/" ? "/" + name : dir + "/" + name);
}

String parentOf(const String &path) {
  int slash = path.lastIndexOf('/');
  return slash <= 0 ? "/" : path.substring(0, slash);
}

bool list(const String &path, std::vector<FsEntry> &out, bool musicOnly) {
  out.clear();
  if (!isMounted) return false;
  File dir = SD.open(path.length() ? path : "/");
  if (!dir || !dir.isDirectory()) return false;

  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String name = f.name();
    if (name.startsWith(".") || name == "System Volume Information") continue;
    bool isDirectory = f.isDirectory();
    if (musicOnly && !isDirectory && !isMp3(name)) continue;
    out.push_back({name, isDirectory, (uint32_t)f.size()});
  }
  std::sort(out.begin(), out.end(), [](const FsEntry &a, const FsEntry &b) {
    if (a.dir != b.dir) return a.dir;
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  return true;
}

bool isDir(const String &path) {
  if (!isMounted) return false;
  if (path == "/") return true;
  File f = SD.open(path);
  return f && f.isDirectory();
}

bool exists(const String &path) { return isMounted && SD.exists(path); }

bool mkdir(const String &path) { return isMounted && SD.mkdir(path); }

bool remove(const String &path) { return isMounted && path != "/" && removeTree(path); }

bool rename(const String &from, const String &to) { return isMounted && SD.rename(from, to); }

uint64_t totalBytes() { return isMounted ? SD.totalBytes() : 0; }
uint64_t usedBytes() { return isMounted ? SD.usedBytes() : 0; }

}  // namespace storage
