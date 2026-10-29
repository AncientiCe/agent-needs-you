// Agent Needs You — desk status light for AI coding agents.
// LilyGO T-Display S3 AMOLED (ESP32-S3, RM67162 240x536 over QSPI, used
// landscape). Claude Code hooks POST their raw stdin JSON to /hook; the same
// JSON lines are accepted over USB serial.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Update.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <U8g2lib.h>

#include "face.h"

// Optional build-time defaults; settings saved on the board win.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

// ---- Pins (LilyGO T-Display S3 AMOLED pins_config.h) ----
#define PIN_LCD_CS 6
#define PIN_LCD_SCK 47
#define PIN_LCD_D0 18
#define PIN_LCD_D1 7
#define PIN_LCD_D2 48
#define PIN_LCD_D3 5
#define PIN_LCD_RST 17
#define PIN_LCD_EN 38  // panel power enable (status LED on v1 boards)
#define PIN_BTN_L 0
#define PIN_BTN_R 21

#define W 536
#define H 240

#define HOSTNAME "claude-light"
#define SETUP_AFTER_MS (60UL * 1000)        // no Wi-Fi this long after boot: open setup
#define SETUP_HOLD_MS 5000                   // hold the R button this long to open setup
#define SETUP_RESTART_MS (10UL * 60 * 1000)  // setup unused this long: restart and retry
#define MAX_SESSIONS 8
#define WORKING_STALE_MS (30UL * 60 * 1000)
#define DONE_FADE_MS (15UL * 60 * 1000)
#define DONE_EXPIRE_MS (2UL * 60 * 60 * 1000)
#define IDLE_EXPIRE_MS (2UL * 60 * 60 * 1000)
#define NEEDS_EXPIRE_MS (8UL * 60 * 60 * 1000)
#define FOCUS_HOLD_MS (30UL * 1000)
// Rotation: every session gets screen time, more urgent ones longer. A
// session that just finished or started waiting is shown at once.
#define ANNOUNCE_MS (8UL * 1000)
#define DWELL_NEEDS_MS (15UL * 1000)
#define DWELL_WORKING_MS (8UL * 1000)
#define DWELL_DONE_MS (4UL * 1000)
#define DWELL_ACKED_MS (2UL * 1000)
#define DWELL_IDLE_MS (3UL * 1000)
// Animation frame intervals.
#define FRAME_MS 33
#define FRAME_IDLE_MS 50
#define POP_MS 600  // eyes boing for this long after a state change

// ---- Layout: eyes on the left, text panel on the right ----
#define FACE_X 150
#define FACE_Y 120
#define PANEL_X 306
#define PANEL_W (W - PANEL_X - 12)

// ---- Colours (RGB565), bright accents on AMOLED black ----
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_WORKING 0x451F
#define C_NEEDS 0xFD80
#define C_DONE 0x3EEF
#define C_ACKED 0x2B68
#define C_IDLE 0x7BD4
#define C_DIM 0x6B6D

Arduino_DataBus *bus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);
Arduino_RM67162 *panel = new Arduino_RM67162(bus, PIN_LCD_RST, 1 /* landscape */);
Arduino_Canvas *gfx = new Arduino_Canvas(W, H, panel);

WiFiServer server(80);

// Wi-Fi and update settings, kept in NVS so a board can be set up without
// rebuilding. Any secrets.h values are the defaults until something is saved.
struct Config {
  char ssid[33];
  char pass[65];
  uint32_t ip, gateway, subnet;  // all 0 = DHCP
  char updatePass[33];
} cfg;

// Setup portal: an open access point whose every page is the settings form.
bool setupActive = false;
uint32_t setupAt = 0;
char apName[24];
DNSServer dns;
WebServer setupWeb(80);
String scanned;  // <option>s of nearby networks for the form
static void setupStart();

enum State : uint8_t { ST_IDLE, ST_WORKING, ST_DONE, ST_NEEDS };

struct Session {
  bool used;
  char id[40];
  char project[28];
  char agent[10];  // "cursor", "codex"; empty for Claude Code
  char msg[96];
  State st;
  bool acked;
  uint32_t since;    // last event
  uint32_t changed;  // last change of state
};

Session sessions[MAX_SESSIONS];
int focusIdx = -1;          // manual focus (R button), -1 = auto
uint32_t focusAt = 0;
int rotIdx = -1;            // session shown by the rotation
uint32_t rotAt = 0, rotFor = 0;
bool dirty = true;
uint32_t testPattern = 0;  // millis() when the test pattern was requested, 0 = off

// ---------------------------------------------------------------- sessions

static void copyStr(char *dst, size_t n, const char *src) {
  strncpy(dst, src ? src : "", n - 1);
  dst[n - 1] = 0;
}

