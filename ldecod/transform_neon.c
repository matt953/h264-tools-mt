/*!
 ***************************************************************************
 * \file transform_neon.c
 *
 * \brief
 *    NEON-optimized 8x8 inverse integer transform for H.264
 *
 *    Implements the same butterfly as inverse8x8() in transform.c but
 *    processes all 8 columns in parallel using ARM NEON int16x8_t vectors.
 *    The 16-bit intermediate precision is sufficient for 8-bit video
 *    (dequantized coefficients fit in 16 bits at QP <= 51, 8-bit depth).
 *
 * \date
 *    26 March 2026
 **************************************************************************
 */

#ifdef __aarch64__

#include <arm_neon.h>
#include "global.h"
#include "transform.h"

/*
 * Transpose an 8x8 matrix held in eight int16x8_t registers.
 *
 * Uses the standard NEON zip/unzip sequence:
 *   stage 1: interleave pairs   (16-bit -> 32-bit groups)
 *   stage 2: interleave quads   (32-bit -> 64-bit groups)
 *   stage 3: interleave octets  (64-bit -> 128-bit halves)
 */
static inline void transpose_8x8_s16(
    int16x8_t *r0, int16x8_t *r1, int16x8_t *r2, int16x8_t *r3,
    int16x8_t *r4, int16x8_t *r5, int16x8_t *r6, int16x8_t *r7)
{
    /* Stage 1 -- interleave 16-bit elements pairwise */
    int16x8x2_t t01 = vzipq_s16(*r0, *r1);
    int16x8x2_t t23 = vzipq_s16(*r2, *r3);
    int16x8x2_t t45 = vzipq_s16(*r4, *r5);
    int16x8x2_t t67 = vzipq_s16(*r6, *r7);

    /* Reinterpret as 32-bit for the next zip stage */
    int32x4x2_t u02 = vzipq_s32(vreinterpretq_s32_s16(t01.val[0]),
                                  vreinterpretq_s32_s16(t23.val[0]));
    int32x4x2_t u13 = vzipq_s32(vreinterpretq_s32_s16(t01.val[1]),
                                  vreinterpretq_s32_s16(t23.val[1]));
    int32x4x2_t u46 = vzipq_s32(vreinterpretq_s32_s16(t45.val[0]),
                                  vreinterpretq_s32_s16(t67.val[0]));
    int32x4x2_t u57 = vzipq_s32(vreinterpretq_s32_s16(t45.val[1]),
                                  vreinterpretq_s32_s16(t67.val[1]));

    /* Stage 3 -- combine low/high 64-bit halves to finish the transpose */
    *r0 = vreinterpretq_s16_s64(
        vcombine_s64(vget_low_s64(vreinterpretq_s64_s32(u02.val[0])),
                     vget_low_s64(vreinterpretq_s64_s32(u46.val[0]))));
    *r1 = vreinterpretq_s16_s64(
        vcombine_s64(vget_high_s64(vreinterpretq_s64_s32(u02.val[0])),
                     vget_high_s64(vreinterpretq_s64_s32(u46.val[0]))));
    *r2 = vreinterpretq_s16_s64(
        vcombine_s64(vget_low_s64(vreinterpretq_s64_s32(u02.val[1])),
                     vget_low_s64(vreinterpretq_s64_s32(u46.val[1]))));
    *r3 = vreinterpretq_s16_s64(
        vcombine_s64(vget_high_s64(vreinterpretq_s64_s32(u02.val[1])),
                     vget_high_s64(vreinterpretq_s64_s32(u46.val[1]))));
    *r4 = vreinterpretq_s16_s64(
        vcombine_s64(vget_low_s64(vreinterpretq_s64_s32(u13.val[0])),
                     vget_low_s64(vreinterpretq_s64_s32(u57.val[0]))));
    *r5 = vreinterpretq_s16_s64(
        vcombine_s64(vget_high_s64(vreinterpretq_s64_s32(u13.val[0])),
                     vget_high_s64(vreinterpretq_s64_s32(u57.val[0]))));
    *r6 = vreinterpretq_s16_s64(
        vcombine_s64(vget_low_s64(vreinterpretq_s64_s32(u13.val[1])),
                     vget_low_s64(vreinterpretq_s64_s32(u57.val[1]))));
    *r7 = vreinterpretq_s16_s64(
        vcombine_s64(vget_high_s64(vreinterpretq_s64_s32(u13.val[1])),
                     vget_high_s64(vreinterpretq_s64_s32(u57.val[1]))));
}

