#pragma once
// FieldScope — magnetic field visualiser
// Reads raw mag data from the ICM-20948 AK09916 sub-sensor (agmt.mag.axes).
//
// ── MAG AXIS MAP NOTE ───────────────────────────────────────────────────────
// The AK09916 magnetometer has its own physical axis orientation that differs
// from the accel/gyro axes AND is NOT remapped by localizeMotionFrame().
// The mapping below is a *first-pass guess* based on the AK09916 datasheet
// (X forward, Y left, Z up when the ICM-20948 is mounted component-side up).
// After flashing, hold the panel flat, align a compass, rotate 90°, and check
// which raw axis deflects most.  Adjust FS_MAG_PANEL_X / FS_MAG_PANEL_Y
// and their signs until north stays in the right direction as you rotate.
// ────────────────────────────────────────────────────────────────────────────

namespace fieldscope_detail {

// ── Tunables ────────────────────────────────────────────────────────────────
// Mag smoothing (per-frame IIR alpha).  Mag ODR on ICM-20948 is ~100 Hz but
// agmt.mag refreshes only when the DMP reads the slave sensor, typically
// ~50 Hz.  alpha=0.15 gives ~130 ms lag — comfortable for a visualiser.
static constexpr uint8_t kAlphaNum = 3;   // alpha = kAlphaNum / kAlphaDen
static constexpr uint8_t kAlphaDen = 20;  //        = 0.15

// Baseline magnitude IIR (very slow ~10 s at 50 Hz).
// alpha_base = 1/500 → time-constant ≈ 500 frames ≈ 10 s
static constexpr int32_t kBaseAlphaDiv = 500;

// Anomaly curve mapping: anomaly ratio is clamped to [kAnomalyLo, kAnomalyHi]
// then linearly scaled to [0, 255].  At ratio 1.0 the output is 0 (calm).
static constexpr uint8_t kAnomalyHi_x10 = 30;  // 3.0× → full storm
static constexpr uint8_t kAnomalyLo_x10 =  4;  // 0.4× → also storm

// Aurora noise speed (noise coordinate advance per frame along field direction)
// Bigger = faster drift
static constexpr uint8_t kAuroraDriftSpeed = 2;

// Filament: dim white line across center — fades to nothing at max anomaly
static constexpr uint8_t kFilamentMaxBright = 40;

// ── Mag axis map constants ───────────────────────────────────────────────────
// Source axis mapped to panel +X (right) and panel +Y (up).
// Adjust signs (1 or -1) and axis choice empirically (see top of file).
// Current guess: AK09916 X → panel X,  AK09916 Y → panel Y  (both +ve).
static constexpr int8_t kMagSignX  =  1; // sign applied to source axis for panel X
static constexpr int8_t kMagSignY  =  1; // sign applied to source axis for panel Y
// "X" means agmt.mag.axes.x, "Y" means .y, "Z" means .z
// Use 0=x, 1=y, 2=z to select axis:
static constexpr uint8_t kMagSrcX  =  0; // which raw axis feeds panel X (0=x,1=y,2=z)
static constexpr uint8_t kMagSrcY  =  1; // which raw axis feeds panel Y

// Integer sqrt approximation (good to ~1% for arguments < 65535)
// Uses the classic Babylonian / Newton iteration seeded with a shift estimate.
static uint16_t isqrt16(uint32_t n) {
    if (n == 0) return 0;
    uint32_t x = n;
    uint32_t root = 0;
    uint32_t bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit > 0) {
        if (x >= root + bit) {
            x -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint16_t)root;
}

// Read one of the three raw mag axes by index (0=x, 1=y, 2=z)
static inline int16_t magAxis(const ICM_20948_AGMT_t &agmt, uint8_t idx) {
    switch (idx) {
        case 0: return agmt.mag.axes.x;
        case 1: return agmt.mag.axes.y;
        default: return agmt.mag.axes.z;
    }
}

} // namespace fieldscope_detail

// ────────────────────────────────────────────────────────────────────────────

class FieldScope : public Pattern {
public:
    FieldScope() {}

    const char *description() { return "FieldScope"; }

