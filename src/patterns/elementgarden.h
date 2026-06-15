#pragma once
// ElementGarden — falling-sand cellular automaton ecosystem
// All helpers are inside namespace elementgarden_detail or the ElementGarden class.
// No global-scope names introduced.

namespace elementgarden_detail {

// ── Tunables ──────────────────────────────────────────────────────────────────
static constexpr uint32_t kTickIntervalMs      = 40;    // CA tick: ~25 Hz
static constexpr uint8_t  kInitSandFraction    = 64;    // out of 255: ~25%
static constexpr uint8_t  kInitWaterFraction   = 38;    // out of 255: ~15%
static constexpr uint8_t  kWaterRainFloor      = 8;     // min water cells before rain
static constexpr uint8_t  kRainProbability     = 40;    // out of 255 per tick
static constexpr uint8_t  kWetSandSproutProb   = 6;     // out of 255 per tick
static constexpr uint8_t  kPlantGrowProb       = 30;    // out of 255; decays with age
static constexpr uint8_t  kPlantMaxAge         = 200;   // age ticks before growth stops
static constexpr uint8_t  kFireLifeTicks       = 12;    // ticks fire lives before becoming SMOKE
static constexpr uint8_t  kSmokeLifeTicks      = 18;    // ticks smoke lives before AIR
static constexpr uint16_t kAmplitudeThreshold  = 800;   // raw amplitude units for loud-clap
static constexpr uint8_t  kShakeThreshold      = 3;     // multiples of 1g (8192 LSB/g)
static constexpr int16_t  kGravSign            = 1;     // flip to -1 if things fall "up" on device
static constexpr uint8_t  kWaterSideSpread     = 80;    // out of 255 chance to spread sideways
static constexpr uint8_t  kSandWetAbsorb       = 60;    // out of 255 chance wet_sand forms near water
static constexpr uint8_t  kShakeFraction       = 100;   // out of 255: fraction of cells churned on shake
static constexpr uint8_t  kFireIgniteProb      = 80;    // out of 255 chance adjacent plant ignites

// ── Cell states ───────────────────────────────────────────────────────────────
enum CellState : uint8_t { AIR=0, SAND, WATER, WET_SAND, PLANT, FIRE, SMOKE };

// ── Hex direction table (axial dq, dr) ───────────────────────────────────────
// Directions 0-5, clockwise: right(0°), downright(60°), downleft(120°),
//                            left(180°), upleft(240°), upright(300°)
struct HDir { int8_t dq, dr; };
static constexpr HDir kDirs[6] = {
  { 1,  0},  // 0: right
  { 0,  1},  // 1: downright
  {-1,  1},  // 2: downleft
  {-1,  0},  // 3: left
  { 0, -1},  // 4: upleft
  { 1, -1},  // 5: upright
};

// Opposite direction index
static constexpr uint8_t kOppDir[6] = {3, 4, 5, 0, 1, 2};

// Left/right diagonal neighbors of direction d (for sliding):
inline uint8_t leftSlide(uint8_t d)  { return (d + 5) % 6; }
inline uint8_t rightSlide(uint8_t d) { return (d + 1) % 6; }

// Left/right perpendicular to d (for water spreading):
inline uint8_t leftPerp(uint8_t d)  { return (d + 4) % 6; }
inline uint8_t rightPerp(uint8_t d) { return (d + 2) % 6; }

// ── Gravity direction from in-plane accelerometer vector ─────────────────────
// acc.x = panel right, acc.y = panel up (after localization in ledgraph.h).
// The accel vector points away from gravity; we negate it to get down.
// We find the hex direction whose rect projection best matches (ax, ay).
// hexToRect (size=1): x = q + 0.5*r,  y = -sqrt(3)/2 * r  (approx y = -0.866*r)
inline uint8_t gravityDirection(int16_t ax, int16_t ay) {
  int32_t best = INT32_MIN;
  uint8_t bestDir = 1; // default: downright
  for (uint8_t d = 0; d < 6; ++d) {
    // dir_x * 1000, dir_y * 1000 (fixed-point, no floats)
    int32_t dir_x = (int32_t)kDirs[d].dq * 1000 + (int32_t)kDirs[d].dr * 500;
    int32_t dir_y = (int32_t)kDirs[d].dr * (-866);
    int32_t dot   = dir_x * (int32_t)ax + dir_y * (int32_t)ay;
    if (dot > best) {
      best    = dot;
      bestDir = d;
    }
  }
  return bestDir;
}

// ── Per-cell storage ──────────────────────────────────────────────────────────
struct Cell {
  uint8_t state;   // CellState
  uint8_t age;     // ticks alive in current state (used for PLANT/FIRE/SMOKE)
  uint8_t parity;  // tick-parity: prevents double-stepping in a single CA tick
};

} // namespace elementgarden_detail