static const char *baseName(const char *path) {
  const char *b = path;
  for (const char *p = path; *p; p++)
    if ((*p == '/' || *p == '\\') && p[1]) b = p + 1;
  return b;
}

static Session *findSession(const char *id, bool create) {
  for (auto &s : sessions)
    if (s.used && strcmp(s.id, id) == 0) return &s;
  if (!create) return nullptr;
  Session *slot = nullptr;
  for (auto &s : sessions)
    if (!s.used) { slot = &s; break; }
  if (!slot) {  // evict the oldest non-NEEDS session
    for (auto &s : sessions)
      if (s.st != ST_NEEDS && (!slot || s.since < slot->since)) slot = &s;
    if (!slot) slot = &sessions[0];
  }
  memset(slot, 0, sizeof(*slot));
  slot->used = true;
  copyStr(slot->id, sizeof(slot->id), id);
  return slot;
}

static void setState(Session *s, State st) {
  if (s->st != st) s->since = s->changed = millis();
  s->st = st;
  s->acked = false;
  if (st != ST_NEEDS) s->msg[0] = 0;
}

static void handleEvent(JsonDocument &doc) {
  const char *ev = doc["hook_event_name"] | "";
  const char *id = doc["session_id"] | "";
  if (!*ev || !*id) return;
  Serial.printf("event %s %s\n", ev, id);

  if (strcmp(ev, "SessionEnd") == 0) {
    Session *s = findSession(id, false);
    if (s) s->used = false;
    dirty = true;
    return;
  }

  Session *s = findSession(id, true);
  State before = s->st;
  const char *cwd = doc["cwd"] | "";
  if (*cwd) copyStr(s->project, sizeof(s->project), baseName(cwd));
  if (!s->project[0]) copyStr(s->project, sizeof(s->project), "session");
  const char *agent = doc["agent"] | "";
  if (*agent) copyStr(s->agent, sizeof(s->agent), agent);

  if (strcmp(ev, "UserPromptSubmit") == 0 || strcmp(ev, "PostToolUse") == 0 ||
      strcmp(ev, "PreToolUse") == 0 || strcmp(ev, "SessionStart") == 0) {
    setState(s, strcmp(ev, "SessionStart") == 0 ? ST_IDLE : ST_WORKING);
  } else if (strcmp(ev, "Notification") == 0 || strcmp(ev, "PermissionRequest") == 0) {
    setState(s, ST_NEEDS);
    copyStr(s->msg, sizeof(s->msg), doc["message"] | "");
  } else if (strcmp(ev, "Stop") == 0) {
    setState(s, ST_DONE);
  }
  s->since = millis();
  // Announce a session that just finished or started waiting.
  if (s->st != before && (s->st == ST_DONE || s->st == ST_NEEDS)) {
    focusIdx = -1;
    rotIdx = s - sessions;
    rotAt = millis();
    rotFor = s->st == ST_NEEDS ? DWELL_NEEDS_MS : ANNOUNCE_MS;
  }
  dirty = true;
}

static void expireSessions() {
  uint32_t now = millis();
  for (auto &s : sessions) {
    if (!s.used) continue;
    uint32_t age = now - s.since;
    if (s.st == ST_WORKING && age > WORKING_STALE_MS) { s.st = ST_IDLE; s.since = s.changed = now; dirty = true; }
    else if (s.st == ST_DONE && age > DONE_EXPIRE_MS) { s.used = false; dirty = true; }
    else if (s.st == ST_DONE && !s.acked && age > DONE_FADE_MS) { s.acked = true; dirty = true; }
    else if (s.st == ST_IDLE && age > IDLE_EXPIRE_MS) { s.used = false; dirty = true; }
    else if (s.st == ST_NEEDS && age > NEEDS_EXPIRE_MS) { s.used = false; dirty = true; }
  }
}

// Higher is more urgent: needs you, working, done, acked done, idle.
static int rank(const Session &s) {
  switch (s.st) {
    case ST_NEEDS: return 4;
    case ST_WORKING: return 3;
    case ST_DONE: return s.acked ? 1 : 2;
    default: return 0;
  }
}

static uint32_t dwell(const Session &s) {
  switch (s.st) {
    case ST_NEEDS: return DWELL_NEEDS_MS;
    case ST_WORKING: return DWELL_WORKING_MS;
    case ST_DONE: return s.acked ? DWELL_ACKED_MS : DWELL_DONE_MS;
    default: return DWELL_IDLE_MS;
  }
}

