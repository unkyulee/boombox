#include "ui.h"

#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>

#include "config.h"
#include "player.h"
#include "storage.h"
#include "web.h"

// Fonts come from the U8g2 library; Arduino_GFX can render them directly.
extern "C" {
extern const uint8_t u8g2_font_helvB24_tf[];
extern const uint8_t u8g2_font_helvR18_tf[];
extern const uint8_t u8g2_font_helvB14_tf[];
extern const uint8_t u8g2_font_helvR14_tf[];
extern const uint8_t u8g2_font_helvR10_tf[];
extern const uint8_t u8g2_font_helvB10_tf[];
// Unifont subsets (16 px) for anything the Helvetica fonts cannot show.
extern const uint8_t u8g2_font_unifont_t_korean2[];
extern const uint8_t u8g2_font_unifont_t_japanese3[];
extern const uint8_t u8g2_font_unifont_t_gb2312[];
}

namespace {

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}
constexpr uint16_t COL_BG = rgb(14, 16, 22);
constexpr uint16_t COL_BAR = rgb(26, 29, 38);
constexpr uint16_t COL_BTN = rgb(38, 43, 56);
constexpr uint16_t COL_BTN_DOWN = rgb(70, 78, 98);
constexpr uint16_t COL_ROW = rgb(30, 34, 44);
constexpr uint16_t COL_ACCENT = rgb(0, 168, 255);
constexpr uint16_t COL_ACCENT_DOWN = rgb(0, 120, 190);
constexpr uint16_t COL_TEXT = rgb(240, 242, 245);
constexpr uint16_t COL_DIM = rgb(140, 146, 160);
constexpr uint16_t COL_TRACK = rgb(52, 58, 74);
constexpr uint16_t COL_FOLDER = rgb(255, 196, 80);

struct Rect {
  int16_t x, y, w, h;
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
  int16_t cx() const { return x + w / 2; }
  int16_t cy() const { return y + h / 2; }
};

enum Screen { SCREEN_PLAYER, SCREEN_BROWSER };

// Player screen
enum PlayButton { BTN_PREV, BTN_PLAY, BTN_NEXT, BTN_COUNT };
constexpr int PB_W = 110, PB_H = 58, PB_GAP = 16, PB_Y = 186;
constexpr int PB_X0 = (LCD_WIDTH - (BTN_COUNT * PB_W + (BTN_COUNT - 1) * PB_GAP)) / 2;
constexpr int BAR_X = 24, BAR_W = LCD_WIDTH - 2 * BAR_X, BAR_Y = 150, BAR_H = 6;
constexpr Rect SEEK_AREA = {0, BAR_Y - 20, LCD_WIDTH, 44};
constexpr Rect TRACK_AREA = {0, 0, LCD_WIDTH, 128};  // tapping the track opens the browser
// Status line along the bottom: a hairline, then the text under it.
constexpr int STATUS_Y = 248, STATUS_BASE = STATUS_Y + 18;
constexpr int TEXT_MAX_W = LCD_WIDTH - 32;

// Browser screen
enum BrowseButton { BR_UP, BR_PREV, BR_NEXT, BR_PLAY, BR_CLOSE, BR_COUNT };
constexpr int ROWS = 4, ROW_H = 44, ROW_Y0 = 40;
constexpr int FB_W = 88, FB_H = 48, FB_GAP = 8, FB_Y = 218;
constexpr int FB_X0 = (LCD_WIDTH - (BR_COUNT * FB_W + (BR_COUNT - 1) * FB_GAP)) / 2;

Arduino_DataBus *bus;
Arduino_GFX *lcd;
Arduino_Canvas *gfx;
TAMC_GT911 *touch;

Rect playButtons[BTN_COUNT];
Rect browseButtons[BR_COUNT];
Rect rows[ROWS];

Screen screen = SCREEN_PLAYER;
volatile bool reloadPending = false;
int pressedButton = -1;
int pressedRow = -1;
bool touchDown = false;
bool screenOn = true;
uint32_t lastActive = 0;  // last touch, or last moment something was playing
bool scrubbing = false;
uint32_t scrubSec = 0;

// Browser state
String browsePath = MUSIC_DIR;
std::vector<FsEntry> entries;
int page = 0;

// What was last drawn, to redraw only on change.
PlayerStatus shown;
String shownNet;
Screen shownScreen = SCREEN_BROWSER;
bool dirty = true;
uint32_t fittedVersion = UINT32_MAX;
String fitTitle, fitArtist, fitAlbum;