/*
 * One-dimensional 8-point H.264 inverse transform butterfly.
 *
 * Operates on 8 int16x8_t vectors where each vector holds one "row"
 * (or one "column" after transposition) of 8 values.  After the call
 * the vectors contain the transformed values.
 *
 * The butterfly matches the scalar code in inverse8x8() exactly:
 *
 *   Even part (inputs p0, p2, p4, p6):
 *     a0 =  p0 + p4
 *     a1 =  p0 - p4
 *     a2 =  p6 - (p2 >> 1)
 *     a3 =  p2 + (p6 >> 1)
 *     b0 =  a0 + a3
 *     b2 =  a1 - a2
 *     b4 =  a1 + a2
 *     b6 =  a0 - a3
 *
 *   Odd part (inputs p1, p3, p5, p7):
 *     a0 = -p3 + p5 - p7 - (p7 >> 1)
 *     a1 =  p1 + p7 - p3 - (p3 >> 1)
 *     a2 = -p1 + p7 + p5 + (p5 >> 1)
 *     a3 =  p3 + p5 + p1 + (p1 >> 1)
 *     b1 =  a0 + (a3 >> 2)
 *     b3 =  a1 + (a2 >> 2)
 *     b5 =  a2 - (a1 >> 2)
 *     b7 =  a3 - (a0 >> 2)
 *
 *   Output:
 *     out0 = b0 + b7,  out1 = b2 - b5,  out2 = b4 + b3,  out3 = b6 + b1
 *     out4 = b6 - b1,  out5 = b4 - b3,  out6 = b2 + b5,  out7 = b0 - b7
 */
static inline void idct8_1d_s16(
    int16x8_t *p0, int16x8_t *p1, int16x8_t *p2, int16x8_t *p3,
    int16x8_t *p4, int16x8_t *p5, int16x8_t *p6, int16x8_t *p7)
{
    /* ---------- even part ---------- */
    int16x8_t a0 = vaddq_s16(*p0, *p4);
    int16x8_t a1 = vsubq_s16(*p0, *p4);
    int16x8_t a2 = vsubq_s16(*p6, vshrq_n_s16(*p2, 1));
    int16x8_t a3 = vaddq_s16(*p2, vshrq_n_s16(*p6, 1));

    int16x8_t b0 = vaddq_s16(a0, a3);
    int16x8_t b2 = vsubq_s16(a1, a2);
    int16x8_t b4 = vaddq_s16(a1, a2);
    int16x8_t b6 = vsubq_s16(a0, a3);

    /* ---------- odd part ---------- */
    /* a0 = -p3 + p5 - p7 - (p7 >> 1) */
    a0 = vsubq_s16(vsubq_s16(vsubq_s16(*p5, *p3), *p7),
                    vshrq_n_s16(*p7, 1));
    /* a1 =  p1 + p7 - p3 - (p3 >> 1) */
    a1 = vsubq_s16(vsubq_s16(vaddq_s16(*p1, *p7), *p3),
                    vshrq_n_s16(*p3, 1));
    /* a2 = -p1 + p7 + p5 + (p5 >> 1) */
    a2 = vaddq_s16(vaddq_s16(vsubq_s16(*p7, *p1), *p5),
                    vshrq_n_s16(*p5, 1));
    /* a3 =  p3 + p5 + p1 + (p1 >> 1) */
    a3 = vaddq_s16(vaddq_s16(vaddq_s16(*p3, *p5), *p1),
                    vshrq_n_s16(*p1, 1));

    int16x8_t b1 = vaddq_s16(a0, vshrq_n_s16(a3, 2));
    int16x8_t b3 = vaddq_s16(a1, vshrq_n_s16(a2, 2));
    int16x8_t b5 = vsubq_s16(a2, vshrq_n_s16(a1, 2));
    int16x8_t b7 = vsubq_s16(a3, vshrq_n_s16(a0, 2));

    /* ---------- combine ---------- */
    *p0 = vaddq_s16(b0, b7);
    *p1 = vsubq_s16(b2, b5);
    *p2 = vaddq_s16(b4, b3);
    *p3 = vaddq_s16(b6, b1);
    *p4 = vsubq_s16(b6, b1);
    *p5 = vsubq_s16(b4, b3);
    *p6 = vaddq_s16(b2, b5);
    *p7 = vsubq_s16(b0, b7);
}

/*!
 ***********************************************************************
 * \brief
 *    NEON-optimized 8x8 inverse integer transform (H.264 spec 8.5.12).
 *
 *    Drop-in replacement for inverse8x8() in transform.c.
 *    Both horizontal and vertical passes are computed entirely in
 *    NEON registers using 16-bit arithmetic.
 *
 * \param tblock
 *    Input  -- pointer-to-pointer rows of transform coefficients
 * \param block
 *    Output -- pointer-to-pointer rows of spatial-domain residual
 * \param pos_x
 *    Horizontal offset (column) within the row arrays
 ***********************************************************************
 */
