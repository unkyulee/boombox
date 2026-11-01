#include "player.h"

#include <Audio.h>
#include <Preferences.h>
#include <SD.h>

#include "config.h"
#include "storage.h"

namespace {

struct Msg {
  PlayerCmd cmd;
  int32_t arg;
};

Audio *audio = nullptr;
QueueHandle_t queue;
SemaphoreHandle_t mutex;
Preferences prefs;

// Shared with other tasks, guarded by `mutex`.
PlayerStatus st;
std::vector<Track> playlist;
String folder = MUSIC_DIR;
String pendingPath;

// Owned by the audio task.
bool active = false;
bool paused = false;
bool artistFromTag = false;
uint32_t startedAt = 0;

struct Lock {
  Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex); }
};

void copyStr(char *dst, size_t n, const String &src) { strlcpy(dst, src.c_str(), n); }

String currentFolder() {
  Lock l;
  return folder;
}

// Title/artist/album guessed from "Folder/Artist - Title.mp3" until ID3 tags arrive.
void setMetaFromName(const String &dir, const String &name) {
  String base = name;
  int dot = base.lastIndexOf('.');
  if (dot > 0) base = base.substring(0, dot);

  String artist, title = base;
  int sep = base.indexOf(" - ");
  if (sep > 0) {
    artist = base.substring(0, sep);
    title = base.substring(sep + 3);
  }
  String album = dir == "/" ? "" : dir.substring(dir.lastIndexOf('/') + 1);

  copyStr(st.title, sizeof(st.title), title);
  copyStr(st.artist, sizeof(st.artist), artist);
  copyStr(st.album, sizeof(st.album), album);
  copyStr(st.folder, sizeof(st.folder), dir);
  copyStr(st.file, sizeof(st.file), storage::join(dir, name));
}

void applyVolume() { audio->setVolume(st.volume); }

void stopPlayback() {
  audio->stopSong();
  active = false;
  paused = false;
}

// Reads `dir` into the playlist. Keeps the selection on `keepFile` when it is still there.
void loadFolder(const String &dir, const String &keepFile = "") {
  std::vector<FsEntry> entries;
  storage::list(dir, entries, true);

  std::vector<Track> found;
  for (const FsEntry &e : entries) {
    if (!e.dir) found.push_back({e.name, e.size});
  }

  Lock l;
  folder = dir;
  playlist.swap(found);
  st.count = playlist.size();
  st.index = -1;
  for (size_t i = 0; i < playlist.size(); i++) {
    if (storage::join(dir, playlist[i].name) == keepFile) st.index = i;
  }
  if (st.index < 0 && st.count) st.index = 0;

  if (st.count) {
    setMetaFromName(dir, playlist[st.index].name);
  } else {
    st.title[0] = st.artist[0] = st.album[0] = st.file[0] = 0;
    copyStr(st.folder, sizeof(st.folder), dir);
  }
  st.position = st.duration = 0;
  st.version++;
}

bool startTrack(int index) {
  String path, dir = currentFolder();
  {
    Lock l;
    int count = playlist.size();
    if (count == 0) return false;
    index = ((index % count) + count) % count;
    path = storage::join(dir, playlist[index].name);
    st.index = index;
    st.position = st.duration = 0;
    setMetaFromName(dir, playlist[index].name);
    st.version++;
  }
  artistFromTag = false;
  audio->stopSong();
  paused = false;
  active = audio->connecttoFS(SD, path.c_str());
  startedAt = millis();
  prefs.putString("folder", dir);
  prefs.putString("file", path);
  if (!active) Serial.printf("[player] cannot play %s\n", path.c_str());
  return active;
}

// Starts `index`, skipping unplayable files in direction `step`.
void playFrom(int index, int step) {
  int count = player::status().count;
  for (int i = 0; i < count; i++, index += step) {
    if (startTrack(index)) return;
  }
  stopPlayback();
}

void handlePlayPath() {
  String path;
  {
    Lock l;
    path = pendingPath;
    pendingPath = "";
  }
  if (!path.length()) return;

  if (storage::isDir(path)) {
    loadFolder(path);
    playFrom(0, 1);
    return;
  }
  loadFolder(storage::parentOf(path), path);
  int index = player::status().index;
  if (index >= 0) playFrom(index, 1);
}

