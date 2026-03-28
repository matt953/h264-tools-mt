/*!
 *************************************************************************************
 * \file mc_prediction_neon.c
 *
 * \brief
 *    NEON-optimized H.264 motion compensation interpolation filters
 *
 *    Provides ARM NEON SIMD implementations of the 6-tap FIR interpolation
 *    filters used for half-pel luma motion compensation.
 *    Processes 8 pixels at a time using 128-bit NEON vectors.
 *
 *    The H.264 6-tap filter kernel is:
 *      result = (p[-2] + p[3]) - 5*(p[-1] + p[2]) + 20*(p[0] + p[1])
 *
 *    For half-pel positions: output = clip((result + 16) >> 5)
 *    For diagonal (H+V):    output = clip((result + 512) >> 10)
 *
 * \note
 *    imgpel is uint16 in this codebase (IMGTYPE=1), so we use uint16x8_t
 *    vectors and widen to int32x4_t for the filter arithmetic.
 *
 *************************************************************************************
 */

#ifdef __aarch64__

#include <arm_neon.h>
#include <string.h>
#include "global.h"
#include "mc_prediction.h"

/* ============================================================================
 *  filter_6tap_u16 -- Apply the 6-tap H.264 FIR filter to 8 uint16 samples.
 *
 *  Given six uint16x8_t vectors (t0..t5) representing the 6 tap positions,
 *  computes:  (t0 + t5) - 5*(t1 + t4) + 20*(t2 + t3)
 *
 *  Returns two int32x4_t vectors (lo and hi halves) since the intermediate
 *  results can exceed 16 bits.
 * ============================================================================
 */
static inline void filter_6tap_u16(uint16x8_t t0, uint16x8_t t1,
                                   uint16x8_t t2, uint16x8_t t3,
                                   uint16x8_t t4, uint16x8_t t5,
                                   int32x4_t *out_lo, int32x4_t *out_hi)
{
    /* Widen all taps to signed 32-bit for safe arithmetic.
     * Worst case for 10-bit input: |1+5+20+20+5+1| * 1023 = 53196,
     * well within int32 range. */
    int32x4_t s0_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(t0)));
    int32x4_t s1_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(t1)));
    int32x4_t s2_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(t2)));
    int32x4_t s3_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(t3)));
    int32x4_t s4_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(t4)));
    int32x4_t s5_lo = vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(t5)));

    int32x4_t s0_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(t0)));
    int32x4_t s1_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(t1)));
    int32x4_t s2_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(t2)));
    int32x4_t s3_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(t3)));
    int32x4_t s4_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(t4)));
    int32x4_t s5_hi = vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(t5)));

    int32x4_t sum05_lo = vaddq_s32(s0_lo, s5_lo);
    int32x4_t sum05_hi = vaddq_s32(s0_hi, s5_hi);
    int32x4_t sum14_lo = vaddq_s32(s1_lo, s4_lo);
    int32x4_t sum14_hi = vaddq_s32(s1_hi, s4_hi);
    int32x4_t sum23_lo = vaddq_s32(s2_lo, s3_lo);
    int32x4_t sum23_hi = vaddq_s32(s2_hi, s3_hi);

    int32x4_t five   = vdupq_n_s32(5);
    int32x4_t twenty = vdupq_n_s32(20);

    /* result = (t0+t5) + 20*(t2+t3) - 5*(t1+t4) */
    *out_lo = vaddq_s32(sum05_lo,
                vsubq_s32(vmulq_s32(twenty, sum23_lo),
                           vmulq_s32(five, sum14_lo)));
    *out_hi = vaddq_s32(sum05_hi,
                vsubq_s32(vmulq_s32(twenty, sum23_hi),
                           vmulq_s32(five, sum14_hi)));
}

/* ============================================================================
 *  filter_6tap_s32 -- Apply the 6-tap filter to 8 int32 samples stored as
 *  two int32x4_t pairs per tap row. Used in the vertical pass of get_luma_22
 *  where the horizontally-filtered intermediates are already int32.
 *
 *  Computes: (r0 + r5) - 5*(r1 + r4) + 20*(r2 + r3)
 * ============================================================================
 */
