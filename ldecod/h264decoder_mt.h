/*!
 *************************************************************************************
 * \file h264decoder_mt.h
 *
 * \brief
 *    Multi-threaded MVC H.264 decoder interface.
 *    Uses two independent decoder instances (one per view) with a
 *    NALU stream splitter for parallel MVC decoding.
 *
 * \author
 *    Multi-threaded MVC decoder project
 *************************************************************************************
 */

#ifndef _H264DECODER_MT_H_
#define _H264DECODER_MT_H_

#include "global.h"
#include "annexb.h"
#include "h264decoder.h"
#include "mvc_split.h"

/*!
 * \brief Multi-threaded MVC decoder context
 */
typedef struct mvc_decoder_mt {
    /* Per-view decoder state (each has own p_Dec, p_Vid, etc.) */
    DecoderParams *dec[2];         /*!< dec[0]=View 0, dec[1]=View 1 */
    ANNEXB_t      *view_ring[2];   /*!< Per-view input ring buffers */

    /* Stream splitter */
    MVCSplitter    splitter;
    pthread_t      splitter_thread;

    /* Per-view decoder threads */
    pthread_t      view_thread[2];
    volatile int   view_finished[2];
    volatile int   view_error[2];

    /* Inter-view reference queue: View 0 pushes (never blocks),
     * View 1 pops (blocks until matching POC available). */
    #define IV_REF_QUEUE_SIZE 64
    struct {
        struct storable_picture *pic;
        int              poc;
    } iv_queue[64];
    int             iv_queue_head;   /* next write position */
    int             iv_queue_tail;   /* next read position */
    int             iv_queue_count;
    int             iv_eof;          /* View 0 finished */
    volatile int    shutdown;        /* Global shutdown flag */
    int             flushed;         /* FlushDecoderMT already called */
    pthread_mutex_t iv_mutex;
    pthread_cond_t  iv_cond;

    /* Input ring buffer (from FFmpeg or caller) */
    ANNEXB_t       *input_ring;

    /* Per-view raw picture callbacks */
    RawPicOutputFunc raw_func[2];
    void            *raw_ctx[2];

    /* Overall raw picture callback (set by caller) */
    RawPicOutputFunc output_func;
    void            *output_ctx;

    int ring_size;
} MVCDecoderMT;

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * \brief Open multi-threaded MVC decoder.
 *        Creates two decoder instances, a stream splitter, and
 *        per-view ring buffers. The caller feeds data to the
 *        returned input ring buffer.
 * \param p_Inp      Input parameters (infile is ignored)
 * \param ring_size  Input ring buffer size (power of 2)
 * \param pp_input   Output: pointer to input ANNEXB_t for feeding data
 * \return Pointer to MVCDecoderMT context, or NULL on error
 */
MVCDecoderMT *OpenDecoderMT(InputParameters *p_Inp, int ring_size, ANNEXB_t **pp_input);

/*!
 * \brief Set the raw picture output callback for the MT decoder.
 *        The callback is invoked from both view threads.
 */
void SetRawPicOutputMT(MVCDecoderMT *mt, RawPicOutputFunc func, void *ctx);

/*!
 * \brief Start the splitter and decoder threads.
 *        Must be called after SetRawPicOutputMT so callbacks are set
 *        before threads begin decoding.
 * \return 0 on success, -1 on error
 */
int StartDecoderMT(MVCDecoderMT *mt);

/*!
 * \brief Signal EOF on the input ring buffer and wait for all
 *        threads to finish.
 */
void FlushDecoderMT(MVCDecoderMT *mt);

/*!
 * \brief Close the MT decoder and free all resources.
 */
void CloseDecoderMT(MVCDecoderMT *mt);

#ifdef __cplusplus
}
#endif

#endif /* _H264DECODER_MT_H_ */
