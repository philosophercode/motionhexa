#pragma once

// FireflySynchrony: flocking fireflies whose blinking Kuramoto-synchronizes.
// All helpers/constants live in namespace fireflies_detail — no new global names.

namespace fireflies_detail {

  // ---- tunables -------------------------------------------------------
  static constexpr int   kCount         = 24;    // agent count
  static constexpr float kMaxSpeed      = 8.0f;  // mm/s lazy drift cap
  static constexpr float kSepRadius     = 6.0f;  // mm, strong separation
  static constexpr float kNbrRadius     = 18.0f; // mm, cohesion/alignment/Kuramoto
  static constexpr float kSepForce      = 140.0f;
  static constexpr float kCohForce      = 4.0f;
  static constexpr float kAlignForce    = 2.0f;
  static constexpr float kBoundaryForce = 60.0f;
  static constexpr float kInradius      = 17.5f; // hex inradius mm
  static constexpr float kTiltForce     = 0.012f;
  static constexpr float kTiltSign      = 1.0f;  // flip to invert tilt direction
  static constexpr float kShakeThresh   = 13000.0f; // acc lsb magnitude for scatter
  static constexpr float kShakeKick     = 55.0f; // mm/s impulse magnitude
  static constexpr float kK             = 2.2f;  // Kuramoto coupling strength
  static constexpr float kOmegaBase     = 0.55f; // base Hz (×2π = rad/s)
  static constexpr float kOmegaSpread   = 0.20f; // ±Hz spread across agents
  static constexpr float kFlashDur      = 0.25f; // gaussian envelope sigma-ish, seconds
  static constexpr float kEmberBright   = 6.0f;  // 0-255 between-flash visibility
  static constexpr int   kPhysicsHz     = 25;    // boids + Kuramoto rate

  inline float frng(uint16_t &seed) {
    seed = seed * 6364u + 1013u;
    return (seed & 0x7FFF) / 32767.0f;
  }

  // Returns how far outside the flat-top hexagon the point is (>0 means outside).
  inline float hexOutside(float x, float y) {
    float a = fabsf(y);
    float b = fabsf(0.5f * x + kSqrtThreeOverTwo * y);
    float c = fabsf(0.5f * x - kSqrtThreeOverTwo * y);
    float m = (a > b ? (a > c ? a : c) : (b > c ? b : c));
    return m - kInradius;
  }

  struct Agent {
    float px, py;   // position mm, panel-centred
    float vx, vy;   // velocity mm/s
    float theta;    // Kuramoto phase [0, 2π)
    float omega;    // natural frequency rad/s
  };

} // namespace fireflies_detail


class FireflySynchrony : public Pattern {
  fireflies_detail::Agent agents[fireflies_detail::kCount];
  float dthetaBuf[fireflies_detail::kCount];
  unsigned long lastPhysicsMs = 0;
  bool shakeActive = false;

  void setup() override {
    using namespace fireflies_detail;
    uint16_t seed = (uint16_t)(millis() ^ 0xA5C3u);
    float r = kInradius * 0.6f;
    for (int i = 0; i < kCount; ++i) {
      Agent &a = agents[i];
      a.px    = (frng(seed) * 2.0f - 1.0f) * r;
      a.py    = (frng(seed) * 2.0f - 1.0f) * r;
      a.vx    = (frng(seed) * 2.0f - 1.0f) * 3.0f;
      a.vy    = (frng(seed) * 2.0f - 1.0f) * 3.0f;
      a.theta = frng(seed) * 6.2832f;
      a.omega = (kOmegaBase + kOmegaSpread * (frng(seed) * 2.0f - 1.0f)) * 6.2832f;
    }
    lastPhysicsMs = millis();
  }

  void physicsStep(float dt, float ax_lsb, float ay_lsb) {
    using namespace fireflies_detail;

    // Kuramoto: accumulate pairwise sin(θj−θi) once, use antisymmetry
    for (int i = 0; i < kCount; ++i) dthetaBuf[i] = 0.0f;
    for (int i = 0; i < kCount; ++i) {
      for (int j = i + 1; j < kCount; ++j) {
        float dx = agents[j].px - agents[i].px;
        float dy = agents[j].py - agents[i].py;
        if (dx * dx + dy * dy < kNbrRadius * kNbrRadius) {
          float s = sinf(agents[j].theta - agents[i].theta);
          dthetaBuf[i] += s;
          dthetaBuf[j] -= s;
        }
      }
    }

    for (int i = 0; i < kCount; ++i) {
      Agent &a = agents[i];

      float fx = 0.0f, fy = 0.0f;
      float cx = 0.0f, cy = 0.0f;
      float avx = 0.0f, avy = 0.0f;
      int   nNbr = 0;

      for (int j = 0; j < kCount; ++j) {
        if (j == i) continue;
        float dx = a.px - agents[j].px;
        float dy = a.py - agents[j].py;
        float d2 = dx * dx + dy * dy;
        if (d2 < kSepRadius * kSepRadius && d2 > 0.001f) {
          float inv_d = 1.0f / sqrtf(d2);
          fx += kSepForce * dx * inv_d / (d2 * 0.1f + 1.0f);
          fy += kSepForce * dy * inv_d / (d2 * 0.1f + 1.0f);
        }
        if (d2 < kNbrRadius * kNbrRadius) {
          cx += agents[j].px;
          cy += agents[j].py;
          avx += agents[j].vx;
          avy += agents[j].vy;
          ++nNbr;
        }
      }
      if (nNbr > 0) {
        float inv_n = 1.0f / nNbr;
        fx += kCohForce   * (cx * inv_n - a.px);
        fy += kCohForce   * (cy * inv_n - a.py);
        fx += kAlignForce * (avx * inv_n - a.vx);
        fy += kAlignForce * (avy * inv_n - a.vy);
      }

      fx += kTiltSign * kTiltForce * ax_lsb;
      fy += kTiltSign * kTiltForce * ay_lsb;

      // soft hex boundary repulsion
      float over = hexOutside(a.px, a.py);
      if (over > 0.0f) {
        float eps = 0.5f;
        float gx = (hexOutside(a.px + eps, a.py) - hexOutside(a.px - eps, a.py)) / (2.0f * eps);
        float gy = (hexOutside(a.px, a.py + eps) - hexOutside(a.px, a.py - eps)) / (2.0f * eps);
        fx -= kBoundaryForce * (1.0f + over) * gx;
        fy -= kBoundaryForce * (1.0f + over) * gy;
      }

      a.vx += fx * dt;
      a.vy += fy * dt;
      float spd = sqrtf(a.vx * a.vx + a.vy * a.vy);
      if (spd > kMaxSpeed) {
        float inv = kMaxSpeed / spd;
        a.vx *= inv;
        a.vy *= inv;
      }
      a.px += a.vx * dt;
      a.py += a.vy * dt;

      a.theta += (a.omega + kK * dthetaBuf[i]) * dt;
      while (a.theta >= 6.2832f) a.theta -= 6.2832f;
      while (a.theta <  0.0f)    a.theta += 6.2832f;
    }
  }

