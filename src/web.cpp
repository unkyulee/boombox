#include "web.h"

#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <SD.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiMulti.h>
#include <lwip/sockets.h>

#include "config.h"
#include "player.h"
#include "storage.h"
#include "ui.h"
#include "web_page.h"

namespace {

WebServer server(80);
WiFiMulti multi;
SemaphoreHandle_t labelMutex;
String netLabel = "Wi-Fi...";
bool apMode = false;

void setLabel(const String &s) {
  xSemaphoreTake(labelMutex, portMAX_DELAY);
  netLabel = s;
  xSemaphoreGive(labelMutex);
}

void sendJson(int code, const JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

void sendError(int code, const String &msg) {
  StaticJsonDocument<256> doc;
  doc["error"] = msg;
  sendJson(code, doc);
}

void sendOk() {
  StaticJsonDocument<64> doc;
  doc["ok"] = true;
  sendJson(200, doc);
}

// Every file operation goes through here: rejects bad paths and refuses while the card
// belongs to a computer.
bool takePath(const char *arg, String &path) {
  path = storage::cleanPath(server.arg(arg));
  if (!path.length()) {
    sendError(400, "bad path");
    return false;
  }
  return true;
}

String cleanName(String name) {
  name = name.substring(max(name.lastIndexOf('/'), name.lastIndexOf('\\')) + 1);
  for (size_t i = 0; i < name.length(); i++) {
    if (strchr(":*?\"<>|", name[i]) || (uint8_t)name[i] < 0x20) name.setCharAt(i, '_');
  }
  name.trim();
  return name;
}

void filesChanged() {
  player::refresh();
  ui::refresh();
}

// ---- Wi-Fi networks, stored as JSON on the card ----

void loadNetworks(DynamicJsonDocument &doc) {
  doc.to<JsonArray>();
  File f = SD.open(WIFI_FILE);
  if (f) {
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (!err && doc.is<JsonArray>()) return;
    doc.to<JsonArray>();
  }
  // One-time import of the old two-line file.
  File old = SD.open(WIFI_FILE_OLD);
  if (old) {
    String ssid = old.readStringUntil('\n');
    String pass = old.readStringUntil('\n');
    old.close();
    ssid.trim();
    if (pass.endsWith("\r")) pass.remove(pass.length() - 1);
    if (ssid.length()) {
      JsonObject net = doc.createNestedObject();
      net["ssid"] = ssid;
      net["pass"] = pass;
    }
  }
}

// Returns an empty string on success, otherwise what went wrong.
String saveNetworks(const JsonDocument &doc) {
  storage::Guard guard;
  if (!storage::ensureMounted()) return "the SD card is not mounted";

  if (!SD.exists("/config") && !SD.mkdir("/config")) return "could not create /config on the card";
  File f = SD.open(WIFI_FILE, FILE_WRITE);
  if (!f) return "could not open " WIFI_FILE " for writing";

  size_t written = serializeJson(doc, f);
  f.close();
  if (!written) return "nothing was written to " WIFI_FILE;

  Serial.printf("[web] saved %u bytes to %s\n", (unsigned)written, WIFI_FILE);
  return "";
}

void startAp() {
  apMode = true;
  WiFi.mode(WIFI_AP_STA);  // STA stays on so the settings page can scan for networks
  WiFi.softAP(AP_SSID, AP_PASS);
  setLabel(WiFi.softAPIP().toString());
}

void connect() {
  WiFi.setHostname(HOSTNAME);
  DynamicJsonDocument doc(2048);
  loadNetworks(doc);

  int count = 0;
  for (JsonObject net : doc.as<JsonArray>()) {
    const char *ssid = net["ssid"];
    if (ssid && strlen(ssid)) {
      multi.addAP(ssid, net["pass"] | "");
      count++;
    }
  }
  if (count == 0) {
    Serial.println("[web] no saved networks, starting access point");
    startAp();
    return;
  }

  ui::message("Connecting to Wi-Fi", count == 1 ? (const char *)doc[0]["ssid"] : "saved networks");
  WiFi.mode(WIFI_STA);
  if (multi.run(WIFI_TIMEOUT_MS) == WL_CONNECTED) {
    apMode = false;
    WiFi.setSleep(false);  // modem sleep makes the web UI sluggish
    setLabel(WiFi.localIP().toString());
    return;
  }
  Serial.println("[web] could not join any saved network, starting access point");
  startAp();
}

// ---- Player ----

void handleStatus() {
  PlayerStatus s = player::status();
  DynamicJsonDocument doc(1024);
  doc["loaded"] = s.loaded;
  doc["playing"] = s.playing;
  doc["volume"] = s.volume;
  doc["volumeMax"] = VOLUME_MAX;
  doc["index"] = s.index;
  doc["count"] = s.count;
  doc["position"] = s.position;
  doc["duration"] = s.duration;
  doc["title"] = s.title;
  doc["artist"] = s.artist;
  doc["album"] = s.album;
  doc["folder"] = s.folder;
  doc["file"] = s.file;
  sendJson(200, doc);
}

void handleCommand() {
  String c = server.arg("c");
  int32_t v = server.arg("v").toInt();
  if (c == "toggle") player::send(PlayerCmd::Toggle);
  else if (c == "play") player::send(PlayerCmd::Play, v);
  else if (c == "stop") player::send(PlayerCmd::Stop);
  else if (c == "next") player::send(PlayerCmd::Next);
  else if (c == "prev") player::send(PlayerCmd::Prev);
  else if (c == "seek") player::send(PlayerCmd::Seek, v);
  else if (c == "volume") player::send(PlayerCmd::SetVolume, v);
  else return sendError(400, "unknown command");
  sendOk();
}

// Plays a file, or a folder from its first track.
void handlePlayPath() {
  String path;
  if (!takePath("path", path)) return;
  if (!storage::exists(path)) return sendError(404, "no such file");
  player::playPath(path);
  sendOk();
}

// ---- File system ----

void handleList() {
  String path;
  if (!takePath("path", path)) return;

  std::vector<FsEntry> entries;
  storage::Guard guard;
  if (!storage::list(path, entries)) return sendError(404, "no such folder");

  DynamicJsonDocument doc(16384);
  doc["path"] = path;
  doc["parent"] = storage::parentOf(path);
  doc["total"] = storage::totalBytes();
  doc["used"] = storage::usedBytes();
  JsonArray items = doc.createNestedArray("entries");
  for (const FsEntry &e : entries) {
    JsonObject o = items.createNestedObject();
    o["name"] = e.name;
    o["dir"] = e.dir;
    o["size"] = e.size;
    if (doc.overflowed()) break;  // very large folders are truncated rather than dropped
  }
  doc["truncated"] = doc.overflowed();
  sendJson(200, doc);
}

void handleMkdir() {
  String path;
  if (!takePath("path", path)) return;
  storage::Guard guard;
  if (storage::exists(path)) return sendError(409, "already exists");
  if (!storage::mkdir(path)) return sendError(500, "could not create the folder");
  filesChanged();
  sendOk();
}

void handleDelete() {
  String path;
  if (!takePath("path", path)) return;
  if (path == "/") return sendError(400, "bad path");
  if (!storage::exists(path)) return sendError(404, "no such file");
  player::release(path);
  {
    storage::Guard guard;
    if (!storage::remove(path)) return sendError(500, "could not delete");
  }
  filesChanged();
  sendOk();
}

// Renames, and moves when `to` is in another folder.
void handleRename() {
  String path, to;
  if (!takePath("path", path)) return;
  to = storage::cleanPath(server.arg("to"));
  if (!to.length() || to == "/") return sendError(400, "bad destination");
  if (!storage::exists(path)) return sendError(404, "no such file");
  if (storage::exists(to)) return sendError(409, "the destination already exists");
  player::release(path);
  {
    storage::Guard guard;
    if (!storage::rename(path, to)) return sendError(500, "could not move");
  }
  filesChanged();
  sendOk();
}

// ---- Uploads ----
//
// WebServer drops an upload when the data pauses for more than a second, which over Wi-Fi is
// all but certain somewhere in a large file. So the page sends a file as a series of chunks,
// each its own POST, and a dropped chunk is simply sent again. Query parameters:
//   path    the folder                     offset  where this chunk starts in the file
//   total   size of the whole file
// Without offset and total the upload is the whole file, which suits curl -F.
// The file grows as "<name>.part" and takes its real name when the last chunk lands.
//
// The page sends its chunks to the fast upload server below; /api/fs/upload takes the same
// chunks as multipart forms, for curl.

struct Upload {
  File file;
  String target, part, error;
  int code = 200;
  uint32_t offset = 0, total = 0, received = 0;
  int32_t have = -1;  // on an out-of-step chunk, how much of the file the card already holds
};

void uploadFail(Upload &u, int code, const String &msg) {
  if (u.file) u.file.close();
  if (!u.error.length()) {
    u.code = code;
    u.error = msg;
    Serial.printf("[web] upload %s: %s\n", u.target.c_str(), msg.c_str());
  }
}

void uploadStart(Upload &u, const String &dirArg, const String &nameArg, uint32_t offset, uint32_t total) {
  u.error = "";
  u.code = 200;
  u.received = 0;
  u.have = -1;
  u.offset = offset;
  u.total = total;

  String dir = storage::cleanPath(dirArg);
  String name = cleanName(nameArg);
  u.target = storage::join(dir, name);
  u.part = u.target + ".part";
  if (!dir.length() || !name.length()) return uploadFail(u, 400, "bad path");
  if (!storage::isDir(dir)) return uploadFail(u, 404, "no such folder");

  if (u.offset == 0) {
    uint64_t free = storage::totalBytes() - storage::usedBytes();
    if (u.total > free) {
      return uploadFail(u, 507, "not enough space on the card (" + String((uint32_t)(free >> 20)) + " MB free)");
    }
    if (SD.exists(u.part)) SD.remove(u.part);
    u.file = SD.open(u.part, FILE_WRITE);
  } else {
    // A later chunk. A retried chunk may have partly landed already, so write from its offset.
    File f = SD.open(u.part);
    u.have = f ? (int32_t)f.size() : 0;
    if (f) f.close();
    if ((uint32_t)u.have < u.offset) return uploadFail(u, 409, "out of step with the card");
    u.file = SD.open(u.part, "r+");
    if (u.file && !u.file.seek(u.offset)) u.file.close();
  }
  if (!u.file) return uploadFail(u, 500, "cannot write " + u.part);
  u.file.setBufferSize(16384);  // fewer, larger card writes
}

void uploadWrite(Upload &u, const uint8_t *data, size_t len) {
  if (!u.file) return;
  if (u.file.write(data, len) != len) return uploadFail(u, 500, "write failed (card full?)");
  u.received += len;
}

void uploadFinish(Upload &u) {
  u.file.close();
  if (u.offset + u.received < u.total) return;  // more chunks to come

  player::release(u.target);
  if (SD.exists(u.target)) SD.remove(u.target);
  if (!SD.rename(u.part, u.target)) uploadFail(u, 500, "could not rename " + u.part);
}

// Fills in the reply to a chunk and returns its status code.
int uploadReply(const Upload &u, JsonDocument &doc) {
  if (u.error.length()) {
    doc["error"] = u.error;
    if (u.have >= 0) doc["size"] = u.have;
    return u.code;
  }
  if (u.offset + u.received >= u.total) filesChanged();
  doc["ok"] = true;
  return 200;
}

Upload formUpload;

void handleUploadData() {
  HTTPUpload &up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START: {
      // Without `total`, the request size is a close enough upper bound for the space check.
      uint32_t offset = strtoul(server.arg("offset").c_str(), nullptr, 10);
      uint32_t total = server.hasArg("total") ? strtoul(server.arg("total").c_str(), nullptr, 10)
                                              : offset + server.clientContentLength();
      uploadStart(formUpload, server.arg("path"), up.filename, offset, total);
      break;
    }
    case UPLOAD_FILE_WRITE:
      uploadWrite(formUpload, up.buf, up.currentSize);
      break;
    case UPLOAD_FILE_END:
      if (!formUpload.file) break;
      if (!server.hasArg("total")) formUpload.total = formUpload.offset + formUpload.received;  // whole file in one go
      uploadFinish(formUpload);
      break;
    case UPLOAD_FILE_ABORTED:
      // No reply reaches the page; it retries this chunk, and the .part file stays for that.
      uploadFail(formUpload, 500, "upload interrupted");
      break;
  }
}

void handleUploadDone() {
  StaticJsonDocument<256> doc;
  int code = uploadReply(formUpload, doc);
  sendJson(code, doc);
}

// ---- Fast upload server ----
//
// WebServer reads a multipart upload one byte at a time and sleeps 2 ms whenever its small
// receive buffer runs dry, which held uploads to about 40 KB/s. This small server on
// UPLOAD_PORT takes each chunk as a plain request body instead, and reads the socket in large
// blocks straight onto the card. POST /upload with the query parameters above, plus `name`.
// It is another port, so another origin to the browser: replies carry CORS headers.

const size_t FAST_BUF = 16384;

String percentDecode(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == '%' && i + 2 < s.length()) {
      out += (char)strtol(s.substring(i + 1, i + 3).c_str(), nullptr, 16);
      i += 2;
    } else {
      out += s[i] == '+' ? ' ' : s[i];
    }
  }
  return out;
}