// Sessions in rotation order, most urgent first (slot order within a rank).
// Idle sessions only take part when nothing else is going on.
static int rotation(int *out) {
  int n = 0;
  for (int r = 4; r >= 1; r--)
    for (int i = 0; i < MAX_SESSIONS; i++)
      if (sessions[i].used && rank(sessions[i]) == r) out[n++] = i;
  if (n == 0)
    for (int i = 0; i < MAX_SESSIONS; i++)
      if (sessions[i].used) out[n++] = i;
  return n;
}

static int currentFocus() {
  if (focusIdx >= 0 && sessions[focusIdx].used && millis() - focusAt < FOCUS_HOLD_MS) return focusIdx;
  focusIdx = -1;
  int order[MAX_SESSIONS];
  int n = rotation(order);
  int pos = -1;
  for (int i = 0; i < n; i++)
    if (order[i] == rotIdx) pos = i;
  if (pos >= 0 && millis() - rotAt < rotFor) return rotIdx;
  // Dwell over (or the shown session left the rotation): move on, wrapping
  // back to the most urgent session.
  int next = n ? order[(pos + 1) % n] : -1;
  if (next != rotIdx) dirty = true;
  rotIdx = next;
  rotAt = millis();
  rotFor = next >= 0 ? dwell(sessions[next]) : 0;
  return rotIdx;
}

// ---------------------------------------------------------------- display

static int textWidth(const char *t) {
  int16_t x1, y1;
  uint16_t w, h;
  gfx->getTextBounds(t, 0, 0, &x1, &y1, &w, &h);
  return w;
}

static void text(const uint8_t *font, const char *t, int x, int y, uint16_t col) {
  gfx->setFont(font);
  gfx->setTextColor(col);
  gfx->setCursor(x, y);
  gfx->print(t);
}

// Prints `t` at (x, y) in the current font, cut short with "..." to fit maxW.
static void fitText(const char *t, int x, int y, int maxW) {
  char buf[104];
  copyStr(buf, sizeof(buf), t);
  int n = strlen(buf);
  while (n > 0 && textWidth(buf) > maxW) {
    n--;
    copyStr(buf, n + 1, t);
    strcat(buf, "...");
  }
  gfx->setCursor(x, y);
  gfx->print(buf);
}

// Word-wraps `t` into at most `lines` lines of maxW pixels, starting at
// baseline y; whatever doesn't fit ends in "...".
static void wrapText(const char *t, int x, int y, int maxW, int lineH, int lines) {
  char buf[100];
  for (int l = 0; l < lines && *t; l++) {
    const char *end = t, *q = t;
    while (*q) {
      const char *e = q;
      while (*e && *e != ' ') e++;
      copyStr(buf, min((int)(e - t) + 1, (int)sizeof(buf)), t);
      if (textWidth(buf) > maxW) break;
      end = e;
      q = *e ? e + 1 : e;
    }
    if (end == t || (l == lines - 1 && *end)) {  // overlong word or last line
      fitText(t, x, y + l * lineH, maxW);
      return;
    }
    copyStr(buf, min((int)(end - t) + 1, (int)sizeof(buf)), t);
    gfx->setCursor(x, y + l * lineH);
    gfx->print(buf);
    t = *end ? end + 1 : end;
  }
}

// Red / green / blue / white bars with labels and a white frame to check
// colour order, inversion and edge offsets.
static void drawTestPattern() {
  gfx->setFont();
  gfx->fillScreen(C_BLACK);
  const uint16_t cols[] = {0xF800, 0x07E0, 0x001F, C_WHITE};
  const char *names[] = {"RED", "GREEN", "BLUE", "WHITE"};
  const int bh = (H - 8) / 4;
  for (int i = 0; i < 4; i++) {
    gfx->fillRect(4, 4 + i * bh, W - 8, bh - 4, cols[i]);
    gfx->setTextSize(4);
    gfx->setTextColor(i == 3 ? C_BLACK : C_WHITE);
    gfx->setCursor(20, 14 + i * bh);
    gfx->print(names[i]);
  }
  gfx->setTextSize(1);
  gfx->drawRect(0, 0, W, H, C_WHITE);
  gfx->flush();
}

static Mood moodOf(const Session &s) {
  switch (s.st) {
    case ST_NEEDS: return MOOD_CURIOUS;
    case ST_WORKING: return MOOD_FOCUSED;
    case ST_DONE: return s.acked ? MOOD_CONTENT : MOOD_HAPPY;
    default: return MOOD_DROWSY;
  }
}

static uint16_t colourOf(const Session &s) {
  switch (s.st) {
    case ST_NEEDS: return C_NEEDS;
    case ST_WORKING: return C_WORKING;
    case ST_DONE: return s.acked ? C_ACKED : C_DONE;
    default: return C_IDLE;
  }
}

