/*!
 *************************************************************************************
 * \file mvc_split.c
 *
 * \brief
 *    MVC NAL Unit stream splitter for multi-threaded decoding.
 *    Reads an Annex B byte stream from an input ring buffer and routes
 *    each NALU to the appropriate per-view output ring buffer(s) based
 *    on the NAL unit type and MVC extension header view_id.
 *
 * \author
 *    Multi-threaded MVC decoder project
 *************************************************************************************
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "mvc_split.h"

#define SPLITTER_BUF_SIZE   (2 * 1024 * 1024)
#define SPLITTER_IO_SIZE    (512 * 1024)

/* ---- Internal byte-stream reading (from input ring buffer) ---- */

static int splitter_fill_buffer(MVCSplitter *s)
{
    int mask = s->input->ring_size - 1;

    pthread_mutex_lock(&s->input->ring_mutex);
    while (s->input->ring_read == s->input->ring_write && !s->input->ring_eof && !s->input->ring_abort)
        pthread_cond_wait(&s->input->ring_cond, &s->input->ring_mutex);

    int64_t avail = s->input->ring_write - s->input->ring_read;
    if (avail <= 0 || s->input->ring_abort) {
        pthread_mutex_unlock(&s->input->ring_mutex);
        s->is_eof = 1;
        return 0;
    }

    int readbytes = (avail < s->iobuffer_size) ? (int)avail : s->iobuffer_size;
    int rpos = (int)(s->input->ring_read & mask);
    int first = s->input->ring_size - rpos;
    if (readbytes <= first) {
        memcpy(s->iobuffer, s->input->ring_buf + rpos, readbytes);
    } else {
        memcpy(s->iobuffer, s->input->ring_buf + rpos, first);
        memcpy(s->iobuffer + first, s->input->ring_buf, readbytes - first);
    }
    s->input->ring_read += readbytes;
    pthread_cond_signal(&s->input->ring_cond);
    pthread_mutex_unlock(&s->input->ring_mutex);

    s->bytes_in_buffer = readbytes;
    s->iobuffer_read = s->iobuffer;
    return readbytes;
}

static inline int splitter_getbyte(MVCSplitter *s, byte *out)
{
    if (s->bytes_in_buffer == 0) {
        if (splitter_fill_buffer(s) == 0)
            return 0;  /* EOF */
    }
    s->bytes_in_buffer--;
    *out = *s->iobuffer_read++;
    return 1;
}

/*!
 * \brief Extract one NALU from the byte stream.
 *        Returns NALU data (first byte = NAL header) in s->buf[0..len-1].
 *        Also sets *out_sc_len to 3 or 4 (the start code prefix length).
 * \return NALU length (>0), 0 on EOF, -1 on error
 */
