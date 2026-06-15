#pragma once
// AsherShow — a delight-loop for a 2-year-old named Asher.
// Act 1 SCROLL  (~6s): "ASHER" scrolls right-to-left in rainbow letters.
// Act 2 SMILEY  (~4s): big yellow smiley face, mouth animates open, eyes blink.
// Act 3 PUPPY   (~4s): pixel-art puppy face, tongue/ear wiggle, tail wag.
// Then loops forever. Fade-through-black (~300 ms) between acts.
//
// All helpers live in namespace asher_detail — no global-scope names.
// Reuses the rect-position → hex-lattice sampling approach from IsaacLetters.

#include <FastLED.h>
#include "hexaphysics.h"  // UMPoint
#include "ledgraph.h"     // hexGrid, LED_COUNT, kMeridian

// ─────────────────────────────────────────────────────────────────────────────
namespace asher_detail {

// Orientation: match IsaacLetters' current working values
static constexpr bool kFlipX = false;
static constexpr bool kFlipY = false;

// ── 5×7 bitmap font (bit4=leftmost col, bit0=rightmost) ──────────────────────
//  Row 0 = top, row 6 = bottom.
//  Copied/adapted from IsaacLetters — no modification to that class.

// A (same as IsaacLetters)
static const uint8_t kFontA[7] = {
  0b00100,  // ..#..
  0b01010,  // .#.#.
  0b10001,  // #...#
  0b10001,  // #...#
  0b11111,  // #####
  0b10001,  // #...#
  0b10001,  // #...#
};

// S (same as IsaacLetters)
static const uint8_t kFontS[7] = {
  0b01111,  // .####
  0b10000,  // #....
  0b10000,  // #....
  0b01110,  // .###.
  0b00001,  // ....#
  0b00001,  // ....#
  0b11110,  // ####.
};

// H
static const uint8_t kFontH[7] = {
  0b10001,  // #...#
  0b10001,  // #...#
  0b10001,  // #...#
  0b11111,  // #####
  0b10001,  // #...#
  0b10001,  // #...#
  0b10001,  // #...#
};

// E
static const uint8_t kFontE[7] = {
  0b11111,  // #####
  0b10000,  // #....
  0b10000,  // #....
  0b11110,  // ####.
  0b10000,  // #....
  0b10000,  // #....
  0b11111,  // #####
};

// R
static const uint8_t kFontR[7] = {
  0b11110,  // ####.
  0b10001,  // #...#
  0b10001,  // #...#
  0b11110,  // ####.
  0b10010,  // #..#.
  0b10001,  // #...#
  0b10001,  // #...#
};

// Letter order: A, S, H, E, R
static const uint8_t * const kLetterFont[5] = {
  kFontA, kFontS, kFontH, kFontE, kFontR
};

// Rainbow hues for A, S, H, E, R
static const uint8_t kLetterHue[5] = {
  0,    // A: red
  42,   // S: orange-yellow
  85,   // H: green
  128,  // E: cyan
  192,  // R: purple/magenta
};

// ── Panel geometry constants ──────────────────────────────────────────────────
// Pixel spacing = 3.9 mm = 3900 µm.
// Row vertical step = spacing * sqrt(3)/2 ≈ 3376 µm (pointy-top hex rows).
// Panel half-height from center to top ≈ 9 * 3900 * sqrt(3)/2 ≈ 30 390 µm.
// Full panel height ≈ 60 780 µm.
//
// For letters: 65% of panel height → fontH ≈ 39 507 µm.
//   cellH = fontH/7 ≈ 5644 µm  → use 5600 µm (matches IsaacLetters).
static constexpr int32_t kCellH = 5600;   // µm per font row
static constexpr int32_t kCellW = 5600;   // µm per font col

// For faces: scale a 13-wide × 13-tall bitmap so the face fills ~80% of panel.
//   13 cells, each cellF wide.  Panel full width ≈ 19*3900=74100 µm.
//   80% → 59280 µm / 13 ≈ 4560 µm.  Use 4500 µm for a nice fit.
static constexpr int32_t kFaceCellH = 4500;   // µm per face bitmap row
static constexpr int32_t kFaceCellW = 4500;   // µm per face bitmap col

// ── Scroll act timing ─────────────────────────────────────────────────────────
// Each letter is 5 cells wide + 1 cell gap = 6 cells.
// "ASHER" = 5 letters × 6 cols = 30 cols.  Plus ~5 extra cols for leading space.
// At ~1.5 s per letter transit across panel (panel width = kMeridian=19 pixels ≈ 19*3900=74100 µm):
// Scroll speed = 74100 µm / 1.5 s ≈ 49400 µm/s — use 50000 µm/s.
static constexpr int32_t kScrollSpeedUMs = 50000; // µm per second

// ── Fade timing ───────────────────────────────────────────────────────────────
static constexpr unsigned long kFadeMs   = 300;
static constexpr unsigned long kSmileyMs = 4000;
static constexpr unsigned long kPuppyMs  = 4000;

// ── Smiley face bitmap palette ────────────────────────────────────────────────
// 13×13 grid, palette index per cell:
//   0 = transparent/black
//   1 = yellow face
//   2 = dark (eyes, outline)
//   3 = pink cheeks
//   4 = white teeth
static constexpr int kFaceW = 13;
static constexpr int kFaceH = 13;

// Mouth states: 0=neutral, 1=small smile, 2=open grin
// We'll define them inline in the draw function for compactness.

// smiley face base (face disk + eyes; no mouth here)
// Row 0 = top of face
static const uint8_t kSmileyBase[13][13] = {
  //  0  1  2  3  4  5  6  7  8  9 10 11 12
  {  0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0 },  // 0
  {  0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0 },  // 1
  {  0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 },  // 2
  {  1, 1, 1, 2, 2, 1, 1, 1, 2, 2, 1, 1, 1 },  // 3  eyes
  {  1, 1, 1, 2, 2, 1, 1, 1, 2, 2, 1, 1, 1 },  // 4  eyes
  {  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },  // 5
  {  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },  // 6
  {  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },  // 7
  {  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },  // 8
  {  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },  // 9
  {  0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 },  // 10
  {  0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0 },  // 11
  {  0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0 },  // 12
};

// Mouth keyframes (rows 6-10 of the face, cols 0-12)
// Neutral: flat line at row 8
// Smile: gentle curve
// Grin: big U shape with teeth

// We store per-keyframe for rows 5..11 (7 rows), all 13 cols.
// palette: 0=face(inherit from base, i.e. yellow), 2=dark, 3=pink cheek, 4=white tooth
// For mouth rows: 0=keep yellow, 2=outline, 4=tooth
// A value of 0 means "use yellow (1)" from the base; we overlay on top.

// mouth0 = neutral (flat)
static const uint8_t kMouth0[7][13] = {
  { 0,0,0,0,0,0,0,0,0,0,0,0,0 }, // row5
  { 0,0,0,0,0,0,0,0,0,0,0,0,0 }, // row6
  { 0,0,0,2,0,0,0,0,0,2,0,0,0 }, // row7: corner dots
  { 0,0,0,0,2,2,2,2,2,0,0,0,0 }, // row8: flat line
  { 0,0,0,0,0,0,0,0,0,0,0,0,0 }, // row9
  { 0,0,0,0,0,0,0,0,0,0,0,0,0 }, // row10
  { 0,0,0,0,0,0,0,0,0,0,0,0,0 }, // row11
};

// mouth1 = small smile
static const uint8_t kMouth1[7][13] = {
  { 0,0,0,0,0,0,0,0,0,0,0,0,0 }, // row5
  { 0,0,0,0,0,0,0,0,0,0,0,0,0 }, // row6
  { 0,0,0,2,0,0,0,0,0,2,0,0,0 }, // row7: corners
  { 0,0,0,0,2,0,0,0,2,0,0,0,0 }, // row8
  { 0,0,0,0,0,2,2,2,0,0,0,0,0 }, // row9
  { 0,0,0,0,0,0,0,0,0,0,0,0,0 }, // row10
  { 0,0,0,0,0,0,0,0,0,0,0,0,0 }, // row11
};

// mouth2 = big open grin
static const uint8_t kMouth2[7][13] = {
  { 0,0,0,0,0,0,0,0,0,0,0,0,0 }, // row5
  { 0,0,2,0,0,0,0,0,0,0,2,0,0 }, // row6: wide corners
  { 0,2,0,4,4,4,4,4,4,4,0,2,0 }, // row7: top of mouth
  { 0,2,4,4,4,4,4,4,4,4,4,2,0 }, // row8: teeth
  { 0,2,0,0,0,0,0,0,0,0,0,2,0 }, // row9: inside
  { 0,0,2,0,0,0,0,0,0,0,2,0,0 }, // row10: bottom curve
  { 0,0,0,2,2,2,2,2,2,2,0,0,0 }, // row11: chin
};

// cheek blush positions (in face grid): rows 5-6, cols 1-2 and 10-11
// Applied at full-grin only, palette 3 = pink

// ── Puppy face bitmap ─────────────────────────────────────────────────────────
// Palette:
//   0 = transparent
//   5 = tan/light-brown (face)
//   6 = dark brown (ears, nose outline)
//   2 = dark (eyes, nose center)
//   3 = pink (tongue)
//   7 = white (highlight)

// 13×13 puppy face (ears in top corners, head in middle, nose/tongue at bottom)
static const uint8_t kPuppy[13][13] = {
  //  0  1  2  3  4  5  6  7  8  9 10 11 12
  {  6, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 6 },  // 0  ear tops
  {  6, 6, 6, 0, 5, 5, 5, 5, 5, 0, 6, 6, 6 },  // 1  ears + head top
  {  6, 6, 6, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6 },  // 2  ears + head
  {  0, 6, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 0 },  // 3  head wide
  {  0, 5, 5, 2, 2, 5, 5, 5, 2, 2, 5, 5, 0 },  // 4  eyes
  {  0, 5, 5, 2, 2, 5, 5, 5, 2, 2, 5, 5, 0 },  // 5  eyes
  {  0, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 0 },  // 6  cheeks
  {  0, 5, 5, 5, 5, 6, 6, 6, 5, 5, 5, 5, 0 },  // 7  nose outline
  {  0, 5, 5, 5, 6, 2, 2, 2, 6, 5, 5, 5, 0 },  // 8  nose
  {  0, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 0 },  // 9
  {  0, 0, 5, 5, 5, 3, 3, 3, 5, 5, 5, 0, 0 },  // 10 tongue
  {  0, 0, 0, 5, 3, 3, 3, 3, 3, 5, 0, 0, 0 },  // 11 tongue wide
  {  0, 0, 0, 0, 5, 5, 5, 5, 5, 0, 0, 0, 0 },  // 12 chin
};

// Palette to CRGB conversion for faces
static CRGB faceColor(uint8_t idx) {
  switch (idx) {
    case 1: return CRGB(255, 220,   0); // yellow
    case 2: return CRGB( 30,  20,  10); // dark brown / almost black
    case 3: return CRGB(255,  80, 120); // pink
    case 4: return CRGB(255, 255, 255); // white teeth
    case 5: return CRGB(210, 150,  70); // tan/light-brown puppy
    case 6: return CRGB(100,  55,  20); // dark brown (ears)
    case 7: return CRGB(255, 240, 200); // highlight
    default: return CRGB(0, 0, 0);
  }
}

// ── Helper: sample a face bitmap cell at LED pixel position ──────────────────
// Returns palette index (0 = transparent).
// faceData is [kFaceH][kFaceW], sampled by rect position with thickening.
//
// offsetX, offsetY: shift center of face in µm (for animations)
static inline uint8_t sampleFace(
    const uint8_t faceData[kFaceH][kFaceW],
    int32_t px_um, int32_t py_um,
    int32_t offsetX = 0, int32_t offsetY = 0)
{
  // face grid origin: centered on panel
  int32_t faceW_um = kFaceW * kFaceCellW;
  int32_t faceH_um = kFaceH * kFaceCellH;
  // rx, ry in face-local coords with +y = up
  int32_t rx = px_um + offsetX;
  int32_t ry = py_um + offsetY;
  // map to col/row
  // col: 0 = left, kFaceW-1 = right
  // row: 0 = top, kFaceH-1 = bottom
  // face is centered: x in [-faceW/2, faceW/2], y in [-faceH/2, faceH/2]
  // col = (rx + faceW/2) / cellW
  // row = (faceH/2 - ry) / cellH   (y inverted)
  int32_t half_faceW = faceW_um / 2;
  int32_t half_faceH = faceH_um / 2;

  // --- exact cell sample ---
  int col = (int)((rx + half_faceW) / kFaceCellW);
  int row = (int)((half_faceH - ry) / kFaceCellH);

  if (col >= 0 && col < kFaceW && row >= 0 && row < kFaceH) {
    uint8_t v = faceData[row][col];
    if (v != 0) return v;
  }

  // --- thickening pass: check nearby cells ---
  // Only check if we're within the extended bounding box
  if (col >= -1 && col <= kFaceW && row >= -1 && row <= kFaceH) {
    for (int dr = -1; dr <= 1; ++dr) {
      for (int dc = -1; dc <= 1; ++dc) {
        if (dr == 0 && dc == 0) continue;
        int nr = row + dr, nc = col + dc;
        if (nr >= 0 && nr < kFaceH && nc >= 0 && nc < kFaceW) {
          uint8_t v = faceData[nr][nc];
          if (v != 0) {
            // distance from pixel center to this cell center
            int32_t cx = (nc) * kFaceCellW + kFaceCellW/2 - half_faceW;
            int32_t cy = half_faceH - ((nr) * kFaceCellH + kFaceCellH/2);
            // using integer squared distance
            int32_t dx = (rx - cx) / 100;
            int32_t dy = (ry - cy) / 100;
            int32_t dist2 = dx*dx + dy*dy;
            int32_t thresh = (kFaceCellW * 55 / 100) / 100;
            if (dist2 < thresh * thresh) return v;
          }
        }
      }
    }
  }
  return 0;
}

// ── Helper: compute rect position with flip ───────────────────────────────────
static inline void pixelRect(PixelIndex px, int32_t &rx, int32_t &ry) {
  UMPoint p = hexGrid.position(px);
  rx = kFlipX ? -p.x : p.x;
  ry = kFlipY ? -p.y :  p.y;
}

// Precomputed pixel rect positions (filled once by AsherShow::setup())
// We store them as int32 pairs for integer math
struct PixelPos {
  int32_t x, y;
};

} // namespace asher_detail

