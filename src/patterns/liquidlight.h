#pragma once
// LiquidLight — shallow-water fluid simulation on the 271-cell hex lattice.
// All helpers and state live inside the class or namespace liquidlight_detail
// so no new names leak into global scope.

namespace liquidlight_detail {

// Fixed-point scale factor: 1.0 physical unit = (1 << FP_SHIFT) counts.
// We use Q12 (shift=12, scale=4096) so that cell heights fit in int32_t.
static constexpr int FP_SHIFT = 12;
static constexpr int32_t FP_ONE = (1 << FP_SHIFT); // 4096

// Volume target: keep average height = FP_ONE (each cell "full" on average
// when flat). ~40-50% of cells actually visible comes from the distribution.
static constexpr int32_t kRestHeight = FP_ONE;

// --- Tunable constants ---

// Damping per sub-step at 120 Hz: (255-kDamp)/256 per step.
// kDamp=8 => ~96.8% per step => after 120 steps (~1 s) ~ 0.97^120 ~ 0.024
// so waves ring for roughly 1-2 s.
static constexpr int kDamp = 8; // 0..255; higher = more damping

// How strongly gravity/centrifugal force drives flux.
// Raw accel is in accelToGScale LSB/g = 8192 LSB/g.
// kFlowGain scales it to flux units: increase to make liquid more reactive.
static constexpr int32_t kFlowGain = 55; // tunable; raise if liquid feels sluggish

// Shake splash: if magnitude of accel (in accel LSB) exceeds this, inject noise.
// 8192 = 1 g; 2 g spike threshold.
static constexpr int32_t kShakeThreshold = 16384; // 2 g in LSB

// Splash injection height/velocity amplitude (in FP units).
static constexpr int32_t kSplashAmp = FP_ONE * 3;

// Volume drift correction: each frame nudge total volume toward target by 1/kVolCorrect.
static constexpr int kVolCorrect = 64;

// Height clamp: prevent negative heights and excessive over-fill.
static constexpr int32_t kMinH = 0;
static constexpr int32_t kMaxH = FP_ONE * 6;

// Velocity clamp (flux per sub-step, in FP units / step).
static constexpr int32_t kMaxFlux = FP_ONE * 2;

// Fixed timestep: target 120 sub-steps per second.
// We run at most kMaxStepsPerFrame sub-steps per update() call.
static constexpr int kSubStepHz = 120;
static constexpr int kMaxStepsPerFrame = 3;

// Render: height threshold below which a cell appears black.
// Cells with h < kDeepThreshold are "shallow" (dark). Above = bright, saturated.
static constexpr int32_t kDeepThreshold = FP_ONE / 4; // 25% of rest height

// Specular: added brightness when height gradient is large and cell faces up.
// Gradient proxy: sum of |h[i] - h[neighbor]| across all neighbors.
static constexpr int32_t kSpecGradThresh = FP_ONE / 3; // gradient trigger
static constexpr uint8_t kSpecBrightness = 80;  // added LED brightness

// Sign flip for gravity forcing — flip if liquid flows "uphill" on device.
static constexpr int kGravitySign = 1; // +1 or -1

} // namespace liquidlight_detail

// Namespace alias — outside the class so it is a proper namespace alias, not a type alias.
namespace liquidlight_ns = liquidlight_detail;

class LiquidLight : public Pattern, PaletteRotation<CRGBPalette256> {
public:

    // -----------------------------------------------------------------------
    // State: height field and flux field, both Q12 fixed-point int32
    // -----------------------------------------------------------------------
    int32_t h[LED_COUNT];    // height of liquid at each cell
    int32_t flux[LED_COUNT]; // accumulated net flux (velocity proxy), per cell

    // Accumulated real time for sub-stepping
    int32_t accumulatedMs = 0;

    // Smoothed acceleration magnitude for shake detection
    int32_t smoothAccMag = 0;

