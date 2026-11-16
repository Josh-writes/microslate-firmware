#include "transfer_service.h"

#include "transfer_page.h"
#include <GfxRenderer.h>
#include "sleep_page.h"
#include "../sleep/sleep_rules.h"
#include "../sleep/sleep_screen.h"
#include "transfer_rules.h"
#include "../file_manager.h"
#include "../wifi_sync.h"
#include "../settings_menu.h"
#include "../sync/cloud_service.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <SDCardManager.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_random.h>

#include <cstring>
#include <string>

extern bool screenDirty;
extern UIState currentState;
extern int selectedFileIndex;
extern int settingsTab;
extern int settingsSelection;
extern bool settingsForgetPending;
extern GfxRenderer renderer;
void registerActivity();

namespace transfer {
namespace {

constexpr const char* kHostName = "microslate";

struct Entry {
  char file[MAX_FILENAME_LEN];
  char title[MAX_TITLE_LEN];
};

Phase g_phase = Phase::Off;
bool g_hotspot = false;
UIState g_backTo = UIState::MAIN_MENU;
bool g_one = false;
bool g_picture = false;   // openPicture(): the page takes a sleep picture
FsFile g_up;              // picture being received
size_t g_upLen = 0;
bool g_upBad = false;
Entry* g_list = nullptr;   // what is on offer, fixed when the screen opens so list positions never move
int g_count = 0;
std::string g_code, g_pass, g_ssid;
char g_what[MAX_TITLE_LEN + 4] = "";
char g_ip[16] = "";
const char* g_error = "";
int g_sent = 0;
WebServer* g_server = nullptr;
DNSServer* g_dns = nullptr;
View g_view;

uint32_t rnd() { return esp_random(); }

// The access code and the hotspot password stay the same on this device, so a bookmark keeps working and a phone that
// joined the hotspot once joins again by itself.
void loadSecrets() {
  Preferences p;
  if (!p.begin("transfer", false)) {
    g_code = makeCode(kCodeLen, rnd);
    g_pass = makeCode(kHotspotPassLen, rnd);
    return;
  }
  g_code = p.getString("code", "").c_str();
  g_pass = p.getString("appass", "").c_str();
  if (!codeLooksValid(g_code, kCodeLen)) { g_code = makeCode(kCodeLen, rnd); p.putString("code", g_code.c_str()); }
  if (!codeLooksValid(g_pass, kHotspotPassLen)) { g_pass = makeCode(kHotspotPassLen, rnd); p.putString("appass", g_pass.c_str()); }
  p.end();
}

void snapshot(const char* noteFile) {
  delete[] g_list;
  g_count = 0;
  FileInfo* files = getFileList();
  const int fc = getFileCount();
  g_list = new Entry[g_one ? 1 : (fc > 0 ? fc : 1)];
  if (g_one) {
    snprintf(g_list[0].file, sizeof(g_list[0].file), "%s", noteFile);
    snprintf(g_list[0].title, sizeof(g_list[0].title), "%s", noteFile);
    for (int i = 0; i < fc; i++)
      if (strcmp(files[i].filename, noteFile) == 0) snprintf(g_list[0].title, sizeof(g_list[0].title), "%s", files[i].title);
    g_count = 1;
    snprintf(g_what, sizeof(g_what), "%s", g_list[0].title);
    return;
  }
  for (int i = 0; i < fc; i++) {
    snprintf(g_list[i].file, sizeof(g_list[i].file), "%s", files[i].filename);
    snprintf(g_list[i].title, sizeof(g_list[i].title), "%s", files[i].title);
  }
  g_count = fc;
  snprintf(g_what, sizeof(g_what), "All notes");
}

void notePath(int i, char* out, size_t n) { snprintf(out, n, "/notes/%s", g_list[i].file); }

void sendPlain(int status, const char* text) {
  g_server->sendHeader("Cache-Control", "no-store");
  g_server->send(status, "text/plain; charset=utf-8", text);
}

void redirectToPage() {
  std::string to = std::string("http://") + g_ip + "/" + g_code + "/";
  g_server->sendHeader("Location", to.c_str(), true);
  g_server->sendHeader("Cache-Control", "no-store");
  g_server->send(302, "text/plain", "");
}

void sendList() {
  std::string j = std::string("{\"mode\":\"") + (g_one ? "one" : "all") + "\",\"notes\":[";
  char path[MAX_FILENAME_LEN + 8];
  for (int i = 0; i < g_count; i++) {
    notePath(i, path, sizeof(path));
    auto f = SdMan.open(path, O_RDONLY);
    if (!f) continue;   // gone since the screen opened
    const unsigned long size = (unsigned long)f.size();
    f.close();
    if (j.back() == '}') j += ',';
    j += "{\"i\":" + std::to_string(i) + ",\"name\":" + jsonString(g_list[i].file) + ",\"title\":" +
         jsonString(g_list[i].title) + ",\"size\":" + std::to_string(size) + "}";
  }
  j += "]}";
  g_server->sendHeader("Cache-Control", "no-store");
  g_server->send(200, "application/json", j.c_str());
}

// Picture mode: the page asks for the screen size to make the picture for.
void sendPictureInfo() {
  char j[64];
  snprintf(j, sizeof(j), "{\"mode\":\"picture\",\"w\":%d,\"h\":%d}", renderer.getScreenWidth(),
           renderer.getScreenHeight());
  g_server->sendHeader("Cache-Control", "no-store");
  g_server->send(200, "application/json", j);
}

void sendJson(int status, const std::string& j) {
  g_server->sendHeader("Cache-Control", "no-store");
  g_server->send(status, "application/json", j.c_str());
}

// The picture arrives in pieces (multipart form): straight to a temporary file, at most kMaxPictureBytes.
void pictureChunk() {
  registerActivity();
  HTTPUpload& u = g_server->upload();
  if (u.status == UPLOAD_FILE_START) {
    if (g_up) g_up.close();
    if (!SdMan.exists("/sleep")) SdMan.mkdir("/sleep");
    SdMan.remove(sleepscr::kUploadTmp);
    g_up = SdMan.open(sleepscr::kUploadTmp, O_WRONLY | O_CREAT | O_TRUNC);
    g_upLen = 0;
    g_upBad = !g_up;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    if (g_upBad) return;
    g_upLen += u.currentSize;
    if (g_upLen > sleepscr::kMaxPictureBytes || g_up.write(u.buf, u.currentSize) != u.currentSize) g_upBad = true;
  } else if (u.status == UPLOAD_FILE_END) {
    if (g_up) g_up.close();
  } else if (u.status == UPLOAD_FILE_ABORTED) {
    if (g_up) g_up.close();
    g_upBad = true;
  }
}

void pictureDone() {
  registerActivity();
  if (g_up) g_up.close();
  std::string name;
  const char* err = "";
  if (g_upBad || g_upLen == 0) {
    SdMan.remove(sleepscr::kUploadTmp);
    sendJson(400, std::string("{\"error\":") +
                      jsonString(g_upLen > sleepscr::kMaxPictureBytes ? "That picture is too large."
                                                                      : "The MicroSlate could not store the picture.") +
                      "}");
    return;
  }
  if (!sleepscr::acceptUpload(name, err)) {
    sendJson(400, std::string("{\"error\":") + jsonString(err) + "}");
    return;
  }
  g_sent++;
  screenDirty = true;
  sendJson(200, std::string("{\"ok\":true,\"label\":") + jsonString(sleepscr::pictureLabel(name)) + "}");
}

void sendNote(int i) {
  if (i < 0 || i >= g_count) { sendPlain(404, "That note is not shared."); return; }
  char path[MAX_FILENAME_LEN + 8];
  notePath(i, path, sizeof(path));
  auto f = SdMan.open(path, O_RDONLY);
  if (!f) { sendPlain(404, "That note is not on the MicroSlate any more."); return; }
  const bool dl = g_server->hasArg("dl");
  g_server->sendHeader("Cache-Control", "no-store");
  if (dl) g_server->sendHeader("Content-Disposition", contentDisposition(g_list[i].file).c_str());
  g_server->setContentLength(f.size());
  g_server->send(200, "text/plain; charset=utf-8", "");
  uint8_t buf[1024];
  WiFiClient client = g_server->client();
  bool ok = true;
  while (ok && f.available()) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    ok = client.write(buf, n) == (size_t)n;
    registerActivity();
  }
  f.close();
  if (ok && !g_server->hasArg("p")) {   // ?p=1: the list page fetching ahead for its Copy buttons, not a send
    g_sent++;
    screenDirty = true;
  }
  DBG_PRINTF("[XFER] sent %s%s%s\n", g_list[i].file, dl ? " (download)" : "", ok ? "" : " (stopped)");
}

void handleRequest() {
  registerActivity();   // the device sleeps on no activity as usual; a page in use counts as activity
  const std::string path = g_server->uri().c_str();
  const Request r = parse(path, g_code);
  switch (r.route) {
    case Route::Page:
      g_server->sendHeader("Cache-Control", "no-store");
      if (g_picture) g_server->send_P(200, "text/html; charset=utf-8", kSleepPage, sizeof(kSleepPage) - 1);
      else g_server->send_P(200, "text/html; charset=utf-8", kTransferPage, sizeof(kTransferPage) - 1);
      return;
    case Route::List: if (g_picture) sendPictureInfo(); else sendList(); return;
    case Route::Note: if (g_picture) break; sendNote(r.index); return;
    case Route::NoCode: redirectToPage(); return;
    case Route::Unknown: break;
  }
  // On the hotspot every address leads to the page: that is what makes a phone pop it up by itself after joining.
  if (g_hotspot) { redirectToPage(); return; }
  sendPlain(404, "MicroSlate: add the code shown on the Transfer screen to the address, for example microslate.local/abcd");
}

bool startServer() {
  g_server = new WebServer(80);
  if (g_picture) g_server->on((std::string("/") + g_code + "/picture").c_str(), HTTP_POST, pictureDone, pictureChunk);
  g_server->onNotFound(handleRequest);
  g_server->begin();
  WiFi.setSleep(false);   // answer at once; only while the screen is open
  MDNS.end();
  if (MDNS.begin(kHostName)) MDNS.addService("http", "tcp", 80);
  const IPAddress ip = g_hotspot ? WiFi.softAPIP() : WiFi.localIP();
  snprintf(g_ip, sizeof(g_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  if (!g_hotspot) g_ssid = WiFi.SSID().c_str();
  g_phase = Phase::Ready;
  screenDirty = true;
  DBG_PRINTF("[XFER] serving %s at http://%s/%s/ (%s.local) on %s%s\n", g_what, g_ip, g_code.c_str(), kHostName,
             g_ssid.c_str(), g_hotspot ? " (hotspot)" : "");
  return true;
}

void stopServer() {
  if (g_server) {
    g_server->stop();
    delete g_server;
    g_server = nullptr;
  }
  if (g_dns) {
    g_dns->stop();
    delete g_dns;
    g_dns = nullptr;
  }
  MDNS.end();
}

void startHotspot() {
  stopServer();
  cloud::radioTakenOver();   // a sync in progress is stopped; it runs again later
  wifiSetHotspot(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  const uint64_t mac = ESP.getEfuseMac();
  char ssid[24];
  snprintf(ssid, sizeof(ssid), "MicroSlate-%02X%02X", (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
  g_ssid = ssid;
  if (!WiFi.softAP(ssid, g_pass.c_str(), 1, 0, 4)) {
    DBG_PRINTLN("[XFER] hotspot did not start");
    g_error = "The hotspot did not start";
    g_phase = Phase::Failed;
    screenDirty = true;
    return;
  }
  delay(100);
  g_hotspot = true;
  g_dns = new DNSServer();
  g_dns->setErrorReplyCode(DNSReplyCode::NoError);
  g_dns->start(53, "*", WiFi.softAPIP());
  startServer();
}

void beginLooking() {
  g_hotspot = false;
  g_error = "";
  if (WiFi.status() == WL_CONNECTED) { startServer(); return; }   // a recent sync left Wi-Fi up
  if (wifiQuickBegin()) { g_phase = Phase::Looking; screenDirty = true; return; }
  g_phase = Phase::NoWifi;   // no saved network at all
  screenDirty = true;
}

}  // namespace

void open(const char* noteFile, UIState backTo) {
  if (g_phase != Phase::Off) stop();
  g_backTo = backTo;
  g_one = noteFile && noteFile[0];
  g_picture = false;
  g_sent = 0;
  snapshot(noteFile);
  loadSecrets();
  wifiHold(true);   // background sync must not switch Wi-Fi off under the page
  currentState = UIState::TRANSFER;
  DBG_PRINTF("[XFER] open: %s\n", g_what);
  beginLooking();
}

void openPicture(UIState backTo) {
  if (g_phase != Phase::Off) stop();
  g_backTo = backTo;
  g_one = false;
  g_picture = true;
  g_sent = 0;
  delete[] g_list;
  g_list = nullptr;
  g_count = 0;
  snprintf(g_what, sizeof(g_what), "Sleep picture");
  loadSecrets();
  wifiHold(true);
  currentState = UIState::TRANSFER;
  DBG_PRINTLN("[XFER] open: sleep picture");
  beginLooking();
}

void stop() {
  if (g_phase == Phase::Off) return;
  stopServer();
  if (g_up) g_up.close();
  if (g_hotspot) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiSetHotspot(false);
  }
  g_hotspot = false;
  g_phase = Phase::Off;
  wifiHold(false);
  WiFi.setSleep(true);
  if (!cloud::wifiInUse()) wifiQuickEnd();   // a sync still running keeps Wi-Fi and switches it off when done
  delete[] g_list;
  g_list = nullptr;
  g_count = 0;
  DBG_PRINTF("[XFER] closed after %d note(s) sent\n", g_sent);
}

void close() {
  const bool one = g_one;
  char file[MAX_FILENAME_LEN] = "";
  if (one && g_list) snprintf(file, sizeof(file), "%s", g_list[0].file);
  stop();
  currentState = g_backTo;
  if (g_backTo == UIState::SLEEP_PICTURES) sleepscr::refresh();
  if (g_backTo == UIState::FILE_BROWSER) {
    refreshFileList();
    // Back on the note that was shared (row 0 is "+ New note").
    FileInfo* files = getFileList();
    for (int i = 0; one && i < getFileCount(); i++)
      if (strcmp(files[i].filename, file) == 0) selectedFileIndex = i + 1;
  }
  screenDirty = true;
}

void loop() {
  if (g_phase == Phase::Off) return;
  if (currentState != UIState::TRANSFER) { stop(); return; }   // left by the power button or a home swipe
  switch (g_phase) {
    case Phase::Looking: {
      const QuickWifi q = wifiQuickPoll();
      if (q == QuickWifi::Connected) startServer();
      else if (q == QuickWifi::Failed || q == QuickWifi::Idle) { g_phase = Phase::NoWifi; screenDirty = true; }
      break;
    }
    case Phase::Ready:
      if (g_dns) g_dns->processNextRequest();
      if (g_server) for (int i = 0; i < 8; i++) g_server->handleClient();
      if (!g_hotspot && WiFi.status() != WL_CONNECTED) {   // the network went away
        stopServer();
        DBG_PRINTLN("[XFER] Wi-Fi lost");
        g_phase = Phase::NoWifi;
        screenDirty = true;
      }
      break;
    default:
      break;
  }
}

// "No Wi-Fi": go and add the network (Settings, Wi-Fi tab, on "Add a network").
static void openWifiSettings() {
  stop();
  wifiSavedRefresh();
  settingsTab = kSettingsTabWifi;
  settingsSelection = wifiSavedCount() + 1;
  settingsForgetPending = false;
  currentState = UIState::SETTINGS;
  screenDirty = true;
}

void key(uint8_t k) {
  if (k == HID_KEY_ESCAPE) { close(); return; }
  if (g_phase == Phase::NoWifi && (k == HID_KEY_RIGHT || k == HID_KEY_DOWN)) { openWifiSettings(); return; }
  if (k != HID_KEY_ENTER) return;
  switch (g_phase) {
    case Phase::NoWifi: startHotspot(); break;            // Start hotspot
    case Phase::Ready: if (!g_hotspot) startHotspot(); break;   // Use hotspot (guest and office networks often
                                                             // keep devices from reaching each other)
    case Phase::Failed: beginLooking(); break;            // Try again
    default: break;
  }
}

const View& view() {
  g_view.phase = g_phase;
  g_view.hotspot = g_hotspot;
  g_view.what = g_what;
  g_view.ssid = g_ssid.c_str();
  g_view.password = g_pass.c_str();
  g_view.code = g_code.c_str();
  snprintf(g_view.ip, sizeof(g_view.ip), "%s", g_ip);
  g_view.sent = g_sent;
  g_view.picture = g_picture;
  g_view.error = g_error;
  return g_view;
}

}  // namespace transfer