// ─────────────────────────────────────────────────────────────────────────────
class AsherShow : public Pattern {
  // ── Per-pixel rect position cache ────────────────────────────────────────
  asher_detail::PixelPos pixPos[LED_COUNT];

  // ── Top-level act enum ────────────────────────────────────────────────────
  enum class Act { SCROLL, FADE_IN, SMILEY, FADE_OUT_SMILEY, FADE_IN_PUPPY, PUPPY, FADE_OUT_PUPPY, FADE_IN_SCROLL };
  Act act = Act::SCROLL;
  unsigned long actStart = 0;

  // ── SCROLL state ──────────────────────────────────────────────────────────
  // Virtual strip: letters A,S,H,E,R laid out with 1-col gaps.
  // Total strip width = 5 letters × (5+1) cols = 30 cols in font cells.
  // Plus 5 leading cols of blank space before the first letter appears.
  // Plus panel-width cols of trailing blank so word fully exits.
  //
  // scroll offset in µm: how far the strip has scrolled (right edge moves left).
  // At offset=0, right edge of virtual strip aligns with right edge of panel.
  // Pixel col on strip = (px.x + panelHalfW + scrollOffset) / cellW
  // (increases as strip scrolls left)
  int32_t scrollOffset = 0;       // µm, updated each frame
  unsigned long scrollLastMs = 0;