// "for 3 min", "finished 2 min ago" and so on, from the last state change.
static void ageText(char *out, size_t n, const Session &s) {
  unsigned long m = (millis() - s.changed) / 60000;
  switch (s.st) {
    case ST_NEEDS: m ? snprintf(out, n, "waiting %lu min", m) : snprintf(out, n, "waiting for you"); break;
    case ST_WORKING: m ? snprintf(out, n, "for %lu min", m) : snprintf(out, n, "just started"); break;
    case ST_DONE: m ? snprintf(out, n, "finished %lu min ago", m) : snprintf(out, n, "just finished"); break;
    default: m ? snprintf(out, n, "quiet for %lu min", m) : snprintf(out, n, "quiet"); break;
  }
}

// Bottom of the panel: a dot and a count per state, greyed out at zero.
static void drawCounts(int x, int y) {
  int counts[4] = {0, 0, 0, 0};
  for (auto &s : sessions)
    if (s.used) counts[s.st]++;
  static const struct { State st; uint16_t c; } chips[] = {
    {ST_NEEDS, C_NEEDS}, {ST_WORKING, C_WORKING}, {ST_DONE, C_DONE}, {ST_IDLE, C_IDLE}};
  for (int i = 0; i < 4; i++) {
    int n = counts[chips[i].st], cx = x + i * 56;
    gfx->fillCircle(cx + 7, y - 7, 7, n ? chips[i].c : mix565(chips[i].c, C_BLACK, 0.7f));
    char num[4];
    snprintf(num, sizeof(num), "%d", n);
    text(u8g2_font_fub14_tr, num, cx + 19, y, n ? C_WHITE : C_DIM);
  }
}

static void draw() {
  if (testPattern && millis() - testPattern < 20000) { drawTestPattern(); return; }
  testPattern = 0;
  int f = currentFocus();
  gfx->fillScreen(C_BLACK);

  if (setupActive) {
    drawFace(gfx, MOOD_CURIOUS, FACE_X, FACE_Y, C_NEEDS, 0);
    text(u8g2_font_fub25_tr, "Setup", PANEL_X, 52, C_NEEDS);
    text(u8g2_font_fur14_tr, "On your phone, join", PANEL_X, 92, C_DIM);
    gfx->setFont(u8g2_font_fur17_tr);
    gfx->setTextColor(C_WHITE);
    fitText(apName, PANEL_X, 120, PANEL_W);
    text(u8g2_font_fur14_tr, "then open", PANEL_X, 160, C_DIM);
    text(u8g2_font_fur17_tr, "192.168.4.1", PANEL_X, 188, C_WHITE);
    gfx->flush();
    return;
  }

  if (f < 0) {
    // Nothing going on: the face sleeps. Everything drifts a little so the
    // AMOLED doesn't burn in.
    uint32_t now = millis();
    int dx = 12 * sinf(2 * PI * (now % 23000) / 23000);
    int dy = 8 * sinf(2 * PI * (now % 31000) / 31000);
    drawFace(gfx, MOOD_SLEEPY, FACE_X + dx, FACE_Y + dy, C_IDLE, 0);
    text(u8g2_font_fub25_tr, "Sleeping", PANEL_X + dx, 72 + dy, C_IDLE);
    if (WiFi.status() == WL_CONNECTED) {
      text(u8g2_font_fur17_tr, "no sessions", PANEL_X + dx, 110 + dy, C_DIM);
      text(u8g2_font_fur14_tr, WiFi.localIP().toString().c_str(), PANEL_X + dx, 184 + dy, C_DIM);
    } else {
      text(u8g2_font_fur17_tr, cfg.ssid[0] ? "wifi..." : "no wifi config", PANEL_X + dx, 110 + dy, C_DIM);
      text(u8g2_font_fur14_tr, "serial ok", PANEL_X + dx, 184 + dy, C_DIM);
    }
    gfx->flush();
    return;
  }

  const Session &s = sessions[f];
  uint16_t col = colourOf(s);
  uint32_t sinceChange = millis() - s.changed;
  float pop = sinceChange < POP_MS ? 1 - (float)sinceChange / POP_MS : 0;
  drawFace(gfx, moodOf(s), FACE_X, FACE_Y, col, pop);

  static const char *titles[] = {"Idle", "Working", "Your turn", "Needs you"};
  text(u8g2_font_fub25_tr, titles[s.st], PANEL_X, 44, col);

  char label[40];
  if (s.agent[0]) snprintf(label, sizeof(label), "%s (%s)", s.project, s.agent);
  else copyStr(label, sizeof(label), s.project);
  gfx->setFont(u8g2_font_fur17_tr);
  gfx->setTextColor(C_WHITE);
  fitText(label, PANEL_X, 80, PANEL_W);

  char age[32];
  ageText(age, sizeof(age), s);
  text(u8g2_font_fur14_tr, age, PANEL_X, 106, C_DIM);

  if (s.st == ST_NEEDS && s.msg[0]) {
    gfx->setFont(u8g2_font_fur14_tr);
    gfx->setTextColor(mix565(C_NEEDS, C_WHITE, 0.6f));
    wrapText(s.msg, PANEL_X, 136, PANEL_W, 22, 3);
  }

  drawCounts(PANEL_X, 224);
  gfx->flush();
}