// The Helvetica fonts only hold Latin-1, so Korean, Japanese and Chinese text is drawn with a
// unifont subset instead. Those subsets include ASCII, so mixed names come out in one piece.
const uint8_t *cjkFontFor(const String &s) {
  bool hangul = false, kana = false, other = false;
  const uint8_t *p = (const uint8_t *)s.c_str();
  while (*p) {
    uint32_t cp = *p;
    int extra = 0;
    if (cp >= 0xF0) { cp &= 0x07; extra = 3; }
    else if (cp >= 0xE0) { cp &= 0x0F; extra = 2; }
    else if (cp >= 0xC0) { cp &= 0x1F; extra = 1; }
    p++;
    while (extra-- > 0 && (*p & 0xC0) == 0x80) cp = (cp << 6) | (*p++ & 0x3F);

    if (cp <= 0xFF) continue;  // Latin-1 is covered by the Helvetica fonts
    if ((cp >= 0xAC00 && cp <= 0xD7FF) || (cp >= 0x1100 && cp <= 0x11FF) ||
        (cp >= 0x3130 && cp <= 0x318F)) hangul = true;
    else if ((cp >= 0x3040 && cp <= 0x30FF) || (cp >= 0xFF66 && cp <= 0xFF9F)) kana = true;
    else other = true;
  }
  if (hangul) return u8g2_font_unifont_t_korean2;
  if (kana) return u8g2_font_unifont_t_japanese3;
  if (other) return u8g2_font_unifont_t_gb2312;
  return nullptr;
}

// Picks the font this string needs and scales unifont up where the Latin font is large.
void applyFont(const String &s, const uint8_t *latin) {
  const uint8_t *cjk = cjkFontFor(s);
  gfx->setFont(cjk ? cjk : latin);
  gfx->setTextSize(cjk && latin == u8g2_font_helvB24_tf ? 2 : 1);
}

int16_t textWidth(const String &s, const uint8_t *latin) {
  applyFont(s, latin);
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(s.c_str(), 0, 100, &x1, &y1, &w, &h);
  return w;
}

// Shortens `s` with "..." until it fits in maxW pixels.
String fit(const String &s, int maxW, const uint8_t *latin) {
  String t = s;
  if (textWidth(t, latin) <= maxW) return t;
  while (t.length()) {
    int i = t.length() - 1;
    while (i > 0 && ((uint8_t)t[i] & 0xC0) == 0x80) i--;  // step back over a whole UTF-8 char
    t.remove(i);
    t.trim();
    if (textWidth(t + "...", latin) <= maxW) break;
  }
  return t + "...";
}

void textAt(const String &s, int x, int baseline, const uint8_t *font, uint16_t color) {
  applyFont(s, font);
  gfx->setTextColor(color);
  gfx->setCursor(x, baseline);
  gfx->print(s);
}

void textCentered(const String &s, int baseline, const uint8_t *font, uint16_t color) {
  textAt(s, (LCD_WIDTH - textWidth(s, font)) / 2, baseline, font, color);
}

void textRight(const String &s, int right, int baseline, const uint8_t *font, uint16_t color) {
  textAt(s, right - textWidth(s, font), baseline, font, color);
}

String clock(uint32_t sec) {
  char buf[12];
  if (sec >= 3600) snprintf(buf, sizeof(buf), "%u:%02u:%02u", sec / 3600, (sec / 60) % 60, sec % 60);
  else snprintf(buf, sizeof(buf), "%u:%02u", sec / 60, sec % 60);
  return buf;
}

// ---- Icons, drawn around a centre point ----

void iconPlay(int cx, int cy, uint16_t c) { gfx->fillTriangle(cx - 9, cy - 13, cx - 9, cy + 13, cx + 13, cy, c); }

void iconPause(int cx, int cy, uint16_t c) {
  gfx->fillRect(cx - 10, cy - 12, 7, 24, c);
  gfx->fillRect(cx + 3, cy - 12, 7, 24, c);
}

void iconSkip(int cx, int cy, bool forward, uint16_t c) {
  int d = forward ? 1 : -1;
  gfx->fillTriangle(cx - 10 * d, cy - 11, cx - 10 * d, cy + 11, cx + 8 * d, cy, c);
  gfx->fillRect(forward ? cx + 8 : cx - 12, cy - 11, 4, 22, c);
}

void iconFolder(int x, int y, uint16_t c) {
  gfx->fillRoundRect(x, y + 2, 24, 16, 3, c);
  gfx->fillRect(x, y, 11, 5, c);
}

void iconNote(int x, int y, uint16_t c) {
  gfx->fillRect(x + 13, y, 3, 14, c);
  gfx->fillRect(x + 13, y, 9, 4, c);
  gfx->fillCircle(x + 10, y + 15, 5, c);
}