void inverse8x8_neon(int **tblock, int **block, int pos_x)
{
    int i;

    /*
     * Load 8 rows from tblock[0..7][pos_x .. pos_x+7].
     * The source is int (32-bit); we narrow to int16 on load.
     * This is safe for 8-bit video where dequantized coefficients
     * fit comfortably in 16 bits.
     */
    int16x8_t r0, r1, r2, r3, r4, r5, r6, r7;

    {
        int *src;

        src = tblock[0] + pos_x;
        r0 = vcombine_s16(vmovn_s32(vld1q_s32(src)),
                           vmovn_s32(vld1q_s32(src + 4)));

        src = tblock[1] + pos_x;
        r1 = vcombine_s16(vmovn_s32(vld1q_s32(src)),
                           vmovn_s32(vld1q_s32(src + 4)));

        src = tblock[2] + pos_x;
        r2 = vcombine_s16(vmovn_s32(vld1q_s32(src)),
                           vmovn_s32(vld1q_s32(src + 4)));

        src = tblock[3] + pos_x;
        r3 = vcombine_s16(vmovn_s32(vld1q_s32(src)),
                           vmovn_s32(vld1q_s32(src + 4)));

        src = tblock[4] + pos_x;
        r4 = vcombine_s16(vmovn_s32(vld1q_s32(src)),
                           vmovn_s32(vld1q_s32(src + 4)));

        src = tblock[5] + pos_x;
        r5 = vcombine_s16(vmovn_s32(vld1q_s32(src)),
                           vmovn_s32(vld1q_s32(src + 4)));

        src = tblock[6] + pos_x;
        r6 = vcombine_s16(vmovn_s32(vld1q_s32(src)),
                           vmovn_s32(vld1q_s32(src + 4)));

        src = tblock[7] + pos_x;
        r7 = vcombine_s16(vmovn_s32(vld1q_s32(src)),
                           vmovn_s32(vld1q_s32(src + 4)));
    }

    /*
     * Pass 1 -- Horizontal transform.
     * Each register r0..r7 holds one row of 8 coefficients.
     * After the 1-D butterfly, each register holds the horizontally
     * transformed row.
     */
    idct8_1d_s16(&r0, &r1, &r2, &r3, &r4, &r5, &r6, &r7);

    /*
     * Transpose the 8x8 matrix so that columns become rows.
     * After this, r0 holds column 0 of the intermediate result
     * across all 8 rows, r1 holds column 1, etc.
     */
    transpose_8x8_s16(&r0, &r1, &r2, &r3, &r4, &r5, &r6, &r7);

    /*
     * Pass 2 -- Vertical transform.
     * Same butterfly, now operating on columns (stored as rows after
     * the transpose).
     */
    idct8_1d_s16(&r0, &r1, &r2, &r3, &r4, &r5, &r6, &r7);

    /*
     * Transpose back so that rows are rows again.
     */
    transpose_8x8_s16(&r0, &r1, &r2, &r3, &r4, &r5, &r6, &r7);

    /*
     * Store the 8 result rows back into block[0..7][pos_x .. pos_x+7].
     * The destination is int (32-bit), so we widen from int16 on store.
     */
    {
        int *dst;

        dst = block[0] + pos_x;
        vst1q_s32(dst,     vmovl_s16(vget_low_s16(r0)));
        vst1q_s32(dst + 4, vmovl_s16(vget_high_s16(r0)));

        dst = block[1] + pos_x;
        vst1q_s32(dst,     vmovl_s16(vget_low_s16(r1)));
        vst1q_s32(dst + 4, vmovl_s16(vget_high_s16(r1)));

        dst = block[2] + pos_x;
        vst1q_s32(dst,     vmovl_s16(vget_low_s16(r2)));
        vst1q_s32(dst + 4, vmovl_s16(vget_high_s16(r2)));

        dst = block[3] + pos_x;
        vst1q_s32(dst,     vmovl_s16(vget_low_s16(r3)));
        vst1q_s32(dst + 4, vmovl_s16(vget_high_s16(r3)));

        dst = block[4] + pos_x;
        vst1q_s32(dst,     vmovl_s16(vget_low_s16(r4)));
        vst1q_s32(dst + 4, vmovl_s16(vget_high_s16(r4)));

        dst = block[5] + pos_x;
        vst1q_s32(dst,     vmovl_s16(vget_low_s16(r5)));
        vst1q_s32(dst + 4, vmovl_s16(vget_high_s16(r5)));

        dst = block[6] + pos_x;
        vst1q_s32(dst,     vmovl_s16(vget_low_s16(r6)));
        vst1q_s32(dst + 4, vmovl_s16(vget_high_s16(r6)));

        dst = block[7] + pos_x;
        vst1q_s32(dst,     vmovl_s16(vget_low_s16(r7)));
        vst1q_s32(dst + 4, vmovl_s16(vget_high_s16(r7)));
    }
}

#endif /* __aarch64__ */
