// Claude Needs You — desk status light for Claude Code sessions.
// LilyGO T-Display S3 AMOLED (ESP32-S3, RM67162 240x536 over QSPI, used
// landscape). Claude Code hooks POST their raw stdin JSON to /hook; the same
// JSON lines are accepted over USB serial.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>

#if __has_include("secrets.h")
#include "secrets.h"
#define HAVE_WIFI 1
#else
#define HAVE_WIFI 0
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
#define MAX_SESSIONS 8
#define WORKING_STALE_MS (30UL * 60 * 1000)
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

// ---- Colours (RGB565) ----
#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_WORKING 0x0239
#define C_NEEDS 0xFD20
#define C_DONE 0x0480
#define C_ACKED 0x2945
#define C_IDLE 0x2104
#define C_DIM 0x8410

Arduino_DataBus *bus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);
Arduino_RM67162 *panel = new Arduino_RM67162(bus, PIN_LCD_RST, 1 /* landscape */);
Arduino_Canvas *gfx = new Arduino_Canvas(W, H, panel);

WiFiServer server(80);

enum State : uint8_t { ST_IDLE, ST_WORKING, ST_DONE, ST_NEEDS };

struct Session {
  bool used;
  char id[40];
  char project[28];
  char agent[10];  // "cursor", "codex"; empty for Claude Code
  char msg[96];
  State st;
  bool acked;
  uint32_t since;
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
  if (s->st != st) s->since = millis();
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
    if (s.st == ST_WORKING && age > WORKING_STALE_MS) { s.st = ST_IDLE; s.since = now; dirty = true; }
    else if (s.st == ST_DONE && age > DONE_EXPIRE_MS) { s.used = false; dirty = true; }
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

// Built-in 6x8 font scaled by `size`.
static void centerText(const char *t, int y, uint8_t size, uint16_t col) {
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  int w = strlen(t) * 6 * size;
  gfx->setCursor((W - w) / 2, y);
  gfx->print(t);
}

static void wrapText(const char *t, int y, int lines, uint8_t size, uint16_t col) {
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  const int perLine = (W - 32) / (6 * size);
  int len = strlen(t);
  for (int l = 0; l < lines && l * perLine < len; l++) {
    char buf[96];
    copyStr(buf, min((int)sizeof(buf), perLine + 1), t + l * perLine);
    gfx->setCursor(16, y + l * 10 * size);
    gfx->print(buf);
  }
}

// Red / green / blue / white bars with labels and a white frame to check
// colour order, inversion and edge offsets.
static void drawTestPattern() {
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
  gfx->drawRect(0, 0, W, H, C_WHITE);
  gfx->flush();
}

static void draw() {
  if (testPattern && millis() - testPattern < 20000) { drawTestPattern(); return; }
  testPattern = 0;
  int f = currentFocus();
  bool blinkOn = (millis() / 500) % 2;
  int counts[4] = {0, 0, 0, 0};
  for (auto &s : sessions)
    if (s.used) counts[s.st]++;

  uint16_t bg = C_IDLE, fg = C_WHITE;
  const char *word = "IDLE";

  if (f < 0) {
    gfx->fillScreen(C_BLACK);
    centerText("Claude", 50, 6, C_DIM);
    if (WiFi.status() == WL_CONNECTED) {
      centerText("no sessions", 130, 3, C_DIM);
      centerText(WiFi.localIP().toString().c_str(), 190, 2, C_DIM);
    } else {
      centerText(HAVE_WIFI ? "wifi..." : "no wifi config", 130, 3, C_DIM);
      centerText("serial ok", 190, 2, C_DIM);
    }
    gfx->flush();
    return;
  }

  const Session &s = sessions[f];
  switch (s.st) {
    case ST_NEEDS: bg = C_NEEDS; fg = C_BLACK; word = "NEEDS YOU"; break;
    case ST_DONE: bg = s.acked ? C_ACKED : C_DONE; word = "YOUR TURN"; break;
    case ST_WORKING: bg = C_WORKING; word = "WORKING"; break;
    default: break;
  }

  gfx->fillScreen(bg);
  if (s.st == ST_NEEDS && blinkOn)
    for (int i = 0; i < 8; i++) gfx->drawRect(i, i, W - 2 * i, H - 40 - 2 * i, C_BLACK);

  centerText(word, 20, 7, fg);
  char label[40];
  if (s.agent[0]) snprintf(label, sizeof(label), "%s (%s)", s.project, s.agent);
  else copyStr(label, sizeof(label), s.project);
  centerText(label, 92, strlen(label) * 18 <= W - 16 ? 3 : 2, fg);
  if (s.st == ST_NEEDS && s.msg[0]) wrapText(s.msg, 130, 2, 2, fg);

  // Bottom row: counts per state
  gfx->fillRect(0, H - 40, W, 40, C_BLACK);
  struct { State st; uint16_t c; const char *l; } chips[] = {
    {ST_NEEDS, C_NEEDS, "need"}, {ST_WORKING, C_WORKING, "work"}, {ST_DONE, C_DONE, "done"}, {ST_IDLE, C_IDLE, "idle"}};
  gfx->setTextSize(2);
  for (int i = 0; i < 4; i++) {
    int x = 16 + i * (W / 4);
    gfx->fillRect(x, H - 32, 24, 24, chips[i].c);
    gfx->setTextColor(counts[chips[i].st] ? C_WHITE : C_DIM);
    gfx->setCursor(x + 32, H - 28);
    gfx->printf("%d %s", counts[chips[i].st], chips[i].l);
  }
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

static void handleHttp() {
  WiFiClient c = server.available();
  if (!c) return;
  c.setTimeout(1000);
  String req = c.readStringUntil('\n');
  bool expectContinue = false;
  while (c.connected()) {
    String h = c.readStringUntil('\n');
    h.trim();
    if (h.length() == 0) break;
    h.toLowerCase();
    if (h.startsWith("expect:") && h.indexOf("100-continue") > 0) expectContinue = true;
  }

  if (req.startsWith("POST /hook")) {
    if (expectContinue) c.print("HTTP/1.1 100 Continue\r\n\r\n");
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, c, DeserializationOption::Filter(hookFilter()));
    c.print(err ? "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"
                : "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n");
    if (!err) handleEvent(doc);
    else Serial.printf("bad hook json: %s\n", err.c_str());
  } else if (req.startsWith("GET / ")) {
    c.print("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n");
    sendStateJson(c);
  } else {
    c.print("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
  }
  c.flush();
  c.stop();
}

static void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (line == "state") { sendStateJson(Serial); Serial.println(); return; }
  // Display tuning: inv0/inv1 colour inversion, test pattern.
  if (line == "inv0" || line == "inv1") { panel->invertDisplay(line == "inv1"); Serial.println(line); return; }
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

static void wifiBegin() {
#if HAVE_WIFI
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
#ifdef STATIC_IP
  WiFi.config(IPAddress(STATIC_IP), IPAddress(STATIC_GATEWAY), IPAddress(STATIC_SUBNET), IPAddress(STATIC_GATEWAY));
#endif
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#endif
}

static void wifiLoop() {
#if HAVE_WIFI
  static bool up = false;
  static uint32_t lastTry = 0;
  bool now = WiFi.status() == WL_CONNECTED;
  if (now && !up) {
    Serial.printf("wifi up: http://%s/  (%s.local)\n", WiFi.localIP().toString().c_str(), HOSTNAME);
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    server.begin();
    dirty = true;
  } else if (!now && millis() - lastTry > 15000) {
    lastTry = millis();
    WiFi.reconnect();
  }
  up = now;
#endif
}

// ---------------------------------------------------------------- main

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BTN_L, INPUT_PULLUP);
  pinMode(PIN_BTN_R, INPUT_PULLUP);
  pinMode(PIN_LCD_EN, OUTPUT);
  digitalWrite(PIN_LCD_EN, HIGH);
  if (!gfx->begin()) Serial.println("display init failed (frame buffer alloc?)");
  Serial.printf("psram %u free\n", ESP.getFreePsram());
  gfx->fillScreen(C_BLACK);
  gfx->flush();
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
  // Redraw on change, and every 250ms while something needs blinking.
  bool blinking = false;
  int f = currentFocus();
  if (f >= 0 && sessions[f].st == ST_NEEDS) blinking = true;
  if (dirty || (blinking && millis() - lastDraw > 250) || millis() - lastDraw > 5000) {
    dirty = false;
    lastDraw = millis();
    draw();
  }
  delay(5);
}
