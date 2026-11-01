#pragma once
#include <Arduino.h>
#include <vector>

// The player owns the Audio object and runs it on its own task. Other tasks (UI, web)
// never touch the decoder directly: they send commands and read status snapshots.
//
// The playlist is one folder. Playing a file plays the rest of its folder after it.

enum class PlayerCmd : uint8_t {
  Toggle,      // play/pause; starts the selected track when stopped
  Play,        // arg = index in the current folder
  Stop,
  Next,
  Prev,        // restarts the current track if more than 3 s in
  Seek,        // arg = seconds
  SetVolume,   // arg = 0..VOLUME_MAX
  PlayPath,    // a file, or a folder to play from its first track (see playPath)
  Refresh,     // re-read the current folder
};

struct Track {
  String name;
  uint32_t size;
};

struct PlayerStatus {
  bool loaded;        // a file is open (playing or paused)
  bool playing;
  bool paused;
  uint8_t volume;
  int index;          // -1 when the folder holds no MP3s
  int count;
  uint32_t position;  // seconds
  uint32_t duration;  // seconds, 0 until known
  char title[96];
  char artist[96];
  char album[96];
  char folder[160];
  char file[224];     // full path of the open (or selected) track
  uint32_t version;   // bumped whenever the track or its metadata changes
};

namespace player {
void begin();
void send(PlayerCmd cmd, int32_t arg = 0);
// Plays a file, or the first track of a folder. Either way the folder becomes the playlist.
void playPath(const String &path);
PlayerStatus status();
std::vector<Track> tracks();  // the current folder's MP3s
// Re-reads the current folder, e.g. after an upload or a delete.
void refresh();
// Stops playback if `path` is the open file (or holds it), so it can be changed or deleted.
void release(const String &path);
}
