/* Host verification harness for Boyle (no hardware).
 * Build: make hosttest  (g++ -std=c++11 -O2 -ffast-math -fno-finite-math-only
 *          -Itest/host -Isrc src/Boyle.cpp test/host/main.cpp -o test/t)
 * Modes: ident · math · curve · attack · release · link · key · limit · mix
 *        · hpf · viz · nan · cpu · asan
 *
 * Every test prints the number it judged, not just PASS (standard §6/§10).
 * Gains are judged EXACTLY where the maths allows (§10: tolerances hide test
 * bugs). To add a feature: add a mode here, add it to the `hosttest` line
 * in the Makefile, and print what it measured. */
#include "Boyle.h"
#include "BoyleMeter.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <string>
#include <ctime>

using boyle::Boyle;

static const float kFs = 48000.0f;
static const float kPi = 3.14159265f;

/* ── the rig: controls held, inputs from a generator, the input kept so the
 *    test can judge the output ─────────────────────────────────────────── */
struct Rig {
    Boyle h;
    uint32_t rng = 12345u;
    long n = 0;                     // samples run so far
    float inL[FRAMELENGTH], inR[FRAMELENGTH];

    Rig()
    {
        // the Lua defaults
        set(h.mCompIn, 0.0f); set(h.mAttackIn, 0.010f); set(h.mReleaseIn, 0.150f);
        set(h.mGainIn, 0.0f); set(h.mMixIn, 1.0f); set(h.mKeyIn, 0.0f);
    }
    void set(od::Inlet &in, float v) { for (int i = 0; i < FRAMELENGTH; ++i) in.buffer()[i] = v; }
    float noise() { rng = rng * 1664525u + 1013904223u; return (float)(int32_t)rng * 4.6566e-10f; }

    // One block: gen(t) gives each channel's sample at time t (seconds).
    template <class G> void block(G gen)
    {
        for (int i = 0; i < FRAMELENGTH; ++i, ++n) {
            float l, r;
            gen((float)n / kFs, l, r);
            inL[i] = h.mLeftIn.buffer()[i] = l;
            inR[i] = h.mRightIn.buffer()[i] = r;
        }
        h.process();
    }
    template <class G> void run(int blocks, G gen) { for (int b = 0; b < blocks; ++b) block(gen); }
    void noiseBlock() { block([this](float, float &l, float &r) { l = noise(); r = noise() * 0.5f; }); }

    // Samples in this block where out != in * g exactly.
    int mismatches(float g)
    {
        int bad = 0;
        for (int i = 0; i < FRAMELENGTH; ++i) {
            if (h.mLeftOut.buffer()[i]  != inL[i] * g) ++bad;
            if (h.mRightOut.buffer()[i] != inR[i] * g) ++bad;
        }
        return bad;
    }
    bool finite()
    {
        for (int i = 0; i < FRAMELENGTH; ++i)
            if (!std::isfinite(h.mLeftOut.buffer()[i]) || !std::isfinite(h.mRightOut.buffer()[i])) return false;
        return true;
    }
    // the gain in force on the last sample of the block, in dB, from channel ch
    float gainDb(int ch = 0)
    {
        const float *in = ch ? inR : inL;
        const float *out = ch ? h.mRightOut.buffer() : h.mLeftOut.buffer();
        return 20.0f * log10f(fabsf(out[FRAMELENGTH - 1]) / fabsf(in[FRAMELENGTH - 1]));
    }
};

static int blocksFor(float seconds) { return (int)(seconds * kFs / FRAMELENGTH) + 1; }
static const int kSettle = 400;   // blocks, ~1 s: every control smoother has snapped

static int report(const char *what, bool ok) { printf("  %s\n", ok ? "ok" : "FAIL"); (void)what; return ok ? 0 : 1; }

/* ── modes ─────────────────────────────────────────────────────────────── */

