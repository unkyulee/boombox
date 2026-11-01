#include <Arduino.h>
#include <SD.h>

#include "config.h"
#include "player.h"
#include "storage.h"
#include "ui.h"
#include "web.h"

void setup() {
  Serial.begin(115200);
  ui::begin();
  ui::message("MP3 Player", "Starting...");

  while (!storage::begin()) {
    ui::message("No SD card", "Insert a FAT32 micro SD card");
    delay(2000);
  }
  if (!SD.exists(MUSIC_DIR)) SD.mkdir(MUSIC_DIR);

  player::begin();  // before the web server, which calls into it
  web::begin();
}

void loop() {
  ui::loop();
  delay(15);
}