// ---------------------------------------------------------------- input

static JsonDocument &hookFilter() {
  static JsonDocument filter;
  if (filter.isNull()) {
    filter["hook_event_name"] = true;
    filter["session_id"] = true;
    filter["cwd"] = true;
    filter["message"] = true;
    filter["agent"] = true;
  }
  return filter;
}

static void sendStateJson(Print &out) {
  JsonDocument doc;
  JsonArray arr = doc["sessions"].to<JsonArray>();
  static const char *names[] = {"idle", "working", "done", "needs_you"};
  int f = currentFocus();
  for (auto &s : sessions) {
    if (!s.used) continue;
    JsonObject o = arr.add<JsonObject>();
    if (&s - sessions == f) o["shown"] = true;
    o["id"] = s.id;
    o["project"] = s.project;
    if (s.agent[0]) o["agent"] = s.agent;
    o["state"] = names[s.st];
    o["acked"] = s.acked;
    o["age_s"] = (millis() - s.since) / 1000;
    if (s.msg[0]) o["message"] = s.msg;
  }
  serializeJson(doc, out);
}

// ---------------------------------------------------------------- update

static const char *updatePassword() { return cfg.updatePass; }

static void drawUpdating(int pct) {
  gfx->fillScreen(C_BLACK);
  drawFace(gfx, MOOD_FOCUSED, FACE_X, FACE_Y, C_WORKING, 0);
  text(u8g2_font_fub25_tr, "Updating", PANEL_X, 72, C_WORKING);
  char t[16];
  snprintf(t, sizeof(t), "%d%%", pct);
  text(u8g2_font_fur17_tr, t, PANEL_X, 110, C_WHITE);
  text(u8g2_font_fur14_tr, "keep it powered", PANEL_X, 184, C_DIM);
  gfx->flush();
}

static void httpStatus(WiFiClient &c, const char *status, const char *body) {
  c.printf("HTTP/1.1 %s\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: %u\r\n\r\n%s",
           status, (unsigned)strlen(body), body);
}

// POST /update with the firmware .bin as the body and the update password in
// X-Update-Password. The image goes to the spare OTA slot; the board only
// boots it once it has been written and verified, then restarts.
static void handleUpdate(WiFiClient &c, size_t len, const String &password, bool expectContinue) {
  const char *want = updatePassword();
  if (!*want) { httpStatus(c, "403 Forbidden", "no update password set\n"); return; }
  if (password != want) { httpStatus(c, "401 Unauthorized", "wrong update password\n"); return; }
  if (!len) { httpStatus(c, "411 Length Required", "send the firmware with a Content-Length\n"); return; }
  if (!Update.begin(len, U_FLASH)) { httpStatus(c, "413 Payload Too Large", Update.errorString()); return; }
  if (expectContinue) c.print("HTTP/1.1 100 Continue\r\n\r\n");
  Serial.printf("update: %u bytes\n", (unsigned)len);
  c.setTimeout(5000);
  static uint8_t buf[4096];
  size_t done = 0;
  int shown = -1;
  while (done < len) {
    size_t n = c.readBytes(buf, min(sizeof(buf), len - done));
    if (n == 0 || Update.write(buf, n) != n) break;
    done += n;
    int pct = done * 100 / len;
    if (pct / 5 != shown) { shown = pct / 5; drawUpdating(pct); }
  }
  if (done < len || !Update.end()) {
    Update.abort();
    Serial.printf("update failed: %s\n", Update.errorString());
    httpStatus(c, "500 Internal Server Error", done < len ? "upload cut short\n" : Update.errorString());
    dirty = true;
    return;
  }
  httpStatus(c, "200 OK", "updated, restarting\n");
  c.flush();
  c.stop();
  Serial.println("update ok, restarting");
  delay(200);
  ESP.restart();
}

// ---------------------------------------------------------------- http