static inline void filter_6tap_s32(int32x4_t r0_lo, int32x4_t r0_hi,
                                   int32x4_t r1_lo, int32x4_t r1_hi,
                                   int32x4_t r2_lo, int32x4_t r2_hi,
                                   int32x4_t r3_lo, int32x4_t r3_hi,
                                   int32x4_t r4_lo, int32x4_t r4_hi,
                                   int32x4_t r5_lo, int32x4_t r5_hi,
                                   int32x4_t *out_lo, int32x4_t *out_hi)
{
    int32x4_t sum05_lo = vaddq_s32(r0_lo, r5_lo);
    int32x4_t sum05_hi = vaddq_s32(r0_hi, r5_hi);
    int32x4_t sum14_lo = vaddq_s32(r1_lo, r4_lo);
    int32x4_t sum14_hi = vaddq_s32(r1_hi, r4_hi);
    int32x4_t sum23_lo = vaddq_s32(r2_lo, r3_lo);
    int32x4_t sum23_hi = vaddq_s32(r2_hi, r3_hi);

    int32x4_t five   = vdupq_n_s32(5);
    int32x4_t twenty = vdupq_n_s32(20);

    *out_lo = vaddq_s32(sum05_lo,
                vsubq_s32(vmulq_s32(twenty, sum23_lo),
                           vmulq_s32(five, sum14_lo)));
    *out_hi = vaddq_s32(sum05_hi,
                vsubq_s32(vmulq_s32(twenty, sum23_hi),
                           vmulq_s32(five, sum14_hi)));
}

/* ============================================================================
 *  shift_clip_to_u16 -- Add rounding offset, arithmetic right shift, clamp
 *  to [0, max], and narrow from int32x4 pair back to uint16x8.
 *
 *  Parameters are pre-computed NEON vectors for the rounding constant,
 *  negative shift amount, and max value, so they can be hoisted out of loops.
 *
 *  Computes: clamp((val + rnd) >> shift, 0, max)
 *  where neg_shift = -shift (vshlq_s32 uses negative values for right shift).
 * ============================================================================
 */
static inline uint16x8_t shift_clip_to_u16(int32x4_t lo, int32x4_t hi,
                                           int32x4_t rnd, int32x4_t neg_shift,
                                           int32x4_t vmax)
{
    int32x4_t zero = vdupq_n_s32(0);

    /* (val + rnd) >> shift */
    lo = vshlq_s32(vaddq_s32(lo, rnd), neg_shift);
    hi = vshlq_s32(vaddq_s32(hi, rnd), neg_shift);

    /* Clamp to [0, max_imgpel_value] */
    lo = vmaxq_s32(lo, zero);
    lo = vminq_s32(lo, vmax);
    hi = vmaxq_s32(hi, zero);
    hi = vminq_s32(hi, vmax);

    /* Narrow from int32x4 to uint16x4 and combine into uint16x8 */
    uint16x4_t lo16 = vmovn_u32(vreinterpretq_u32_s32(lo));
    uint16x4_t hi16 = vmovn_u32(vreinterpretq_u32_s32(hi));
    return vcombine_u16(lo16, hi16);
}


/*!
 ************************************************************************
 * \brief
 *    NEON-optimized half-pel horizontal interpolation (position 2,0)
 *
 *    Same signature and semantics as the scalar get_luma_20 in
 *    mc_prediction.c. For each row, the source pointer starts at
 *    cur_imgY[j][x_pos - 2] (the leftmost tap), so the 6 taps for
 *    output pixel i are at offsets i, i+1, i+2, i+3, i+4, i+5.
 *
 *    Uses vextq_u16 to slide a window of 8 pixels across the 6 tap
 *    positions, avoiding redundant loads.
 ************************************************************************
 */
void get_luma_20_neon(imgpel **block, imgpel **cur_imgY,
                      int block_size_y, int block_size_x,
                      int x_pos, int max_imgpel_value)
{
    int i, j;
    int32x4_t rnd       = vdupq_n_s32(16);
    int32x4_t neg_shift = vdupq_n_s32(-5);
    int32x4_t vmax      = vdupq_n_s32(max_imgpel_value);

    for (j = 0; j < block_size_y; j++)
    {
        imgpel *src = &cur_imgY[j][x_pos - 2];
        imgpel *dst = block[j];

        for (i = 0; i <= block_size_x - 8; i += 8)
        {
            /* Load 16 source pixels starting at src+i.
             * We need src[i+0] through src[i+12] for 8 output pixels.
             * Loading two uint16x8 vectors covers src[i+0..i+15]. */
            uint16x8_t s_lo = vld1q_u16(src + i);
            uint16x8_t s_hi = vld1q_u16(src + i + 8);

            /* Extract the 6 tap vectors using vext (byte-shift and merge) */
            uint16x8_t t0 = s_lo;
            uint16x8_t t1 = vextq_u16(s_lo, s_hi, 1);
            uint16x8_t t2 = vextq_u16(s_lo, s_hi, 2);
            uint16x8_t t3 = vextq_u16(s_lo, s_hi, 3);
            uint16x8_t t4 = vextq_u16(s_lo, s_hi, 4);
            uint16x8_t t5 = vextq_u16(s_lo, s_hi, 5);

            int32x4_t res_lo, res_hi;
            filter_6tap_u16(t0, t1, t2, t3, t4, t5, &res_lo, &res_hi);

            uint16x8_t out = shift_clip_to_u16(res_lo, res_hi, rnd, neg_shift, vmax);
            vst1q_u16(dst + i, out);
        }

        /* Scalar fallback for remaining pixels when block_size_x is not
         * a multiple of 8 (e.g. 4-wide partitions). */
        for (; i < block_size_x; i++)
        {
            imgpel *p = src + i;
            int result = (p[0] + p[5]) - 5 * (p[1] + p[4]) + 20 * (p[2] + p[3]);
            int val = (result + 16) >> 5;
            if (val < 0) val = 0;
            if (val > max_imgpel_value) val = max_imgpel_value;
            dst[i] = (imgpel)val;
        }
    }
}


