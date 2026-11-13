/* Boyle.h — a clean stereo compressor for the ER-301, after the WMD MSCL.
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
 *
 * Feedforward, stereo-linked, all gain maths in dB:
 *
 *   detector  max(L², R²) (or Key², from the key branch) through an optional
 *             high-pass, into a power envelope with attack and release
 *             (one-pole up, one-pole down). Release decays linearly in dB.
 *   computer  a soft knee (kKneeDb wide, the "Over Easy" shape): no
 *             reduction below the knee, slope s = 1 - 1/ratio above it.
 *   comp      the MSCL's one knob, 0..1. It lowers the threshold
 *             (T = range - kCompSpanDb * c), raises the ratio (s = c, so
 *             1:1 .. 4:1 at 0.75 .. ∞:1 at 1) and adds the makeup that keeps
 *             a signal at the top of the range at the same level
 *             (M = s * kCompSpanDb * c), capped by the Limit option.
 *   gain      output gain in dB, ±36.
 *   mix       dry/wet, for parallel compression.
 *
 * Options (each at most three choices, standard §7):
 *   Range   hot / mid / low: where the threshold starts (rangeDb)
 *   Limit   ∞ / 9 dB / 3 dB: the cap on the comp knob's makeup
 *   HPF     off / 60 Hz / 120 Hz: on the detector only, so bass doesn't pump
 *   Detect  self / key: what the detector listens to
 *
 * DEFAULTS ARE AN EXACT BYPASS (standard §4): Comp 0, Gain 0 dB, any Mix ->
 * every term of the gain is exactly 0 dB, the gain is exactly 1 and the
 * output equals the input, bit for bit (test mode `ident`).
 *
 * Audio-thread rules (§2, §2a, §5, §5a): no allocation, no locals bigger than
 * a few scalars, no integer divide or modulo, every inlet sanitised at the
 * read, every accumulator guarded. Coefficients that depend on options are
 * constants chosen by index, never computed in process(). */

#pragma once

#ifndef SWIGLUA
#include <od/objects/Object.h>
#include <od/config.h>
#include <cstdint>
#endif

namespace boyle {

class Boyle : public od::Object
{
public:
    Boyle();
    virtual ~Boyle();

    /* ── THE LUA-CALLABLE API ───────────────────────────────────────────
     * EVERYTHING Lua CALLS ON THE HEAD IS DECLARED HERE, ABOVE THE GUARD.
     * Declarations only, plain types only (standard §1a). */

    float getReduction();     // gain reduction in force, dB, >= 0
    float getInLevel();       // decaying peak of the (linked) input, linear
    float getOutLevel();      // decaying peak of the (linked) output, linear
    float getThreshold();     // dBFS, from Comp and Range
    float getRatio();         // 1 .. 1000 (1000 stands for ∞)
    float getMakeup();        // dB, from Comp, after the Limit cap
    float getOutputGain();    // dB, the gain control

#ifndef SWIGLUA
    void process() override;

    od::Inlet  mLeftIn    {"Left In"};
    od::Inlet  mRightIn   {"Right In"};
    od::Inlet  mKeyIn     {"Key"};
    od::Inlet  mCompIn    {"Comp"};
    od::Inlet  mAttackIn  {"Attack"};
    od::Inlet  mReleaseIn {"Release"};
    od::Inlet  mGainIn    {"Gain"};
    od::Inlet  mMixIn     {"Mix"};
    od::Outlet mLeftOut   {"Left Out"};
    od::Outlet mRightOut  {"Right Out"};
    // 1-based (never 0: CHOICE_UNKNOWN reads as the default)
    od::Option mRangeOpt  {"Range", 2};    // 1 hot, 2 mid, 3 low
    od::Option mLimitOpt  {"Limit", 1};    // 1 ∞, 2 9 dB, 3 3 dB
    od::Option mHpfOpt    {"HPF", 2};      // 1 off, 2 60 Hz, 3 120 Hz
    od::Option mDetectOpt {"Detect", 1};   // 1 self, 2 key

    static constexpr float kSampleRate  = 48000.0f;
    static constexpr float kKneeDb      = 10.0f;    // soft knee width
    static constexpr float kCompSpanDb  = 24.0f;    // how far Comp lowers the threshold
    static constexpr float kAttackMin   = 0.00015f; // s (MSCL: 0.15 ms .. 100 ms)
    static constexpr float kAttackMax   = 0.1f;
    static constexpr float kReleaseMin  = 0.0006f;  // s (MSCL: 0.6 ms .. 4 s)
    static constexpr float kReleaseMax  = 4.0f;
    static constexpr float kGainMaxDb   = 36.0f;
    static constexpr float kEnvFloor    = 1.0e-12f; // -120 dBFS: also keeps the envelope out of denormals
    static constexpr float kSmooth      = 0.002f;   // control one-pole: ~10 ms
    // Closer than this, a smoothed control IS its target (see the template's
    // memory.md: the snap must sit above the one-pole's stall point).
    static constexpr float kSnap        = 1.0e-4f;
    static constexpr float kPeakDecay   = 0.9f;     // per block, for the meter

    // Threshold at Comp 0 for each Range, dBFS of the detector (a full-scale
    // ±1 peak is 0 dB; eurorack ±5 V is about -6).
    static float rangeDb(int choice)
    {
        return choice == 1 ? -6.0f : choice == 3 ? -24.0f : -15.0f;
    }
    static float limitDb(int choice)
    {
        return choice == 2 ? 9.0f : choice == 3 ? 3.0f : kCompSpanDb;
    }

    // The static curve, exposed for the tests: reduction in dB (>= 0) for a
    // detector level x (dB), threshold t (dB) and slope s.
    static float reductionDb(float x, float t, float s)
    {
        const float over = x - t;
        const float h = 0.5f * kKneeDb;
        if (over <= -h) return 0.0f;
        if (over >= h) return s * over;
        const float k = over + h;
        return s * k * k * (0.5f / kKneeDb);
    }

    // log2 and exp2 to ~1e-6 (no libm in the sample loop).
    static float fastLog2(float x);   // x > 0
    static float fastExp2(float x);   // clamps to [-126, 126]
    static float onePole(float tau);  // 1 - e^(-1 / (tau fs)), tau in seconds

private:
    float mEnv = kEnvFloor;           // detector power (accumulator: guarded)
    float mComp = 0.0f, mGainDb = 0.0f, mMix = 1.0f;   // smoothed controls
    float mZ[2][2] = { { 0, 0 }, { 0, 0 } };           // detector HPF states
    float mReduction = 0.0f, mThreshold = -15.0f, mSlope = 0.0f, mMakeup = 0.0f;
    float mInPk = 0.0f, mOutPk = 0.0f;

    static inline float sanitize(float x)
    {
        union { float f; uint32_t u; } v; v.f = x;
        return ((v.u & 0x7f800000u) == 0x7f800000u) ? 0.0f : x;
    }
    static inline float clampf(float x, float lo, float hi)
    {
        if (!(x >= lo)) return lo;           // also catches NaN
        return x > hi ? hi : x;
    }
    static inline float maxAbs(float a, float b)
    {
        a = a < 0.0f ? -a : a;
        b = b < 0.0f ? -b : b;
        return a > b ? a : b;
    }
    static inline float smooth(float g, float target)
    {
        const float d = target - g;
        return (d < kSnap && d > -kSnap) ? target : g + d * kSmooth;
    }
#endif // SWIGLUA
};

} // namespace boyle
