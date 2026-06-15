#pragma once

namespace window_detail {

// Quaternion-rotate v by q: v' = v + 2w*(q×v) + 2*(q×(q×v))
static void quatRotateVec(const Quaternion &q, float vx, float vy, float vz,
                          float &ox, float &oy, float &oz) {
  float tx = 2.0f * (q.y * vz - q.z * vy);
  float ty = 2.0f * (q.z * vx - q.x * vz);
  float tz = 2.0f * (q.x * vy - q.y * vx);
  ox = vx + q.w * tx + (q.y * tz - q.z * ty);
  oy = vy + q.w * ty + (q.z * tx - q.x * tz);
  oz = vz + q.w * tz + (q.x * ty - q.y * tx);
}

// Rotate v by conjugate(q): equivalent to inverse rotation
static void quatRotateByConj(const Quaternion &q, float vx, float vy, float vz,
                              float &ox, float &oy, float &oz) {
  // conj(q) = (w, -x, -y, -z)
  Quaternion qc = {q.w, -q.x, -q.y, -q.z};
  quatRotateVec(qc, vx, vy, vz, ox, oy, oz);
}

struct Star {
  float wx, wy, wz;    // world-space unit direction
  uint8_t baseBright;  // 80..255
  uint8_t hue;         // color temperature: 0=warm, 160=blue-white
  uint8_t twinklePhase;
  uint8_t twinkleRate; // BPM-ish, scaled
};

static const int kStarCount = 64;
static Star gStars[kStarCount];
static bool gStarsInited = false;

// Simple hash for deterministic pseudo-random from seed
static uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x45d9f3b;
  x ^= x >> 16;
  x *= 0x45d9f3b;
  x ^= x >> 16;
  return x;
}

static float hashF(uint32_t seed) {
  return (float)(hash32(seed) & 0xFFFFFF) / (float)0xFFFFFF;
}

static void initStars() {
  if (gStarsInited) return;
  gStarsInited = true;

  // Fibonacci sphere for uniform distribution, with per-star jitter
  const float goldenAngle = 2.39996323f; // radians
  for (int i = 0; i < kStarCount; ++i) {
    float t = (float)i / (float)kStarCount;
    float theta = acosf(1.0f - 2.0f * t); // polar angle from north
    float phi   = goldenAngle * i;          // azimuth

    // jitter: shift each star slightly for non-grid appearance
    float jitter = 0.08f;
    float jt = (hashF(i * 7 + 1) - 0.5f) * jitter;
    float jp = (hashF(i * 7 + 2) - 0.5f) * jitter * 3.0f;
    theta = theta + jt;
    phi   = phi   + jp;

    float sinT = sinf(theta);
    Star &s = gStars[i];
    s.wx = sinT * cosf(phi);
    s.wy = sinT * sinf(phi);
    s.wz = cosf(theta);

    // brightness classes: ~10 bright, rest faint
    uint32_t h = hash32((uint32_t)i * 13 + 999);
    if (i < 8) {
      s.baseBright = 200 + (h & 0x37); // 200-255
    } else if (i < 24) {
      s.baseBright = 120 + (h & 0x3F); // 120-183
    } else {
      s.baseBright = 55  + (h & 0x3F); // 55-118
    }

    // color temperature: mostly white/blue-white, a few warm
    uint8_t colorClass = (h >> 8) & 0x3;
    if (colorClass == 0) {
      s.hue = 160; // blue-white
    } else if (colorClass == 1) {
      s.hue = 20;  // warm yellow
    } else {
      s.hue = 0;   // white (sat near 0, handled at render)
    }

    s.twinklePhase = (uint8_t)(hash32(i * 17 + 3) & 0xFF);
    s.twinkleRate  = 1 + (uint8_t)(hash32(i * 19 + 5) & 0x3); // 1-4
  }
}

} // namespace window_detail

class TheWindow : public Pattern {
public:
  TheWindow() {
    window_detail::initStars();
  }