// Defaults (Comp 0, Gain 0 dB) are an exact bypass (§4) from the first
// sample, with every option and any mix: none of them can move the gain.
static int t_ident()
{
    int fails = 0;
    for (int range = 1; range <= 3; ++range)
    for (int hpf = 1; hpf <= 3; ++hpf)
    for (int det = 1; det <= 2; ++det)
    for (float mix : { 1.0f, 0.5f, 0.0f }) {
        Rig r;
        r.h.mRangeOpt.set(range); r.h.mHpfOpt.set(hpf); r.h.mDetectOpt.set(det);
        r.set(r.h.mMixIn, mix);
        r.set(r.h.mKeyIn, 0.8f);
        long bad = 0;
        for (int b = 0; b < kSettle; ++b) { r.noiseBlock(); bad += r.mismatches(1.0f); }
        if (bad) {
            printf("ident: range %d hpf %d detect %d mix %.1f -> %ld samples differ  FAIL\n", range, hpf, det, (double)mix, bad);
            ++fails;
        }
    }
    printf("ident: defaults over 54 option/mix combinations, %d not bit-exact (expect 0)", fails);
    return report("ident", fails == 0);
}

// fastLog2 / fastExp2 against libm, and 2^0 exactly 1 (the bypass needs it).
static int t_math()
{
    double e1 = 0, e2 = 0;
    for (int k = 0; k <= 200000; ++k) {
        const float x = powf(10.0f, -13.0f + 21.0f * (float)k / 200000.0f);
        const double d = fabs((double)Boyle::fastLog2(x) - log2((double)x));
        if (d > e1) e1 = d;
        const float y = -40.0f + 80.0f * (float)k / 200000.0f;
        const double rel = fabs((double)Boyle::fastExp2(y) / exp2((double)y) - 1.0);
        if (rel > e2) e2 = rel;
    }
    const bool one = Boyle::fastExp2(0.0f) == 1.0f;
    // in dB: log2 error x 3.01 (power), exp2 relative error x 8.69
    printf("math: log2 max err %.2e (%.5f dB), exp2 max rel err %.2e (%.5f dB), 2^0 == 1 %d",
           e1, e1 * 3.0103, e2, e2 * 8.6859, one);
    return report("math", e1 < 2e-5 && e2 < 3e-5 && one);
}

// The static curve: the knee formula at its edges, and the whole unit
// against it with DC in (DC: the detector settles to exactly level², and the
// gain in force can be read off the output).
static int t_curve()
{
    int fails = 0;
    {   // the formula: 0 below the knee, s * over above, continuous at both edges
        const float t = -20.0f, s = 0.75f, h = 0.5f * Boyle::kKneeDb;
        const float below = Boyle::reductionDb(t - h, t, s);
        const float above = Boyle::reductionDb(t + h, t, s);
        const float mid   = Boyle::reductionDb(t, t, s);
        const float far   = Boyle::reductionDb(t + 20.0f, t, s);
        const bool ok = below == 0.0f && fabsf(above - s * h) < 1e-5f && fabsf(mid - s * h * 0.25f) < 1e-5f
                     && fabsf(far - s * 20.0f) < 1e-5f;
        printf("curve: knee -> %.4f at -h, %.4f at t (expect %.4f), %.4f at +h (expect %.4f), %.3f at +20 (expect %.3f)",
               (double)below, (double)mid, (double)(s * h * 0.25f), (double)above, (double)(s * h), (double)far, (double)(s * 20.0f));
        fails += report("curve", ok);
    }
    // the unit: DC at several levels, comp and range, HPF off
    double worst = 0;
    for (int range = 1; range <= 3; ++range)
    for (float comp : { 0.25f, 0.5f, 0.75f, 1.0f })
    for (float lev : { -40.0f, -24.0f, -12.0f, -3.0f }) {
        Rig r;
        r.h.mRangeOpt.set(range); r.h.mHpfOpt.set(1);
        r.set(r.h.mCompIn, comp); r.set(r.h.mGainIn, 3.0f);
        const float a = powf(10.0f, lev / 20.0f);
        r.run(kSettle * 2, [a](float, float &l, float &rr) { l = a; rr = a; });
        const float t = Boyle::rangeDb(range) - Boyle::kCompSpanDb * comp;
        float mk = comp * Boyle::kCompSpanDb * comp;
        if (mk > Boyle::limitDb(1)) mk = Boyle::limitDb(1);
        const float expect = mk - Boyle::reductionDb(lev, t, comp) + 3.0f;
        const double err = fabs((double)r.gainDb() - expect);
        if (err > worst) worst = err;
        if (err > 0.01) {
            printf("curve: range %d comp %.2f DC %.0f dB -> gain %.3f dB, expect %.3f  FAIL\n",
                   range, (double)comp, (double)lev, (double)r.gainDb(), (double)expect);
            ++fails;
        }
    }
    printf("curve: 48 DC operating points, worst error %.5f dB (limit 0.01)", worst);
    fails += report("curve", worst <= 0.01);
    {   // the readouts agree with the maths
        Rig r;
        r.set(r.h.mCompIn, 0.75f); r.run(kSettle, [](float, float &l, float &rr) { l = rr = 0.0f; });
        const bool ok = fabsf(r.h.getRatio() - 4.0f) < 1e-3f && fabsf(r.h.getThreshold() - (-15.0f - 18.0f)) < 1e-3f
                     && fabsf(r.h.getMakeup() - 13.5f) < 1e-3f;
        printf("curve: comp 0.75 mid -> ratio %.3f:1 (4), threshold %.2f (-33), makeup %.2f (13.5)",
               (double)r.h.getRatio(), (double)r.h.getThreshold(), (double)r.h.getMakeup());
        fails += report("curve", ok);
    }
    return fails ? 1 : 0;
}

