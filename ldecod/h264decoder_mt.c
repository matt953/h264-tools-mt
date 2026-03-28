/*!
 *************************************************************************************
 * \file h264decoder_mt.c
 *
 * \brief
 *    Multi-threaded MVC H.264 decoder implementation.
 *    Creates two independent decoder instances (one per MVC view) with a
 *    NALU stream splitter for parallel decoding. View 0 decoded frames are
 *    provided to View 1 via an inter-view reference mechanism so that View 1
 *    can use them as motion-compensation references without searching View 0's DPB.
 *
 * \author
 *    Multi-threaded MVC decoder project
 *************************************************************************************
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#include "h264decoder_mt.h"
#include "mbuffer.h"

#define MT_RING_SIZE      (4 * 1024 * 1024)  /* Per-view ring buffer size */
#define MT_INPUT_RING_SIZE (4 * 1024 * 1024)  /* Input ring buffer size */

/* ---- Direct picture clone (copies pixels from pic, not tempData3) ---- */

static StorablePicture *mt_clone_picture(VideoParameters *p_Vid, StorablePicture *pic)
{
    int i;
    StorablePicture *c = alloc_storable_picture(p_Vid,
        (PictureStructure)pic->structure,
        pic->size_x, pic->size_y,
        pic->size_x_cr, pic->size_y_cr, 0);

    /* Copy metadata */
    c->pic_num        = pic->pic_num;
    c->frame_num      = pic->frame_num;
    c->long_term_frame_idx = pic->long_term_frame_idx;
    c->long_term_pic_num   = pic->long_term_pic_num;
    c->is_long_term   = 0;
    c->non_existing   = pic->non_existing;
    c->max_slice_id   = pic->max_slice_id;
    c->structure      = pic->structure;
    c->size_x         = pic->size_x;
    c->size_y         = pic->size_y;
    c->size_x_cr      = pic->size_x_cr;
    c->size_y_cr      = pic->size_y_cr;
    c->size_x_m1      = pic->size_x - 1;
    c->size_y_m1      = pic->size_y - 1;
    c->size_x_cr_m1   = pic->size_x_cr - 1;
    c->size_y_cr_m1   = pic->size_y_cr - 1;
    c->mb_aff_frame_flag = pic->mb_aff_frame_flag;
    c->seiHasTone_mapping = pic->seiHasTone_mapping;
    c->poc            = pic->poc;
    c->top_poc        = pic->top_poc;
    c->bottom_poc     = pic->bottom_poc;
    c->frame_poc      = pic->frame_poc;
    c->coded_frame    = 1;
    c->qp             = pic->qp;
    c->slice_qp_delta = pic->slice_qp_delta;
    c->chroma_qp_offset[0] = pic->chroma_qp_offset[0];
    c->chroma_qp_offset[1] = pic->chroma_qp_offset[1];
    c->slice_type     = pic->slice_type;
    c->idr_flag       = pic->idr_flag;
    c->no_output_of_prior_pics_flag = pic->no_output_of_prior_pics_flag;
    c->long_term_reference_flag = 0;
    c->adaptive_ref_pic_buffering_flag = 0;
    c->dec_ref_pic_marking_buffer = NULL;
    c->PicWidthInMbs  = pic->PicWidthInMbs;
    c->recovery_frame = pic->recovery_frame;
    c->chroma_format_idc = pic->chroma_format_idc;
    c->frame_mbs_only_flag = pic->frame_mbs_only_flag;
    c->frame_cropping_flag = pic->frame_cropping_flag;
    if (pic->frame_cropping_flag) {
        c->frame_crop_left_offset   = pic->frame_crop_left_offset;
        c->frame_crop_right_offset  = pic->frame_crop_right_offset;
        c->frame_crop_top_offset    = pic->frame_crop_top_offset;
        c->frame_crop_bottom_offset = pic->frame_crop_bottom_offset;
    }
    c->view_id        = pic->view_id;
    c->inter_view_flag = pic->inter_view_flag;
    c->anchor_pic_flag = pic->anchor_pic_flag;

    /* Copy pixel data directly from pic (not from tempData3) */
    for (i = 0; i < pic->size_y; i++)
        memcpy(c->imgY[i], pic->imgY[i], pic->size_x * sizeof(imgpel));
    if (pic->imgUV) {
        for (i = 0; i < pic->size_y_cr; i++)
            memcpy(c->imgUV[0][i], pic->imgUV[0][i], pic->size_x_cr * sizeof(imgpel));
        for (i = 0; i < pic->size_y_cr; i++)
            memcpy(c->imgUV[1][i], pic->imgUV[1][i], pic->size_x_cr * sizeof(imgpel));
    }

    /* Pad edges so View 1's motion compensation reads valid data near borders */
    pad_dec_picture(p_Vid, c);

    return c;
}