    // For palette rotation
    vector32 smoothAccVec;

    LiquidLight() {
        maxColorJump = 20;
        secondsPerPalette = 12;

        // Initialize: fill all cells to rest height, zero flux.
        for (int i = 0; i < LED_COUNT; ++i) {
            h[i] = liquidlight_ns::kRestHeight;
            flux[i] = 0;
        }
        // Tilt the initial surface slightly so it looks natural from frame 1.
        // Use a gentle +y gradient (liquid pools to the "bottom").
        for (int i = 0; i < LED_COUNT; ++i) {
            Axial ax = axial.axialFromPixelIndex(i);
            // ax.r() ranges roughly -9..+9; tilt by half a rest-height unit
            h[i] = liquidlight_ns::kRestHeight + (int32_t)(ax.r()) * (liquidlight_ns::kRestHeight / 18);
            h[i] = constrain(h[i], liquidlight_ns::kMinH, liquidlight_ns::kMaxH);
        }
    }

    // -----------------------------------------------------------------------
    // Physics sub-step: one step of the hex pipe-model
    //   dt_ms: sub-step duration in milliseconds (usually 1000/kSubStepHz ~= 8)
    // -----------------------------------------------------------------------
    void physicsStep(int32_t dt_ms, ICM_20948_AGMT_t &agmt) {
        // For each cell, compute net height exchange with each neighbor.
        // We process every directed pair (i -> neighbor) once.
        // flux[i] acts as a running momentum/velocity for each cell.

        // We use a local scratch array for height deltas to avoid order-dependency.
        static int32_t dh[LED_COUNT];
        for (int i = 0; i < LED_COUNT; ++i) dh[i] = 0;

        for (int i = 0; i < LED_COUNT; ++i) {
            // Get per-cell acceleration (includes centrifugal term from spin).
            // Only compute for a fraction of cells to save cycles:
            // use axial position to derive acceleration cheaply at any cell.
            vector32 accel = accelerationAtPixelIndex((PixelIndex)i, agmt);
            // accel.x = right (+x), accel.y = up (+y) in panel frame
            // Gravity makes liquid "fall" in the direction of acceleration.
            // Multiply by kFlowGain and sign-flip per constant.

            // Iterate over geometric neighbors from the hex adjacency.
            // We only push from i to neighbors where h[i] > h[neighbor],
            // proportional to the height difference + acceleration component.
            auto *node = hexGrid[i];
            for (int nd = 0; nd < 6; ++nd) {
                auto *nbNode = node->neighbors[nd];
                if (!nbNode || !nbNode->isDataNode()) continue;
                PixelIndex j = nbNode->data();

                // Direction vector for this neighbor direction (unit hex edge direction).
                // Neighbor order: 0=ul, 1=ur, 2=r, 3=dr, 4=dl, 5=l
                // In panel space (x right, y up):
                static const int16_t ndx[6] = { -1,  1,  2,  1, -1, -2 }; // * 128
                static const int16_t ndy[6] = {  2,  2,  0, -2, -2,  0 }; // * 128
                // These are proportional to the hex neighbor unit vectors * 256.
                // The neighbor vectors in hex axial correspond to:
                //   ul: dq=-1,dr=-1 => rect approx (-1.5,  0.87) * spacing
                //   ur: dq=+1,dr=-1 => rect approx (+0.5,  0.87)
                //   r:  dq=+1,dr= 0 => rect approx (+1.0,  0.0 )
                //   ... etc.
                // We just use a sign-correct approximation scaled to 256 units.
                // The dot product accel . direction gives the gravitational "slope".
                int32_t gravComp = (int32_t)accel.x * ndx[nd] + (int32_t)accel.y * ndy[nd];
                // gravComp is in accel_LSB * 128 units; divide by 128 to normalize.
                gravComp = (liquidlight_ns::kGravitySign * gravComp) >> 7;

                // Height difference drives flow from high to low.
                int32_t hDiff = h[i] - h[j];

                // Pressure-like forcing: proportional to hDiff + gravity component.
                // We scale down by FP_ONE to keep units consistent.
                int32_t drive = hDiff + (gravComp * liquidlight_ns::kFlowGain * dt_ms) / 1000;

                // Only flow if drive > 0 (from i toward j).
                // Cap by available height to preserve conservation roughly.
                if (drive > 0) {
                    // Amount to transfer this step: limit to fraction of drive.
                    int32_t transfer = drive >> 3; // 1/8 of pressure each step
                    // Cap to avoid over-draining the cell.
                    int32_t maxTransfer = (h[i] - liquidlight_ns::kMinH) >> 2;
                    if (transfer > maxTransfer) transfer = maxTransfer;
                    if (transfer < 0) transfer = 0;
                    dh[i] -= transfer;
                    dh[j] += transfer;
                }
            }
        }

        // Apply height deltas with damping.
        for (int i = 0; i < LED_COUNT; ++i) {
            h[i] += dh[i];
            // Damp flux (stored in h directly; flux[] tracks oscillation velocity).
            // Damping: multiply by (256-kDamp)/256 each step.
            flux[i] = (flux[i] * (256 - liquidlight_ns::kDamp)) >> 8;
            h[i] = constrain(h[i], liquidlight_ns::kMinH, liquidlight_ns::kMaxH);
        }

        // Volume conservation: compute total volume and nudge toward target.
        int32_t totalH = 0;
        for (int i = 0; i < LED_COUNT; ++i) totalH += h[i];
        int32_t target = (int32_t)liquidlight_ns::kRestHeight * LED_COUNT;
        int32_t diff = target - totalH; // positive = need to add volume
        // Spread correction uniformly.
        int32_t correction = diff / (int32_t)(LED_COUNT * liquidlight_ns::kVolCorrect);
        if (correction != 0) {
            for (int i = 0; i < LED_COUNT; ++i) {
                h[i] += correction;
                h[i] = constrain(h[i], liquidlight_ns::kMinH, liquidlight_ns::kMaxH);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Inject a splash: add randomized height/velocity noise scaled by spike.
    // -----------------------------------------------------------------------
    void splash(int32_t spikeMag) {
        // spikeMag in accel LSB above threshold; scale to splash amplitude.
        int32_t amp = (int32_t)liquidlight_ns::kSplashAmp * min(spikeMag, (int32_t)32767) / 32767;
        for (int i = 0; i < LED_COUNT; ++i) {
            // Random signed perturbation.
            int32_t r = (int32_t)(random8()) - 128; // -128..127
            h[i] += (amp * r) >> 8;
            h[i] = constrain(h[i], liquidlight_ns::kMinH, liquidlight_ns::kMaxH);
        }
    }

    // -----------------------------------------------------------------------
    // Pattern update — called every frame
    // -----------------------------------------------------------------------
    void update() override {
        auto agmt = MotionManager::motionFrame.agmt;

        // ---- Shake detection ----
        // Compute acceleration magnitude (no sqrt needed; compare squared later).
        // We use a fast approximate magnitude: |x| + |y| + |z| (Manhattan).
        int32_t ax = agmt.acc.axes.x;
        int32_t ay = agmt.acc.axes.y;
        int32_t az = agmt.acc.axes.z;
        int32_t rawMag = (ax < 0 ? -ax : ax) + (ay < 0 ? -ay : ay) + (az < 0 ? -az : az);

        // Smooth magnitude with a simple IIR.
        smoothAccMag = (smoothAccMag * 7 + rawMag) >> 3;

        if (rawMag > liquidlight_ns::kShakeThreshold) {
            int32_t spike = rawMag - liquidlight_ns::kShakeThreshold;
            splash(spike);
        }

        // ---- Palette rotation driven by gyro z ----
        paletteRotate(agmt.gyr.axes.z / 800);

        // ---- Sub-stepping ----
        unsigned long ft = frameTime();
        if (ft > 100) ft = 100; // clamp to 100 ms max to avoid spiral on pause
        accumulatedMs += (int32_t)ft;

        const int32_t stepMs = 1000 / liquidlight_ns::kSubStepHz; // ~8 ms
        int steps = 0;
        while (accumulatedMs >= stepMs && steps < liquidlight_ns::kMaxStepsPerFrame) {
            physicsStep(stepMs, agmt);
            accumulatedMs -= stepMs;
            ++steps;
        }

        // ---- Render ----
        // Map height to color:
        //   h == 0                => black
        //   h in [kDeepThreshold, kRestHeight] => dim, desaturated
        //   h >= kRestHeight      => full palette color
        // Plus specular shimmer on steep gradient cells.

        for (PixelIndex px = 0; px < LED_COUNT; ++px) {
            int32_t cellH = h[px];

            if (cellH <= liquidlight_ns::kDeepThreshold) {
                ctx.leds[px] = CRGB::Black;
                continue;
            }

            // Brightness: linearly ramp from 0 at kDeepThreshold to 255 at 2*kRestHeight.
            int32_t rampRange = liquidlight_ns::kRestHeight * 2 - liquidlight_ns::kDeepThreshold;
            int32_t rampVal   = cellH - liquidlight_ns::kDeepThreshold;
            if (rampVal > rampRange) rampVal = rampRange;
            uint8_t brightness = (uint8_t)((rampVal * 255) / rampRange);

            // Saturation: full at kRestHeight+, dims below.
            uint8_t sat = (cellH >= liquidlight_ns::kRestHeight) ? 255
                        : (uint8_t)((cellH - liquidlight_ns::kDeepThreshold) * 255
                                    / (liquidlight_ns::kRestHeight - liquidlight_ns::kDeepThreshold));

            // Hue from palette, shifted by depth.
            uint16_t paletteIdx = (uint16_t)(millis() / 80)
                                + (uint16_t)(cellH * 96 / liquidlight_ns::kRestHeight);
            CRGB c = getMirroredPaletteColor((uint8_t)(paletteIdx & 0xFF), brightness);
            // Desaturate shallow cells toward black rather than white.
            // scale8 fades toward black, which is what we want.
            c.nscale8(sat);

            // ---- Specular shimmer ----
            // Proxy: sum of absolute height differences to neighbors.
            int32_t grad = 0;
            auto *node = hexGrid[px];
            uint8_t nbCount = 0;
            for (int nd = 0; nd < 6; ++nd) {
                auto *nbNode = node->neighbors[nd];
                if (!nbNode || !nbNode->isDataNode()) continue;
                PixelIndex j = nbNode->data();
                int32_t dh = h[px] - h[j];
                grad += (dh < 0 ? -dh : dh);
                ++nbCount;
            }
            if (nbCount > 0) {
                grad /= nbCount; // average gradient
                if (grad > liquidlight_ns::kSpecGradThresh) {
                    // Add specular only on upward-facing (high-h) cells.
                    // "Facing up" proxy: height above rest level.
                    if (cellH > liquidlight_ns::kRestHeight) {
                        int32_t specScale = (grad - liquidlight_ns::kSpecGradThresh);
                        if (specScale > liquidlight_ns::kSpecGradThresh) specScale = liquidlight_ns::kSpecGradThresh;
                        uint8_t specB = (uint8_t)(liquidlight_ns::kSpecBrightness
                                        * specScale / liquidlight_ns::kSpecGradThresh);
                        // Add white shimmer.
                        c.r = qadd8(c.r, specB);
                        c.g = qadd8(c.g, specB);
                        c.b = qadd8(c.b, specB);
                    }
                }
            }

            ctx.leds[px] = c;
        }
    }

    const char *description() override {
        return "LiquidLight";
    }
};