// Attack: a step from -40 to 0 dBFS DC, comp 1 (∞:1), hot range (T = -30).
// After one time constant the envelope is 1 - (1 - 1e-4)/e of full, so the
// reduction is 10 log10 of that minus the threshold.
static int t_attack()
{
    int fails = 0;
    // whole blocks of time constant (tau fs = 128 k), read at a block's end
    for (int k : { 1, 4, 30 }) {
        const float tau = (float)(k * FRAMELENGTH) / kFs;
        Rig r;
        r.h.mRangeOpt.set(1); r.h.mHpfOpt.set(1);
        r.set(r.h.mCompIn, 1.0f); r.set(r.h.mAttackIn, tau);
        r.run(kSettle * 2, [](float, float &l, float &rr) { l = rr = 0.01f; });
        const int blocks = k;
        const float t = (float)(blocks * FRAMELENGTH) / kFs;
        r.run(blocks, [](float, float &l, float &rr) { l = rr = 1.0f; });
        const double env = 1.0 - (1.0 - 1e-4) * exp(-(double)t / tau);
        const double expect = 10.0 * log10(env) - (-30.0);
        const double got = r.h.getReduction();
        const bool ok = fabs(got - expect) < 0.05;
        printf("attack: tau %.2f ms, %d samples into a +40 dB step -> reduction %.3f dB, expect %.3f",
               (double)(tau * 1000), blocks * FRAMELENGTH, got, expect);
        fails += report("attack", ok);
    }
    return fails ? 1 : 0;
}

// Release: with the input dropped from 0 to -20 dBFS (still above the knee,
// T = -30, at ∞:1) the envelope decays as 0.01 + 0.99 e^(-t/tau), and the
// reduction falls by -10 log10 of that: about 4.34 / tau dB per second
// while far above the new level.
static int t_release()
{
    int fails = 0;
    for (float tau : { 0.05f, 0.5f, 2.0f }) {
        Rig r;
        r.h.mRangeOpt.set(1); r.h.mHpfOpt.set(1);
        r.set(r.h.mCompIn, 1.0f); r.set(r.h.mReleaseIn, tau);
        r.run(kSettle * 2, [](float, float &l, float &rr) { l = rr = 1.0f; });
        const float gr0 = r.h.getReduction();
        // about 10 dB of fall, so the level stays above -20 + 5 knee
        const int blocks = (int)(tau * 2.3f * kFs / FRAMELENGTH);
        r.run(blocks, [](float, float &l, float &rr) { l = rr = 0.1f; });
        const double t = (double)(blocks * FRAMELENGTH) / kFs;
        const double fell = gr0 - r.h.getReduction();
        const double expect = -10.0 * log10(0.01 + 0.99 * exp(-t / tau));
        const bool ok = fabs(fell / expect - 1.0) < 0.002;
        printf("release: tau %.0f ms, %.3f s after a -20 dB drop -> reduction fell %.3f dB, expect %.3f",
               (double)(tau * 1000), t, fell, expect);
        fails += report("release", ok);
    }
    return fails ? 1 : 0;
}