/* ---- View 0 frame-done callback ---- */

/*!
 * \brief Called from View 0's exit_picture() after a frame is stored in DPB.
 *        Provides the decoded picture to View 1 for inter-view reference.
 */
static void v0_frame_done_callback(VideoParameters *p_Vid, StorablePicture *pic)
{
    MVCDecoderMT *mt = (MVCDecoderMT *)p_Vid->mt_frame_done_ctx;
    if (!mt) return;

    /* Clone the picture directly (not via tempData3) so View 0 can reuse the original */
    StorablePicture *clone = mt_clone_picture(p_Vid, pic);

    pthread_mutex_lock(&mt->iv_mutex);
    /* Block View 0 until queue has space — provides backpressure to prevent
     * View 0 from racing too far ahead and overflowing the queue */
    while (mt->iv_queue_count >= IV_REF_QUEUE_SIZE && !mt->shutdown) {
        pthread_cond_wait(&mt->iv_cond, &mt->iv_mutex);
    }
    if (mt->shutdown) {
        pthread_mutex_unlock(&mt->iv_mutex);
        free_storable_picture(clone);
        return;
    }
    {
        int h = mt->iv_queue_head;
        mt->iv_queue[h].pic = clone;
        mt->iv_queue[h].poc = pic->frame_poc;
        fprintf(stderr, "[MT-V0] push poc=%d count=%d\n", pic->frame_poc, mt->iv_queue_count + 1);
        mt->iv_queue_head = (h + 1) % IV_REF_QUEUE_SIZE;
        mt->iv_queue_count++;
    }
    pthread_cond_signal(&mt->iv_cond);
    pthread_mutex_unlock(&mt->iv_mutex);
}

/* ---- Per-view decoder thread functions ---- */

static void *view0_thread_func(void *arg)
{
    MVCDecoderMT *mt = (MVCDecoderMT *)arg;
    DecodedPicList *pDecPicList;
    int iRet;

    fprintf(stderr, "[MT] View 0 thread started\n");
    /* Set this thread's TLS p_Dec to View 0's decoder instance */
    p_Dec = mt->dec[0];

    /* Enable longjmp-based error handling so error() doesn't call exit() */
    p_Dec->use_error_jmp = 1;
    if (setjmp(p_Dec->error_jmp) != 0) {
        /* Landed here from error() via longjmp — treat as fatal decode error */
        fprintf(stderr, "[MT] View 0: error() caught (code %d), stopping decode\n",
                p_Dec->error_code);
        mt->view_error[0] = 1;
        goto done;
    }

    /* Set per-view raw picture callback */
    SetRawPicOutput(mt->raw_func[0], mt->raw_ctx[0]);

    do {
        iRet = DecodeOneFrame(&pDecPicList);
        if (iRet != DEC_EOS && iRet != DEC_SUCCEED) {
            mt->view_error[0] = 1;
            break;
        }
    } while (iRet == DEC_SUCCEED);

    FinitDecoder(&pDecPicList);

done:
    p_Dec->use_error_jmp = 0;
    mt->view_finished[0] = 1;

    /* Signal EOF on the iv_queue so View 1 stops waiting */
    pthread_mutex_lock(&mt->iv_mutex);
    mt->iv_eof = 1;
    pthread_cond_broadcast(&mt->iv_cond);
    pthread_mutex_unlock(&mt->iv_mutex);

    /* If View 1 also finished, abort the input ring to unblock FFmpeg's feed */
    if (mt->view_finished[1]) {
        mt->input_ring->ring_abort = 1;
        annex_b_ring_signal_eof(mt->input_ring);
    }

    return NULL;
}

static void *view1_thread_func(void *arg)
{
    MVCDecoderMT *mt = (MVCDecoderMT *)arg;
    DecodedPicList *pDecPicList;
    int iRet;

    fprintf(stderr, "[MT] View 1 thread started\n");
    /* Set this thread's TLS p_Dec to View 1's decoder instance */
    p_Dec = mt->dec[1];

    /* Enable longjmp-based error handling so error() doesn't call exit() */
    p_Dec->use_error_jmp = 1;
    if (setjmp(p_Dec->error_jmp) != 0) {
        fprintf(stderr, "[MT] View 1: error() caught (code %d), stopping decode\n",
                p_Dec->error_code);
        mt->view_error[1] = 1;
        goto done;
    }

    /* Set per-view raw picture callback */
    SetRawPicOutput(mt->raw_func[1], mt->raw_ctx[1]);

    do {
        iRet = DecodeOneFrame(&pDecPicList);
        if (iRet != DEC_EOS && iRet != DEC_SUCCEED) {
            mt->view_error[1] = 1;
            break;
        }
    } while (iRet == DEC_SUCCEED);

    FinitDecoder(&pDecPicList);

done:
    p_Dec->use_error_jmp = 0;
    mt->view_finished[1] = 1;

    /* If View 0 also finished, abort the input ring to unblock FFmpeg's feed */
    if (mt->view_finished[0]) {
        mt->input_ring->ring_abort = 1;
        annex_b_ring_signal_eof(mt->input_ring);
    }

    return NULL;
}

