// A pair of animated eyes that show how a session is doing.
#pragma once

#include <Arduino_GFX_Library.h>

enum Mood : uint8_t {
  MOOD_SLEEPY,   // nothing going on: eyes shut, Zzz, the odd peek
  MOOD_DROWSY,   // an idle session: half-lidded, slow blinks
  MOOD_CURIOUS,  // needs you: big eyes, head tilts, a bobbing "?"
  MOOD_FOCUSED,  // working: narrowed eyes reading back and forth
  MOOD_HAPPY,    // your turn: ^ ^ eyes, bouncing, sparkles, blush
  MOOD_CONTENT,  // your turn, seen: calm happy eyes
};

// Blend two RGB565 colours, t = 0 gives a, t = 1 gives b.
uint16_t mix565(uint16_t a, uint16_t b, float t);

// Draws the eyes centred on (cx, cy) over a black background. Call it every
// frame: the face eases between moods and blinks by itself. `pop` runs from
// 1 down to 0 right after the state changes and makes the eyes boing.
void drawFace(Arduino_GFX *g, Mood mood, int cx, int cy, uint16_t col, float pop);