// ─────────────────────────────────────────────────────────────────────────────
class ElementGarden : public Pattern, AmplitudeReceiver {

  // ── World state ────────────────────────────────────────────────────────────
  elementgarden_detail::Cell cells[LED_COUNT];
  uint8_t  tickParity  = 0;
  uint16_t waterCount  = 0;
  uint8_t  downDir     = 1;   // current "down" direction index into kDirs[]
  unsigned long lastTickMs = 0;

  // ── Neighbor lookup ────────────────────────────────────────────────────────
  // Returns LED_COUNT (sentinel = out-of-bounds) when no neighbor exists.
  inline PixelIndex neighborOf(PixelIndex px, uint8_t d) const {
    Axial ax = axial.axialFromPixelIndex(px);
    auto nb = axial.indexAtAxial(
        ax.q() + elementgarden_detail::kDirs[d].dq,
        ax.r() + elementgarden_detail::kDirs[d].dr);
    return nb.has_value() ? nb.value() : (PixelIndex)LED_COUNT;
  }

  // ── Cell-state predicates ─────────────────────────────────────────────────
  static inline bool isAirLike(uint8_t s) {
    return s == elementgarden_detail::AIR || s == elementgarden_detail::SMOKE;
  }
  static inline bool isSolid(uint8_t s) {
    return s == elementgarden_detail::SAND
        || s == elementgarden_detail::WET_SAND
        || s == elementgarden_detail::PLANT;
  }

  // ── Cell movement (swap two cells; mark dest as already-stepped) ──────────
  inline void swapCells(PixelIndex src, PixelIndex dst) {
    elementgarden_detail::Cell tmp = cells[dst];
    cells[dst] = cells[src];
    cells[src] = tmp;
    cells[dst].parity = tickParity; // don't step the moved cell again this tick
  }

  // ── Seed the world ────────────────────────────────────────────────────────
  void seedWorld() {
    for (PixelIndex px = 0; px < LED_COUNT; ++px) {
      cells[px] = {elementgarden_detail::AIR, 0, 0};
    }
    waterCount = 0;
    for (PixelIndex px = 0; px < LED_COUNT; ++px) {
      uint8_t r = random8();
      if (r < elementgarden_detail::kInitSandFraction) {
        cells[px].state = elementgarden_detail::SAND;
      } else if (r < elementgarden_detail::kInitSandFraction
                    + elementgarden_detail::kInitWaterFraction) {
        cells[px].state = elementgarden_detail::WATER;
        waterCount++;
      }
    }
  }

  // ── Gravity direction update from IMU ─────────────────────────────────────
  void updateGravity() {
    auto axes = MotionManager::motionFrame.agmt.acc.axes;
    int16_t ax = (int16_t)(axes.x * elementgarden_detail::kGravSign);
    int16_t ay = (int16_t)(axes.y * elementgarden_detail::kGravSign);
    // Only update when there is meaningful in-plane signal (> ~0.1 g = 820 LSB)
    int32_t mag2 = (int32_t)ax * ax + (int32_t)ay * ay;
    if (mag2 > (int32_t)820 * 820) {
      // Accel points "away from gravity"; negate to get the "gravity" direction.
      downDir = elementgarden_detail::gravityDirection(-ax, -ay);
    }
  }

