#include "face.h"

#include <U8g2lib.h>

#define BLACK 0x0000
#define WHITE 0xFFFF
#define SPARKLE_YELLOW 0xFF2B
#define SWEAT_BLUE 0x96FF
#define BLUSH_PINK 0xFB31

uint16_t mix565(uint16_t a, uint16_t b, float t) {
  int r = (a >> 11) + (int)(((b >> 11) - (a >> 11)) * t);
  int g = ((a >> 5) & 63) + (int)((((b >> 5) & 63) - ((a >> 5) & 63)) * t);
  int bl = (a & 31) + (int)(((b & 31) - (a & 31)) * t);
  return (r << 11) | (g << 5) | bl;
}

namespace {

// One eye: offset from its resting spot, size, and how much of it the lids
// hide. `slant` tilts the top lid (positive = inner corner lower, a frown of
// concentration); `happy` carves the bottom away into a ^ arch.
struct Eye {
  float dx, dy, w, h, lidTop, slant, lidBot, happy;
};

const float EYE_GAP = 62;  // half the distance between the eye centres

Eye curL = {0, 0, 78, 96, 0, 0, 0, 0}, curR = curL;
uint32_t blinkAt = 0, nextBlink = 0;

float phase(uint32_t t, uint32_t period) { return (float)(t % period) / period; }

void easeEye(Eye &e, const Eye &to, float k) {
  e.dx += (to.dx - e.dx) * k;
  e.dy += (to.dy - e.dy) * k;
  e.w += (to.w - e.w) * k;
  e.h += (to.h - e.h) * k;
  e.lidTop += (to.lidTop - e.lidTop) * k;
  e.slant += (to.slant - e.slant) * k;
  e.lidBot += (to.lidBot - e.lidBot) * k;
  e.happy += (to.happy - e.happy) * k;
}

void drawEye(Arduino_GFX *g, float cx, float cy, const Eye &e, bool left, float open, float scale, uint16_t col) {
  float w = e.w * scale, h = e.h * scale * open;
  if (h < 6) h = 6;
  int x0 = cx - w / 2, y0 = cy - h / 2, iw = w, ih = h;
  int x1 = x0 + iw, y1 = y0 + ih;
  int r = min(min(iw, ih) / 2, 28);
  g->fillRoundRect(x0, y0, iw, ih, r, col);
  // Top lid: everything above a (possibly slanted) line goes black.
  if (e.lidTop > 0.01f || fabsf(e.slant) > 0.5f) {
    float base = y0 + e.lidTop * ih;
    int inner = base + e.slant / 2, outer = base - e.slant / 2;
    int yl = left ? outer : inner, yr = left ? inner : outer;
    g->fillTriangle(x0 - 2, y0 - 2, x1 + 2, y0 - 2, x0 - 2, yl, BLACK);
    g->fillTriangle(x1 + 2, y0 - 2, x1 + 2, yr, x0 - 2, yl, BLACK);
  }
  if (e.lidBot > 0.01f) {
    int lb = e.lidBot * ih;
    g->fillRect(x0 - 2, y1 - lb, iw + 4, lb + 2, BLACK);
  }
  if (e.happy > 0.02f) g->fillEllipse(cx, y1 + ih * 0.1f, w * 0.75f, ih * 0.65f * e.happy, BLACK);
}

// A four-pointed twinkle.
void sparkle(Arduino_GFX *g, int x, int y, int s, uint16_t col) {
  if (s < 1) return;
  int t = max(1, s / 4);
  g->fillTriangle(x, y - s, x + t, y, x - t, y, col);
  g->fillTriangle(x, y + s, x + t, y, x - t, y, col);
  g->fillTriangle(x - s, y, x, y - t, x, y + t, col);
  g->fillTriangle(x + s, y, x, y - t, x, y + t, col);
}

void sweatDrop(Arduino_GFX *g, int x, int y, uint16_t col) {
  g->fillCircle(x, y, 7, col);
  g->fillTriangle(x - 7, y - 2, x + 7, y - 2, x, y - 17, col);
}

void text(Arduino_GFX *g, const uint8_t *font, const char *s, int x, int y, uint16_t col) {
  g->setFont(font);
  g->setTextColor(col);
  g->setCursor(x, y);
  g->print(s);
}

}  // namespace