/*!
 ************************************************************************
 * \brief
 *    NEON-optimized half-pel vertical interpolation (position 0,2)
 *
 *    Same signature and semantics as the scalar get_luma_02 in
 *    mc_prediction.c. The 6 vertical taps for each output row are
 *    accessed via flat-buffer pointer arithmetic: p0 starts at
 *    cur_imgY[-2][x_pos], and successive rows are p0 + k*shift_x.
 *
 *    Since all 8 pixels in a NEON vector come from the same row,
 *    we simply load from each of the 6 row pointers and apply the
 *    filter vertically.
 ************************************************************************
 */
void get_luma_02_neon(imgpel **block, imgpel **cur_imgY,
                      int block_size_y, int block_size_x,
                      int x_pos, int shift_x, int max_imgpel_value)
{
    int i, j;
    int32x4_t rnd       = vdupq_n_s32(16);
    int32x4_t neg_shift = vdupq_n_s32(-5);
    int32x4_t vmax      = vdupq_n_s32(max_imgpel_value);

    /* p0 starts 2 rows above the first output row.
     * The original scalar code: p0 = &(cur_imgY[-2][x_pos])
     * and advances p0 by one row (shift_x elements) per output row. */
    imgpel *p0 = &(cur_imgY[-2][x_pos]);

    for (j = 0; j < block_size_y; j++)
    {
        imgpel *p1 = p0 + shift_x;
        imgpel *p2 = p1 + shift_x;
        imgpel *p3 = p2 + shift_x;
        imgpel *p4 = p3 + shift_x;
        imgpel *p5 = p4 + shift_x;
        imgpel *dst = block[j];

        for (i = 0; i <= block_size_x - 8; i += 8)
        {
            uint16x8_t t0 = vld1q_u16(p0 + i);
            uint16x8_t t1 = vld1q_u16(p1 + i);
            uint16x8_t t2 = vld1q_u16(p2 + i);
            uint16x8_t t3 = vld1q_u16(p3 + i);
            uint16x8_t t4 = vld1q_u16(p4 + i);
            uint16x8_t t5 = vld1q_u16(p5 + i);

            int32x4_t res_lo, res_hi;
            filter_6tap_u16(t0, t1, t2, t3, t4, t5, &res_lo, &res_hi);

            uint16x8_t out = shift_clip_to_u16(res_lo, res_hi, rnd, neg_shift, vmax);
            vst1q_u16(dst + i, out);
        }

        /* Scalar fallback for remaining pixels */
        for (; i < block_size_x; i++)
        {
            int result = (p0[i] + p5[i]) - 5 * (p1[i] + p4[i]) + 20 * (p2[i] + p3[i]);
            int val = (result + 16) >> 5;
            if (val < 0) val = 0;
            if (val > max_imgpel_value) val = max_imgpel_value;
            dst[i] = (imgpel)val;
        }

        /* Advance p0 down by one row for the next output row.
         * In the original scalar code this is: p0 = p1 - block_size_x
         * (because p1 was incremented by block_size_x in the inner loop).
         * Since we use indexed access and do not modify p1, we simply
         * set p0 to p1 (which is p0 + shift_x). */
        p0 = p1;
    }
}


/*!
 ************************************************************************
 * \brief
 *    NEON-optimized diagonal half-pel interpolation (position 2,2)
 *
 *    Same signature and semantics as the scalar get_luma_22 in
 *    mc_prediction.c. Two-pass separable filter:
 *
 *    Pass 1 (horizontal): For each of (block_size_y + 5) rows, apply
 *      the 6-tap filter horizontally and store the unscaled int32
 *      intermediates into tmp_res[][].
 *
 *    Pass 2 (vertical): For each output row, apply the 6-tap filter
 *      vertically across the 6 consecutive rows of int32 intermediates,
 *      with final rounding: clip((result + 512) >> 10, 0, max).
 ************************************************************************
 */