String queryArg(const String &query, const String &key) {
  for (int start = 0; start < (int)query.length();) {
    int end = query.indexOf('&', start);
    if (end < 0) end = query.length();
    int eq = query.indexOf('=', start);
    if (eq > start && eq < end && query.substring(start, eq) == key) {
      return percentDecode(query.substring(eq + 1, end));
    }
    start = end + 1;
  }
  return "";
}

void fastSend(int fd, int code, const String &body) {
  String out = "HTTP/1.1 " + String(code) + (code < 300 ? " OK" : " Error") +
               "\r\nAccess-Control-Allow-Origin: *"
               "\r\nAccess-Control-Allow-Methods: POST"
               "\r\nAccess-Control-Allow-Headers: Content-Type"
               "\r\nAccess-Control-Allow-Private-Network: true"
               "\r\nAccess-Control-Max-Age: 86400"
               "\r\nContent-Type: application/json"
               "\r\nContent-Length: " + String(body.length()) +
               "\r\nConnection: close\r\n\r\n" + body;
  for (size_t sent = 0; sent < out.length();) {
    int n = send(fd, out.c_str() + sent, out.length() - sent, 0);
    if (n <= 0) return;
    sent += n;
  }
}

void fastServe(int fd, uint8_t *buf) {
  // The headers, which may arrive together with the start of the body.
  size_t have = 0, headEnd = 0;
  while (!headEnd) {
    if (have == FAST_BUF) return;
    int n = recv(fd, buf + have, FAST_BUF - have, 0);
    if (n <= 0) return;
    for (size_t i = have < 3 ? 3 : have; i < have + n && !headEnd; i++) {
      if (!memcmp(buf + i - 3, "\r\n\r\n", 4)) headEnd = i + 1;
    }
    have += n;
  }
  uint8_t keep = buf[headEnd];
  buf[headEnd] = 0;  // buf has a spare byte past FAST_BUF for this
  String head = (const char *)buf;
  buf[headEnd] = keep;

  int sp1 = head.indexOf(' '), sp2 = head.indexOf(' ', sp1 + 1);
  String method = head.substring(0, sp1), target = head.substring(sp1 + 1, sp2);
  head.toLowerCase();
  int cl = head.indexOf("\ncontent-length:");
  uint32_t length = cl >= 0 ? strtoul(head.c_str() + cl + 16, nullptr, 10) : 0;

  if (method == "OPTIONS") return fastSend(fd, 204, "");  // the browser's CORS preflight
  int q = target.indexOf('?');
  if (method != "POST" || target.substring(0, q) != "/upload") return fastSend(fd, 404, "{\"error\":\"not found\"}");
  String query = q >= 0 ? target.substring(q + 1) : "";

  Upload u;
  uint32_t offset = strtoul(queryArg(query, "offset").c_str(), nullptr, 10);
  String total = queryArg(query, "total");
  uploadStart(u, queryArg(query, "path"), queryArg(query, "name"), offset,
              total.length() ? strtoul(total.c_str(), nullptr, 10) : offset + length);

  // The whole body is read even after an error, so the browser gets the reply rather than a
  // reset connection.
  size_t first = min((size_t)length, have - headEnd);
  uploadWrite(u, buf + headEnd, first);
  for (uint32_t left = length - first; left;) {
    int n = recv(fd, buf, min((size_t)left, FAST_BUF), 0);
    if (n <= 0) return uploadFail(u, 500, "upload interrupted");  // the page retries the chunk
    uploadWrite(u, buf, n);
    left -= n;
  }
  if (u.file) uploadFinish(u);

  StaticJsonDocument<256> doc;
  int code = uploadReply(u, doc);
  String body;
  serializeJson(doc, body);
  fastSend(fd, code, body);
}

