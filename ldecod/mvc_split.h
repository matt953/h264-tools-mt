/*!
 *************************************************************************************
 * \file mvc_split.h
 *
 * \brief
 *    MVC NAL Unit stream splitter for multi-threaded decoding.
 *    Reads an Annex B byte stream and routes NALUs to per-view ring buffers
 *    based on NAL unit type and MVC extension header view_id.
 *************************************************************************************
 */

#ifndef _MVC_SPLIT_H_
#define _MVC_SPLIT_H_

#include "global.h"
#include "annexb.h"

/*!
 * \brief Route classification for a NALU
 */
typedef enum {
    MVC_ROUTE_VIEW0  = 0,    /*!< Send to View 0 only */
    MVC_ROUTE_VIEW1  = 1,    /*!< Send to View 1 only */
    MVC_ROUTE_BOTH   = 2,    /*!< Send to both views (SPS/PPS/SEI) */
    MVC_ROUTE_SKIP   = 3     /*!< Skip (filler data, etc.) */
} MVCRouteTarget;

/*!
 * \brief MVC Offset Metadata (OFMD) extracted from SEI messages
 */
#define OFMD_MAX_PLANES   32
#define OFMD_MAX_FRAMES   250000  /* ~2.9 hours at 24fps */
typedef struct {
    int      num_planes;                      /*!< Number of 3D planes */
    int      frame_count;                     /*!< Total frames accumulated so far */
    int8_t  *offsets[OFMD_MAX_PLANES];        /*!< Per-plane offset arrays (heap allocated) */
    int      offsets_capacity;                /*!< Allocated capacity per plane */
    int      valid;                           /*!< 1 if OFMD was found and parsed */
} OFMDData;

/*!
 * \brief MVC stream splitter context
 */
typedef struct {
    ANNEXB_t *input;           /*!< Source ring buffer (from FFmpeg) */
    ANNEXB_t *view_ring[2];   /*!< Per-view output ring buffers */

    /* Internal NALU extraction state */
    byte *buf;                 /*!< Temp buffer for one NALU */
    int   buf_size;            /*!< Allocated size of buf */

    /* I/O buffer for reading from input ring */
    byte *iobuffer;
    int   iobuffer_size;
    byte *iobuffer_read;
    int   bytes_in_buffer;
    int   is_eof;
    int   next_sc_bytes;       /*!< Bytes of next start code already consumed */
    int   is_first_nalu;

    volatile int stop;         /*!< Signal splitter thread to stop */

    /* OFMD data extracted from mvc_scalable_nesting SEI messages */
    OFMDData ofmd;
} MVCSplitter;

/*!
 * \brief Initialize the MVC splitter
 */
void mvc_splitter_init(MVCSplitter *splitter, ANNEXB_t *input,
                       ANNEXB_t *view0, ANNEXB_t *view1);

/*!
 * \brief Free splitter resources
 */
void mvc_splitter_free(MVCSplitter *splitter);

/*!
 * \brief Run the splitter loop (call from dedicated thread).
 *        Returns when input EOF is reached or stop is signaled.
 * \param arg  Pointer to MVCSplitter (for pthread_create)
 * \return NULL
 */
void *mvc_splitter_run(void *arg);

#endif /* _MVC_SPLIT_H_ */