static void handleHttp() {
  WiFiClient c = server.available();
  if (!c) return;
  c.setTimeout(1000);
  String req = c.readStringUntil('\n');
  bool expectContinue = false;
  size_t contentLength = 0;
  String password;
  while (c.connected()) {
    String h = c.readStringUntil('\n');
    h.trim();
    if (h.length() == 0) break;
    String name = h.substring(0, h.indexOf(':') + 1);
    String value = h.substring(name.length());
    name.toLowerCase();
    value.trim();
    if (name == "expect:" && value.indexOf("100-continue") >= 0) expectContinue = true;
    else if (name == "content-length:") contentLength = value.toInt();
    else if (name == "x-update-password:") password = value;
  }

  if (req.startsWith("POST /hook")) {
    if (expectContinue) c.print("HTTP/1.1 100 Continue\r\n\r\n");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, c, DeserializationOption::Filter(hookFilter()));
    c.print(err ? "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"
                : "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
    if (!err) handleEvent(doc);
    else Serial.printf("bad hook json: %s\n", err.c_str());
  } else if (req.startsWith("POST /update")) {
    handleUpdate(c, contentLength, password, expectContinue);
  } else if (req.startsWith("GET / ")) {
    c.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n");
    sendStateJson(c);
  } else {
    c.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
  }
  c.flush();
  c.stop();
}

static uint8_t lastDisconnect = 0;  // why joining the network last failed

static void printWifi() {
  Serial.printf("network \"%s\" status %d ip %s rssi %d setup %s last failure %u (%s)\n", cfg.ssid, WiFi.status(),
                WiFi.localIP().toString().c_str(), WiFi.RSSI(), setupActive ? apName : "off", lastDisconnect,
                WiFi.disconnectReasonName((wifi_err_reason_t)lastDisconnect));
}

static void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (line == "state") { sendStateJson(Serial); Serial.println(); return; }
  // Display tuning: inv0/inv1 colour inversion, test pattern.
  if (line == "inv0" || line == "inv1") { panel->invertDisplay(line == "inv1"); Serial.println(line); return; }
  if (line == "setup") { setupStart(); return; }
  if (line == "wifi") { printWifi(); return; }
  if (line == "test") { testPattern = millis(); Serial.println("test"); return; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line, DeserializationOption::Filter(hookFilter()));
  if (err) Serial.printf("bad json: %s\n", err.c_str());
  else handleEvent(doc);
}

struct Button {
  uint8_t pin;
  bool last;
  uint32_t at;
};
Button btnL = {PIN_BTN_L, true, 0}, btnR = {PIN_BTN_R, true, 0};

static bool pressed(Button &b) {
  bool v = digitalRead(b.pin);
  if (v != b.last && millis() - b.at > 30) {
    b.last = v;
    b.at = millis();
    return !v;  // active low
  }
  return false;
}

static void handleButtons() {
  // Holding R opens setup, e.g. to move the board to another network.
  if (!btnR.last && millis() - btnR.at > SETUP_HOLD_MS && !setupActive) setupStart();
  if (pressed(btnL)) {
    for (auto &s : sessions)
      if (s.used && s.st == ST_DONE) s.acked = true;
    dirty = true;
  }
  if (pressed(btnR)) {
    int start = currentFocus();
    for (int i = 1; i <= MAX_SESSIONS; i++) {
      int j = (start + i + MAX_SESSIONS) % MAX_SESSIONS;
      if (sessions[j].used) { focusIdx = j; break; }
    }
    focusAt = millis();
    dirty = true;
  }
}

// ---------------------------------------------------------------- wifi

static void loadConfig() {
  memset(&cfg, 0, sizeof(cfg));
#ifdef WIFI_SSID
  copyStr(cfg.ssid, sizeof(cfg.ssid), WIFI_SSID);
  copyStr(cfg.pass, sizeof(cfg.pass), WIFI_PASS);
#endif
#ifdef STATIC_IP
  cfg.ip = IPAddress(STATIC_IP);
  cfg.gateway = IPAddress(STATIC_GATEWAY);
  cfg.subnet = IPAddress(STATIC_SUBNET);
#endif
#ifdef UPDATE_PASSWORD
  copyStr(cfg.updatePass, sizeof(cfg.updatePass), UPDATE_PASSWORD);
#endif
  Preferences p;
  if (!p.begin("anu", true)) return;  // nothing saved yet
  if (p.isKey("ssid")) {
    p.getString("ssid", cfg.ssid, sizeof(cfg.ssid));
    p.getString("pass", cfg.pass, sizeof(cfg.pass));
    cfg.ip = p.getUInt("ip");
    cfg.gateway = p.getUInt("gateway");
    cfg.subnet = p.getUInt("subnet");
  }
  if (p.isKey("updatePass")) p.getString("updatePass", cfg.updatePass, sizeof(cfg.updatePass));
  p.end();
}