/* ---- Helper: create one decoder instance in ring buffer mode ---- */

/*!
 * \brief Creates a decoder instance with its own ring buffer.
 *        Sets the thread-local p_Dec on the calling thread.
 * \return 0 on success
 */
static int create_decoder_instance(InputParameters *p_Inp, int ring_size,
                                   DecoderParams **out_dec, ANNEXB_t **out_ring)
{
    int iRet;

    /* OpenDecoderRing sets the TLS p_Dec and returns the ring buffer */
    iRet = OpenDecoderRing(p_Inp, ring_size, out_ring);
    if (iRet != DEC_OPEN_NOERR)
        return -1;

    /* Save the created decoder instance */
    *out_dec = p_Dec;

    /* Clear TLS so it doesn't point to this instance after we return */
    p_Dec = NULL;

    return 0;
}

/* ---- Public API ---- */

MVCDecoderMT *OpenDecoderMT(InputParameters *p_Inp, int ring_size, ANNEXB_t **pp_input)
{
    MVCDecoderMT *mt;
    InputParameters inp_v0, inp_v1;

    fprintf(stderr, "[MT] OpenDecoderMT called\n");

    mt = (MVCDecoderMT *)calloc(1, sizeof(MVCDecoderMT));
    if (!mt) return NULL;

    mt->ring_size = ring_size > 0 ? ring_size : MT_INPUT_RING_SIZE;

    /* Initialize synchronization primitives */
    pthread_mutex_init(&mt->iv_mutex, NULL);
    pthread_cond_init(&mt->iv_cond, NULL);
    /* iv_queue initialized by calloc (all zeros) */

    /* --- Create input ring buffer (from FFmpeg) --- */
    mt->input_ring = (ANNEXB_t *)calloc(1, sizeof(ANNEXB_t));
    if (!mt->input_ring) goto fail;
    mt->input_ring->ring_buf = (byte *)malloc(mt->ring_size);
    if (!mt->input_ring->ring_buf) goto fail;
    mt->input_ring->ring_size = mt->ring_size;
    mt->input_ring->BitStreamFile = -2;
    pthread_mutex_init(&mt->input_ring->ring_mutex, NULL);
    pthread_cond_init(&mt->input_ring->ring_cond, NULL);
    *pp_input = mt->input_ring;

    /* --- Prepare InputParameters for each view --- */
    memcpy(&inp_v0, p_Inp, sizeof(InputParameters));
    memcpy(&inp_v1, p_Inp, sizeof(InputParameters));

    /* View 0: base view — standard H.264 decode mode.
     * DecodeAllLayers must be 1 so that MVC parameter sets (Subset SPS)
     * are parsed correctly even for the base view instance. */
    inp_v0.DecodeAllLayers = 1;

    /* View 1: dependent view — needs MVC mode */
    inp_v1.DecodeAllLayers = 1;

    /* --- Create View 0 decoder instance --- */
    if (create_decoder_instance(&inp_v0, MT_RING_SIZE,
                                &mt->dec[0], &mt->view_ring[0]) != 0)
        goto fail;

    /* Configure View 0 for MT mode: install frame-done callback */
    mt->dec[0]->p_Vid->mt_mode = 1;
    mt->dec[0]->p_Vid->mt_frame_done = v0_frame_done_callback;
    mt->dec[0]->p_Vid->mt_frame_done_ctx = mt;

    /* --- Create View 1 decoder instance --- */
    if (create_decoder_instance(&inp_v1, MT_RING_SIZE,
                                &mt->dec[1], &mt->view_ring[1]) != 0)
        goto fail;

    /* Configure View 1 for MT mode: install inter-view reference queue mechanism */
    mt->dec[1]->p_Vid->mt_mode = 1;
    mt->dec[1]->p_Vid->mt_iv_mutex = &mt->iv_mutex;
    mt->dec[1]->p_Vid->mt_iv_cond = &mt->iv_cond;
    mt->dec[1]->p_Vid->mt_iv_queue = mt->iv_queue;
    mt->dec[1]->p_Vid->mt_iv_queue_head = &mt->iv_queue_head;
    mt->dec[1]->p_Vid->mt_iv_queue_tail = &mt->iv_queue_tail;
    mt->dec[1]->p_Vid->mt_iv_queue_count = &mt->iv_queue_count;
    mt->dec[1]->p_Vid->mt_iv_queue_eof = &mt->iv_eof;
    mt->dec[1]->p_Vid->mt_shutdown = &mt->shutdown;

    /* --- Initialize the stream splitter --- */
    mvc_splitter_init(&mt->splitter, mt->input_ring,
                      mt->view_ring[0], mt->view_ring[1]);

    /* Threads are NOT started here — caller must call StartDecoderMT()
     * after setting callbacks via SetRawPicOutputMT(). */

    return mt;

fail:
    /* Cleanup on failure */
    if (mt->input_ring) {
        free(mt->input_ring->ring_buf);
        pthread_mutex_destroy(&mt->input_ring->ring_mutex);
        pthread_cond_destroy(&mt->input_ring->ring_cond);
        free(mt->input_ring);
    }
    pthread_mutex_destroy(&mt->iv_mutex);
    pthread_cond_destroy(&mt->iv_cond);
    /* iv_queue cleanup: nothing to free (StorablePictures owned by DPB) */
    free(mt);
    return NULL;
}

