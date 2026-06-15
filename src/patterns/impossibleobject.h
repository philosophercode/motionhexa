#pragma once

// ImpossibleObject: a fake-3D shaded sphere lit from a fixed world-space point.
// Tilt the device and the specular highlight rolls across the surface exactly
// as if you were holding a real lit ball.
//
// PERFORMANCE NOTE: RP2040 is Cortex-M0+ with no FPU.
// Strategy: precompute int8 surface normals once; rotate only the light vector
// (and band axis) into panel frame once per frame via float quat-rotate; then
// all per-pixel work is integer MACs.

#include "../patterns.h"   // Pattern base, ctx, PaletteRotation, LED_COUNT, etc.
#include "../ledgraph.h"   // hexGrid, kMeridian
#include "../MotionManager.h"

namespace impossibleobject_detail {

// -----------------------------------------------------------------------
// Tunables
// -----------------------------------------------------------------------

// World-space light direction (un-normalised; normalised at build-time in init).
// (0.5, 0.5, 0.7) → roughly north-east and upward.
static constexpr float kLightX_w = 0.5f;
static constexpr float kLightY_w = 0.5f;
static constexpr float kLightZ_w = 0.7f;

// Sphere fills this fraction of the panel inradius.
static constexpr float kSphereRadiusFraction = 0.90f;

// Specular power: applied as repeated squarings.
// 3 squarings → x^8, bright tight highlight.
static constexpr int kSpecularSquarings = 3; // pow8

// Specular intensity (0-255 added on top of diffuse lit colour).
static constexpr uint8_t kSpecularMax = 220;

// Rim-light brightness cap (subtle edge glow).
static constexpr uint8_t kRimMax = 60;

// Latitude-band depth: modulates albedo by ±kBandDepth (0-255 scale).
static constexpr uint8_t kBandDepth = 30;

// Background colour (whisper of dark blue for silhouette).
static constexpr CRGB kBgColor = CRGB(0, 0, 8);

// Fixed-point scale for stored normals: int8, range -127..127.
static constexpr int8_t kNormScale = 127;

// -----------------------------------------------------------------------
// Helper: quaternion rotate (identical to TriangleSpin::quatRotate)
// -----------------------------------------------------------------------
inline vectorf qr(const Quaternion &q, vectorf v) {
    float tx = 2.0f * (q.y * v.z - q.z * v.y);
    float ty = 2.0f * (q.z * v.x - q.x * v.z);
    float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return vectorf(
        v.x + q.w * tx + (q.y * tz - q.z * ty),
        v.y + q.w * ty + (q.z * tx - q.x * tz),
        v.z + q.w * tz + (q.x * ty - q.y * tx));
}

// Return a quaternion conjugate (negate xyz).
inline Quaternion conj(const Quaternion &q) {
    return {q.w, -q.x, -q.y, -q.z};
}

// Integer pow via repeated squaring in uint8 space (scale8 divides by 255).
// k squarings: result = v^(2^k) in uint8.
inline uint8_t pow_uint8(uint8_t v, int squarings) {
    for (int i = 0; i < squarings; ++i) {
        v = scale8(v, v);
    }
    return v;
}

} // namespace impossibleobject_detail


class ImpossibleObject : public Pattern, public PaletteRotation<CRGBPalette256> {
public:

    // Per-pixel cached normals (int8, scale kNormScale).
    // nz[i] == -1 means pixel is outside sphere (background).
    int8_t _nx[LED_COUNT];
    int8_t _ny[LED_COUNT];
    int8_t _nz[LED_COUNT]; // -128 sentinel for "outside sphere"

    // Normalised world-space light direction (int8, scale kNormScale).
    int8_t _Lw_x, _Lw_y, _Lw_z;