static void saveConfig() {
  Preferences p;
  p.begin("anu", false);
  p.putString("ssid", cfg.ssid);
  p.putString("pass", cfg.pass);
  p.putUInt("ip", cfg.ip);
  p.putUInt("gateway", cfg.gateway);
  p.putUInt("subnet", cfg.subnet);
  p.putString("updatePass", cfg.updatePass);
  p.end();
}

// ---------------------------------------------------------------- setup

static String htmlEscape(const String &in) {
  String out;
  for (char ch : in) {
    if (ch == '&') out += "&amp;";
    else if (ch == '<') out += "&lt;";
    else if (ch == '>') out += "&gt;";
    else if (ch == '"') out += "&quot;";
    else out += ch;
  }
  return out;
}

static String ipField(uint32_t ip) { return ip ? IPAddress(ip).toString() : String(); }

static void setupForm() {
  String h;
  h.reserve(3000);
  h += F("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>Agent Needs You setup</title><style>"
         "body{font:16px system-ui,sans-serif;max-width:26em;margin:1.5em auto;padding:0 1em;background:#111;color:#eee}"
         "h1{font-size:1.4em;color:#fdb000}summary{margin-top:.9em;color:#999}label{display:block;margin:.9em 0 .2em}small{color:#999}"
         "input{box-sizing:border-box;width:100%;padding:.55em;font:inherit;border-radius:6px;border:1px solid #555;background:#222;color:#eee}"
         "button{margin-top:1.4em;width:100%;padding:.7em;font:inherit;font-weight:600;border:0;border-radius:6px;background:#fdb000;color:#111}"
         "</style><h1>Agent Needs You</h1><form method=post action=/save>"
         "<label>Wi-Fi network</label><input name=ssid list=nets required maxlength=32 value=\"");
  h += htmlEscape(cfg.ssid);
  h += F("\"><datalist id=nets>");
  h += scanned;
  h += F("</datalist><label>Wi-Fi password</label><input name=pass type=password maxlength=64>"
         "<label>Fixed IP <small>(optional, e.g. 192.168.1.50; blank: the router picks one)</small></label>"
         "<input name=ip autocapitalize=off autocorrect=off value=\"");
  h += ipField(cfg.ip);
  h += F("\"><details><summary>Router settings for a fixed IP</summary>"
         "<label>Gateway <small>(blank: the IP ending in .1)</small></label><input name=gateway autocapitalize=off value=\"");
  h += ipField(cfg.gateway);
  h += F("\"><label>Subnet mask <small>(blank: 255.255.255.0)</small></label><input name=subnet autocapitalize=off value=\"");
  h += ipField(cfg.subnet);
  h += F("\"></details><label>Update password <small>(for firmware updates over Wi-Fi; blank keeps the current one)</small></label>"
         "<input name=update type=password maxlength=32><button>Save and restart</button></form>");
  setupWeb.send(200, "text/html", h);
}

static void setupSave() {
  String ssid = setupWeb.arg("ssid"), ip = setupWeb.arg("ip"), gw = setupWeb.arg("gateway"), sn = setupWeb.arg("subnet");
  ssid.trim(); ip.trim(); gw.trim(); sn.trim();
  // Number pads in some locales only offer a comma.
  ip.replace(',', '.'); gw.replace(',', '.'); sn.replace(',', '.');
  IPAddress a, g, m(255, 255, 255, 0);
  const char *err = nullptr;
  if (ssid.isEmpty()) err = "Pick a Wi-Fi network.";
  else if (ip.length() && !a.fromString(ip)) err = "The fixed IP isn't a valid address.";
  else if (ip.length() && gw.length() && !g.fromString(gw)) err = "The gateway isn't a valid address.";
  else if (ip.length() && sn.length() && !m.fromString(sn)) err = "The subnet mask isn't valid.";
  if (!err && ip.length() && gw.isEmpty()) g = IPAddress(a[0], a[1], a[2], 1);  // routers are usually .1
  if (err) {
    setupWeb.send(400, "text/html", String(F("<meta name=viewport content='width=device-width'><p>")) + err +
                                        F("</p><p><a href=/>Back</a>"));
    return;
  }
  copyStr(cfg.ssid, sizeof(cfg.ssid), ssid.c_str());
  copyStr(cfg.pass, sizeof(cfg.pass), setupWeb.arg("pass").c_str());
  cfg.ip = ip.length() ? (uint32_t)a : 0;
  cfg.gateway = ip.length() ? (uint32_t)g : 0;
  cfg.subnet = ip.length() ? (uint32_t)m : 0;
  if (setupWeb.arg("update").length()) copyStr(cfg.updatePass, sizeof(cfg.updatePass), setupWeb.arg("update").c_str());
  saveConfig();
  setupWeb.send(200, "text/html",
                F("<meta name=viewport content='width=device-width'><p>Saved. The board is restarting and joining your "
                  "network; its address shows on the screen.</p>"));
  Serial.printf("setup saved for %s, restarting\n", cfg.ssid);
  delay(500);
  ESP.restart();
}