  // Panel half-width estimate (from kMeridian × spacing / 2)
  // = 19 * 3900 / 2 = 37050 µm → use 37000
  static constexpr int32_t kPanelHalfW = 37050;
  // Leading blank cols before A (in µm)
  static constexpr int32_t kLeadBlank = asher_detail::kCellW * 5;
  // Trailing blank after R (in µm) — enough that panel fully clears
  static constexpr int32_t kTrailBlank = kPanelHalfW * 2 + asher_detail::kCellW;
  // Total virtual strip width
  static constexpr int32_t kStripW = kLeadBlank
      + 5 * (5 + 1) * asher_detail::kCellW   // 5 letters × (5 cols + 1 gap)
      + kTrailBlank;

  // ── SMILEY state ──────────────────────────────────────────────────────────
  // mouth animation: 0=neutral, 1=smile, 2=grin
  // transitions: 0→1 at ~0.8s, 1→2 at ~1.5s
  // blink: eyes go dark at ~2.5s for ~120ms
  bool smileEyesBlink = false;

  // Composite face buffer for smiley (13×13 after mouth overlay)
  // We compute per-frame (cheap: just index arithmetic)

  // ── PUPPY state ───────────────────────────────────────────────────────────
  // Tongue wiggle: +/-half-cell horizontal oscillation at ~2 Hz
  // Ear flop: left ear y-offset oscillation at ~0.8 Hz
  // Tail wag: a small arc of 3 pixels at bottom edge, swinging at ~1.5 Hz

