/* Boyle.cpp — a clean stereo compressor for the ER-301, after the WMD MSCL.
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
 *
 * See Boyle.h for the design. */

#include "Boyle.h"

namespace boyle {

// Detector high-pass: 2nd-order Butterworth at 48 kHz, one row per HPF
// choice (row 0 unused: off). b1 = -2 b0, b2 = b0. Computed off-device
// (K = tan(pi f / fs)), so process() only picks a row (§2).
static const float kHpfB0[3] = { 1.0f, 0.99446179f, 0.98895425f };
static const float kHpfA1[3] = { 0.0f, -1.98889291f, -1.97778648f };
static const float kHpfA2[3] = { 0.0f, 0.98895425f, 0.97803051f };

Boyle::Boyle()
{
    // Every port is registered here, or Lua's connect() fails on the device
    // (tools/check-ports.sh gates this).
    addInput(mLeftIn);
    addInput(mRightIn);
    addInput(mKeyIn);
    addInput(mCompIn);
    addInput(mAttackIn);
    addInput(mReleaseIn);
    addInput(mGainIn);
    addInput(mMixIn);
    addOutput(mLeftOut);
    addOutput(mRightOut);
    addOption(mRangeOpt);
    addOption(mLimitOpt);
    addOption(mHpfOpt);
    addOption(mDetectOpt);
}

Boyle::~Boyle() {}

/* ── fast maths ────────────────────────────────────────────────────────── */

// log2(x) = exponent + log2(mantissa), the mantissa folded into
// [sqrt(1/2), sqrt(2)) and expanded as 2/ln2 * atanh(s), s = (m-1)/(m+1),
// |s| < 0.172: the next term is under 2e-6.
float Boyle::fastLog2(float x)
{
    union { float f; uint32_t u; } v; v.f = x;
    float e = (float)(int)((v.u >> 23) & 0xffu) - 127.0f;
    v.u = (v.u & 0x007fffffu) | 0x3f800000u;
    float m = v.f;
    if (m > 1.41421356f) { m *= 0.5f; e += 1.0f; }
    const float s = (m - 1.0f) / (m + 1.0f);
    const float s2 = s * s;
    return e + s * (2.88539008f + s2 * (0.96179669f + s2 * (0.57707802f + s2 * 0.41219858f)));
}

// 2^x = 2^i * 2^f, 2^f by its Taylor series to f^6 (relative error < 2e-5
// at f -> 1, about 1e-4 dB). poly(0) is exactly 1, so 2^0 is exactly 1.
float Boyle::fastExp2(float x)
{
    x = clampf(x, -126.0f, 126.0f);
    int i = (int)x;
    if ((float)i > x) --i;
    const float f = x - (float)i;
    const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f
                  + f * (0.00961813f + f * (0.00133336f + f * 0.00015404f)))));
    union { float f; uint32_t u; } v;
    v.u = (uint32_t)(i + 127) << 23;
    return p * v.f;
}

// The one-pole coefficient 1 - e^(-y), y = 1 / (tau fs), by its own series:
// 1 - fastExp2(...) would cancel to nothing (4 s is y = 5e-6). y <= 0.139
// over the allowed times, where the y^6 term is under 1e-8.
float Boyle::onePole(float tau)
{
    const float y = 1.0f / (tau * kSampleRate);
    return y * (1.0f - y * (0.5f - y * (0.16666667f - y * (0.04166667f - y * 0.00833333f))));
}

/* ── Lua-callable API (UI thread; reads scalars only) ──────────────────── */

float Boyle::getReduction()  { return mReduction; }
float Boyle::getInLevel()    { return mInPk; }
float Boyle::getOutLevel()   { return mOutPk; }
float Boyle::getThreshold()  { return mThreshold; }
float Boyle::getMakeup()     { return mMakeup; }
float Boyle::getOutputGain() { return mGainDb; }

float Boyle::getRatio()
{
    const float s = mSlope;
    if (s >= 0.999f) return 1000.0f;
    return 1.0f / (1.0f - (s > 0.0f ? s : 0.0f));
}

/* ── audio thread ──────────────────────────────────────────────────────── */