static void setupStart() {
  if (setupActive) return;
  server.stop();
  MDNS.end();
  // Stop retrying the saved network: the access point has to stay on one
  // channel, and joining would pull it elsewhere under the phone.
  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apName, sizeof(apName), "AgentNeedsYou-%02X%02X", mac[4], mac[5]);
  int n = WiFi.scanNetworks();
  scanned = "";
  for (int i = 0; i < n; i++) scanned += "<option value=\"" + htmlEscape(WiFi.SSID(i)) + "\">";
  WiFi.scanDelete();
  WiFi.softAP(apName);
  dns.start(53, "*", WiFi.softAPIP());
  setupWeb.on("/", HTTP_GET, setupForm);
  setupWeb.on("/save", HTTP_POST, setupSave);
  // Phones probe random URLs to detect a captive portal; send them all to the form.
  setupWeb.onNotFound([] {
    setupWeb.sendHeader("Location", "http://192.168.4.1/");
    setupWeb.send(302, "text/plain", "");
  });
  setupWeb.begin();
  setupActive = true;
  setupAt = millis();
  dirty = true;
  Serial.printf("setup: join %s, open http://192.168.4.1/\n", apName);
}

static void setupLoop() {
  if (!setupActive) return;
  dns.processNextRequest();
  setupWeb.handleClient();
  // Saved settings that just didn't work (router down after a power cut?):
  // go back to trying them every so often while nobody is setting up.
  if (cfg.ssid[0] && WiFi.softAPgetStationNum() == 0 && millis() - setupAt > SETUP_RESTART_MS) ESP.restart();
}

static void wifiBegin() {
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    lastDisconnect = info.wifi_sta_disconnected.reason;
    Serial.printf("wifi: can't join \"%s\": %s\n", cfg.ssid, WiFi.disconnectReasonName((wifi_err_reason_t)lastDisconnect));
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  if (!cfg.ssid[0]) { setupStart(); return; }
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  if (cfg.ip) WiFi.config(IPAddress(cfg.ip), IPAddress(cfg.gateway), IPAddress(cfg.subnet), IPAddress(cfg.gateway));
  WiFi.setSleep(false);
  WiFi.begin(cfg.ssid, cfg.pass);
}

static void wifiLoop() {
  if (setupActive) { setupLoop(); return; }
  static bool up = false;
  static uint32_t lastTry = 0;
  static bool everUp = false;
  bool now = WiFi.status() == WL_CONNECTED;
  everUp |= now;
  if (now && !up) {
    Serial.printf("wifi up: http://%s/  (%s.local)\n", WiFi.localIP().toString().c_str(), HOSTNAME);
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    server.begin();
    dirty = true;
  } else if (!now && !everUp && millis() > SETUP_AFTER_MS) {
    setupStart();
  } else if (!now && millis() - lastTry > 15000) {
    lastTry = millis();
    WiFi.reconnect();
  }
  up = now;
}

// ---------------------------------------------------------------- main

// The face is always alive (blinking, easing between moods), so every
// screen animates; the sleeping one a little slower.
static uint32_t frameInterval(int f) {
  if (testPattern) return 0;
  return f < 0 ? FRAME_IDLE_MS : FRAME_MS;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN_L, INPUT_PULLUP);
  pinMode(PIN_BTN_R, INPUT_PULLUP);
  pinMode(PIN_LCD_EN, OUTPUT);
  digitalWrite(PIN_LCD_EN, HIGH);
  if (!gfx->begin()) Serial.println("display init failed (frame buffer alloc?)");
  gfx->setTextWrap(false);
  Serial.printf("psram %u free\n", ESP.getFreePsram());
  gfx->fillScreen(C_BLACK);
  gfx->flush();
  loadConfig();
  wifiBegin();
  Serial.println("claude-light ready");
}

void loop() {
  static uint32_t lastDraw = 0, lastExpire = 0;
  wifiLoop();
  if (WiFi.status() == WL_CONNECTED) handleHttp();
  handleSerial();
  handleButtons();
  if (millis() - lastExpire > 5000) { lastExpire = millis(); expireSessions(); }
  // Redraw on change, and every frame while the shown screen is animated.
  uint32_t frame = frameInterval(currentFocus());
  if (dirty || (frame && millis() - lastDraw > frame) || millis() - lastDraw > 5000) {
    dirty = false;
    lastDraw = millis();
    draw();
  }
  delay(5);
}