void SetRawPicOutputMT(MVCDecoderMT *mt, RawPicOutputFunc func, void *ctx)
{
    if (!mt) return;
    mt->output_func = func;
    mt->output_ctx = ctx;
    /* Both views share the same callback — the caller distinguishes by view_id */
    mt->raw_func[0] = func;
    mt->raw_ctx[0] = ctx;
    mt->raw_func[1] = func;
    mt->raw_ctx[1] = ctx;
}

int StartDecoderMT(MVCDecoderMT *mt)
{
    if (!mt) return -1;

    /* Start splitter thread first */
    if (pthread_create(&mt->splitter_thread, NULL, mvc_splitter_run, &mt->splitter) != 0)
        return -1;

    /* Start decoder threads */
    if (pthread_create(&mt->view_thread[0], NULL, view0_thread_func, mt) != 0)
        return -1;
    if (pthread_create(&mt->view_thread[1], NULL, view1_thread_func, mt) != 0)
        return -1;

    return 0;
}

void FlushDecoderMT(MVCDecoderMT *mt)
{
    if (!mt || mt->flushed) return;
    mt->flushed = 1;

    fprintf(stderr, "[FLUSH] FlushDecoderMT: signalling EOF on input ring\n");

    /* Signal EOF on input ring — splitter will propagate to view rings.
     * Do NOT abort rings or set shutdown — let decoders finish naturally. */
    annex_b_ring_signal_eof(mt->input_ring);

    /* Wait for splitter to finish (it propagates EOF to view rings) */
    fprintf(stderr, "[FLUSH] joining splitter...\n");
    pthread_join(mt->splitter_thread, NULL);
    fprintf(stderr, "[FLUSH] splitter joined. joining view0...\n");

    /* Wait for View 0 to finish (it sets iv_eof when done) */
    pthread_join(mt->view_thread[0], NULL);
    fprintf(stderr, "[FLUSH] view0 joined. joining view1...\n");

    /* Now that View 0 is done, ensure iv_eof is set to unblock View 1
     * (view0_thread_func already does this, but be safe) */
    pthread_mutex_lock(&mt->iv_mutex);
    mt->iv_eof = 1;
    pthread_cond_broadcast(&mt->iv_cond);
    pthread_mutex_unlock(&mt->iv_mutex);

    /* Wait for View 1 to finish */
    pthread_join(mt->view_thread[1], NULL);
    fprintf(stderr, "[FLUSH] all threads joined\n");

    /* Set shutdown for cleanup code that checks it */
    mt->shutdown = 1;
}

void CloseDecoderMT(MVCDecoderMT *mt)
{
    if (!mt) return;

    /* Ensure all threads are stopped */
    FlushDecoderMT(mt);

    /* Close each decoder instance */
    /* We need to set TLS p_Dec for each CloseDecoder call since it uses p_Dec */
    p_Dec = mt->dec[0];
    CloseDecoder();

    p_Dec = mt->dec[1];
    CloseDecoder();

    p_Dec = NULL;

    /* Free splitter resources */
    mvc_splitter_free(&mt->splitter);

    /* Free input ring buffer */
    if (mt->input_ring) {
        free(mt->input_ring->ring_buf);
        pthread_mutex_destroy(&mt->input_ring->ring_mutex);
        pthread_cond_destroy(&mt->input_ring->ring_cond);
        free(mt->input_ring);
    }

    /* Destroy sync primitives */
    pthread_mutex_destroy(&mt->iv_mutex);
    pthread_cond_destroy(&mt->iv_cond);
    /* iv_queue cleanup: nothing to free (StorablePictures owned by DPB) */

    free(mt);
}
