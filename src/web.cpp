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
// each its own multipart POST, and a dropped chunk is simply sent again. Query parameters:
//   path    the folder                     offset  where this chunk starts in the file
//   total   size of the whole file
// Without offset and total the upload is the whole file, which suits curl -F.
// The file grows as "<name>.part" and takes its real name when the last chunk lands.

struct Upload {
  File file;
  String target, part, error;
  int code = 200;
  uint32_t offset = 0, total = 0, received = 0;
  int32_t have = -1;  // on an out-of-step chunk, how much of the file the card already holds
} upload;

void uploadFail(int code, const String &msg) {
  if (upload.file) upload.file.close();
  if (!upload.error.length()) {
    upload.code = code;
    upload.error = msg;
    Serial.printf("[web] upload %s: %s\n", upload.target.c_str(), msg.c_str());
  }
}

void uploadStart() {
  upload.error = "";
  upload.code = 200;
  upload.received = 0;
  upload.have = -1;

  String dir = storage::cleanPath(server.arg("path"));
  String name = cleanName(server.upload().filename);
  upload.target = storage::join(dir, name);
  upload.part = upload.target + ".part";
  if (!dir.length() || !name.length()) return uploadFail(400, "bad path");
  if (!storage::isDir(dir)) return uploadFail(404, "no such folder");

  // Without `total`, the request size is a close enough upper bound for the space check.
  upload.offset = strtoul(server.arg("offset").c_str(), nullptr, 10);
  upload.total = server.hasArg("total") ? strtoul(server.arg("total").c_str(), nullptr, 10)
                                        : upload.offset + server.clientContentLength();

  if (upload.offset == 0) {
    uint64_t free = storage::totalBytes() - storage::usedBytes();
    if (upload.total > free) {
      return uploadFail(507, "not enough space on the card (" + String((uint32_t)(free >> 20)) + " MB free)");
    }
    if (SD.exists(upload.part)) SD.remove(upload.part);
    upload.file = SD.open(upload.part, FILE_WRITE);
  } else {
    // A later chunk. A retried chunk may have partly landed already, so write from its offset.
    File f = SD.open(upload.part);
    upload.have = f ? (int32_t)f.size() : 0;
    if (f) f.close();
    if ((uint32_t)upload.have < upload.offset) return uploadFail(409, "out of step with the card");
    upload.file = SD.open(upload.part, "r+");
    if (upload.file && !upload.file.seek(upload.offset)) upload.file.close();
  }
  if (!upload.file) return uploadFail(500, "cannot write " + upload.part);
  upload.file.setBufferSize(16384);  // fewer, larger card writes
}

void uploadFinish() {
  upload.file.close();
  if (!server.hasArg("total")) upload.total = upload.offset + upload.received;  // whole file in one go
  if (upload.offset + upload.received < upload.total) return;  // more chunks to come

  player::release(upload.target);
  if (SD.exists(upload.target)) SD.remove(upload.target);
  if (!SD.rename(upload.part, upload.target)) uploadFail(500, "could not rename " + upload.part);
}

void handleUploadData() {
  HTTPUpload &up = server.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      uploadStart();
      break;
    case UPLOAD_FILE_WRITE:
      if (!upload.file) break;
      if (upload.file.write(up.buf, up.currentSize) != up.currentSize) {
        uploadFail(500, "write failed (card full?)");
        break;
      }
      upload.received += up.currentSize;
      break;
    case UPLOAD_FILE_END:
      if (upload.file) uploadFinish();
      break;
    case UPLOAD_FILE_ABORTED:
      // No reply reaches the page; it retries this chunk, and the .part file stays for that.
      uploadFail(500, "upload interrupted");
      break;
  }
}

void handleUploadDone() {
  if (upload.error.length()) {
    StaticJsonDocument<256> doc;
    doc["error"] = upload.error;
    if (upload.have >= 0) doc["size"] = upload.have;
    return sendJson(upload.code, doc);
  }
  if (upload.offset + upload.received >= upload.total) filesChanged();
  sendOk();
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
}

String label() {
  xSemaphoreTake(labelMutex, portMAX_DELAY);
  String s = netLabel;
  xSemaphoreGive(labelMutex);
  return s;
}

}  // namespace web