    ImpossibleObject() {
        secondsPerPalette = 20;
        maxColorJump = 40;

        // ---------------------------------------------------------------
        // Precompute sphere radius in micrometers.
        // Panel inradius ≈ (kMeridian-1)/2 * sin(60°) * spacing_mm * 1000 µm
        // spacing is in mm; hexGrid uses fromMM() which multiplies by 1000.
        // We can derive inradius from the position of a known edge pixel,
        // but a clean formula: inradius = ((kMeridian-1)/2) * colSpacing_um
        // colSpacing_um = sin(PI/3) * 3900   (3.9 mm * 1000)
        // ---------------------------------------------------------------
        constexpr float kSpacingUM   = 3900.0f;           // 3.9 mm in µm
        constexpr float kColSpacingUM = 0.86602540f * kSpacingUM; // sin(60°)
        constexpr float kInradiusUM  = ((kMeridian - 1) / 2.0f) * kColSpacingUM;
        const float     sphereRadiusUM = kInradiusUM * impossibleobject_detail::kSphereRadiusFraction;
        const float     invSphereR     = 1.0f / sphereRadiusUM;

        // ---------------------------------------------------------------
        // Precompute per-pixel normals
        // ---------------------------------------------------------------
        for (PixelIndex i = 0; i < LED_COUNT; ++i) {
            UMPoint pos = hexGrid.position(i);
            float px = pos.x * invSphereR;
            float py = pos.y * invSphereR;
            float r2 = px * px + py * py;
            if (r2 >= 1.0f) {
                // Outside sphere: sentinel
                _nx[i] = 0;
                _ny[i] = 0;
                _nz[i] = -128; // sentinel
            } else {
                float pz = sqrtf(1.0f - r2);
                _nx[i] = (int8_t)(px * impossibleobject_detail::kNormScale);
                _ny[i] = (int8_t)(py * impossibleobject_detail::kNormScale);
                _nz[i] = (int8_t)(pz * impossibleobject_detail::kNormScale);
            }
        }

        // ---------------------------------------------------------------
        // Normalise and store world-space light direction (int8)
        // ---------------------------------------------------------------
        using namespace impossibleobject_detail;
        float lx = kLightX_w, ly = kLightY_w, lz = kLightZ_w;
        float llen = sqrtf(lx*lx + ly*ly + lz*lz);
        _Lw_x = (int8_t)( (lx / llen) * kNormScale );
        _Lw_y = (int8_t)( (ly / llen) * kNormScale );
        _Lw_z = (int8_t)( (lz / llen) * kNormScale );
    }

