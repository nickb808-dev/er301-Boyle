/* BoyleMeter.h — the main graphic (in, reduction, out) and the sub-display
 * readout.
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Nicholas Breinich (nickb808) — see LICENSE and NOTICE.
 *
 * BoyleMeter: three bars in a dB scale. Input on the left with a tick at the
 * threshold, gain reduction in the middle hanging down from the top, output
 * on the right. The reduction is the one idea in WHITE; the level bars are
 * GRAY7 and the boxes GRAY3 (standard §8).
 *
 * BoyleInfo (the sub display): reduction, threshold, ratio, makeup and
 * output gain.
 *
 * Both only read scalars from the engine (no pool memory, standard §12d),
 * and hold it with attach / release (§13: holding, not borrowing). Every
 * virtual is defined INLINE (no out-of-line virtuals on a framework
 * subclass). Only od::FrameBuffer primitives the standard lists are used. */

#pragma once

#include <od/graphics/Graphic.h>
#include <od/graphics/constants.h>
#include "Boyle.h"
#include <cstdio>

namespace boyle {

class BoyleMeter : public od::Graphic
{
public:
    BoyleMeter(int left, int bottom, int width, int height)
        : od::Graphic(left, bottom, width, height), mpHead(0) {}
    virtual ~BoyleMeter() { if (mpHead) mpHead->release(); }

    void follow(Boyle *p)
    {
        if (mpHead) mpHead->release();
        mpHead = p;
        if (mpHead) mpHead->attach();
    }

#ifndef SWIGLUA
    static constexpr float kFloorDb = -48.0f;   // bottom of the level bars
    static constexpr float kGrMaxDb = 24.0f;    // full length of the reduction bar

    // a linear level as a fraction of the bar, on the dB scale
    static float levelFrac(float a)
    {
        if (!(a > 1.0e-6f)) return 0.0f;
        const float db = 6.02059991f * Boyle::fastLog2(a);
        const float f = (db - kFloorDb) * (1.0f / -kFloorDb);
        return f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    }

    virtual void draw(od::FrameBuffer &fb)
    {
        Boyle *h = mpHead;
        const int pad = 4;
        const int bottom = mWorldBottom + pad;
        const int top = mWorldBottom + mHeight - pad;
        const int span = top - bottom;
        const int barW = (mWidth - 4 * pad) / 3;
        for (int b = 0; b < 3; ++b) {
            const int left = mWorldLeft + pad + b * (barW + pad);
            const int right = left + barW;
            fb.box(GRAY3, left, bottom, right, top);
            if (!h) continue;
            if (b == 1) {
                // reduction: down from the top
                float gr = h->getReduction() * (1.0f / kGrMaxDb);
                if (!(gr >= 0.0f)) gr = 0.0f;
                if (gr > 1.0f) gr = 1.0f;
                const int y = top - (int)(gr * (float)span);
                if (y < top) fb.fill(WHITE, left + 1, y, right - 1, top - 1);
            } else {
                const float f = levelFrac(b == 0 ? h->getInLevel() : h->getOutLevel());
                const int y = bottom + (int)(f * (float)span);
                if (y > bottom) fb.fill(GRAY7, left + 1, bottom + 1, right - 1, y);
                if (b == 0) {
                    // the threshold, as a tick across the input bar (a peak
                    // level of 0 dBFS is a detector level of 0 dB)
                    float tf = (h->getThreshold() - kFloorDb) * (1.0f / -kFloorDb);
                    if (!(tf >= 0.0f)) tf = 0.0f;
                    if (tf > 1.0f) tf = 1.0f;
                    fb.hline(WHITE, left - 1, right + 1, bottom + (int)(tf * (float)span));
                }
            }
        }
    }
#endif

private:
    Boyle *mpHead;
};

class BoyleInfo : public od::Graphic
{
public:
    BoyleInfo(int left, int bottom, int width, int height)
        : od::Graphic(left, bottom, width, height), mpHead(0) {}
    virtual ~BoyleInfo() { if (mpHead) mpHead->release(); }

    void follow(Boyle *p)
    {
        if (mpHead) mpHead->release();
        mpHead = p;
        if (mpHead) mpHead->attach();
    }

#ifndef SWIGLUA
    virtual void draw(od::FrameBuffer &fb)
    {
        Boyle *h = mpHead;
        if (!h) return;
        const int x0 = mWorldLeft + 4;
        const int top = mWorldBottom + mHeight;
        char buf[32];
        snprintf(buf, sizeof buf, "GR %.1f dB", (double)h->getReduction());
        fb.text(WHITE, x0, top - 14, buf, 12);
        const float ratio = h->getRatio();
        if (ratio >= 999.0f) snprintf(buf, sizeof buf, "thr %.0f  inf:1", (double)h->getThreshold());
        else                 snprintf(buf, sizeof buf, "thr %.0f  %.1f:1", (double)h->getThreshold(), (double)ratio);
        fb.text(GRAY7, x0, top - 30, buf, 10);
        snprintf(buf, sizeof buf, "mkup %+.1f  out %+.1f", (double)h->getMakeup(), (double)h->getOutputGain());
        fb.text(GRAY7, x0, top - 44, buf, 10);
    }
#endif

private:
    Boyle *mpHead;
};

} // namespace boyle
