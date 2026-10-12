/*
 * Math functions as V8 computes them, without a C library: ports of V8's
 * own fdlibm, src/base/ieee754.cc of V8 12.4.254.21 (Node 22), line for
 * line, so that each gives V8's bits on every target. V8 runs these
 * functions for Math.x and for the operators alike, in every tier and in
 * its constant folding (docs/extending.md, "To make number results
 * bit-identical to V8").
 *
 * Its notices:
 *
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 *
 * Developed at SunSoft, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 *
 * The original source code covered by the above license above has been
 * modified significantly by Google Inc.
 * Copyright 2016 the V8 project authors. All rights reserved.
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of Google Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "internal.h"

/* Every operation rounds once, as fdlibm's do in V8's build: never a fused
 * multiply-add, which a compiler may otherwise make of a * b + c. */
#pragma STDC FP_CONTRACT OFF

/* fdlibm's word access (GET_HIGH_WORD, GET_LOW_WORD, SET_HIGH_WORD,
 * SET_LOW_WORD). A high word is signed, as fdlibm's are. */
static int32_t high_word(double x) {
    return (int32_t)(uint32_t)(sjs_double_bits(x) >> 32);
}

static uint32_t low_word(double x) {
    return (uint32_t)sjs_double_bits(x);
}

static double with_high_word(double x, uint32_t high) {
    return sjs_bits_double((sjs_double_bits(x) & 0xFFFFFFFFull) | ((uint64_t)high << 32));
}

static double with_low_word(double x, uint32_t low) {
    return sjs_bits_double((sjs_double_bits(x) & 0xFFFFFFFF00000000ull) | low);
}

static double fabs_bits(double x) {
    return sjs_bits_double(sjs_double_bits(x) & 0x7FFFFFFFFFFFFFFFull);
}

/* scalbn(z, n), where pow calls it: z * 2^n for a z near 1 and an n from
 * -1075 to -1022, whose result is subnormal or zero. z * 2^-1000 is exact,
 * so the one rounding is the last product's, as scalbn rounds. */
static double scale_down(double z, int32_t n) {
    double y = z * 0x1p-1000;
    return y * sjs_bits_double((uint64_t)(0x3FF + n + 1000) << 52);
}

/*
 * ES2019 Draft 2019-01-02 12.6.4
 * Math.pow & Exponentiation Operator
 *
 * Return X raised to the Yth power
 *
 * Method:
 *     Let x =  2   * (1+f)
 *     1. Compute and return log2(x) in two pieces:
 *        log2(x) = w1 + w2,
 *        where w1 has 53-24 = 29 bit trailing zeros.
 *     2. Perform y*log2(x) = n+y' by simulating muti-precision
 *        arithmetic, where |y'|<=0.5.
 *     3. Return x**y = 2**n*exp(y'*log2)
 *
 * Special cases:
 *     1.  (anything) ** 0  is 1
 *     2.  (anything) ** 1  is itself
 *     3.  (anything) ** NAN is NAN
 *     4.  NAN ** (anything except 0) is NAN
 *     5.  +-(|x| > 1) **  +INF is +INF
 *     6.  +-(|x| > 1) **  -INF is +0
 *     7.  +-(|x| < 1) **  +INF is +0
 *     8.  +-(|x| < 1) **  -INF is +INF
 *     9.  +-1         ** +-INF is NAN
 *     10. +0 ** (+anything except 0, NAN)               is +0
 *     11. -0 ** (+anything except 0, NAN, odd integer)  is +0
 *     12. +0 ** (-anything except 0, NAN)               is +INF
 *     13. -0 ** (-anything except 0, NAN, odd integer)  is +INF
 *     14. -0 ** (odd integer) = -( +0 ** (odd integer) )
 *     15. +INF ** (+anything except 0,NAN) is +INF
 *     16. +INF ** (-anything except 0,NAN) is +0
 *     17. -INF ** (anything)  = -0 ** (-anything)
 *     18. (-anything) ** (integer) is (-1)**(integer)*(+anything**integer)
 *     19. (-anything except 0 and inf) ** (non-integer) is NAN
 *
 * Accuracy:
 *      pow(x,y) returns x**y nearly rounded. In particular,
 *      pow(integer, integer) always returns the correct integer provided it is
 *      representable.
 *
 * V8 returns a signalling NaN where this returns a quiet one: staticjs
 * has no way to read a NaN's bits.
 */