  // ── helpers ──────────────────────────────────────────────────────────────
  unsigned long actElapsed() const {
    return millis() - actStart;
  }
  void enterAct(Act a) {
    act = a;
    actStart = millis();
  }

  // ── SCROLL draw ──────────────────────────────────────────────────────────
  void drawScroll() {
    using namespace asher_detail;

    // update scroll offset
    unsigned long now = millis();
    if (scrollLastMs != 0) {
      unsigned long dt = now - scrollLastMs;
      scrollOffset += (int32_t)((int64_t)kScrollSpeedUMs * dt / 1000);
    }
    scrollLastMs = now;

    // strip is conceptually laid to the right; as offset grows, it moves left.
    // For pixel px with rect x=rx:
    //   strip_x = (rx + kPanelHalfW) + scrollOffset  [both in µm]
    //   strip_x < 0 → before start of strip
    //   col_on_strip = strip_x / cellW

    // kLeadBlank µm → first letter A starts at kLeadBlank
    // letter[li] starts at kLeadBlank + li * 6 * cellW
    // letter occupies cols [0..4] within its 6-col block

    ctx.leds.fill_solid(CRGB::Black);

    for (PixelIndex px = 0; px < LED_COUNT; ++px) {
      int32_t rx = pixPos[px].x;
      int32_t strip_x = rx + kPanelHalfW + scrollOffset;
      if (strip_x < 0 || strip_x >= kStripW) continue;

      // which letter block?
      int32_t in_content = strip_x - kLeadBlank;
      if (in_content < 0) continue; // leading blank

      int blockW = (5 + 1) * kCellW; // 6 cols per letter block
      int li = (int)(in_content / blockW);  // letter index 0..4
      if (li >= 5) continue; // trailing blank

      int32_t in_block = in_content - (int32_t)li * blockW;
      int col = (int)(in_block / kCellW);
      if (col >= 5) continue; // gap column

      // font row
      int32_t ry = pixPos[px].y;
      int32_t fontH = 7 * kCellH;
      int32_t in_row = (fontH / 2) - ry; // from top
      int row = (int)(in_row / kCellH);
      if (row < 0 || row >= 7) continue;

      const uint8_t *font = kLetterFont[li];
      bool lit = (font[row] & (0x10 >> col)) != 0;

      if (!lit) {
        // thickening: check neighbor cells in same letter
        bool nearLit = false;
        for (int dr = -1; dr <= 1 && !nearLit; ++dr) {
          for (int dc = -1; dc <= 1 && !nearLit; ++dc) {
            if (dr == 0 && dc == 0) continue;
            int nr = row + dr;
            int nc = col + dc;
            if (nc < 0 || nc >= 5 || nr < 0 || nr >= 7) continue;
            if (font[nr] & (0x10 >> nc)) {
              // distance check
              int32_t cx = (int32_t)(nc) * kCellW + kCellW/2
                           - (kPanelHalfW + scrollOffset - (int32_t)li * blockW - kLeadBlank)
                           + rx; // back to panel frame... simpler: just use cell coords
              // Redo: distance in strip coords
              int32_t cell_cx_strip = kLeadBlank + li * blockW + nc * kCellW + kCellW/2;
              int32_t cell_cy = (int32_t)(fontH/2) - (nr * kCellH + kCellH/2);
              int32_t px_strip_x = strip_x; // already computed
              int32_t ddx = (px_strip_x - cell_cx_strip) / 100;
              int32_t ddy = (ry - cell_cy) / 100;
              int32_t dist2 = ddx*ddx + ddy*ddy;
              int32_t thresh = (kCellW * 55 / 100) / 100;
              if (dist2 < thresh*thresh) nearLit = true;
            }
          }
        }
        if (!nearLit) continue;
      }

      ctx.leds[px] = CHSV(kLetterHue[li], 255, 255);
    }

    // scroll done when strip has fully passed the left edge of the panel
    // i.e. scrollOffset > kStripW - kLeadBlank + kPanelHalfW
    if (scrollOffset > kStripW) {
      // transition to smiley
      enterAct(Act::FADE_IN);
    }
  }