void fastTask(void *) {
  uint8_t *buf = (uint8_t *)malloc(FAST_BUF + 1);
  int srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(UPLOAD_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (!buf || srv < 0 || bind(srv, (sockaddr *)&addr, sizeof(addr)) || listen(srv, 2)) {
    Serial.println("[web] could not start the upload server");
    vTaskDelete(nullptr);
  }
  for (;;) {
    int fd = accept(srv, nullptr, nullptr);
    if (fd < 0) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    timeval tv = {10, 0};  // Wi-Fi hiccups longer than this cost a retried chunk
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    fastServe(fd, buf);
    close(fd);
  }
}

void handleDownload() {
  String path;
  if (!takePath("path", path)) return;
  File f = SD.open(path);
  if (!f || f.isDirectory()) return sendError(404, "no such file");
  String name = path.substring(path.lastIndexOf('/') + 1);
  name.replace("\"", "");
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.streamFile(f, storage::isMp3(name) ? "audio/mpeg" : "application/octet-stream");
  f.close();
}

// ---- Fetching a file from a URL ----
//
// The player downloads it itself, on its own task, so the browser does not sit waiting.
// Progress is polled through /api/fs/fetch.

struct Fetch {
  volatile bool active = false;
  volatile bool done = false;
  volatile uint32_t received = 0;
  volatile int32_t total = -1;  // -1 when the server does not say
  String url, folder, name, error;
} fetchJob;

// Picks a file name from the URL, and steps aside if that name is taken.
String fetchName(const String &url, const String &folder) {
  String name = url;
  int q = name.indexOf('?');
  if (q >= 0) name = name.substring(0, q);
  name = name.substring(name.lastIndexOf('/') + 1);

  String decoded;  // percent-decoding, so "%ED%95%9C.mp3" lands as its real name
  for (size_t i = 0; i < name.length(); i++) {
    if (name[i] == '%' && i + 2 < name.length()) {
      decoded += (char)strtol(name.substring(i + 1, i + 3).c_str(), nullptr, 16);
      i += 2;
    } else {
      decoded += name[i];
    }
  }
  decoded = cleanName(decoded);
  if (!decoded.length()) decoded = "download.mp3";

  String base = decoded, ext;
  int dot = decoded.lastIndexOf('.');
  if (dot > 0) {
    base = decoded.substring(0, dot);
    ext = decoded.substring(dot);
  }
  String candidate = decoded;
  for (int n = 2; storage::exists(storage::join(folder, candidate)) && n < 100; n++) {
    candidate = base + " (" + String(n) + ")" + ext;
  }
  return candidate;
}

void fetchTask(void *) {
  String target = storage::join(fetchJob.folder, fetchJob.name);
  WiFiClient plain;
  WiFiClientSecure secure;
  secure.setInsecure();  // no certificate check: this only fetches files onto the card
  bool https = fetchJob.url.startsWith("https:");

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  http.setUserAgent("mp3-player");

  if (!http.begin(https ? (WiFiClient &)secure : plain, fetchJob.url)) {
    fetchJob.error = "bad URL";
  } else {
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
      fetchJob.error = code > 0 ? "server said " + String(code) : "could not connect";
    } else {
      fetchJob.total = http.getSize();
      player::release(target);
      File out = SD.open(target, FILE_WRITE);
      if (!out) {
        fetchJob.error = "cannot write " + target;
      } else {
        WiFiClient *stream = http.getStreamPtr();
        uint8_t buf[2048];
        uint32_t lastData = millis();
        while (http.connected() && (fetchJob.total < 0 || fetchJob.received < (uint32_t)fetchJob.total)) {
          size_t avail = stream->available();
          if (avail) {
            int n = stream->readBytes(buf, min(avail, sizeof(buf)));
            if (n <= 0 || out.write(buf, n) != (size_t)n) {
              fetchJob.error = "write failed (card full?)";
              break;
            }
            fetchJob.received += n;
            lastData = millis();
          } else {
            if (!stream->connected()) break;
            if (millis() - lastData > 20000) {
              fetchJob.error = "the download stalled";
              break;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
          }
        }
        out.close();
        if (!fetchJob.error.length() && fetchJob.total > 0 &&
            fetchJob.received < (uint32_t)fetchJob.total) {
          fetchJob.error = "the download was cut short";
        }
        if (fetchJob.error.length()) SD.remove(target);
      }
    }
  }
  http.end();

  Serial.printf("[web] fetch %s -> %s (%u bytes) %s\n", fetchJob.url.c_str(), target.c_str(),
                (unsigned)fetchJob.received, fetchJob.error.length() ? fetchJob.error.c_str() : "ok");
  filesChanged();
  fetchJob.done = true;
  fetchJob.active = false;
  vTaskDelete(nullptr);
}

void handleFetchStart() {
  String folder;
  if (!takePath("path", folder)) return;
  if (!storage::isDir(folder)) return sendError(404, "no such folder");
  if (fetchJob.active) return sendError(409, "a download is already running");

  String url = server.arg("url");
  url.trim();
  if (!url.startsWith("http://") && !url.startsWith("https://")) {
    return sendError(400, "the URL must start with http:// or https://");
  }

  fetchJob.url = url;
  fetchJob.folder = folder;
  fetchJob.name = server.arg("name").length() ? cleanName(server.arg("name")) : fetchName(url, folder);
  fetchJob.error = "";
  fetchJob.received = 0;
  fetchJob.total = -1;
  fetchJob.done = false;
  fetchJob.active = true;

  // Idle priority, like the web task, so downloading cannot starve core 0's idle task.
  if (xTaskCreatePinnedToCore(fetchTask, "fetch", 8192, nullptr, tskIDLE_PRIORITY, nullptr, 0) != pdPASS) {
    fetchJob.active = false;
    return sendError(500, "could not start the download");
  }
  StaticJsonDocument<128> doc;
  doc["ok"] = true;
  doc["name"] = fetchJob.name;
  sendJson(200, doc);
}

void handleFetchStatus() {
  StaticJsonDocument<256> doc;
  doc["active"] = fetchJob.active;
  doc["done"] = fetchJob.done;
  doc["name"] = fetchJob.name;
  doc["received"] = fetchJob.received;
  doc["total"] = fetchJob.total;
  doc["error"] = fetchJob.error;
  sendJson(200, doc);
}

// ---- Settings ----

void handleWifiGet() {
  DynamicJsonDocument saved(2048);
  loadNetworks(saved);

  DynamicJsonDocument doc(2560);
  doc["mode"] = apMode ? "ap" : "sta";
  doc["ssid"] = apMode ? AP_SSID : WiFi.SSID();
  doc["ip"] = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  doc["hostname"] = HOSTNAME;
  doc["connected"] = WiFi.status() == WL_CONNECTED;
  JsonArray list = doc.createNestedArray("saved");
  for (JsonObject net : saved.as<JsonArray>()) list.add(net["ssid"]);
  sendJson(200, doc);
}

void handleWifiScan() {
  int found = WiFi.scanNetworks();
  DynamicJsonDocument doc(4096);
  JsonArray list = doc.to<JsonArray>();
  for (int i = 0; i < found && !doc.overflowed(); i++) {
    JsonObject o = list.createNestedObject();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(200, doc);
}

void handleWifiAdd() {
  String ssid = server.arg("ssid");
  if (!ssid.length()) return sendError(400, "SSID is required");

  DynamicJsonDocument doc(2048);
  loadNetworks(doc);
  JsonArray nets = doc.as<JsonArray>();
  for (JsonObject net : nets) {
    if (ssid == (const char *)net["ssid"]) {
      net["pass"] = server.arg("pass");  // updating the password of a known network
      String err = saveNetworks(doc);
      if (err.length()) return sendError(500, err);
      return sendOk();
    }
  }
  JsonObject net = nets.createNestedObject();
  net["ssid"] = ssid;
  net["pass"] = server.arg("pass");
  String err = saveNetworks(doc);
  if (err.length()) return sendError(500, err);
  sendOk();
}

void handleWifiRemove() {
  String ssid = server.arg("ssid");

  DynamicJsonDocument doc(2048);
  loadNetworks(doc);
  JsonArray nets = doc.as<JsonArray>();
  for (size_t i = 0; i < nets.size(); i++) {
    if (ssid == (const char *)nets[i]["ssid"]) {
      nets.remove(i);
      String err = saveNetworks(doc);
      if (err.length()) return sendError(500, err);
      return sendOk();
    }
  }
  sendError(404, "not a saved network");
}

void handleRestart() {
  sendOk();
  delay(500);
  ESP.restart();
}

// ---- Firmware update ----
//
// The new image goes into the other app slot. Only once it is complete and verified does the
// boot slot switch, so a failed or interrupted upload leaves the running firmware in place.

String firmwareError;

void handleFirmwareInfo() {
  StaticJsonDocument<256> doc;
  doc["built"] = __DATE__ " " __TIME__;
  doc["size"] = ESP.getSketchSize();
  doc["max"] = ESP.getFreeSketchSpace();
  doc["sdk"] = ESP.getSdkVersion();
  sendJson(200, doc);
}

void handleFirmwareData() {
  HTTPUpload &up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      firmwareError = "";
      Serial.printf("[web] firmware update: %s\n", up.filename.c_str());
      player::send(PlayerCmd::Stop);  // flash writes stall SD reads, so playback would stutter
      if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) firmwareError = Update.errorString();
      break;
    case UPLOAD_FILE_WRITE:
      if (!firmwareError.length() && Update.write(up.buf, up.currentSize) != up.currentSize) {
        firmwareError = Update.errorString();  // also catches a file that is not an ESP32 image
        Update.abort();
      }
      break;
    case UPLOAD_FILE_END:
      if (!firmwareError.length() && !Update.end(true)) firmwareError = Update.errorString();
      break;
    case UPLOAD_FILE_ABORTED:
      Update.abort();
      firmwareError = "upload aborted";
      break;
  }
}