static int splitter_get_nalu(MVCSplitter *s, int *out_sc_len)
{
    byte b;
    int nalu_start;  /* index in s->buf where NALU data starts */
    int pos = 0;     /* current write position in s->buf */

    /* --- Phase 1: Read the start code prefix --- */
    if (s->next_sc_bytes != 0) {
        /* Previous call left the start code bytes for us */
        int i;
        for (i = 0; i < s->next_sc_bytes - 1; i++)
            s->buf[pos++] = 0;
        s->buf[pos++] = 1;
    } else {
        /* Read bytes until we get a non-zero byte */
        while (1) {
            if (!splitter_getbyte(s, &b))
                return (pos == 0) ? 0 : -1;
            s->buf[pos++] = b;
            if (b != 0) break;
        }
    }

    if (s->is_eof)
        return 0;

    /* The start code should end with ...00 00 01 (3-byte) or ...00 00 00 01 (4-byte) */
    if (s->buf[pos-1] != 1 || pos < 3)
        return -1;

    *out_sc_len = (pos == 3) ? 3 : 4;
    nalu_start = pos;  /* NALU data starts here (first byte = NAL header) */

    /* --- Phase 2: Read NALU body until next start code --- */
    while (1) {
        if (!splitter_getbyte(s, &b)) {
            /* EOF — trim trailing zeros */
            while (pos > nalu_start && s->buf[pos-1] == 0)
                pos--;
            s->next_sc_bytes = 0;
            break;
        }

        /* Ensure buffer space */
        if (pos >= s->buf_size - 1) {
            int new_size = s->buf_size * 2;
            byte *new_buf = (byte *)realloc(s->buf, new_size);
            if (!new_buf) return -1;
            s->buf = new_buf;
            s->buf_size = new_size;
        }

        s->buf[pos++] = b;

        /* Check for start code 00 00 00 01 */
        if (pos >= nalu_start + 4 &&
            s->buf[pos-4] == 0 && s->buf[pos-3] == 0 &&
            s->buf[pos-2] == 0 && s->buf[pos-1] == 1) {
            /* Found 4-byte start code — trim trailing zeros of current NALU */
            pos -= 4;
            while (pos > nalu_start && s->buf[pos-1] == 0)
                pos--;
            s->next_sc_bytes = 4;
            break;
        }

        /* Check for start code 00 00 01 */
        if (pos >= nalu_start + 3 &&
            s->buf[pos-3] == 0 && s->buf[pos-2] == 0 && s->buf[pos-1] == 1) {
            pos -= 3;
            s->next_sc_bytes = 3;
            break;
        }
    }

    int nalu_len = pos - nalu_start;
    if (nalu_len <= 0)
        return -1;

    /* Move NALU data to beginning of buffer for convenience */
    if (nalu_start > 0)
        memmove(s->buf, s->buf + nalu_start, nalu_len);

    return nalu_len;
}

/*!
 * \brief Determine routing target for a NALU based on its header.
 * \param buf     NALU data (first byte is NAL header byte)
 * \param len     NALU length in bytes
 * \return MVCRouteTarget
 */
static MVCRouteTarget classify_nalu(const byte *buf, int len)
{
    int nal_unit_type = buf[0] & 0x1f;

    switch (nal_unit_type) {
    case 1:  /* SLICE */
    case 2:  /* DPA */
    case 3:  /* DPB */
    case 4:  /* DPC */
    case 5:  /* IDR */
        return MVC_ROUTE_VIEW0;   /* Base view coded slices */

    case 6:  /* SEI */
    case 7:  /* SPS */
    case 8:  /* PPS */
    case 9:  /* AUD */
    case 10: /* EOSEQ */
    case 11: /* EOSTREAM */
    case 13: /* SPS_EXT */
    case 15: /* SUB_SPS (Subset SPS for MVC) */
    case 24: /* VDRD */
        return MVC_ROUTE_BOTH;

    case 12: /* FILL */
        return MVC_ROUTE_SKIP;

    case 14: /* PREFIX — MVC prefix for base view */
        return MVC_ROUTE_VIEW0;

    case 20: /* SLC_EXT — MVC coded slice extension */
        if (len >= 4) {
            /* 3-byte MVC NAL unit header extension after the NAL header byte:
             * byte[1]: svc_extension_flag(1) | non_idr_flag(1) | priority_id(6)
             * byte[2]: view_id[9:2] (8 bits)
             * byte[3]: view_id[1:0](2) | anchor_pic_flag(1) | inter_view_flag(1) |
             *          reserved_one_bit(1) | temporal_id(3)
             */
            int svc_ext_flag = (buf[1] >> 7) & 1;
            if (svc_ext_flag == 0) {
                /* MVC extension — extract 10-bit view_id */
                int view_id = ((buf[2] & 0xFF) << 2) | ((buf[3] >> 6) & 0x03);
                return (view_id == 0) ? MVC_ROUTE_VIEW0 : MVC_ROUTE_VIEW1;
            }
        }
        return MVC_ROUTE_VIEW0;  /* fallback for short/malformed */

    default:
        return MVC_ROUTE_BOTH;   /* unknown — send to both */
    }
}

/*!
 * \brief Write a NALU (with start code prefix) to a ring buffer.
 */
static int write_nalu_to_ring(ANNEXB_t *ring, const byte *nalu_data, int nalu_len, int sc_len)
{
    byte sc[4] = {0, 0, 0, 1};
    int sc_offset = (sc_len == 3) ? 1 : 0;
    if (ring->ring_abort) return -1;
    if (annex_b_ring_feed(ring, sc + sc_offset, sc_len) < 0) return -1;
    if (annex_b_ring_feed(ring, nalu_data, nalu_len) < 0) return -1;
    return 0;
}