void drawFace(Arduino_GFX *g, Mood mood, int cx, int cy, uint16_t col, float pop) {
  uint32_t t = millis();
  Eye l = {0, 0, 78, 96, 0, 0, 0, 0}, r = l;
  float bob = 0;  // whole-face vertical offset, not eased
  bool blinks = true;

  switch (mood) {
    case MOOD_SLEEPY: {
      // Shut eyes breathing slowly; every so often one eye peeks open.
      l = r = {0, 10, 80, 96, 0.88f, 0, 0, 0};
      bob = 4 * sinf(2 * PI * phase(t, 3200));
      float p = phase(t, 14000);
      if (p > 0.62f && p < 0.72f) r = {6, 4, 80, 96, 0.5f, -6, 0, 0};
      blinks = false;
      break;
    }
    case MOOD_DROWSY:
      l = r = {0, 6, 78, 96, 0.5f, -4, 0.05f, 0};
      break;
    case MOOD_CURIOUS: {
      // Big eyes looking around, head tilting side to side, little hops.
      bool tilt = phase(t, 5200) < 0.5f;
      Eye big = {0, -8, 92, 114, 0, 0, 0, 0}, small = {0, -2, 76, 92, 0.08f, -10, 0, 0};
      l = tilt ? big : small;
      r = tilt ? small : big;
      static const int8_t gaze[][2] = {{0, 0}, {16, -10}, {-16, -6}, {0, 4}};
      int i = (t / 1300) % 4;
      l.dx += gaze[i][0]; r.dx += gaze[i][0];
      l.dy += gaze[i][1]; r.dy += gaze[i][1];
      float hop = phase(t, 3000);
      if (hop < 0.2f) bob = -12 * fabsf(sinf(PI * hop / 0.1f));
      break;
    }
    case MOOD_FOCUSED: {
      // Narrowed, frowning eyes reading a line left to right in jumps, then
      // snapping back. Now and then a harder squint.
      l = r = {0, 8, 80, 90, 0.32f, 14, 0.12f, 0};
      uint32_t k = t % 2200;
      float gx = k < 1800 ? -20 + (int)(k / 450) * 13 : -20;
      l.dx = r.dx = gx;
      if (phase(t, 7000) < 0.1f) { l.lidTop = r.lidTop = 0.48f; l.lidBot = r.lidBot = 0.22f; }
      break;
    }
    case MOOD_HAPPY:
      l = r = {0, 0, 84, 96, 0, 0, 0, 1};
      bob = -10 * fabsf(sinf(PI * phase(t, 700)));
      blinks = false;
      break;
    case MOOD_CONTENT:
      l = r = {0, 6, 78, 88, 0, 0, 0, 0.85f};
      blinks = false;
      break;
  }

  easeEye(curL, l, 0.3f);
  easeEye(curR, r, 0.3f);

  // Blinks every few seconds; curious eyes sometimes blink twice.
  float open = 1;
  if (blinks) {
    if (t >= nextBlink) {
      blinkAt = t;
      bool twice = mood == MOOD_CURIOUS && random(3) == 0;
      nextBlink = t + (twice ? 280 : random(2200, mood == MOOD_DROWSY ? 4500 : 6500));
    }
    uint32_t dur = mood == MOOD_DROWSY ? 450 : 160, d = t - blinkAt;
    if (d < dur) open = max(0.05f, fabsf(1 - 2.0f * d / dur));
  }
  float scale = pop > 0 ? 1 + 0.22f * sinf(PI * pop) : 1;
  float y = cy + bob;

  if (mood == MOOD_HAPPY) {  // blush under the eyes, bouncing with them
    uint16_t blush = mix565(BLUSH_PINK, BLACK, 0.35f);
    g->fillEllipse(cx - EYE_GAP - 8, y + 50, 18, 9, blush);
    g->fillEllipse(cx + EYE_GAP + 8, y + 50, 18, 9, blush);
  }

  drawEye(g, cx - EYE_GAP + curL.dx, y + curL.dy, curL, true, open, scale, col);
  drawEye(g, cx + EYE_GAP + curR.dx, y + curR.dy, curR, false, open, scale, col);

  switch (mood) {
    case MOOD_SLEEPY:  // z, z, Z drifting up and fading out
      for (int i = 0; i < 3; i++) {
        float p = phase(t + i * 1200, 3600);
        const uint8_t *font = p < 0.35f ? u8g2_font_fub11_tr : p < 0.7f ? u8g2_font_fub14_tr : u8g2_font_fub20_tr;
        text(g, font, p < 0.7f ? "z" : "Z", cx + 92 + p * 34 + 6 * sinf(p * 2 * PI), cy - 34 - p * 70,
             mix565(col, BLACK, p * p));
      }
      break;
    case MOOD_CURIOUS:
      text(g, u8g2_font_fub25_tr, "?", cx + 112 + 3 * sinf(2 * PI * phase(t, 900)),
           cy - 58 + 6 * sinf(2 * PI * phase(t, 1300)), col);
      break;
    case MOOD_FOCUSED: {  // a bead of sweat now and then
      float p = phase(t, 9000) * 9000 / 1800;
      if (p < 1) sweatDrop(g, cx + EYE_GAP + 52, cy - 44 + p * 44, mix565(SWEAT_BLUE, BLACK, p > 0.7f ? (p - 0.7f) / 0.3f : 0));
      break;
    }
    case MOOD_HAPPY: {
      static const int16_t spots[][2] = {{-132, -62}, {124, -74}, {-118, 58}, {136, 42}, {4, -92}, {-50, 92}};
      static const uint16_t cols[] = {WHITE, SPARKLE_YELLOW, WHITE, SPARKLE_YELLOW, WHITE, SPARKLE_YELLOW};
      for (int i = 0; i < 6; i++) {
        float p = phase(t + i * 410, 1500);
        if (p < 0.6f) sparkle(g, cx + spots[i][0], cy + spots[i][1], 13 * sinf(PI * p / 0.6f), cols[i]);
      }
      break;
    }
    default:
      break;
  }
}