// Stereo link: a loud left and a quiet right get the same gain, so the
// image doesn't move.
static int t_link()
{
    Rig r;
    r.h.mHpfOpt.set(1);
    r.set(r.h.mCompIn, 0.8f);
    r.run(kSettle, [](float t, float &l, float &rr) { l = 0.7f * sinf(2 * kPi * 440 * t) + 0.02f; rr = 0.02f; });
    float worst = 0;
    for (int b = 0; b < 20; ++b) {
        r.block([](float t, float &l, float &rr) { l = 0.7f * sinf(2 * kPi * 440 * t) + 0.02f; rr = 0.02f; });
        for (int i = 0; i < FRAMELENGTH; ++i) {
            const float gl = r.h.mLeftOut.buffer()[i] / r.inL[i];
            const float gr = r.h.mRightOut.buffer()[i] / r.inR[i];
            if (std::isfinite(gl) && fabsf(r.inL[i]) > 0.01f && fabsf(gl - gr) > worst) worst = fabsf(gl - gr);
        }
    }
    printf("link: loud left / quiet right -> reduction %.2f dB, worst L-R gain difference %.2e",
           (double)r.h.getReduction(), (double)worst);
    return report("link", r.h.getReduction() > 3.0f && worst < 1e-5f);
}

// Key: Detect = key ducks quiet inputs by a loud key; Detect = self ignores it.
static int t_key()
{
    int fails = 0;
    for (int det = 1; det <= 2; ++det) {
        Rig r;
        r.h.mDetectOpt.set(det);
        r.set(r.h.mCompIn, 0.8f);
        for (int b = 0; b < kSettle; ++b) {
            for (int i = 0; i < FRAMELENGTH; ++i) r.h.mKeyIn.buffer()[i] = 0.8f * sinf(2 * kPi * 200 * (float)(r.n + i) / kFs);
            r.block([](float t, float &l, float &rr) { l = rr = 0.005f * sinf(2 * kPi * 330 * t); });
        }
        const float gr = r.h.getReduction();
        const bool ok = det == 2 ? gr > 10.0f : gr == 0.0f;
        printf("key: detect %s, -46 dB input, loud key -> reduction %.2f dB (expect %s)",
               det == 2 ? "key" : "self", (double)gr, det == 2 ? "> 10" : "0");
        fails += report("key", ok);
    }
    return fails ? 1 : 0;
}

// Limit: the comp knob's makeup is capped at 24 (∞ setting), 9 or 3 dB.
static int t_limit()
{
    int fails = 0;
    for (int lim = 1; lim <= 3; ++lim) {
        Rig r;
        r.h.mLimitOpt.set(lim);
        r.set(r.h.mCompIn, 1.0f);
        r.run(kSettle, [](float, float &l, float &rr) { l = rr = 0.0f; });
        const float expect = lim == 1 ? 24.0f : lim == 2 ? 9.0f : 3.0f;
        printf("limit: choice %d at comp 1 -> makeup %.3f dB, expect %.1f", lim, (double)r.h.getMakeup(), (double)expect);
        fails += report("limit", r.h.getMakeup() == expect);
    }
    return fails ? 1 : 0;
}