void drawButton(const Rect &r, bool down, bool primary, const String &label, const uint8_t *font) {
  uint16_t fill = primary ? (down ? COL_ACCENT_DOWN : COL_ACCENT) : (down ? COL_BTN_DOWN : COL_BTN);
  gfx->fillRoundRect(r.x, r.y, r.w, r.h, 12, fill);
  if (label.length()) textAt(label, r.cx() - textWidth(label, font) / 2, r.cy() + 6, font, COL_TEXT);
}

// ---- Player screen ----

void drawStatus(const String &left, const String &middle, const String &right) {
  gfx->drawFastHLine(0, STATUS_Y, LCD_WIDTH, COL_TRACK);
  textAt(left, 12, STATUS_BASE, u8g2_font_helvB10_tf, COL_DIM);
  textCentered(fit(middle, 220, u8g2_font_helvR10_tf), STATUS_BASE, u8g2_font_helvR10_tf, COL_DIM);
  textRight(right, LCD_WIDTH - 12, STATUS_BASE, u8g2_font_helvR10_tf, COL_DIM);
}

void drawPlayer(const PlayerStatus &s, const String &net) {
  drawStatus(s.count ? String(s.index + 1) + " / " + String(s.count) : String("No tracks"), s.folder, net);

  if (s.count == 0) {
    textCentered("No MP3 files here", 78, u8g2_font_helvB24_tf, COL_TEXT);
    textCentered("Tap to pick another folder,", 110, u8g2_font_helvR14_tf, COL_DIM);
    textCentered("or add files at http://" + net, 132, u8g2_font_helvR14_tf, COL_DIM);
  } else {
    if (fittedVersion != s.version) {
      fitTitle = fit(s.title, TEXT_MAX_W, u8g2_font_helvB24_tf);
      fitArtist = fit(s.artist, TEXT_MAX_W, u8g2_font_helvR18_tf);
      fitAlbum = fit(s.album, TEXT_MAX_W, u8g2_font_helvR14_tf);
      fittedVersion = s.version;
    }
    textCentered(fitTitle, 72, u8g2_font_helvB24_tf, COL_TEXT);
    textCentered(fitArtist, 104, u8g2_font_helvR18_tf, COL_ACCENT);
    textCentered(fitAlbum, 128, u8g2_font_helvR14_tf, COL_DIM);
  }

  uint32_t pos = scrubbing ? scrubSec : s.position;
  float frac = s.duration ? min(1.0f, (float)pos / s.duration) : 0;
  int filled = frac * BAR_W;
  gfx->fillRoundRect(BAR_X, BAR_Y, BAR_W, BAR_H, BAR_H / 2, COL_TRACK);
  if (filled > 0) gfx->fillRoundRect(BAR_X, BAR_Y, max(filled, BAR_H), BAR_H, BAR_H / 2, COL_ACCENT);
  if (s.loaded) gfx->fillCircle(BAR_X + filled, BAR_Y + BAR_H / 2, scrubbing ? 10 : 7, COL_TEXT);
  textAt(clock(pos), BAR_X, BAR_Y + 26, u8g2_font_helvR10_tf, COL_DIM);
  textRight(s.duration ? clock(s.duration) : String("--:--"), BAR_X + BAR_W, BAR_Y + 26,
            u8g2_font_helvR10_tf, COL_DIM);

  for (int i = 0; i < BTN_COUNT; i++) {
    const Rect &r = playButtons[i];
    drawButton(r, i == pressedButton, i == BTN_PLAY, "", nullptr);
    switch (i) {
      case BTN_PREV: iconSkip(r.cx(), r.cy(), false, COL_TEXT); break;
      case BTN_NEXT: iconSkip(r.cx(), r.cy(), true, COL_TEXT); break;
      case BTN_PLAY: s.playing ? iconPause(r.cx(), r.cy(), COL_TEXT) : iconPlay(r.cx(), r.cy(), COL_TEXT); break;
    }
  }
}

// ---- Browser screen ----

void reloadBrowser() {
  storage::Guard guard;
  if (!storage::isDir(browsePath)) browsePath = "/";
  storage::list(browsePath, entries, true);
  int pages = max(1, (int)((entries.size() + ROWS - 1) / ROWS));
  page = constrain(page, 0, pages - 1);
  dirty = true;
}