  void update() {
    using namespace window_detail;

    // Deep blue background
    ctx.leds.fill_solid(CRGB(0, 0, 6));

    Quaternion q = MotionManager::motionFrame.quat;

    // Guard: quat is all zeros during startup — just show dim background
    float qMagSq = q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z;
    if (qMagSq < 0.5f) return;

    // Panel half-radius in axial units (kMeridian/2 - 1)
    const float kPanelRadius = (float)(kMeridian / 2) - 0.5f; // ~9.0
    // Splat falloff radius in axial units
    const float kSplatR = 1.6f;
    const float kEdgeFade = kPanelRadius - 1.5f; // start fading stars near edge

    uint32_t now = millis();

    for (int si = 0; si < kStarCount; ++si) {
      Star &s = gStars[si];

      // Rotate world star direction into panel frame using conj(q)
      float ppx, ppy, ppz;
      quatRotateByConj(q, s.wx, s.wy, s.wz, ppx, ppy, ppz);

      // Only render stars in front of panel (z > threshold)
      if (ppz < 0.15f) continue;

      // Orthographic projection: panel-space x,y map to hex axes
      // Per README Example 3 convention: rectToHex(dy, -dx)
      // Scale so that pz=1 at panel edge ~ kPanelRadius axial units
      float scale = kPanelRadius * 0.85f; // tunable: field-of-view scale
      float projX = ppy * scale;  // rectToHex expects (y_panel, -x_panel)
      float projY = -ppx * scale;
      fAxial starAx = axial.rectToHex(vectorT<float>(projX, projY), 1.0f);

      // Edge fade
      float distFromCenter = fabsf(starAx.q()) + fabsf(starAx.r()) + fabsf(starAx.s());
      float edgeFactor = 1.0f;
      if (distFromCenter > kEdgeFade) {
        float overshoot = distFromCenter - kEdgeFade;
        edgeFactor = 1.0f - overshoot / 2.0f;
        if (edgeFactor <= 0.0f) continue;
      }

      // Twinkle: slow sine modulation
      uint8_t twPhase = s.twinklePhase + (uint8_t)((now / 20) * s.twinkleRate);
      uint8_t twinkle = sin8(twPhase); // 0..255
      // Keep amplitude subtle: brightness = base * (0.85 + 0.15*twinkle)
      uint8_t bright = (uint8_t)((uint16_t)s.baseBright * (217 + (twinkle >> 3)) / 255);
      bright = (uint8_t)((uint16_t)bright * (uint8_t)(edgeFactor * 255.0f) / 255);

      // Color: hue=0 and sat low = white; hue=160 = blue; hue=20 = warm
      uint8_t sat = (s.hue == 0) ? 0 : 60;
      CRGB starColor = CHSV(s.hue, sat, bright);

      // Sub-pixel splat: distribute brightness over nearest pixels by axial distance
      for (PixelIndex px2 = 0; px2 < LED_COUNT; ++px2) {
        fAxial cellAx = axial.axialFromPixelIndex(px2);
        float dq = starAx.q() - cellAx.q();
        float dr = starAx.r() - cellAx.r();
        float ds = starAx.s() - cellAx.s();
        float dist = (fabsf(dq) + fabsf(dr) + fabsf(ds)) * 0.5f; // hex distance
        if (dist >= kSplatR) continue;
        float weight = 1.0f - dist / kSplatR;
        uint8_t w8 = (uint8_t)(weight * 255.0f);
        CRGB c = starColor;
        c.nscale8(w8);
        // Additive blend
        ctx.leds[px2].r = qadd8(ctx.leds[px2].r, c.r);
        ctx.leds[px2].g = qadd8(ctx.leds[px2].g, c.g);
        ctx.leds[px2].b = qadd8(ctx.leds[px2].b, c.b);
      }
    }
  }

  const char *description() {
    return "TheWindow";
  }
};