/* ---- OFMD extraction from SEI NALUs ---- */

/*!
 * \brief Search for "OFMD" magic bytes in a buffer.
 * \return Pointer to the 'O' in "OFMD", or NULL if not found.
 */
static const byte *find_ofmd_magic(const byte *data, int len)
{
    int i;
    for (i = 0; i <= len - 4; i++) {
        if (data[i]   == 0x4F && data[i+1] == 0x46 &&
            data[i+2] == 0x4D && data[i+3] == 0x44)
            return &data[i];
    }
    return NULL;
}

/*!
 * \brief Parse OFMD payload found within an mvc_scalable_nesting SEI.
 *        Appends per-frame offsets to the splitter's OFMDData.
 *
 * OFMD binary format (from OFSExtractor / BD3D2MK3D):
 *   Byte 0-3:  "OFMD" magic
 *   Byte 4:    frame_rate (lower 4 bits)
 *   Byte 5-9:  reserved
 *   Byte 10:   num_planes (lower 7 bits)
 *   Byte 11:   frame_count per this SEI message (lower 7 bits)
 *   Byte 12-13: reserved
 *   Byte 14+:  offset data [num_planes * frame_count bytes]
 *              Layout: plane 0 frames first, then plane 1, etc.
 *
 * Offset encoding: 0-127 = positive, 128 = undefined, 129-255 = negative (128 - val)
 */
static void parse_ofmd_payload(MVCSplitter *s, const byte *ofmd, int remaining)
{
    OFMDData *d = &s->ofmd;
    int frame_rate, num_planes, frame_count;
    int data_needed, i, j;

    if (remaining < 14)
        return;

    frame_rate = ofmd[4] & 0x0F;
    if (frame_rate < 1 || frame_rate > 7)
        return;

    num_planes  = ofmd[10] & 0x7F;
    frame_count = ofmd[11] & 0x7F;

    if (num_planes <= 0 || num_planes > OFMD_MAX_PLANES || frame_count <= 0)
        return;

    data_needed = 14 + num_planes * frame_count;
    if (remaining < data_needed)
        return;

    /* First OFMD message: initialize */
    if (!d->valid) {
        d->num_planes = num_planes;
        d->frame_count = 0;
        d->offsets_capacity = 4096;  /* initial capacity per plane */
        for (i = 0; i < num_planes; i++) {
            d->offsets[i] = (int8_t *)malloc(d->offsets_capacity * sizeof(int8_t));
            if (!d->offsets[i]) return;
        }
        d->valid = 1;
        fprintf(stderr, "[OFMD] Found OFMD: %d planes, frame_rate=%d\n",
                num_planes, frame_rate);
    }

    /* Grow capacity if needed */
    if (d->frame_count + frame_count > d->offsets_capacity) {
        int new_cap = d->offsets_capacity;
        while (new_cap < d->frame_count + frame_count)
            new_cap *= 2;
        if (new_cap > OFMD_MAX_FRAMES)
            new_cap = OFMD_MAX_FRAMES;
        for (i = 0; i < d->num_planes; i++) {
            int8_t *new_buf = (int8_t *)realloc(d->offsets[i], new_cap * sizeof(int8_t));
            if (!new_buf) return;
            d->offsets[i] = new_buf;
        }
        d->offsets_capacity = new_cap;
    }

    /* Append offsets for each plane */
    for (i = 0; i < d->num_planes && i < num_planes; i++) {
        const byte *plane_data = ofmd + 14 + i * frame_count;
        for (j = 0; j < frame_count; j++) {
            byte raw = plane_data[j];
            int8_t val;
            if (raw <= 127)
                val = (int8_t)raw;
            else if (raw == 128)
                val = 0;  /* undefined → treat as zero offset */
            else
                val = (int8_t)(128 - (int)raw);  /* 129→-1, 130→-2, etc. */
            d->offsets[i][d->frame_count + j] = val;
        }
    }
    d->frame_count += frame_count;
}