double sjs_math_pow(double x, double y) {
    static const double bp[] = { 1.0, 1.5 },
        dp_h[] = { 0.0, 5.84962487220764160156e-01 }, /* 0x3FE2B803, 0x40000000 */
        dp_l[] = { 0.0, 1.35003920212974897128e-08 }, /* 0x3E4CFDEB, 0x43CFD006 */
        zero = 0.0, one = 1.0, two = 2.0,
        two53 = 9007199254740992.0, /* 0x43400000, 0x00000000 */
        huge = 1.0e300, tiny = 1.0e-300,
        /* poly coefs for (3/2)*(log(x)-2s-2/3*s**3 */
        L1 = 5.99999999999994648725e-01,     /* 0x3FE33333, 0x33333303 */
        L2 = 4.28571428578550184252e-01,     /* 0x3FDB6DB6, 0xDB6FABFF */
        L3 = 3.33333329818377432918e-01,     /* 0x3FD55555, 0x518F264D */
        L4 = 2.72728123808534006489e-01,     /* 0x3FD17460, 0xA91D4101 */
        L5 = 2.30660745775561754067e-01,     /* 0x3FCD864A, 0x93C9DB65 */
        L6 = 2.06975017800338417784e-01,     /* 0x3FCA7E28, 0x4A454EEF */
        P1 = 1.66666666666666019037e-01,     /* 0x3FC55555, 0x5555553E */
        P2 = -2.77777777770155933842e-03,    /* 0xBF66C16C, 0x16BEBD93 */
        P3 = 6.61375632143793436117e-05,     /* 0x3F11566A, 0xAF25DE2C */
        P4 = -1.65339022054652515390e-06,    /* 0xBEBBBD41, 0xC5D26BF1 */
        P5 = 4.13813679705723846039e-08,     /* 0x3E663769, 0x72BEA4D0 */
        lg2 = 6.93147180559945286227e-01,    /* 0x3FE62E42, 0xFEFA39EF */
        lg2_h = 6.93147182464599609375e-01,  /* 0x3FE62E43, 0x00000000 */
        lg2_l = -1.90465429995776804525e-09, /* 0xBE205C61, 0x0CA86C39 */
        ovt = 8.0085662595372944372e-0017,   /* -(1024-log2(ovfl+.5ulp)) */
        cp = 9.61796693925975554329e-01,     /* 0x3FEEC709, 0xDC3A03FD =2/(3ln2) */
        cp_h = 9.61796700954437255859e-01,   /* 0x3FEEC709, 0xE0000000 =(float)cp */
        cp_l = -7.02846165095275826516e-09,  /* 0xBE3E2FE0, 0x145B01F5 =tail cp_h */
        ivln2 = 1.44269504088896338700e+00,  /* 0x3FF71547, 0x652B82FE =1/ln2 */
        ivln2_h = 1.44269502162933349609e+00, /* 0x3FF71547, 0x60000000 =24b 1/ln2 */
        ivln2_l = 1.92596299112661746887e-08; /* 0x3E54AE0B, 0xF85DDF44 =1/ln2 tail */

    int32_t hx = high_word(x);
    uint32_t lx = low_word(x);
    int32_t hy = high_word(y);
    uint32_t ly = low_word(y);
    int32_t ix = hx & 0x7fffffff;
    int32_t iy = hy & 0x7fffffff;

    /* y==zero: x**0 = 1 */
    if (iy == 0 && ly == 0) return one;

    /* +-NaN return x+y */
    if (ix > 0x7ff00000 || (ix == 0x7ff00000 && lx != 0) || iy > 0x7ff00000 || (iy == 0x7ff00000 && ly != 0)) {
        return x + y;
    }

    /* determine if y is an odd int when x < 0
     * yisint = 0 ... y is not an integer
     * yisint = 1 ... y is an odd int
     * yisint = 2 ... y is an even int
     */
    int32_t yisint = 0;
    if (hx < 0) {
        if (iy >= 0x43400000) {
            yisint = 2; /* even integer y */
        } else if (iy >= 0x3ff00000) {
            int32_t k = (iy >> 20) - 0x3ff; /* exponent */
            if (k > 20) {
                uint32_t j = ly >> (52 - k);
                if ((j << (52 - k)) == ly) yisint = 2 - (int32_t)(j & 1u);
            } else if (ly == 0) {
                int32_t j = iy >> (20 - k);
                if ((j << (20 - k)) == iy) yisint = 2 - (j & 1);
            }
        }
    }

    /* special value of y */
    if (ly == 0) {
        if (iy == 0x7ff00000) { /* y is +-inf */
            if (ix == 0x3ff00000 && lx == 0) {
                return y - y; /* inf**+-1 is NaN */
            } else if (ix >= 0x3ff00000) { /* (|x|>1)**+-inf = inf,0 */
                return hy >= 0 ? y : zero;
            } else { /* (|x|<1)**-,+inf = inf,0 */
                return hy < 0 ? -y : zero;
            }
        }
        if (iy == 0x3ff00000) { /* y is  +-1 */
            return hy < 0 ? one / x : x;
        }
        if (hy == 0x40000000) return x * x; /* y is  2 */
        if (hy == 0x3fe00000 && hx >= 0) {  /* y is  0.5, x >= +0 */
            return sjs_number_sqrt(x);
        }
    }

    double ax = fabs_bits(x);
    /* special value of x */
    if (lx == 0 && (ix == 0x7ff00000 || ix == 0 || ix == 0x3ff00000)) {
        double z = ax;              /*x is +-0,+-inf,+-1*/
        if (hy < 0) z = one / z;    /* z = (1/|x|) */
        if (hx < 0) {
            if (ix == 0x3ff00000 && yisint == 0) {
                z = zero / zero; /* (-1)**non-int is NaN */
            } else if (yisint == 1) {
                z = -z; /* (x<0)**odd = -(|x|**odd) */
            }
        }
        return z;
    }

    /* (x<0)**(non-int) is NaN */
    if (hx < 0 && yisint == 0) return zero / zero;

    double s = one; /* s (sign of result -ve**odd) = -1 else = 1 */
    if (hx < 0 && yisint == 1) s = -one; /* (-ve)**(odd int) */

    double t1, t2;
    if (iy > 0x41e00000) {     /* if |y| > 2**31 */
        if (iy > 0x43f00000) { /* if |y| > 2**64, must o/uflow */
            if (ix <= 0x3fefffff) return hy < 0 ? huge * huge : tiny * tiny;
            if (ix >= 0x3ff00000) return hy > 0 ? huge * huge : tiny * tiny;
        }
        /* over/underflow if x is not close to one */
        if (ix < 0x3fefffff) return hy < 0 ? s * huge * huge : s * tiny * tiny;
        if (ix > 0x3ff00000) return hy > 0 ? s * huge * huge : s * tiny * tiny;
        /* now |1-x| is tiny <= 2**-20, suffice to compute
           log(x) by x-x^2/2+x^3/3-x^4/4 */
        double t = ax - one; /* t has 20 trailing zeros */
        double w = (t * t) * (0.5 - t * (0.3333333333333333333333 - t * 0.25));
        double u = ivln2_h * t; /* ivln2_h has 21 sig. bits */
        double v = t * ivln2_l - w * ivln2;
        t1 = with_low_word(u + v, 0);
        t2 = v - (t1 - u);
    } else {
        int32_t n = 0;
        /* take care subnormal number */
        if (ix < 0x00100000) {
            ax *= two53;
            n -= 53;
            ix = high_word(ax);
        }
        n += (ix >> 20) - 0x3ff;
        int32_t j = ix & 0x000fffff;
        /* determine interval */
        ix = j | 0x3ff00000; /* normalize ix */
        int32_t k;
        if (j <= 0x3988E) {
            k = 0; /* |x|<sqrt(3/2) */
        } else if (j < 0xBB67A) {
            k = 1; /* |x|<sqrt(3)   */
        } else {
            k = 0;
            n += 1;
            ix -= 0x00100000;
        }
        ax = with_high_word(ax, (uint32_t)ix);

        /* compute ss = s_h+s_l = (x-1)/(x+1) or (x-1.5)/(x+1.5) */
        double u = ax - bp[k]; /* bp[0]=1.0, bp[1]=1.5 */
        double v = one / (ax + bp[k]);
        double ss = u * v;
        double s_h = with_low_word(ss, 0);
        /* t_h=ax+bp[k] High */
        double t_h = with_high_word(zero, (uint32_t)(((ix >> 1) | 0x20000000) + 0x00080000 + (k << 18)));
        double t_l = ax - (t_h - bp[k]);
        double s_l = v * ((u - s_h * t_h) - s_h * t_l);
        /* compute log(ax) */
        double s2 = ss * ss;
        double r = s2 * s2 * (L1 + s2 * (L2 + s2 * (L3 + s2 * (L4 + s2 * (L5 + s2 * L6)))));
        r += s_l * (s_h + ss);
        s2 = s_h * s_h;
        t_h = with_low_word(3.0 + s2 + r, 0);
        t_l = r - ((t_h - 3.0) - s2);
        /* u+v = ss*(1+...) */
        u = s_h * t_h;
        v = s_l * t_h + t_l * ss;
        /* 2/(3log2)*(ss+...) */
        double p_h = with_low_word(u + v, 0);
        double p_l = v - (p_h - u);
        double z_h = cp_h * p_h; /* cp_h+cp_l = 2/(3*log2) */
        double z_l = cp_l * p_h + p_l * cp + dp_l[k];
        /* log2(ax) = (ss+..)*2/(3*log2) = n + dp_h + z_h + z_l */
        double t = (double)n;
        t1 = with_low_word(((z_h + z_l) + dp_h[k]) + t, 0);
        t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
    }

    /* split up y into y1+y2 and compute (y1+y2)*(t1+t2) */
    double y1 = with_low_word(y, 0);
    double p_l = (y - y1) * t1 + y * t2;
    double p_h = y1 * t1;
    double z = p_l + p_h;
    int32_t j = high_word(z);
    uint32_t i = low_word(z);
    if (j >= 0x40900000) {                   /* z >= 1024 */
        if (j != 0x40900000 || i != 0) {      /* if z > 1024 */
            return s * huge * huge;           /* overflow */
        } else {
            if (p_l + ovt > z - p_h) return s * huge * huge; /* overflow */
        }
    } else if ((j & 0x7fffffff) >= 0x4090cc00) { /* z <= -1075 */
        if ((uint32_t)j != 0xc090cc00u || i != 0) { /* z < -1075 */
            return s * tiny * tiny;                 /* underflow */
        } else {
            if (p_l <= z - p_h) return s * tiny * tiny; /* underflow */
        }
    }
    /*
     * compute 2**(p_h+p_l)
     */
    int32_t m = j & 0x7fffffff;
    int32_t k = (m >> 20) - 0x3ff;
    int32_t n = 0;
    if (m > 0x3fe00000) { /* if |z| > 0.5, set n = [z+0.5] */
        n = j + (0x00100000 >> (k + 1));
        k = ((n & 0x7fffffff) >> 20) - 0x3ff; /* new k for n */
        double t = with_high_word(zero, (uint32_t)(n & ~(0x000fffff >> k)));
        n = ((n & 0x000fffff) | 0x00100000) >> (20 - k);
        if (j < 0) n = -n;
        p_h -= t;
    }
    double t = with_low_word(p_l + p_h, 0);
    double u = t * lg2_h;
    double v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
    z = u + v;
    double w = v - (z - u);
    t = z * z;
    t1 = z - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    double r = (z * t1) / ((t1 - two) - (w + z * w));
    z = one - (r - z);
    uint32_t high = (uint32_t)high_word(z) + ((uint32_t)n << 20);
    if (((int32_t)high >> 20) <= 0) {
        z = scale_down(z, n); /* subnormal output */
    } else {
        z = with_high_word(z, high);
    }
    return s * z;
}