// Mix: 0 is the dry signal exactly, however hard it compresses; 0.5 sits
// between dry and wet.
static int t_mix()
{
    int fails = 0;
    Rig r;
    r.set(r.h.mCompIn, 1.0f); r.set(r.h.mMixIn, 0.0f);
    r.run(kSettle, [](float t, float &l, float &rr) { l = rr = 0.8f * sinf(2 * kPi * 220 * t); });
    r.block([](float t, float &l, float &rr) { l = rr = 0.8f * sinf(2 * kPi * 220 * t); });
    const int bad = r.mismatches(1.0f);
    printf("mix: 0 with %.1f dB of reduction -> %d samples differ from dry (expect 0)", (double)r.h.getReduction(), bad);
    fails += report("mix", bad == 0);
    return fails ? 1 : 0;
}

// HPF: a loud 30 Hz tone pumps the unfiltered detector; 120 Hz takes most
// of it out.
static int t_hpf()
{
    float gr[4] = { 0, 0, 0, 0 };
    for (int hpf = 1; hpf <= 3; ++hpf) {
        Rig r;
        r.h.mHpfOpt.set(hpf);
        r.set(r.h.mCompIn, 0.6f);
        r.run(kSettle, [](float t, float &l, float &rr) { l = rr = 0.5f * sinf(2 * kPi * 30 * t); });
        float pk = 0;
        for (int b = 0; b < 40; ++b) {
            r.block([](float t, float &l, float &rr) { l = rr = 0.5f * sinf(2 * kPi * 30 * t); });
            if (r.h.getReduction() > pk) pk = r.h.getReduction();
        }
        gr[hpf] = pk;
    }
    printf("hpf: 30 Hz at -6 dBFS -> peak reduction off %.2f, 60 Hz %.2f, 120 Hz %.2f dB (expect falling)",
           (double)gr[1], (double)gr[2], (double)gr[3]);
    return report("hpf", gr[1] > gr[2] && gr[2] > gr[3] && gr[1] - gr[3] > 6.0f);
}

/* ── a FrameBuffer that records what was drawn ─────────────────────────── */
struct RecFB : od::FrameBuffer {
    int fills = 0, boxes = 0, lines = 0, texts = 0;
    int minX = 1 << 20, maxX = -(1 << 20), minY = 1 << 20, maxY = -(1 << 20);
    std::vector<std::string> words;
    void pt(int x, int y) { if (x < minX) minX = x; if (x > maxX) maxX = x; if (y < minY) minY = y; if (y > maxY) maxY = y; }
    void pixel(od::Color, int x, int y) override { pt(x, y); }
    void line(od::Color, int x0, int y0, int x1, int y1) override { pt(x0, y0); pt(x1, y1); ++lines; }
    void hline(od::Color, int x, int x2, int y, int) override { pt(x, y); pt(x2, y); ++lines; }
    void vline(od::Color, int x, int y, int y2, int) override { pt(x, y); pt(x, y2); ++lines; }
    void box(od::Color, int l, int b, int r, int t) override { pt(l, b); pt(r, t); ++boxes; }
    void fill(od::Color, int l, int b, int r, int t) override { pt(l, b); pt(r, t); ++fills; }
    int  text(od::Color, int x, int y, const char *s, int size, int) override
    { pt(x, y); ++texts; words.push_back(s); return x + (int)strlen(s) * size / 2; }
    void circle(od::Color, int x, int y, int) override { pt(x, y); }
    void fillCircle(od::Color, int x, int y, int) override { pt(x, y); }
};