  // ── Shake detection ───────────────────────────────────────────────────────
  void checkShake() {
    using namespace elementgarden_detail;
    auto axes = MotionManager::motionFrame.agmt.acc.axes;
    int32_t mag2 = (int32_t)axes.x * axes.x
                 + (int32_t)axes.y * axes.y
                 + (int32_t)axes.z * axes.z;
    int32_t thresh = (int32_t)kShakeThreshold * 8192;
    if (mag2 > thresh * thresh) {
      for (PixelIndex px = 0; px < LED_COUNT; ++px) {
        if (random8() < kShakeFraction) {
          uint8_t s = cells[px].state;
          if (s == SAND || s == WATER || s == WET_SAND) {
            uint8_t start = random8(6);
            for (uint8_t di = 0; di < 6; ++di) {
              uint8_t d  = (start + di) % 6;
              PixelIndex nb = neighborOf(px, d);
              if (nb < LED_COUNT && cells[nb].state == AIR) {
                swapCells(px, nb);
                break;
              }
            }
          }
        }
      }
    }
  }

  // ── One CA tick ───────────────────────────────────────────────────────────
  void tick() {
    using namespace elementgarden_detail;

    tickParity ^= 1;

    // Recount water for ecosystem balance
    waterCount = 0;
    for (PixelIndex i = 0; i < LED_COUNT; ++i) {
      if (cells[i].state == WATER) waterCount++;
    }

    bool leftToRight = (tickParity == 1);

    // ── Main cell update pass ────────────────────────────────────────────────
    for (int ii = 0; ii < LED_COUNT; ++ii) {
      PixelIndex i = leftToRight ? (PixelIndex)ii : (PixelIndex)(LED_COUNT - 1 - ii);

      if (cells[i].parity == tickParity) continue; // already moved this tick
      cells[i].parity = tickParity;

      uint8_t s = cells[i].state;

      // ── SAND / WET_SAND ────────────────────────────────────────────────
      if (s == SAND || s == WET_SAND) {
        PixelIndex below = neighborOf(i, downDir);
        bool moved = false;

        if (below < LED_COUNT) {
          uint8_t bs = cells[below].state;
          if (isAirLike(bs) || bs == WATER) {
            swapCells(i, below);
            moved = true;
          }
        }
        if (!moved) {
          uint8_t dL = leftSlide(downDir);
          uint8_t dR = rightSlide(downDir);
          PixelIndex diagL = neighborOf(i, dL);
          PixelIndex diagR = neighborOf(i, dR);
          bool canL = (diagL < LED_COUNT && isAirLike(cells[diagL].state));
          bool canR = (diagR < LED_COUNT && isAirLike(cells[diagR].state));
          if (canL && canR) {
            if (random8() & 1) swapCells(i, diagL); else swapCells(i, diagR);
            moved = true;
          } else if (canL) {
            swapCells(i, diagL); moved = true;
          } else if (canR) {
            swapCells(i, diagR); moved = true;
          }
        }
        // Wet-sand formation: dry sand next to water absorbs it
        if (!moved && s == SAND) {
          for (uint8_t d = 0; d < 6; ++d) {
            PixelIndex nb = neighborOf(i, d);
            if (nb < LED_COUNT && cells[nb].state == WATER && random8() < kSandWetAbsorb) {
              cells[i].state = WET_SAND;
              cells[nb].state = AIR;
              cells[nb].parity = tickParity;
              if (waterCount > 0) waterCount--;
              break;
            }
          }
        }
        continue;
      }

      // ── WATER ─────────────────────────────────────────────────────────
      if (s == WATER) {
        PixelIndex below = neighborOf(i, downDir);
        bool moved = false;

        if (below < LED_COUNT && isAirLike(cells[below].state)) {
          swapCells(i, below);
          moved = true;
        }
        if (!moved) {
          uint8_t dL = leftSlide(downDir);
          uint8_t dR = rightSlide(downDir);
          PixelIndex diagL = neighborOf(i, dL);
          PixelIndex diagR = neighborOf(i, dR);
          bool canL = (diagL < LED_COUNT && isAirLike(cells[diagL].state));
          bool canR = (diagR < LED_COUNT && isAirLike(cells[diagR].state));
          if (canL && canR) {
            if (random8() & 1) swapCells(i, diagL); else swapCells(i, diagR);
            moved = true;
          } else if (canL) {
            swapCells(i, diagL); moved = true;
          } else if (canR) {
            swapCells(i, diagR); moved = true;
          }
        }
        // Sideways spread to find level
        if (!moved && random8() < kWaterSideSpread) {
          uint8_t dPL = leftPerp(downDir);
          uint8_t dPR = rightPerp(downDir);
          PixelIndex sideL = neighborOf(i, dPL);
          PixelIndex sideR = neighborOf(i, dPR);
          bool canL = (sideL < LED_COUNT && isAirLike(cells[sideL].state));
          bool canR = (sideR < LED_COUNT && isAirLike(cells[sideR].state));
          if (canL && canR) {
            if (random8() & 1) swapCells(i, sideL); else swapCells(i, sideR);
          } else if (canL) {
            swapCells(i, sideL);
          } else if (canR) {
            swapCells(i, sideR);
          }
        }
        continue;
      }

      // ── PLANT ─────────────────────────────────────────────────────────
      if (s == PLANT) {
        if (cells[i].age < 255) cells[i].age++;

        // Check adjacency for fire ignition
        for (uint8_t d = 0; d < 6; ++d) {
          PixelIndex nb = neighborOf(i, d);
          if (nb < LED_COUNT && cells[nb].state == FIRE) {
            cells[i].state = FIRE;
            cells[i].age = 0;
            break;
          }
        }
        if (cells[i].state != PLANT) continue;

        // Upward growth (probability decays with age)
        if (cells[i].age < kPlantMaxAge) {
          uint8_t growProb = (uint8_t)((uint16_t)kPlantGrowProb
                             * (kPlantMaxAge - cells[i].age) / kPlantMaxAge);
          if (random8() < growProb) {
            uint8_t upDir = kOppDir[downDir];
            uint8_t growDirs[3] = {upDir, leftSlide(upDir), rightSlide(upDir)};
            for (uint8_t gi = 0; gi < 3; ++gi) {
              PixelIndex nb = neighborOf(i, growDirs[gi]);
              if (nb < LED_COUNT && cells[nb].state == AIR) {
                cells[nb].state = PLANT;
                cells[nb].age   = cells[i].age + 1;
                cells[nb].parity = tickParity;
                break;
              }
            }
          }
        }
        continue;
      }

      // ── FIRE ──────────────────────────────────────────────────────────
      if (s == FIRE) {
        if (cells[i].age < 255) cells[i].age++;
        if (cells[i].age >= kFireLifeTicks) {
          cells[i].state = SMOKE;
          cells[i].age   = 0;
          continue;
        }
        // Rise upward
        uint8_t upDir = kOppDir[downDir];
        PixelIndex above = neighborOf(i, upDir);
        if (above < LED_COUNT && cells[above].state == AIR) {
          swapCells(i, above);
          continue;
        }
        // Diagonal rise
        {
          uint8_t slides[2] = {leftSlide(upDir), rightSlide(upDir)};
          for (uint8_t si = 0; si < 2; ++si) {
            PixelIndex nb = neighborOf(i, slides[si]);
            if (nb < LED_COUNT && cells[nb].state == AIR) {
              swapCells(i, nb);
              break;
            }
          }
        }
        // Ignite adjacent plants
        for (uint8_t d = 0; d < 6; ++d) {
          PixelIndex nb = neighborOf(i, d);
          if (nb < LED_COUNT && cells[nb].state == PLANT && random8() < kFireIgniteProb) {
            cells[nb].state = FIRE;
            cells[nb].age   = 0;
          }
        }
        continue;
      }

      // ── SMOKE ─────────────────────────────────────────────────────────
      if (s == SMOKE) {
        if (cells[i].age < 255) cells[i].age++;
        if (cells[i].age >= kSmokeLifeTicks) {
          cells[i].state = AIR;
          cells[i].age   = 0;
          continue;
        }
        uint8_t upDir = kOppDir[downDir];
        PixelIndex above = neighborOf(i, upDir);
        if (above < LED_COUNT && cells[above].state == AIR) {
          swapCells(i, above);
          continue;
        }
        {
          uint8_t slides[2] = {leftSlide(upDir), rightSlide(upDir)};
          for (uint8_t si = 0; si < 2; ++si) {
            PixelIndex nb = neighborOf(i, slides[si]);
            if (nb < LED_COUNT && cells[nb].state == AIR) {
              swapCells(i, nb);
              break;
            }
          }
        }
        continue;
      }

    } // end main scan

    // ── WET_SAND sprouting pass ────────────────────────────────────────────
    for (PixelIndex i = 0; i < LED_COUNT; ++i) {
      if (cells[i].state == WET_SAND) {
        bool hasAir = false;
        for (uint8_t d = 0; d < 6; ++d) {
          PixelIndex nb = neighborOf(i, d);
          if (nb < LED_COUNT && cells[nb].state == AIR) { hasAir = true; break; }
        }
        if (hasAir && random8() < kWetSandSproutProb) {
          cells[i].state = PLANT;
          cells[i].age   = 0;
        }
      }
    }

    // ── Ecosystem: rain water from the upward edge when running low ────────
    if (waterCount < kWaterRainFloor && random8() < kRainProbability) {
      uint8_t upDir = kOppDir[downDir];
      uint8_t tries = 8;
      while (tries--) {
        PixelIndex px  = random16() % LED_COUNT;
        PixelIndex above = neighborOf(px, upDir);
        // Edge pixel: no neighbor in upward direction
        if (above >= LED_COUNT && cells[px].state == AIR) {
          cells[px].state = WATER;
          cells[px].age   = 0;
          waterCount++;
          break;
        }
      }
    }
  }