    void update() override {
        using namespace impossibleobject_detail;

        // ---------------------------------------------------------------
        // 1. Get quaternion; guard against un-initialised DMP (all-zero).
        // ---------------------------------------------------------------
        Quaternion q = MotionManager::motionFrame.quat;
        bool dmpReady = (q.w != 0.0f || q.x != 0.0f || q.y != 0.0f || q.z != 0.0f);
        if (!dmpReady) {
            // Light straight ahead (identity rotation)
            q = {1.0f, 0.0f, 0.0f, 0.0f};
        }

        // ---------------------------------------------------------------
        // 2. Rotate world-space light into panel frame (1 quat-rotate / frame).
        //    Convention (proven by compass pattern): conjugate of motionFrame.quat
        //    rotates a world-frame vector into panel frame.
        // ---------------------------------------------------------------
        Quaternion qc = conj(q);
        vectorf Lw( (float)_Lw_x, (float)_Lw_y, (float)_Lw_z );
        vectorf Lp = qr(qc, Lw);   // panel-frame light, still in int8 scale

        // Convert to int8 panel-frame light components.
        int8_t Lpx = (int8_t)constrain((int)Lp.x, -127, 127);
        int8_t Lpy = (int8_t)constrain((int)Lp.y, -127, 127);
        int8_t Lpz = (int8_t)constrain((int)Lp.z, -127, 127);

        // ---------------------------------------------------------------
        // 3. Half-vector H = normalize(Lp + view(0,0,kNormScale)) — also
        //    in int8 scale relative to Lp. Compute in float once per frame.
        // ---------------------------------------------------------------
        float Hx = Lp.x;
        float Hy = Lp.y;
        float Hz = Lp.z + (float)kNormScale; // view = (0,0,kNormScale)
        float Hlen = sqrtf(Hx*Hx + Hy*Hy + Hz*Hz);
        if (Hlen < 0.001f) Hlen = 0.001f;
        int8_t Hhx = (int8_t)((Hx / Hlen) * kNormScale);
        int8_t Hhy = (int8_t)((Hy / Hlen) * kNormScale);
        int8_t Hhz = (int8_t)((Hz / Hlen) * kNormScale);

        // ---------------------------------------------------------------
        // 4. Rotate world-space band axis (0,0,1) into panel frame (1 quat-rotate).
        //    Per-pixel band = n · bandAxis_panel (integer MAC).
        // ---------------------------------------------------------------
        vectorf bandAxisW(0.0f, 0.0f, (float)kNormScale); // world Z pole
        vectorf bandAxisP = qr(qc, bandAxisW);
        int8_t bax = (int8_t)constrain((int)bandAxisP.x, -127, 127);
        int8_t bay = (int8_t)constrain((int)bandAxisP.y, -127, 127);
        int8_t baz = (int8_t)constrain((int)bandAxisP.z, -127, 127);

        // ---------------------------------------------------------------
        // 5. Per-pixel shading — all integer MACs after this point.
        // ---------------------------------------------------------------
        uint32_t t = millis();
        uint16_t paletteOffset = (uint16_t)(t / 100);

        for (PixelIndex i = 0; i < LED_COUNT; ++i) {
            if (_nz[i] == -128) {
                // Background: near-black dark blue silhouette
                ctx.leds[i] = kBgColor;
                continue;
            }

            int8_t nx = _nx[i];
            int8_t ny = _ny[i];
            int8_t nz = _nz[i];

            // --- Diffuse: max(0, n · L) ---
            // dot is sum of int8 products → int32, in scale kNormScale²
            int32_t diffDot = (int32_t)nx * Lpx + (int32_t)ny * Lpy + (int32_t)nz * Lpz;
            // Clamp to [0, kNormScale²], map to uint8 [0,255]
            uint8_t diffuse;
            if (diffDot <= 0) {
                diffuse = 0;
            } else {
                // max value is kNormScale² = 127² = 16129
                // scale to [0,255]: diffuse = diffDot * 255 / (127*127)
                diffuse = (uint8_t)((uint32_t)diffDot * 255u / (127u * 127u));
            }

            // --- Specular: pow8(max(0, n · H)) ---
            int32_t specDot = (int32_t)nx * Hhx + (int32_t)ny * Hhy + (int32_t)nz * Hhz;
            uint8_t spec = 0;
            if (specDot > 0) {
                uint8_t sv = (uint8_t)((uint32_t)specDot * 255u / (127u * 127u));
                sv = pow_uint8(sv, kSpecularSquarings);
                spec = scale8(sv, kSpecularMax);
            }

            // --- Rim light: bright when nz near 0 (edge of sphere) ---
            // rim = kRimMax * (1 - |nz|/kNormScale)² — use scale8 for cheap squaring
            uint8_t nzAbs = (uint8_t)((nz < 0 ? -nz : nz));
            uint8_t rimFactor = 255u - (uint8_t)((uint16_t)nzAbs * 255u / 127u);
            rimFactor = scale8(rimFactor, rimFactor); // square it for tighter rim
            uint8_t rim = scale8(rimFactor, kRimMax);

            // --- Banding: n · bandAxis (latitude in world frame) → slow sine-like modulation ---
            int32_t bandDot = (int32_t)nx * bax + (int32_t)ny * bay + (int32_t)nz * baz;
            // bandDot in [-127², +127²]; map to [0,255]
            uint8_t bandU = (uint8_t)(((int32_t)bandDot + 16129) * 255u / (2u * 16129u));
            // sin-like: use sin8(bandU) → [0,255]; center and halve for ± modulation
            int16_t bandMod = (int16_t)sin8(bandU) - 128; // [-128,127]
            // scale to ±kBandDepth
            int16_t bandAdj = (bandMod * (int16_t)kBandDepth) >> 7; // ÷128 → ±kBandDepth

            // --- Base albedo colour from slowly rotating palette ---
            // Use diffuse as colour index offset for a bit of shading colour shift
            uint16_t colorIdx = paletteOffset + (uint16_t)diffuse;
            CRGB baseColor = getMirroredPaletteColor(colorIdx);

            // --- Modulate by diffuse, add banding, add rim, add specular ---
            // Diffuse modulation: scale colour by diffuse (keep some ambient)
            uint8_t ambientPlusD = qadd8(60, scale8(diffuse, 195)); // ambient=60/255, diffuse up to 195/255
            baseColor = baseColor.nscale8(ambientPlusD);

            // Banding: brighten or darken the base colour
            if (bandAdj > 0) {
                uint8_t ba = (uint8_t)bandAdj;
                baseColor.r = qadd8(baseColor.r, scale8(ba, baseColor.r >> 1));
                baseColor.g = qadd8(baseColor.g, scale8(ba, baseColor.g >> 1));
                baseColor.b = qadd8(baseColor.b, scale8(ba, baseColor.b >> 1));
            } else if (bandAdj < 0) {
                uint8_t ba = (uint8_t)(-bandAdj);
                baseColor.r = qsub8(baseColor.r, scale8(ba, baseColor.r >> 1));
                baseColor.g = qsub8(baseColor.g, scale8(ba, baseColor.g >> 1));
                baseColor.b = qsub8(baseColor.b, scale8(ba, baseColor.b >> 1));
            }

            // Rim light (blueish-white tint)
            CRGB rimColor(scale8(rim, 80), scale8(rim, 100), rim);
            baseColor += rimColor;

            // Specular (white)
            uint8_t s3 = spec;
            baseColor.r = qadd8(baseColor.r, s3);
            baseColor.g = qadd8(baseColor.g, s3);
            baseColor.b = qadd8(baseColor.b, s3);

            ctx.leds[i] = baseColor;
        }
    }

    const char *description() override {
        return "ImpossibleObject";
    }
};
