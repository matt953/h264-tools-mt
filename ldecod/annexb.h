
/*!
 *************************************************************************************
 * \file annexb.h
 *
 * \brief
 *    Annex B byte stream buffer handling.
 *
 *************************************************************************************
 */

#ifndef _ANNEXB_H_
#define _ANNEXB_H_

#include "nalucommon.h"
#include <pthread.h>
#include <stdint.h>

typedef struct annex_b_struct
{
  int  BitStreamFile;                //!< the bit stream file, -1=closed, -2=ring buffer mode
  byte *iobuffer;
  byte *iobufferread;
  int bytesinbuffer;
  int is_eof;
  int iIOBufferSize;

  int IsFirstByteStreamNALU;
  int nextstartcodebytes;
  byte *Buf;

  // Ring buffer mode (BitStreamFile == -2)
  byte *ring_buf;                    //!< ring buffer storage
  int   ring_size;                   //!< ring buffer capacity (power of 2)
  volatile int64_t ring_write;       //!< write position (producer/FFmpeg thread)
  volatile int64_t ring_read;        //!< read position (consumer/decoder thread)
  volatile int ring_eof;             //!< producer signals no more data
  pthread_mutex_t ring_mutex;
  pthread_cond_t  ring_cond;
} ANNEXB_t;

extern int  get_annex_b_NALU (VideoParameters *p_Vid, NALU_t *nalu, ANNEXB_t *annex_b);

extern void open_annex_b     (char *fn, ANNEXB_t *annex_b);
extern void open_annex_b_ring(int ring_size, ANNEXB_t *annex_b);
extern int  annex_b_ring_feed(ANNEXB_t *annex_b, const byte *data, int size);
extern int  annex_b_ring_try_feed(ANNEXB_t *annex_b, const byte *data, int size);
extern void annex_b_ring_signal_eof(ANNEXB_t *annex_b);
extern void close_annex_b    (ANNEXB_t *annex_b);
extern void malloc_annex_b   (VideoParameters *p_Vid, ANNEXB_t **p_annex_b);
extern void free_annex_b     (ANNEXB_t **p_annex_b);
extern void init_annex_b     (ANNEXB_t *annex_b);
extern void reset_annex_b    (ANNEXB_t *annex_b);
#endif