  // ── Render: map cell states to LED colors ─────────────────────────────────
  void render() {
    using namespace elementgarden_detail;
    unsigned long mils = millis();

    for (PixelIndex px = 0; px < LED_COUNT; ++px) {
      uint8_t s   = cells[px].state;
      uint8_t age = cells[px].age;
      CRGB color;

      switch (s) {
        case AIR:
          color = CRGB(2, 3, 5);
          break;

        case SAND:
          color = CHSV(28, 160, 200);
          break;

        case WET_SAND:
          color = CHSV(22, 210, 120);
          break;

        case WATER: {
          uint8_t shimmer = beatsin8(17, 0, 40, (uint32_t)mils, (uint8_t)(px * 37));
          color = CHSV(148, 230, (uint8_t)(155 + shimmer));
          break;
        }

        case PLANT: {
          uint8_t val = (uint8_t)constrain(220 - (int)age / 2, 60, 220);
          color = CHSV(96, 210, val);
          break;
        }

        case FIRE: {
          uint8_t flicker = random8(50);
          uint8_t hue     = (uint8_t)constrain(12 - (int)age * 2 + (int)flicker / 4, 0, 40);
          uint8_t val     = (uint8_t)constrain(255 - (int)age * 10, 80, 255);
          uint8_t sat     = (uint8_t)constrain(250 - (int)age * 8, 160, 255);
          color = CHSV(hue, sat, val);
          break;
        }

        case SMOKE: {
          uint8_t fade = (uint8_t)constrain(110 - (int)age * 110 / (int)kSmokeLifeTicks, 0, 110);
          color = CHSV(0, 0, fade);
          break;
        }

        default:
          color = CRGB::Black;
          break;
      }

      ctx.leds[px] = color;
    }
  }

public:
  ElementGarden() : AmplitudeReceiver(audioInput) {}

  void setup() override {
    seedWorld();
    lastTickMs = millis();
    updateGravity();
  }

  void update() override {
    using namespace elementgarden_detail;

    updateGravity();
    checkShake();

    unsigned long mils = millis();
    if (mils - lastTickMs >= kTickIntervalMs) {
      lastTickMs = mils;

      // Loud-clap audio trigger: ignite up to 2 random PLANT cells
      int amplitude = amplitudeFrame();
      if (amplitude > (int)kAmplitudeThreshold) {
        uint8_t ignitions = 0;
        for (uint8_t tries = 0; tries < 20 && ignitions < 2; ++tries) {
          PixelIndex px = random16() % LED_COUNT;
          if (cells[px].state == PLANT) {
            cells[px].state = FIRE;
            cells[px].age   = 0;
            ignitions++;
          }
        }
      }

      tick();
    }

    render();
  }

  const char *description() override {
    return "ElementGarden";
  }
};