// The meter stays inside its box, shows in / reduction / out, and the sub
// display says what the unit is doing.
static int t_viz()
{
    int fails = 0;
    Rig r;
    const int W = 42, H = 64;
    boyle::BoyleMeter meter(0, 0, W, H);
    meter.follow(&r.h);
    {   // silence: three empty boxes and the threshold tick
        RecFB fb; meter.draw(fb);
        const bool ok = fb.boxes == 3 && fb.fills == 0 && fb.lines == 1;
        printf("viz: silent -> %d boxes, %d bars, %d ticks (expect 3, 0, 1)", fb.boxes, fb.fills, fb.lines);
        fails += report("viz", ok);
    }
    {   // compressing: three bars, everything inside the graphic
        r.set(r.h.mCompIn, 0.8f);
        r.run(kSettle, [](float t, float &l, float &rr) { l = rr = 0.7f * sinf(2 * kPi * 440 * t); });
        RecFB fb; meter.draw(fb);
        const bool ok = fb.fills == 3 && fb.minX >= 0 && fb.maxX <= W && fb.minY >= 0 && fb.maxY <= H;
        printf("viz: compressing -> %d bars, in %.3f out %.3f GR %.1f dB, bbox x %d..%d y %d..%d (inside %dx%d)",
               fb.fills, (double)r.h.getInLevel(), (double)r.h.getOutLevel(), (double)r.h.getReduction(),
               fb.minX, fb.maxX, fb.minY, fb.maxY, W, H);
        fails += report("viz", ok);
    }
    {   // the sub display
        boyle::BoyleInfo info(0, 0, 128, 64);
        info.follow(&r.h);
        RecFB fb; info.draw(fb);
        bool gr = false, ratio = false;
        for (auto &w : fb.words) {
            if (w.compare(0, 3, "GR ") == 0) gr = true;
            if (w.find("5.0:1") != std::string::npos) ratio = true;
        }
        printf("viz: sub display -> 'GR ...' %d, ratio '5.0:1' %d (%s)", gr, ratio, fb.words.size() > 1 ? fb.words[1].c_str() : "");
        fails += report("viz", gr && ratio);
    }
    {   // follow() holds the head (§13): the info graphic released its hold, the meter still has one
        const bool ok = r.h.mRefs == 1;
        printf("viz: head refs with only the meter left %d (expect 1)", r.h.mRefs);
        fails += report("viz", ok);
    }
    return fails ? 1 : 0;
}

// Poison every inlet with NaN and +/-inf in turn while compressing hard:
// outputs stay finite, and with clean inputs and default controls again the
// unit is an exact bypass (§5).
static int t_nan()
{
    {   // the build must be able to see a NaN at all (§5: -fno-finite-math-only)
        volatile float n = NAN;
        if (std::isfinite(n)) { printf("nan: this build cannot see NaN (missing -fno-finite-math-only)  FAIL\n"); return 1; }
    }
    int fails = 0, cases = 0;
    const float poisons[] = { NAN, INFINITY, -INFINITY };
    const char *names[] = { "Left In", "Right In", "Key", "Comp", "Attack", "Release", "Gain", "Mix" };
    for (int det = 1; det <= 2; ++det)
    for (int port = 0; port < 8; ++port) for (float p : poisons) {
        ++cases;
        Rig r;
        r.h.mDetectOpt.set(det);
        r.set(r.h.mCompIn, 0.9f);
        r.run(10, [](float t, float &l, float &rr) { l = rr = 0.5f * sinf(2 * kPi * 100 * t); });
        od::Inlet *in[] = { &r.h.mLeftIn, &r.h.mRightIn, &r.h.mKeyIn, &r.h.mCompIn,
                            &r.h.mAttackIn, &r.h.mReleaseIn, &r.h.mGainIn, &r.h.mMixIn };
        bool clean = true;
        for (int b = 0; b < 4; ++b) {
            if (port >= 2) r.set(*in[port], p);
            if (port < 2) {
                for (int i = 0; i < FRAMELENGTH; ++i) {
                    r.inL[i] = r.h.mLeftIn.buffer()[i] = r.noise();
                    r.inR[i] = r.h.mRightIn.buffer()[i] = r.noise();
                }
                r.set(*in[port], p);
                r.h.process();
            } else {
                r.noiseBlock();
            }
            clean = clean && r.finite();
        }
        r.set(r.h.mCompIn, 0.0f); r.set(r.h.mGainIn, 0.0f); r.set(r.h.mMixIn, 1.0f);
        r.set(r.h.mAttackIn, 0.01f); r.set(r.h.mReleaseIn, 0.15f); r.set(r.h.mKeyIn, 0.0f);
        r.run(kSettle, [](float, float &l, float &rr) { l = rr = 0.1f; });
        r.noiseBlock();
        const int bad = r.mismatches(1.0f);
        clean = clean && r.finite();
        if (!clean || bad) {
            printf("nan: detect %d, %s = %f -> finite %d, %d samples off bypass after  FAIL\n", det, names[port], (double)p, clean, bad);
            ++fails;
        }
    }
    printf("nan: %d failures over %d cases\n", fails, cases);
    return fails ? 1 : 0;
}