void get_luma_22_neon(imgpel **block, imgpel **cur_imgY, int **tmp_res,
                      int block_size_y, int block_size_x,
                      int x_pos, int max_imgpel_value)
{
    int i, j;

    /* ================================================================
     *  Pass 1: Horizontal 6-tap filter into tmp_res (int32)
     *
     *  Iterates over block_size_y + 5 rows (from row -2 to
     *  row block_size_y + 2 relative to the first output row).
     * ================================================================ */
    int jj = -2;
    for (j = 0; j < block_size_y + 5; j++)
    {
        imgpel *src    = &cur_imgY[jj++][x_pos - 2];
        int    *tmp_ln = tmp_res[j];

        for (i = 0; i <= block_size_x - 8; i += 8)
        {
            uint16x8_t s_lo = vld1q_u16(src + i);
            uint16x8_t s_hi = vld1q_u16(src + i + 8);

            uint16x8_t t0 = s_lo;
            uint16x8_t t1 = vextq_u16(s_lo, s_hi, 1);
            uint16x8_t t2 = vextq_u16(s_lo, s_hi, 2);
            uint16x8_t t3 = vextq_u16(s_lo, s_hi, 3);
            uint16x8_t t4 = vextq_u16(s_lo, s_hi, 4);
            uint16x8_t t5 = vextq_u16(s_lo, s_hi, 5);

            int32x4_t res_lo, res_hi;
            filter_6tap_u16(t0, t1, t2, t3, t4, t5, &res_lo, &res_hi);

            /* Store unscaled int32 intermediates */
            vst1q_s32(tmp_ln + i,     res_lo);
            vst1q_s32(tmp_ln + i + 4, res_hi);
        }

        /* Scalar tail */
        for (; i < block_size_x; i++)
        {
            imgpel *p = src + i;
            tmp_ln[i] = (p[0] + p[5]) - 5 * (p[1] + p[4]) + 20 * (p[2] + p[3]);
        }
    }

    /* ================================================================
     *  Pass 2: Vertical 6-tap filter on int32 intermediates
     *
     *  For output row j, the 6 tap rows are tmp_res[j] .. tmp_res[j+5].
     *  Final output: clip((result + 512) >> 10, 0, max_imgpel_value)
     * ================================================================ */
    {
        int32x4_t rnd       = vdupq_n_s32(512);
        int32x4_t neg_shift = vdupq_n_s32(-10);
        int32x4_t vmax      = vdupq_n_s32(max_imgpel_value);

        for (j = 0; j < block_size_y; j++)
        {
            int *x0 = tmp_res[j];
            int *x1 = tmp_res[j + 1];
            int *x2 = tmp_res[j + 2];
            int *x3 = tmp_res[j + 3];
            int *x4 = tmp_res[j + 4];
            int *x5 = tmp_res[j + 5];
            imgpel *dst = block[j];

            for (i = 0; i <= block_size_x - 8; i += 8)
            {
                int32x4_t r0_lo = vld1q_s32(x0 + i);
                int32x4_t r0_hi = vld1q_s32(x0 + i + 4);
                int32x4_t r1_lo = vld1q_s32(x1 + i);
                int32x4_t r1_hi = vld1q_s32(x1 + i + 4);
                int32x4_t r2_lo = vld1q_s32(x2 + i);
                int32x4_t r2_hi = vld1q_s32(x2 + i + 4);
                int32x4_t r3_lo = vld1q_s32(x3 + i);
                int32x4_t r3_hi = vld1q_s32(x3 + i + 4);
                int32x4_t r4_lo = vld1q_s32(x4 + i);
                int32x4_t r4_hi = vld1q_s32(x4 + i + 4);
                int32x4_t r5_lo = vld1q_s32(x5 + i);
                int32x4_t r5_hi = vld1q_s32(x5 + i + 4);

                int32x4_t res_lo, res_hi;
                filter_6tap_s32(r0_lo, r0_hi, r1_lo, r1_hi,
                                r2_lo, r2_hi, r3_lo, r3_hi,
                                r4_lo, r4_hi, r5_lo, r5_hi,
                                &res_lo, &res_hi);

                uint16x8_t out = shift_clip_to_u16(res_lo, res_hi,
                                                   rnd, neg_shift, vmax);
                vst1q_u16(dst + i, out);
            }

            /* Scalar tail */
            for (; i < block_size_x; i++)
            {
                int result = (x0[i] + x5[i]) - 5 * (x1[i] + x4[i])
                           + 20 * (x2[i] + x3[i]);
                int val = (result + 512) >> 10;
                if (val < 0) val = 0;
                if (val > max_imgpel_value) val = max_imgpel_value;
                dst[i] = (imgpel)val;
            }
        }
    }
}

#endif /* __aarch64__ */