  // ── SMILEY draw ───────────────────────────────────────────────────────────
  void drawSmiley(unsigned long elapsed) {
    using namespace asher_detail;

    // determine mouth keyframe
    const uint8_t (*mouth)[kFaceW] = kMouth0;
    if (elapsed > 800) mouth = kMouth1;
    if (elapsed > 1500) mouth = kMouth2;

    // blink: eyes dark from 2500..2620 ms
    bool eyesBlink = (elapsed >= 2500 && elapsed < 2620);

    // cheek blush at full grin
    bool cheeks = (elapsed > 1700);

    ctx.leds.fill_solid(CRGB::Black);

    for (PixelIndex px = 0; px < LED_COUNT; ++px) {
      int32_t rx = pixPos[px].x;
      int32_t ry = pixPos[px].y;

      // Face grid coords
      int32_t faceW_um = kFaceW * kFaceCellW;
      int32_t faceH_um = kFaceH * kFaceCellH;
      int32_t half_faceW = faceW_um / 2;
      int32_t half_faceH = faceH_um / 2;

      int col = (int)((rx + half_faceW) / kFaceCellW);
      int row = (int)((half_faceH - ry) / kFaceCellH);

      uint8_t colorIdx = 0;

      // Base smiley (with thickening) — same logic as sampleFace but inline
      auto sampleWithThickening = [&](int r, int c) -> uint8_t {
        if (r >= 0 && r < kFaceH && c >= 0 && c < kFaceW) {
          uint8_t v = kSmileyBase[r][c];
          if (v != 0) return v;
          // check mouth overlay rows 5..11
          if (r >= 5 && r <= 11) {
            uint8_t mv = mouth[r-5][c];
            if (mv != 0) return mv;
          }
        }
        return 0;
      };

      // First: exact cell
      if (col >= 0 && col < kFaceW && row >= 0 && row < kFaceH) {
        colorIdx = kSmileyBase[row][col];
        // mouth overlay
        if (row >= 5 && row <= 11) {
          uint8_t mv = mouth[row-5][col];
          if (mv != 0) colorIdx = mv;
        }
        // eye blink
        if (eyesBlink && row >= 3 && row <= 4 && col >= 3 && col <= 4) colorIdx = 2;
        if (eyesBlink && row >= 3 && row <= 4 && col >= 8 && col <= 9) colorIdx = 2;
        // cheek blush
        if (cheeks) {
          if ((row == 5 || row == 6) && (col == 1 || col == 2)) colorIdx = 3;
          if ((row == 5 || row == 6) && (col == 10 || col == 11)) colorIdx = 3;
        }
      }

      // Thickening pass if not already lit
      if (colorIdx == 0 && col >= -1 && col <= kFaceW && row >= -1 && row <= kFaceH) {
        for (int dr = -1; dr <= 1 && colorIdx == 0; ++dr) {
          for (int dc = -1; dc <= 1 && colorIdx == 0; ++dc) {
            if (dr == 0 && dc == 0) continue;
            int nr = row + dr, nc = col + dc;
            uint8_t v = sampleWithThickening(nr, nc);
            if (v != 0) {
              int32_t cx = nc * kFaceCellW + kFaceCellW/2 - half_faceW;
              int32_t cy = half_faceH - (nr * kFaceCellH + kFaceCellH/2);
              int32_t ddx = (rx - cx) / 100;
              int32_t ddy = (ry - cy) / 100;
              int32_t dist2 = ddx*ddx + ddy*ddy;
              int32_t thresh = (kFaceCellW * 55 / 100) / 100;
              if (dist2 < thresh*thresh) colorIdx = v;
            }
          }
        }
      }

      if (colorIdx != 0) {
        ctx.leds[px] = faceColor(colorIdx);
      }
    }
  }