static int t_cpu()
{
    Rig r;
    r.set(r.h.mCompIn, 0.7f);
    r.run(100, [](float t, float &l, float &rr) { l = rr = 0.5f * sinf(2 * kPi * 100 * t); });
    const int blocks = 200000;
    clock_t t0 = clock();
    for (int b = 0; b < blocks; ++b) r.h.process();
    const double us = 1e6 * (double)(clock() - t0) / CLOCKS_PER_SEC / blocks;
    printf("cpu: %.3f us per block on this host (budget on device: 2667 us, shared)\n", us);
    return us < 20.0 ? 0 : 1;
}

// Sweep the controls past their ranges with every option, including the
// CHOICE_UNKNOWN value 0 and out-of-range 4; outputs must stay finite and
// no louder than the input times the most gain the controls allow (+60 dB).
// Built with ASAN/UBSAN.
static int t_asan()
{
    int bad = 0;
    for (int opt = 0; opt <= 4; ++opt)
    for (float comp : { -5.0f, 0.0f, 0.5f, 1.0f, 7.0f })
    for (float gain : { -100.0f, 0.0f, 100.0f })
    for (float tm : { -1.0f, 0.0f, 0.01f, 100.0f }) {
        Rig r;
        r.h.mRangeOpt.set(opt); r.h.mLimitOpt.set(opt); r.h.mHpfOpt.set(opt); r.h.mDetectOpt.set(opt);
        r.set(r.h.mCompIn, comp); r.set(r.h.mGainIn, gain);
        r.set(r.h.mAttackIn, tm); r.set(r.h.mReleaseIn, tm); r.set(r.h.mMixIn, comp);
        for (int b = 0; b < 30; ++b) {
            r.noiseBlock();
            if (!r.finite()) { ++bad; break; }
            for (int i = 0; i < FRAMELENGTH; ++i)
                if (fabsf(r.h.mLeftOut.buffer()[i]) > fabsf(r.inL[i]) * 1000.0f + 1e-30f) { ++bad; break; }
        }
        (void)r.h.getRatio(); (void)r.h.getThreshold();
    }
    printf("asan: %d dirty runs\n", bad);
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: t <mode>\n"); return 2; }
    const char *m = argv[1];
    int r = 2;
    if      (!strcmp(m, "ident"))   r = t_ident();
    else if (!strcmp(m, "math"))    r = t_math();
    else if (!strcmp(m, "curve"))   r = t_curve();
    else if (!strcmp(m, "attack"))  r = t_attack();
    else if (!strcmp(m, "release")) r = t_release();
    else if (!strcmp(m, "link"))    r = t_link();
    else if (!strcmp(m, "key"))     r = t_key();
    else if (!strcmp(m, "limit"))   r = t_limit();
    else if (!strcmp(m, "mix"))     r = t_mix();
    else if (!strcmp(m, "hpf"))     r = t_hpf();
    else if (!strcmp(m, "viz"))     r = t_viz();
    else if (!strcmp(m, "nan"))     r = t_nan();
    else if (!strcmp(m, "cpu"))     r = t_cpu();
    else if (!strcmp(m, "asan"))    r = t_asan();
    else { fprintf(stderr, "unknown mode %s\n", m); return 2; }
    printf("%s: %s\n", m, r == 0 ? "PASS" : "FAIL");
    return r;
}