void Boyle::process()
{
    const float *inL = mLeftIn.buffer();
    const float *inR = mRightIn.buffer();
    const float *key = mKeyIn.buffer();
    const float *comp = mCompIn.buffer();
    const float *gain = mGainIn.buffer();
    const float *mix = mMixIn.buffer();
    float *outL = mLeftOut.buffer();
    float *outR = mRightOut.buffer();

    // block constants are read once (§5c)
    const float attack  = clampf(sanitize(mAttackIn.buffer()[FRAMELENGTH - 1]), kAttackMin, kAttackMax);
    const float release = clampf(sanitize(mReleaseIn.buffer()[FRAMELENGTH - 1]), kReleaseMin, kReleaseMax);
    const float aUp = onePole(attack);
    const float aDn = onePole(release);
    const float range = rangeDb(mRangeOpt.value());
    const float cap = limitDb(mLimitOpt.value());
    const bool external = mDetectOpt.value() == 2;
    int hpf = mHpfOpt.value() - 1;
    if (hpf < 0 || hpf > 2) hpf = 1;              // CHOICE_UNKNOWN -> the default
    const float b0 = kHpfB0[hpf], a1 = kHpfA1[hpf], a2 = kHpfA2[hpf];

    // accumulator guards (§5)
    float env = mEnv;
    if (!(env >= kEnvFloor && env <= 1.0e8f)) env = kEnvFloor;
    float c = mComp, gdb = mGainDb, m = mMix;
    if (!(c >= 0.0f && c <= 1.0f)) c = 0.0f;
    if (!(gdb >= -kGainMaxDb && gdb <= kGainMaxDb)) gdb = 0.0f;
    if (!(m >= 0.0f && m <= 1.0f)) m = 1.0f;
    float z[2][2];
    for (int ch = 0; ch < 2; ++ch) for (int k = 0; k < 2; ++k) {
        float s = mZ[ch][k];
        // finite and not denormal-bound: silence would decay the states there
        if (!(s > -1.0e8f && s < 1.0e8f) || (s > -1.0e-20f && s < 1.0e-20f)) s = 0.0f;
        z[ch][k] = s;
    }

    float t = range, s = 0.0f, gr = 0.0f, mk = 0.0f;
    float pkIn = 0.0f, pkOut = 0.0f;

    for (int i = 0; i < FRAMELENGTH; ++i) {
        c   = smooth(c,   clampf(sanitize(comp[i]), 0.0f, 1.0f));
        gdb = smooth(gdb, clampf(sanitize(gain[i]), -kGainMaxDb, kGainMaxDb));
        m   = smooth(m,   clampf(sanitize(mix[i]), 0.0f, 1.0f));

        const float l = sanitize(inL[i]);
        const float r = sanitize(inR[i]);

        // the detector: what it hears, high-passed (TDF-II biquad)
        float dl = external ? sanitize(key[i]) : l;
        float dr = external ? dl : r;
        if (hpf) {
            float y = b0 * dl + z[0][0];
            z[0][0] = -2.0f * b0 * dl - a1 * y + z[0][1];
            z[0][1] = b0 * dl - a2 * y;
            dl = y;
            if (external) {
                dr = dl;
            } else {
                y = b0 * dr + z[1][0];
                z[1][0] = -2.0f * b0 * dr - a1 * y + z[1][1];
                z[1][1] = b0 * dr - a2 * y;
                dr = y;
            }
        }
        float p = dl * dl > dr * dr ? dl * dl : dr * dr;
        if (p > 1.0e8f) p = 1.0e8f;
        env += (p > env ? aUp : aDn) * (p - env);
        if (env < kEnvFloor) env = kEnvFloor;

        // the computer, in dB (10 log10 env = 3.0103 log2 env)
        const float x = 3.01029996f * fastLog2(env);
        t = range - kCompSpanDb * c;
        s = c;
        gr = reductionDb(x, t, s);
        mk = s * kCompSpanDb * c;
        if (mk > cap) mk = cap;

        // the gain: exactly 1 when every term is 0 (the bypass, §4)
        const float tot = mk - gr + gdb;
        const float g = tot == 0.0f ? 1.0f : fastExp2(tot * 0.16609640f);   // dB -> log2

        const float ol = l + m * (l * g - l);
        const float orr = r + m * (r * g - r);
        outL[i] = ol;
        outR[i] = orr;

        const float ai = maxAbs(l, r), ao = maxAbs(ol, orr);
        if (ai > pkIn) pkIn = ai;
        if (ao > pkOut) pkOut = ao;
    }

    mEnv = env;
    mComp = c; mGainDb = gdb; mMix = m;
    for (int ch = 0; ch < 2; ++ch) for (int k = 0; k < 2; ++k) mZ[ch][k] = z[ch][k];
    mReduction = gr; mThreshold = t; mSlope = s; mMakeup = mk;

    // the meter: scalars per block, cheap enough to keep live (§5b)
    mInPk  = pkIn  > mInPk  * kPeakDecay ? pkIn  : mInPk  * kPeakDecay;
    mOutPk = pkOut > mOutPk * kPeakDecay ? pkOut : mOutPk * kPeakDecay;
}

} // namespace boyle