void handle(const Msg &m) {
  PlayerStatus s = player::status();
  switch (m.cmd) {
    case PlayerCmd::Toggle:
      if (active) {
        if (audio->pauseResume()) paused = !paused;
      } else {
        playFrom(s.index < 0 ? 0 : s.index, 1);
      }
      break;
    case PlayerCmd::Play: playFrom(m.arg, 1); break;
    case PlayerCmd::Stop: stopPlayback(); break;
    case PlayerCmd::Next: playFrom(s.index + 1, 1); break;
    case PlayerCmd::Prev:
      if (active && s.position > 3) playFrom(s.index, 1);
      else playFrom(s.index - 1, -1);
      break;
    case PlayerCmd::Seek:
      if (active) audio->setAudioPlayPosition(max<int32_t>(0, m.arg));
      break;
    case PlayerCmd::SetVolume: {
      Lock l;
      st.volume = constrain(m.arg, 0, VOLUME_MAX);
      applyVolume();
      break;
    }
    case PlayerCmd::PlayPath: handlePlayPath(); break;
    case PlayerCmd::Refresh: {
      String open = s.file;
      loadFolder(currentFolder(), open);
      // The open file may have been deleted or moved away underneath us.
      if (active && !storage::exists(open)) stopPlayback();
      break;
    }
  }
}

void updateStatus() {
  Lock l;
  st.loaded = active;
  st.paused = active && paused;
  st.playing = active && !paused;
  if (active) {
    st.position = audio->getAudioCurrentTime();
    st.duration = audio->getAudioFileDuration();
  }
}

void audioTask(void *) {
  uint32_t lastStatus = 0;

  for (;;) {
    Msg m;
    while (xQueueReceive(queue, &m, 0) == pdTRUE) handle(m);

    audio->loop();

    // The library calls stopSong() itself at end of file (or on a decode error).
    if (active && !paused && !audio->isRunning() && millis() - startedAt > 500) {
      active = false;
      int next = player::status().index + 1;
      if (next < player::status().count || REPEAT_ALL) playFrom(next, 1);
    }

    if (millis() - lastStatus > 200) {
      lastStatus = millis();
      updateStatus();
    }
    vTaskDelay(1);
  }
}

}  // namespace

// ---- ESP32-audioI2S callbacks (run on the audio task) ----

void audio_id3data(const char *info) {
  struct Tag {
    const char *prefix;
    char *dst;
    size_t n;
  };
  const Tag tags[] = {
      {"Title: ", st.title, sizeof(st.title)},
      {"Artist: ", st.artist, sizeof(st.artist)},
      {"Band: ", st.artist, sizeof(st.artist)},  // album artist, only if there is no artist
      {"Album: ", st.album, sizeof(st.album)},
  };
  for (const Tag &t : tags) {
    size_t len = strlen(t.prefix);
    if (strncmp(info, t.prefix, len) != 0 || info[len] == 0) continue;
    bool isBand = t.prefix[0] == 'B';
    if (isBand && artistFromTag) return;
    if (t.dst == st.artist && !isBand) artistFromTag = true;
    Lock l;
    strlcpy(t.dst, info + len, t.n);
    st.version++;
    return;
  }
}

void audio_info(const char *info) {
  if (CORE_DEBUG_LEVEL >= 3) Serial.printf("[audio] %s\n", info);
}

// ---- Public API ----

namespace player {

void begin() {
  mutex = xSemaphoreCreateMutex();
  queue = xQueueCreate(16, sizeof(Msg));

  prefs.begin("player");
  // Always starts at full output: the speaker sets the listening volume. The API can still
  // turn it down for a session, but that is deliberately not remembered.
  st.volume = VOLUME_DEFAULT;

  audio = new Audio();
  audio->setPinout(PIN_I2S_BCLK, PIN_I2S_LRCK, PIN_I2S_DOUT);
  applyVolume();

  String last = prefs.getString("folder", MUSIC_DIR);
  if (!storage::isDir(last)) last = MUSIC_DIR;
  loadFolder(last, prefs.getString("file", ""));

  // Core 1 alongside the UI, but at a higher priority so drawing never starves the decoder.
  // Wi-Fi and the web server live on core 0.
  xTaskCreatePinnedToCore(audioTask, "audio", 12288, nullptr, 3, nullptr, 1);
}

void send(PlayerCmd cmd, int32_t arg) {
  Msg m{cmd, arg};
  xQueueSend(queue, &m, pdMS_TO_TICKS(50));
}

void playPath(const String &path) {
  {
    Lock l;
    pendingPath = path;
  }
  send(PlayerCmd::PlayPath);
}

PlayerStatus status() {
  Lock l;
  return st;
}

std::vector<Track> tracks() {
  Lock l;
  return playlist;
}

void refresh() { send(PlayerCmd::Refresh); }

void release(const String &path) {
  PlayerStatus s = status();
  if (!s.loaded) return;
  String open = s.file;
  bool affected = open == path || open.startsWith(path + "/");
  if (!affected) return;
  send(PlayerCmd::Stop);
  for (int i = 0; i < 50 && status().loaded; i++) delay(20);
}

}  // namespace player
