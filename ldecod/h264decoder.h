
/*!
 ************************************************************************
 *  \file
 *     h264decoder.h
 *  \brief
 *     interface for H.264 decoder.
 *  \author
 *     Copyright (C) 2009 Dolby
 *  Yuwen He (yhe@dolby.com)
 *  
 ************************************************************************
 */
#ifndef _H264DECODER_H_
#define _H264DECODER_H_

#include "global.h"
#include "annexb.h"

typedef enum
{
  DEC_GEN_NOERR = 0,
  DEC_OPEN_NOERR = 0,
  DEC_CLOSE_NOERR = 0,  
  DEC_SUCCEED = 0,
  DEC_EOS =1,
  DEC_NEED_DATA = 2,
  DEC_INVALID_PARAM = 3,
  DEC_ERRMASK = 0x8000
//  DEC_ERRMASK = 0x80000000
}DecErrCode;

typedef struct dec_set_t
{
  int iPostprocLevel; // valid interval are [0..100]
  int bDBEnable;
  int bAllLayers;
  int time_incr;
  int bDecCompAdapt;
} DecSet_t;

/* Raw decoded picture for zero-copy callback (bypasses img2buf) */
typedef struct {
    imgpel **imgY;      /* luma row pointers (uncropped) */
    imgpel **imgU;      /* chroma-U row pointers, NULL for YUV400 */
    imgpel **imgV;      /* chroma-V row pointers, NULL for YUV400 */
    int width;          /* cropped luma width (pixels) */
    int height;         /* cropped luma height (pixels) */
    int width_cr;       /* cropped chroma width (pixels) */
    int height_cr;      /* cropped chroma height (pixels) */
    int crop_x;         /* luma pixels to skip from left */
    int crop_y;         /* luma rows to skip from top */
    int crop_x_cr;      /* chroma pixels to skip from left */
    int crop_y_cr;      /* chroma rows to skip from top */
    int view_id;
    int poc;
} RawDecodedPic;

typedef void (*RawPicOutputFunc)(void *ctx, const RawDecodedPic *pic);

#ifdef __cplusplus
extern "C" {
#endif

int OpenDecoder(InputParameters *p_Inp);
int OpenDecoderRing(InputParameters *p_Inp, int ring_size, ANNEXB_t **pp_annex_b);
int DecodeOneFrame(DecodedPicList **ppDecPic);
int FinitDecoder(DecodedPicList **ppDecPicList);
int CloseDecoder();
int SetOptsDecoder(DecSet_t *pDecOpts);
void SetRawPicOutput(RawPicOutputFunc func, void *ctx);

#ifdef __cplusplus
}
#endif
#endif