void drawBrowser(const PlayerStatus &s) {
  textAt(fit(browsePath, 360, u8g2_font_helvB10_tf), 12, 22, u8g2_font_helvB10_tf, COL_TEXT);
  int pages = max(1, (int)((entries.size() + ROWS - 1) / ROWS));
  textRight(String(page + 1) + " / " + String(pages), LCD_WIDTH - 12, 22, u8g2_font_helvR10_tf, COL_DIM);
  gfx->drawFastHLine(0, 32, LCD_WIDTH, COL_TRACK);

  if (entries.empty()) {
    textCentered("Folder is empty", 120, u8g2_font_helvR18_tf, COL_DIM);
  }

  for (int i = 0; i < ROWS; i++) {
    size_t idx = page * ROWS + i;
    if (idx >= entries.size()) break;
    const FsEntry &e = entries[idx];
    const Rect &r = rows[i];
    gfx->fillRoundRect(r.x, r.y, r.w, r.h, 8, i == pressedRow ? COL_BTN_DOWN : COL_ROW);

    String full = storage::join(browsePath, e.name);
    bool isOpen = full == s.file;
    uint16_t ink = e.dir ? COL_TEXT : (isOpen ? COL_ACCENT : COL_TEXT);
    if (e.dir) iconFolder(r.x + 14, r.cy() - 9, COL_FOLDER);
    else iconNote(r.x + 14, r.cy() - 10, isOpen ? COL_ACCENT : COL_DIM);

    textAt(fit(e.name, r.w - 70, u8g2_font_helvR14_tf), r.x + 50, r.cy() + 6, u8g2_font_helvR14_tf, ink);
  }

  const char *labels[BR_COUNT] = {"Up", "<", ">", "Play all", "Close"};
  for (int i = 0; i < BR_COUNT; i++) {
    drawButton(browseButtons[i], i == pressedButton, i == BR_PLAY, labels[i], u8g2_font_helvB14_tf);
  }
}

void draw(const PlayerStatus &s, const String &net) {
  gfx->fillScreen(COL_BG);
  switch (screen) {
    case SCREEN_PLAYER: drawPlayer(s, net); break;
    case SCREEN_BROWSER: drawBrowser(s); break;
  }
  gfx->flush();
}

bool changed(const PlayerStatus &s, const String &net) {
  return dirty || screen != shownScreen || s.version != shown.version ||
         s.position != shown.position || s.duration != shown.duration || s.playing != shown.playing ||
         s.loaded != shown.loaded || s.count != shown.count || s.index != shown.index || net != shownNet;
}

// ---- Touch ----

uint32_t seekSecondsAt(int x, uint32_t duration) {
  float frac = constrain((float)(x - BAR_X) / BAR_W, 0.0f, 1.0f);
  return frac * duration;
}

void openBrowser(const String &path) {
  browsePath = path.length() ? path : String("/");
  page = 0;
  screen = SCREEN_BROWSER;
  reloadBrowser();
}

void pressPlayer(int x, int y) {
  PlayerStatus s = player::status();
  if (TRACK_AREA.contains(x, y)) {
    openBrowser(s.folder[0] ? String(s.folder) : String(MUSIC_DIR));
    return;
  }
  if (SEEK_AREA.contains(x, y) && s.loaded && s.duration) {
    scrubbing = true;
    scrubSec = seekSecondsAt(x, s.duration);
    dirty = true;
    return;
  }
  for (int i = 0; i < BTN_COUNT; i++) {
    if (!playButtons[i].contains(x, y)) continue;
    pressedButton = i;
    dirty = true;
    switch (i) {
      case BTN_PREV: player::send(PlayerCmd::Prev); break;
      case BTN_PLAY: player::send(PlayerCmd::Toggle); break;
      case BTN_NEXT: player::send(PlayerCmd::Next); break;
    }
    return;
  }
}

void pressBrowser(int x, int y) {
  for (int i = 0; i < ROWS; i++) {
    size_t idx = page * ROWS + i;
    if (idx >= entries.size() || !rows[i].contains(x, y)) continue;
    pressedRow = i;
    dirty = true;
    const FsEntry e = entries[idx];
    String full = storage::join(browsePath, e.name);
    if (e.dir) {
      openBrowser(full);
    } else {
      player::playPath(full);
      screen = SCREEN_PLAYER;
    }
    return;
  }
  for (int i = 0; i < BR_COUNT; i++) {
    if (!browseButtons[i].contains(x, y)) continue;
    pressedButton = i;
    dirty = true;
    int pages = max(1, (int)((entries.size() + ROWS - 1) / ROWS));
    switch (i) {
      case BR_UP: openBrowser(storage::parentOf(browsePath)); break;
      case BR_PREV: page = (page + pages - 1) % pages; break;
      case BR_NEXT: page = (page + 1) % pages; break;
      case BR_PLAY:
        player::playPath(browsePath);
        screen = SCREEN_PLAYER;
        break;
      case BR_CLOSE: screen = SCREEN_PLAYER; break;
    }
    return;
  }
}