    void update() {
        using namespace fieldscope_detail;

        // ── 1. Read & smooth raw mag ─────────────────────────────────────
        ICM_20948_AGMT_t agmt = MotionManager::motionFrame.agmt;

        int16_t rawX = kMagSignX * magAxis(agmt, kMagSrcX);
        int16_t rawY = kMagSignY * magAxis(agmt, kMagSrcY);
        int16_t rawZ = agmt.mag.axes.z; // used only for magnitude

        // Guard all-zero reads (sensor not yet ready / DMP not populating)
        bool magReady = (rawX != 0) || (rawY != 0) || (rawZ != 0);

        if (magReady) {
            // Exponential smooth, integer: smooth = ((den-num)*smooth + num*raw) / den
            _smoothX = ((int32_t)(kAlphaDen - kAlphaNum) * _smoothX + (int32_t)kAlphaNum * rawX) / kAlphaDen;
            _smoothY = ((int32_t)(kAlphaDen - kAlphaNum) * _smoothY + (int32_t)kAlphaNum * rawY) / kAlphaDen;
            _smoothZ = ((int32_t)(kAlphaDen - kAlphaNum) * _smoothZ + (int32_t)kAlphaNum * rawZ) / kAlphaDen;
        }

        // ── 2. Magnitude & baseline ──────────────────────────────────────
        // magnitude² in int32 (LSB units, typical Earth field ~300 LSB per axis)
        int32_t magSq = (int32_t)_smoothX * _smoothX
                       + (int32_t)_smoothY * _smoothY
                       + (int32_t)_smoothZ * _smoothZ;
        if (magSq < 0) magSq = 0; // guard overflow during startup
        uint16_t mag = isqrt16((uint32_t)min((int32_t)0x0FFFFFFF, magSq));

        // Slow baseline magnitude estimate (very small alpha)
        // baseline started at a reasonable guess (400 LSB ≈ 60 uT Earth field)
        if (_baseline == 0) _baseline = max((uint16_t)200, mag);
        _baseline = (_baseline * (kBaseAlphaDiv - 1) + (int32_t)mag) / kBaseAlphaDiv;

        // ── 3. Anomaly ratio (fixed-point ×10) ──────────────────────────
        // anomaly_x10 = 10 * mag / baseline
        uint8_t anomaly_x10 = 10; // default calm
        if (_baseline > 0) {
            uint32_t ratio = (uint32_t)mag * 10 / max((uint32_t)1, (uint32_t)_baseline);
            anomaly_x10 = (uint8_t)constrain((int32_t)ratio, 0, 255);
        }

        // Map anomaly_x10 to 0..255 storm level:
        // calm at ratio==10 (1.0×), full storm at >=30 (3.0×) or <=4 (0.4×)
        uint8_t stormLevel;
        if (anomaly_x10 >= 10) {
            // Above baseline — heat up toward 3.0×
            uint8_t excess = anomaly_x10 - 10;  // 0..~200
            uint8_t range  = kAnomalyHi_x10 - 10; // 20
            stormLevel = (excess >= range) ? 255 : (uint8_t)((uint16_t)excess * 255 / range);
        } else {
            // Below baseline — null / cancel field direction
            uint8_t deficit = 10 - anomaly_x10; // 0..10
            uint8_t range   = 10 - kAnomalyLo_x10; // 6
            stormLevel = (deficit >= range) ? 255 : (uint8_t)((uint16_t)deficit * 255 / range);
        }

        // Smooth stormLevel slightly to avoid jitter
        _stormSmooth = ((int32_t)7 * _stormSmooth + stormLevel) >> 3; // alpha ≈ 0.125
        uint8_t storm = (uint8_t)_stormSmooth;

        // ── 4. In-plane field direction ──────────────────────────────────
        // Panel coordinates: +X = right, +Y = up.
        // _smoothX/Y are already mapped to panel axes above.
        // We encode direction as a unit vector in fixed-point ×128.
        int16_t fx = (int16_t)constrain(_smoothX, (int32_t)-30000, (int32_t)30000);
        int16_t fy = (int16_t)constrain(_smoothY, (int32_t)-30000, (int32_t)30000);
        uint16_t fmag2d = isqrt16((uint32_t)((int32_t)fx*fx + (int32_t)fy*fy));
        if (fmag2d == 0) fmag2d = 1;

        // Normalised direction * 128
        int16_t dirX128 = (int16_t)((int32_t)fx * 128 / fmag2d);
        int16_t dirY128 = (int16_t)((int32_t)fy * 128 / fmag2d);

        // ── 5. Advance aurora drift phase ────────────────────────────────
        // Accumulate phase offset along the field direction.
        // _phaseAccX/Y are noise coordinate offsets (8-bit wrapping)
        _phaseAccX += (int32_t)dirX128 * kAuroraDriftSpeed / 128;
        _phaseAccY += (int32_t)dirY128 * kAuroraDriftSpeed / 128;

        // ── 6. Render ────────────────────────────────────────────────────
        // Hue: calm teal (96-112), heats to magenta-red (192-240) at full storm
        // calm hue ~104 (teal-green), storm hue ~224 (magenta)
        uint8_t baseHue    = 104;
        uint8_t stormHue   = 224;
        uint8_t hue        = lerp8by8(baseHue, stormHue, storm);

        // Base brightness: rises with storm
        uint8_t baseBright = lerp8by8(80, 220, storm);

        for (PixelIndex px = 0; px < LED_COUNT; ++px) {
            // Get pixel's rect position for noise lookup
            vectorf r = axial.rectFromPixelIndex(px);
            // Scale rect coords into ~0-255 noise space.
            // Panel spans roughly ±37 mm (~kMeridian/2 * spacing).
            // We map to ±64 noise units → divide by ~0.6 of meridian
            // Use integer: scale r.x, r.y by 3 (empirical)
            int16_t nx = (int16_t)(r.x * 3) + (int16_t)_phaseAccX;
            int16_t ny = (int16_t)(r.y * 3) + (int16_t)_phaseAccY;

            // 3 octaves of inoise8, folded together
            uint8_t t0 = (uint8_t)(millis() / 40);
            uint8_t t1 = (uint8_t)(millis() / 60 + 80);
            uint8_t t2 = (uint8_t)(millis() / 80 + 160);
            uint8_t n0 = inoise8((uint16_t)nx,          (uint16_t)ny,           t0);
            uint8_t n1 = inoise8((uint16_t)(nx*2 + 40), (uint16_t)(ny*2 + 77),  t1);
            uint8_t n2 = inoise8((uint16_t)(nx*4 +120), (uint16_t)(ny*4 +200),  t2);
            // Weighted blend: 4:2:1
            uint16_t nblend = ((uint16_t)n0 * 4 + (uint16_t)n1 * 2 + (uint16_t)n2) / 7;
            uint8_t nval = (uint8_t)nblend;

            // Contrast boost during storm: stretch nval away from 128.
            // At storm=0, nval passes through unchanged.
            // At storm=255, nval is clamped hard to 0 or 255.
            if (storm > 0) {
                uint8_t contrast = scale8(storm, 220); // max contrast factor
                if (nval > 128) {
                    uint16_t stretched = 128 + (uint16_t)scale8(nval - 128, 128 + (contrast >> 1));
                    nval = (uint8_t)min((uint16_t)255, stretched);
                } else {
                    uint8_t diff = 128 - nval;
                    uint16_t stretched = (uint16_t)scale8(diff, 128 + (contrast >> 1));
                    nval = (uint8_t)(stretched >= 128 ? 0 : 128 - stretched);
                }
            }

            // Brightness from noise + base, saturation full during storm
            uint8_t bri = scale8(nval, baseBright);
            uint8_t sat = lerp8by8(180, 255, storm);

            // Hue jitter: add small noise-driven hue variation for aurora shimmer
            uint8_t hueJitter = scale8(nval, lerp8by8(20, 40, storm));
            uint8_t pixHue = hue + hueJitter;

            ctx.leds[px] = CHSV(pixHue, sat, bri);
        }

        // ── 7. Filament — dim field-direction needle ─────────────────────
        // Fades out as storm rises (filament lost in storm noise)
        uint8_t filBright = scale8(kFilamentMaxBright, 255 - storm);
        if (filBright > 3) {
            // Build axial endpoints along the field direction
            // Scale direction to reach panel edge (kMeridian/2 ≈ 9 axial units)
            const float kEdge = kMeridian / 2.0f - 0.5f;
            // dirX128/dirY128 are panel rect units × 128
            // Convert rect direction to axial using rectToHex logic inline:
            // q = x/size,  r = 2/sqrt(3) * y/size  (size=1 for unit rect)
            // dirX128/128 = rect unit direction, scale to kEdge axial units
            float rdx = (float)dirX128 / 128.0f;
            float rdy = (float)dirY128 / 128.0f;
            // rectToHex with size=1: q = rdx - (1/sqrt(3))*rdy, r = (2/sqrt(3))*rdy
            // Use integer-friendly approximation: 1/sqrt(3) ≈ 37/64, 2/sqrt(3) ≈ 74/64
            // But we already have float here — keep it simple
            float aq =  rdx - 0.5774f * rdy;
            float ar =  1.1547f * rdy;
            // scale to edge
            float len = sqrtf(aq*aq + ar*ar);
            if (len < 0.001f) len = 0.001f;
            float edgeScale = kEdge / len;
            fAxial tip( aq * edgeScale,  ar * edgeScale);
            fAxial tail(-aq * edgeScale, -ar * edgeScale);

            CRGB filColor = CHSV(0, 0, filBright);
            hexline(ctx, tail, tip, filColor);
        }
    }

private:
    // Smoothed mag components in IMU LSB
    int32_t _smoothX{0}, _smoothY{0}, _smoothZ{0};
    // Baseline magnitude (slow drift estimate)
    int32_t _baseline{0};
    // Storm level smooth
    int32_t _stormSmooth{0};
    // Aurora drift phase accumulator (noise coordinate offset)
    int32_t _phaseAccX{0}, _phaseAccY{0};
};