  // ── PUPPY draw ────────────────────────────────────────────────────────────
  void drawPuppy(unsigned long elapsed) {
    using namespace asher_detail;

    // tongue wiggle: small horizontal shift, ±0.3 cell, at 2 Hz
    // sin approximation using triwave8 (0..255→0..255 triangle)
    uint8_t tonguePhase = (uint8_t)((elapsed * 2 * 256 / 1000) & 0xFF);
    // triwave8: 0→128→0 over 0..255; shift to -128..128 range
    int32_t tongueShift = (int32_t)triwave8(tonguePhase) - 128;
    // scale to ±0.3 cell
    int32_t tongueOffsetX = tongueShift * kFaceCellW * 3 / 10 / 128;

    // ear flop: left ear (cols 0-1) y offset, 0.8 Hz
    uint8_t earPhase = (uint8_t)((elapsed * 4 / 5 * 256 / 1000) & 0xFF);
    int32_t earShift = (int32_t)triwave8(earPhase) - 128;
    int32_t earOffsetY = earShift * kFaceCellH * 2 / 10 / 128; // ±0.2 cell

    // tail wag: a small bright arc at bottom of panel
    // Represented as 3 pixels near the bottom; swing arc approximated by brightness swipe
    // We paint it after the main face
    uint8_t tailPhase = (uint8_t)((elapsed * 3 * 256 / 2000) & 0xFF);
    int32_t tailSwing = (int32_t)triwave8(tailPhase) - 128; // -128..127
    // tail center x in µm, swings ±1.5 cells
    int32_t tailCenterX = tailSwing * kFaceCellW * 15 / 10 / 128;
    // tail is at bottom of panel: y ≈ -half_face_height - 1 cell
    int32_t faceH_um = kFaceH * kFaceCellH;
    int32_t tailCenterY = -(faceH_um / 2) - kFaceCellH;

    ctx.leds.fill_solid(CRGB::Black);

    for (PixelIndex px = 0; px < LED_COUNT; ++px) {
      int32_t rx = pixPos[px].x;
      int32_t ry = pixPos[px].y;

      int32_t faceW_um = kFaceW * kFaceCellW;
      int32_t half_faceW = faceW_um / 2;
      int32_t half_faceH = faceH_um / 2;

      // tail wag: paint before face (face may overdraw)
      {
        int32_t dtx = (rx - tailCenterX) / 100;
        int32_t dty = (ry - tailCenterY) / 100;
        int32_t tailDist2 = dtx*dtx + dty*dty;
        int32_t tailRadius = (kFaceCellW * 80 / 100) / 100;
        if (tailDist2 < tailRadius*tailRadius) {
          ctx.leds[px] = faceColor(5); // tan
          continue;
        }
      }

      // ── sample puppy face ──
      // left ear (cols 0-1): apply earOffsetY shift
      // tongue area (rows 10-11): apply tongueOffsetX shift
      // rest: normal
      int col = (int)((rx + half_faceW) / kFaceCellW);
      int row = (int)((half_faceH - ry) / kFaceCellH);

      uint8_t colorIdx = 0;

      // determine if this pixel is in ear zone, tongue zone, or normal
      // we sample with offsets
      auto getPuppyColor = [&](int r, int c) -> uint8_t {
        if (r >= 0 && r < kFaceH && c >= 0 && c < kFaceW) {
          return kPuppy[r][c];
        }
        return 0;
      };

      // Determine shifted coords for ear / tongue
      int32_t sampleX = rx;
      int32_t sampleY = ry;

      // Left ear: rows 0-2, cols 0-2
      bool inLeftEarZone = (col <= 2 && row <= 2);
      if (inLeftEarZone) {
        sampleY = ry - earOffsetY;
      }

      // Tongue: rows 10-11 in face grid → adjust by tongueOffsetX
      bool inTongueZone = (row >= 10 && row <= 11);
      if (inTongueZone) {
        sampleX = rx - tongueOffsetX;
      }

      // recompute col/row for shifted coords
      int scol = (int)((sampleX + half_faceW) / kFaceCellW);
      int srow = (int)((half_faceH - sampleY) / kFaceCellH);

      if (scol >= 0 && scol < kFaceW && srow >= 0 && srow < kFaceH) {
        colorIdx = getPuppyColor(srow, scol);
      }

      // Thickening pass
      if (colorIdx == 0 && scol >= -1 && scol <= kFaceW && srow >= -1 && srow <= kFaceH) {
        for (int dr = -1; dr <= 1 && colorIdx == 0; ++dr) {
          for (int dc = -1; dc <= 1 && colorIdx == 0; ++dc) {
            if (dr == 0 && dc == 0) continue;
            int nr = srow + dr, nc = scol + dc;
            uint8_t v = getPuppyColor(nr, nc);
            if (v != 0) {
              int32_t cx = nc * kFaceCellW + kFaceCellW/2 - half_faceW;
              int32_t cy = half_faceH - (nr * kFaceCellH + kFaceCellH/2);
              int32_t ddx = (sampleX - cx) / 100;
              int32_t ddy = (sampleY - cy) / 100;
              int32_t dist2 = ddx*ddx + ddy*ddy;
              int32_t thresh = (kFaceCellW * 55 / 100) / 100;
              if (dist2 < thresh*thresh) colorIdx = v;
            }
          }
        }
      }

      if (colorIdx != 0) {
        ctx.leds[px] = faceColor(colorIdx);
      }
    }
  }

public:
  AsherShow() { }