void handleFirmwareDone() {
  if (!firmwareError.length() && !Update.isFinished()) firmwareError = "no firmware file received";
  if (firmwareError.length()) {
    Serial.printf("[web] firmware update failed: %s\n", firmwareError.c_str());
    return sendError(400, firmwareError);
  }
  Serial.println("[web] firmware update done, restarting");
  sendOk();
  delay(500);
  ESP.restart();
}

// ---- Task ----

void updateLabel() {
  if (apMode) return;
  setLabel(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("Wi-Fi lost"));
}

void webTask(void *) {
  uint32_t lastLabel = 0;
  for (;;) {
    server.handleClient();
    if (millis() - lastLabel > 2000) {
      lastLabel = millis();
      updateLabel();
    }
    vTaskDelay(2);
  }
}

}  // namespace

namespace web {

void begin() {
  labelMutex = xSemaphoreCreateMutex();
  connect();
  Serial.printf("[web] http://%s\n", label().c_str());

  if (MDNS.begin(HOSTNAME)) MDNS.addService("http", "tcp", 80);

  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", INDEX_HTML); });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/cmd", HTTP_POST, handleCommand);
  server.on("/api/play", HTTP_POST, handlePlayPath);

  server.on("/api/fs/list", HTTP_GET, handleList);
  server.on("/api/fs/mkdir", HTTP_POST, handleMkdir);
  server.on("/api/fs/delete", HTTP_POST, handleDelete);
  server.on("/api/fs/rename", HTTP_POST, handleRename);
  server.on("/api/fs/upload", HTTP_POST, handleUploadDone, handleUploadData);
  server.on("/api/fs/download", HTTP_GET, handleDownload);
  server.on("/api/fs/fetch", HTTP_POST, handleFetchStart);
  server.on("/api/fs/fetch", HTTP_GET, handleFetchStatus);

  server.on("/api/wifi", HTTP_GET, handleWifiGet);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/wifi/add", HTTP_POST, handleWifiAdd);
  server.on("/api/wifi/remove", HTTP_POST, handleWifiRemove);
  server.on("/api/restart", HTTP_POST, handleRestart);
  server.on("/api/firmware", HTTP_GET, handleFirmwareInfo);
  server.on("/api/firmware", HTTP_POST, handleFirmwareDone, handleFirmwareData);

  server.onNotFound([] { sendError(404, "not found"); });
  server.begin();

  // Idle priority on purpose: WebServer parses a whole upload inside one handleClient() call
  // without blocking, which at any higher priority starves IDLE0 and trips the task watchdog.
  // At equal priority FreeRTOS time-slices the two, so the watchdog keeps getting fed.
  xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, tskIDLE_PRIORITY, nullptr, 0);
  // The upload server blocks on its socket rather than polling, so it can sit above idle.
  xTaskCreatePinnedToCore(fastTask, "upload", 8192, nullptr, 1, nullptr, 0);
}

String label() {
  xSemaphoreTake(labelMutex, portMAX_DELAY);
  String s = netLabel;
  xSemaphoreGive(labelMutex);
  return s;
}

}  // namespace web