  void renderAgents() {
    using namespace fireflies_detail;
    for (int i = 0; i < kCount; ++i) {
      const Agent &a = agents[i];

      // Distance in time to the nearest phase-zero crossing
      float thetaFrac = a.theta / 6.2832f;
      float distFrac  = thetaFrac < 0.5f ? thetaFrac : (1.0f - thetaFrac);
      float periodS   = 6.2832f / a.omega;
      float timeDist  = distFrac * periodS;

      float sig = kFlashDur * 0.4f;
      float env = expf(-(timeDist * timeDist) / (2.0f * sig * sig));

      uint8_t flashBright = (uint8_t)(env * 255.0f);
      uint8_t emberBright = (uint8_t)kEmberBright;
      uint8_t bright = flashBright > emberBright ? flashBright : emberBright;

      // hue: ember=blue-green(128), flash=yellow-green(64)
      uint8_t hue = (uint8_t)(128 - (int)(64.0f * env));

      CRGB color = CHSV(hue, 0xFF, bright);

      vectorT<float> mmPos(a.px / pixelSpacing, a.py / pixelSpacing);
      fAxial fax = axial.rectToHex(mmPos, 1.0f);

      int qi = (int)roundf(fax.q());
      int ri = (int)roundf(fax.r());
      for (int dq = -1; dq <= 1; ++dq) {
        for (int dr = -1; dr <= 1; ++dr) {
          int q = qi + dq;
          int r = ri + dr;
          auto pxOpt = axial.indexAtAxial(q, r);
          if (!pxOpt.has_value()) continue;
          float aq   = fax.q() - q;
          float ar   = fax.r() - r;
          float as_  = fax.s() - (float)(-q - r);
          float dist = (fabsf(aq) + fabsf(ar) + fabsf(as_)) * 0.5f;
          float weight = 1.0f - dist;
          if (weight <= 0.0f) continue;
          CRGB c = color;
          c.nscale8((uint8_t)(weight * 255.0f));
          PixelIndex px = pxOpt.value();
          if (ctx.leds[px].r < c.r) ctx.leds[px].r = c.r;
          if (ctx.leds[px].g < c.g) ctx.leds[px].g = c.g;
          if (ctx.leds[px].b < c.b) ctx.leds[px].b = c.b;
        }
      }
    }
  }

public:
  void update() override {
    using namespace fireflies_detail;
    unsigned long now = millis();

    ctx.leds.fill_solid(CRGB(0, 3, 5)); // dim dark blue-green ambient base

    auto agmt = MotionManager::motionFrame.agmt;
    float ax = (float)agmt.acc.axes.x;
    float ay = (float)agmt.acc.axes.y;
    float az = (float)agmt.acc.axes.z;

    float accMag = sqrtf(ax * ax + ay * ay + az * az);
    if (!shakeActive && accMag > kShakeThresh) {
      shakeActive = true;
      uint16_t seed = (uint16_t)(now ^ 0x3C7Fu);
      for (int i = 0; i < kCount; ++i) {
        agents[i].vx += (frng(seed) * 2.0f - 1.0f) * kShakeKick;
        agents[i].vy += (frng(seed) * 2.0f - 1.0f) * kShakeKick;
        agents[i].theta += (frng(seed) * 2.0f - 1.0f) * 2.0f;
        while (agents[i].theta >= 6.2832f) agents[i].theta -= 6.2832f;
        while (agents[i].theta <  0.0f)    agents[i].theta += 6.2832f;
      }
    } else if (accMag < kShakeThresh * 0.6f) {
      shakeActive = false;
    }

    unsigned long physIntervalMs = 1000u / (unsigned long)kPhysicsHz;
    if (lastPhysicsMs == 0 || now - lastPhysicsMs >= physIntervalMs) {
      float dt = (lastPhysicsMs == 0)
                  ? (1.0f / kPhysicsHz)
                  : ((float)(now - lastPhysicsMs) * 0.001f);
      if (dt > 0.1f) dt = 0.1f;
      physicsStep(dt, ax, ay);
      lastPhysicsMs = now;
    }

    renderAgents();
  }

  const char *description() override {
    return "FireflySynchrony";
  }
};