  void setup() override {
    // Cache pixel rect positions
    for (PixelIndex px = 0; px < LED_COUNT; ++px) {
      asher_detail::pixelRect(px, pixPos[px].x, pixPos[px].y);
    }
    // Start with scroll act
    scrollOffset = 0;
    scrollLastMs = 0;
    enterAct(Act::SCROLL);
  }

  void update() override {
    unsigned long elapsed = actElapsed();

    switch (act) {

      // ── SCROLL ──────────────────────────────────────────────────────────
      case Act::SCROLL:
        drawScroll();
        // transition to fade is triggered inside drawScroll
        break;

      // ── FADE IN (black → smiley) ─────────────────────────────────────
      case Act::FADE_IN: {
        // Show first frame of smiley faded in
        drawSmiley(0);
        uint8_t bright = (uint8_t)((long)elapsed * 255 / asher_detail::kFadeMs);
        if (bright < 255) {
          ctx.leds.nscale8(bright);
        } else {
          enterAct(Act::SMILEY);
        }
        break;
      }

      // ── SMILEY ──────────────────────────────────────────────────────────
      case Act::SMILEY:
        drawSmiley(elapsed);
        if (elapsed >= asher_detail::kSmileyMs) {
          enterAct(Act::FADE_OUT_SMILEY);
        }
        break;

      // ── FADE OUT (smiley → black) ────────────────────────────────────
      case Act::FADE_OUT_SMILEY: {
        drawSmiley(asher_detail::kSmileyMs); // hold last frame
        uint8_t bright = (uint8_t)(255 - (long)elapsed * 255 / asher_detail::kFadeMs);
        ctx.leds.nscale8(bright);
        if (elapsed >= asher_detail::kFadeMs) {
          enterAct(Act::FADE_IN_PUPPY);
        }
        break;
      }

      // ── FADE IN (black → puppy) ──────────────────────────────────────
      case Act::FADE_IN_PUPPY: {
        drawPuppy(0);
        uint8_t bright = (uint8_t)((long)elapsed * 255 / asher_detail::kFadeMs);
        if (bright < 255) {
          ctx.leds.nscale8(bright);
        } else {
          enterAct(Act::PUPPY);
        }
        break;
      }

      // ── PUPPY ────────────────────────────────────────────────────────
      case Act::PUPPY:
        drawPuppy(elapsed);
        if (elapsed >= asher_detail::kPuppyMs) {
          enterAct(Act::FADE_OUT_PUPPY);
        }
        break;

      // ── FADE OUT (puppy → black) ─────────────────────────────────────
      case Act::FADE_OUT_PUPPY: {
        drawPuppy(asher_detail::kPuppyMs);
        uint8_t bright = (uint8_t)(255 - (long)elapsed * 255 / asher_detail::kFadeMs);
        ctx.leds.nscale8(bright);
        if (elapsed >= asher_detail::kFadeMs) {
          enterAct(Act::FADE_IN_SCROLL);
        }
        break;
      }

      // ── FADE IN (black → scroll) ─────────────────────────────────────
      case Act::FADE_IN_SCROLL: {
        // Reset scroll for next loop
        if (elapsed == 0 || scrollOffset == 0) {
          scrollOffset = 0;
          scrollLastMs = 0;
        }
        // Draw scroll but with dim
        drawScroll();
        uint8_t bright = (uint8_t)((long)elapsed * 255 / asher_detail::kFadeMs);
        if (bright < 255) {
          ctx.leds.nscale8(bright);
        } else {
          // scroll is now running normally; re-enter SCROLL act
          // but drawScroll may have already advanced offset; keep it
          enterAct(Act::SCROLL);
        }
        break;
      }
    }
  }

  const char *description() override {
    return "AsherShow";
  }
};