void onPress(int x, int y) {
  switch (screen) {
    case SCREEN_PLAYER: pressPlayer(x, y); break;
    case SCREEN_BROWSER: pressBrowser(x, y); break;
  }
}

void onDrag(int x, int) {
  if (!scrubbing) return;
  uint32_t sec = seekSecondsAt(x, player::status().duration);
  if (sec != scrubSec) {
    scrubSec = sec;
    dirty = true;
  }
}

void onRelease() {
  if (scrubbing) {
    player::send(PlayerCmd::Seek, scrubSec);
    scrubbing = false;
  }
  pressedButton = -1;
  pressedRow = -1;
  dirty = true;
}

// ---- Screen sleep ----

// Sleeping the panel itself, not only the backlight, so it stops driving the static image.
void sleepScreen() {
  digitalWrite(PIN_LCD_BL, LOW);
  lcd->displayOff();
  screenOn = false;
}

void wakeScreen() {
  lcd->displayOn();
  screenOn = true;
  lastActive = millis();
  dirty = true;  // redrawn in loop() before anything shows; the backlight comes on there
}

void pollTouch() {
  touch->read();
  bool down = touch->isTouched && touch->touches > 0;
  if (down) {
    int x = touch->points[0].x, y = touch->points[0].y;
    if (TOUCH_FLIP_X) x = LCD_WIDTH - 1 - x;
    if (TOUCH_FLIP_Y) y = LCD_HEIGHT - 1 - y;
    if (TOUCH_DEBUG) Serial.printf("[touch] %d,%d\n", x, y);
    lastActive = millis();
    if (!touchDown && !screenOn) wakeScreen();  // the waking tap presses nothing
    else if (!touchDown) onPress(x, y);
    else onDrag(x, y);
  } else if (touchDown) {
    onRelease();
  }
  touchDown = down;
}

}  // namespace

namespace ui {

void begin() {
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  bus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);
  lcd = new Arduino_NV3041A(bus, GFX_NOT_DEFINED, 0 /* rotation */, true /* IPS */);
  gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, lcd);
  if (!gfx->begin()) Serial.println("[ui] display init failed");
  gfx->setUTF8Print(true);
  gfx->setTextWrap(false);

  touch = new TAMC_GT911(PIN_TOUCH_SDA, PIN_TOUCH_SCL, PIN_TOUCH_INT, PIN_TOUCH_RST, LCD_WIDTH, LCD_HEIGHT);
  touch->begin();
  touch->setRotation(ROTATION_INVERTED);  // this driver's "inverted" is the panel's native orientation

  for (int i = 0; i < BTN_COUNT; i++) {
    playButtons[i] = {(int16_t)(PB_X0 + i * (PB_W + PB_GAP)), PB_Y, PB_W, PB_H};
  }
  for (int i = 0; i < BR_COUNT; i++) {
    browseButtons[i] = {(int16_t)(FB_X0 + i * (FB_W + FB_GAP)), FB_Y, FB_W, FB_H};
  }
  for (int i = 0; i < ROWS; i++) {
    rows[i] = {8, (int16_t)(ROW_Y0 + i * ROW_H), LCD_WIDTH - 16, ROW_H - 4};
  }
}

void message(const char *line1, const char *line2) {
  gfx->fillScreen(COL_BG);
  textCentered(line1, 128, u8g2_font_helvB24_tf, COL_TEXT);
  textCentered(line2, 162, u8g2_font_helvR14_tf, COL_DIM);
  gfx->flush();
}

// Called from the web task, so it only raises a flag; the UI task does the reading.
void refresh() { reloadPending = true; }

void loop() {
  pollTouch();

  if (reloadPending) {
    reloadPending = false;
    if (screen == SCREEN_BROWSER) reloadBrowser();
    dirty = true;
  }

  PlayerStatus s = player::status();
  if (s.playing) {
    lastActive = millis();
    if (!screenOn) wakeScreen();  // e.g. playback started from the web page
  }
  if (screenOn && SCREEN_OFF_MS && millis() - lastActive > SCREEN_OFF_MS) sleepScreen();
  if (!screenOn) return;

  String net = web::label();
  if (changed(s, net)) {
    draw(s, net);
    shown = s;
    shownNet = net;
    shownScreen = screen;
    dirty = false;
    digitalWrite(PIN_LCD_BL, HIGH);  // after the draw, so waking never shows the stale frame
  }
}

}  // namespace ui
