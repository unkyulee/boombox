#pragma once

// Pin map for the Guition JC4827W543C (capacitive touch version).
// If something does not work, check these against the schematic that ships with the board.

// Display: NV3041A, 480x272, QSPI
#define PIN_LCD_CS    45
#define PIN_LCD_SCK   47
#define PIN_LCD_D0    21
#define PIN_LCD_D1    48
#define PIN_LCD_D2    40
#define PIN_LCD_D3    39
#define PIN_LCD_BL    1
#define LCD_WIDTH     480
#define LCD_HEIGHT    272

// Touch: GT911 over I2C
#define PIN_TOUCH_SDA 8
#define PIN_TOUCH_SCL 4
#define PIN_TOUCH_INT 3
#define PIN_TOUCH_RST 38
// If taps land in the wrong place, flip these (1 = on). Enable TOUCH_DEBUG to print raw points.
#define TOUCH_FLIP_X  0
#define TOUCH_FLIP_Y  0
#define TOUCH_DEBUG   0

// The screen sleeps after this long without playback or touches, to avoid burn-in.
// A tap wakes it (that tap only wakes it, it does not press anything), and so does playback
// starting from the web page. 0 keeps the screen on.
#define SCREEN_OFF_MS (10UL * 60 * 1000)

// Micro SD card: SPI
#define PIN_SD_CS     10
#define PIN_SD_MOSI   11
#define PIN_SD_SCK    12
#define PIN_SD_MISO   13
#define SD_SPI_FREQ   20000000   // lower to 4000000 if the card drops out; MP3 playback needs far less

// Audio out: external PCM5102A DAC over I2S (stereo, 3.5 mm jack).
// For the board's own NS4168 speaker amplifier instead, use BCLK 42, LRCK 2, DOUT 41.
#define PIN_I2S_BCLK  9    // DAC: BCK
#define PIN_I2S_LRCK  5    // DAC: LCK
#define PIN_I2S_DOUT  14   // DAC: DIN

// Player. A folder is the playlist: playing a file plays the rest of its folder after it.
#define MUSIC_DIR       "/music"   // created at first boot, and where the player starts
#define VOLUME_MAX      19         // ESP32-audioI2S volume range is 0..21
#define VOLUME_DEFAULT  19         // line level for the DAC; the speaker has its own volume knob
#define REPEAT_ALL      true       // after the last track in a folder, start again from the first

// Wi-Fi. Networks are stored on the SD card in WIFI_FILE, and managed from the web page.
// Until one joins, the player starts its own access point.
#define WIFI_FILE       "/config/wifi.json"
#define WIFI_FILE_OLD   "/wifi.txt"   // imported once, if present
#define WIFI_TIMEOUT_MS 15000
#define AP_SSID         "MP3-Player"
#define AP_PASS         "mp3player"   // at least 8 characters
#define HOSTNAME        "mp3player"   // http://mp3player.local