/*!
 * \brief Scan a SEI NALU for OFMD payload within mvc_scalable_nesting messages.
 *        The NALU data starts with the NAL header byte (type 6 = SEI).
 */
static void scan_sei_for_ofmd(MVCSplitter *s, const byte *nalu, int len)
{
    const byte *ofmd;

    /* Quick scan: look for "OFMD" anywhere in the SEI NALU.
     * This is what OFSExtractor does — the mvc_scalable_nesting header
     * is variable-length, so scanning for the magic is the simplest approach. */
    ofmd = find_ofmd_magic(nalu, len);
    if (ofmd) {
        int remaining = len - (int)(ofmd - nalu);
        parse_ofmd_payload(s, ofmd, remaining);
    }
}

/* ---- Public API ---- */

void mvc_splitter_init(MVCSplitter *splitter, ANNEXB_t *input,
                       ANNEXB_t *view0, ANNEXB_t *view1)
{
    memset(splitter, 0, sizeof(*splitter));
    splitter->input = input;
    splitter->view_ring[0] = view0;
    splitter->view_ring[1] = view1;

    splitter->buf_size = SPLITTER_BUF_SIZE;
    splitter->buf = (byte *)malloc(splitter->buf_size);

    splitter->iobuffer_size = SPLITTER_IO_SIZE;
    splitter->iobuffer = (byte *)malloc(splitter->iobuffer_size);
    splitter->iobuffer_read = splitter->iobuffer;
    splitter->bytes_in_buffer = 0;
    splitter->is_eof = 0;
    splitter->next_sc_bytes = 0;
    splitter->is_first_nalu = 1;
    splitter->stop = 0;
}

void mvc_splitter_free(MVCSplitter *splitter)
{
    int i;
    free(splitter->buf);
    splitter->buf = NULL;
    free(splitter->iobuffer);
    splitter->iobuffer = NULL;

    /* Free OFMD data */
    for (i = 0; i < OFMD_MAX_PLANES; i++) {
        free(splitter->ofmd.offsets[i]);
        splitter->ofmd.offsets[i] = NULL;
    }
}

void *mvc_splitter_run(void *arg)
{
    MVCSplitter *s = (MVCSplitter *)arg;
    int nalu_len, sc_len;

    while (!s->stop) {
        nalu_len = splitter_get_nalu(s, &sc_len);
        if (nalu_len <= 0)
            break;

        /* s->buf[0..nalu_len-1] = NALU data (first byte = NAL header) */

        /* Check SEI NALUs for OFMD data before routing */
        {
            int nal_type = s->buf[0] & 0x1f;
            if (nal_type == 6)  /* SEI */
                scan_sei_for_ofmd(s, s->buf, nalu_len);
        }

        MVCRouteTarget target = classify_nalu(s->buf, nalu_len);

        {
            int nal_type = s->buf[0] & 0x1f;
            fprintf(stderr, "[SPLIT] NALU type=%d len=%d -> %s\n", nal_type, nalu_len,
                    target == MVC_ROUTE_VIEW0 ? "V0" :
                    target == MVC_ROUTE_VIEW1 ? "V1" :
                    target == MVC_ROUTE_BOTH  ? "BOTH" : "SKIP");
        }

        switch (target) {
        case MVC_ROUTE_VIEW0:
            write_nalu_to_ring(s->view_ring[0], s->buf, nalu_len, sc_len);
            break;
        case MVC_ROUTE_VIEW1:
            write_nalu_to_ring(s->view_ring[1], s->buf, nalu_len, sc_len);
            break;
        case MVC_ROUTE_BOTH:
            write_nalu_to_ring(s->view_ring[0], s->buf, nalu_len, sc_len);
            write_nalu_to_ring(s->view_ring[1], s->buf, nalu_len, sc_len);
            break;
        case MVC_ROUTE_SKIP:
            break;
        }
    }

    /* Signal EOF to both view ring buffers */
    annex_b_ring_signal_eof(s->view_ring[0]);
    annex_b_ring_signal_eof(s->view_ring[1]);

    return NULL;
}
