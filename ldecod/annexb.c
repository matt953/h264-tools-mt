
/*!
 *************************************************************************************
 * \file annexb.c
 *
 * \brief
 *    Annex B Byte Stream format
 *
 * \author
 *    Main contributors (see contributors.h for copyright, address and affiliation details)
 *      - Stephan Wenger                  <stewe@cs.tu-berlin.de>
 *************************************************************************************
 */

#include "global.h"
#include "annexb.h"
#include "memalloc.h" 
#include "fast_memory.h"

static const int IOBUFFERSIZE = 512*1024; //65536;

void malloc_annex_b(VideoParameters *p_Vid, ANNEXB_t **p_annex_b)
{
  if ( ((*p_annex_b) = (ANNEXB_t *) calloc(1, sizeof(ANNEXB_t))) == NULL)
  {
    snprintf(errortext, ET_SIZE, "Memory allocation for Annex_B file failed");
    error(errortext,100);
  }
  if (((*p_annex_b)->Buf = (byte*) malloc(p_Vid->nalu->max_size)) == NULL)
  {
    error("malloc_annex_b: Buf", 101);
  }
}


void init_annex_b(ANNEXB_t *annex_b)
{
  annex_b->BitStreamFile = -1;
  annex_b->iobuffer = NULL;
  annex_b->iobufferread = NULL;
  annex_b->bytesinbuffer = 0;
  annex_b->is_eof = FALSE;
  annex_b->IsFirstByteStreamNALU = 1;
  annex_b->nextstartcodebytes = 0;
  annex_b->ring_buf = NULL;
  annex_b->ring_size = 0;
  annex_b->ring_write = 0;
  annex_b->ring_read = 0;
  annex_b->ring_eof = 0;
}

void free_annex_b(ANNEXB_t **p_annex_b)
{
  free((*p_annex_b)->Buf);
  (*p_annex_b)->Buf = NULL;
  free(*p_annex_b);
  *p_annex_b = NULL;  
}

/*!
************************************************************************
* \brief
*    fill IO buffer
************************************************************************
*/
static inline int getChunk_ring(ANNEXB_t *annex_b)
{
  unsigned int readbytes = 0;
  int mask = annex_b->ring_size - 1;

  pthread_mutex_lock(&annex_b->ring_mutex);

  // Wait until data is available or EOF
  while (annex_b->ring_read == annex_b->ring_write && !annex_b->ring_eof)
    pthread_cond_wait(&annex_b->ring_cond, &annex_b->ring_mutex);

  int64_t avail = annex_b->ring_write - annex_b->ring_read;
  if (avail <= 0)
  {
    pthread_mutex_unlock(&annex_b->ring_mutex);
    annex_b->is_eof = TRUE;
    return 0;
  }

  readbytes = (avail < annex_b->iIOBufferSize) ? (int)avail : annex_b->iIOBufferSize;

  // Copy from ring buffer (may wrap around)
  int rpos = (int)(annex_b->ring_read & mask);
  int first = annex_b->ring_size - rpos;
  if ((int)readbytes <= first)
  {
    memcpy(annex_b->iobuffer, annex_b->ring_buf + rpos, readbytes);
  }
  else
  {
    memcpy(annex_b->iobuffer, annex_b->ring_buf + rpos, first);
    memcpy(annex_b->iobuffer + first, annex_b->ring_buf, readbytes - first);
  }
  annex_b->ring_read += readbytes;

  // Signal producer that space is available
  pthread_cond_signal(&annex_b->ring_cond);
  pthread_mutex_unlock(&annex_b->ring_mutex);

  annex_b->bytesinbuffer = readbytes;
  annex_b->iobufferread = annex_b->iobuffer;

  return readbytes;
}

static inline int getChunk(ANNEXB_t *annex_b)
{
  unsigned int readbytes;

  if (annex_b->BitStreamFile == -2)
    return getChunk_ring(annex_b);

  readbytes = read (annex_b->BitStreamFile, annex_b->iobuffer, annex_b->iIOBufferSize);
  if (0==readbytes)
  {
    annex_b->is_eof = TRUE;
    return 0;
  }

  annex_b->bytesinbuffer = readbytes;
  annex_b->iobufferread = annex_b->iobuffer;
  return readbytes;
}

/*!
************************************************************************
* \brief
*    returns a byte from IO buffer
************************************************************************
*/
static inline byte getfbyte(ANNEXB_t *annex_b)
{
  if (0 == annex_b->bytesinbuffer)
  {
    if (0 == getChunk(annex_b))
      return 0;
  }
  annex_b->bytesinbuffer--;
  return (*annex_b->iobufferread++);
}

/*!
 ************************************************************************
 * \brief
 *    returns if new start code is found at byte aligned position buf.
 *    new-startcode is of form N 0x00 bytes, followed by a 0x01 byte.
 *
 *  \return
 *     1 if start-code is found or                      \n
 *     0, indicating that there is no start code
 *
 *  \param Buf
 *     pointer to byte-stream
 *  \param zeros_in_startcode
 *     indicates number of 0x00 bytes in start-code.
 ************************************************************************
 */
static inline int FindStartCode (unsigned char *Buf, int zeros_in_startcode)
{
  int i;

  for (i = 0; i < zeros_in_startcode; i++)
  {
    if(*(Buf++) != 0)
    {
      return 0;
    }
  }

  if(*Buf != 1)
    return 0;

  return 1;
}


/*!
 ************************************************************************
 * \brief
 *    Returns the size of the NALU (bits between start codes in case of
 *    Annex B.  nalu->buf and nalu->len are filled.  Other field in
 *    nalu-> remain uninitialized (will be taken care of by NALUtoRBSP.
 *
 * \return
 *     0 if there is nothing any more to read (EOF)
 *    -1 in case of any error
 *
 *  \note Side-effect: Returns length of start-code in bytes.
 *
 * \note
 *   get_annex_b_NALU expects start codes at byte aligned positions in the file
 *
 ************************************************************************
 */

int get_annex_b_NALU (VideoParameters *p_Vid, NALU_t *nalu, ANNEXB_t *annex_b)
{
  int i;
  int info2 = 0, info3 = 0, pos = 0;
  int StartCodeFound = 0;
  int LeadingZero8BitsCount = 0;
  byte *pBuf = annex_b->Buf;

  if (annex_b->nextstartcodebytes != 0)
  {
    for (i=0; i<annex_b->nextstartcodebytes-1; i++)
    {
      (*pBuf++) = 0;
      pos++;
    }
    (*pBuf++) = 1;
    pos++;
  }
  else
  {
    while(!annex_b->is_eof)
    {
      pos++;
      if ((*(pBuf++)= getfbyte(annex_b))!= 0)
        break;
    }
  }
  if(annex_b->is_eof == TRUE)
  {
    if(pos==0)
    {
      return 0;
    }
    else
    {
      printf( "get_annex_b_NALU can't read start code\n");
      return -1;
    }
  }  

  if(*(pBuf - 1) != 1 || pos < 3)
  {
    printf ("get_annex_b_NALU: no Start Code at the beginning of the NALU, return -1\n");
    return -1;
  }

  if (pos == 3)
  {
    nalu->startcodeprefix_len = 3;
  }
  else
  {
    LeadingZero8BitsCount = pos - 4;
    nalu->startcodeprefix_len = 4;
  }

  //the 1st byte stream NAL unit can has leading_zero_8bits, but subsequent ones are not
  //allowed to contain it since these zeros(if any) are considered trailing_zero_8bits
  //of the previous byte stream NAL unit.
  if(!annex_b->IsFirstByteStreamNALU && LeadingZero8BitsCount > 0)
  {
    printf ("get_annex_b_NALU: The leading_zero_8bits syntax can only be present in the first byte stream NAL unit, return -1\n");
    return -1;
  }

  LeadingZero8BitsCount = pos;
  annex_b->IsFirstByteStreamNALU = 0;

  while (!StartCodeFound)
  {
    if (annex_b->is_eof == TRUE)
    {
      pBuf -= 2;
      while(*(pBuf--)==0)
        pos--;

      nalu->len = (pos - 1) - LeadingZero8BitsCount;
      memcpy (nalu->buf, annex_b->Buf + LeadingZero8BitsCount, nalu->len);
      nalu->forbidden_bit     = (*(nalu->buf) >> 7) & 1;
      nalu->nal_reference_idc = (NalRefIdc) ((*(nalu->buf) >> 5) & 3);
      nalu->nal_unit_type     = (NaluType) ((*(nalu->buf)) & 0x1f);
      annex_b->nextstartcodebytes = 0;

      // printf ("get_annex_b_NALU, eof case: pos %d nalu->len %d, nalu->reference_idc %d, nal_unit_type %d \n", pos, nalu->len, nalu->nal_reference_idc, nalu->nal_unit_type);

#if TRACE
      fprintf (p_Dec->p_trace, "\n\nLast NALU in File\n\n");
      fprintf (p_Dec->p_trace, "Annex B NALU w/ %s startcode, len %d, forbidden_bit %d, nal_reference_idc %d, nal_unit_type %d\n\n",
        nalu->startcodeprefix_len == 4?"long":"short", nalu->len, nalu->forbidden_bit, nalu->nal_reference_idc, nalu->nal_unit_type);
      fflush (p_Dec->p_trace);
#endif
      return (pos - 1);
    }

    pos++;
    *(pBuf ++)  = getfbyte(annex_b);    
    info3 = FindStartCode(pBuf - 4, 3);
    if(info3 != 1)
    {
      info2 = FindStartCode(pBuf - 3, 2);
      StartCodeFound = info2 & 0x01;
    }
    else
      StartCodeFound = 1;
  }

  // Here, we have found another start code (and read length of startcode bytes more than we should
  // have.  Hence, go back in the file
  if(info3 == 1)  //if the detected start code is 00 00 01, trailing_zero_8bits is sure not to be present
  {
    pBuf -= 5;
    while(*(pBuf--) == 0)
      pos--;
    annex_b->nextstartcodebytes = 4;
  }
  else if (info2 == 1)
    annex_b->nextstartcodebytes = 3;
  else
  {
    printf(" Panic: Error in next start code search \n");
    return -1;
  }

  pos -= annex_b->nextstartcodebytes;

  // Here the leading zeros(if any), Start code, the complete NALU, trailing zeros(if any)
  // and the next start code is in the Buf.
  // The size of Buf is pos - rewind, pos are the number of bytes excluding the next
  // start code, and (pos) - LeadingZero8BitsCount
  // is the size of the NALU.

  nalu->len = pos - LeadingZero8BitsCount;
  fast_memcpy (nalu->buf, annex_b->Buf + LeadingZero8BitsCount, nalu->len);
  nalu->forbidden_bit     = (*(nalu->buf) >> 7) & 1;
  nalu->nal_reference_idc = (NalRefIdc) ((*(nalu->buf) >> 5) & 3);
  nalu->nal_unit_type     = (NaluType) ((*(nalu->buf)) & 0x1f);
  nalu->lost_packets = 0;

  
  //printf ("get_annex_b_NALU, regular case: pos %d nalu->len %d, nalu->reference_idc %d, nal_unit_type %d \n", pos, nalu->len, nalu->nal_reference_idc, nalu->nal_unit_type);
#if TRACE
  fprintf (p_Dec->p_trace, "\n\nAnnex B NALU w/ %s startcode, len %d, forbidden_bit %d, nal_reference_idc %d, nal_unit_type %d\n\n",
    nalu->startcodeprefix_len == 4?"long":"short", nalu->len, nalu->forbidden_bit, nalu->nal_reference_idc, nalu->nal_unit_type);
  fflush (p_Dec->p_trace);
#endif

  return (pos);

}



/*!
 ************************************************************************
 * \brief
 *    Opens the bit stream file named fn
 * \return
 *    none
 ************************************************************************
 */
void open_annex_b (char *fn, ANNEXB_t *annex_b)
{
  if (NULL != annex_b->iobuffer)
  {
    error ("open_annex_b: tried to open Annex B file twice",500);
  }
  if ((annex_b->BitStreamFile = open(fn, OPENFLAGS_READ)) == -1)
  {
    snprintf (errortext, ET_SIZE, "Cannot open Annex B ByteStream file '%s'", fn);
    error(errortext,500);
  }

  annex_b->iIOBufferSize = IOBUFFERSIZE * sizeof (byte);
  annex_b->iobuffer = malloc (annex_b->iIOBufferSize);
  if (NULL == annex_b->iobuffer)
  {
    error ("open_annex_b: cannot allocate IO buffer",500);
  }
  annex_b->is_eof = FALSE;
  getChunk(annex_b);
}


/*!
 ************************************************************************
 * \brief
 *    Closes the bit stream file
 ************************************************************************
 */
/*!
 ************************************************************************
 * \brief
 *    Opens annex B in ring buffer mode for memory-based feeding.
 *    The ring_size must be a power of 2.
 ************************************************************************
 */
void open_annex_b_ring(int ring_size, ANNEXB_t *annex_b)
{
  if (NULL != annex_b->iobuffer)
    error("open_annex_b_ring: tried to open Annex B twice", 500);

  annex_b->BitStreamFile = -2;  // sentinel for ring buffer mode

  annex_b->ring_buf = (byte *)malloc(ring_size);
  if (NULL == annex_b->ring_buf)
    error("open_annex_b_ring: cannot allocate ring buffer", 500);
  annex_b->ring_size = ring_size;
  annex_b->ring_read = 0;
  annex_b->ring_write = 0;
  annex_b->ring_eof = 0;
  pthread_mutex_init(&annex_b->ring_mutex, NULL);
  pthread_cond_init(&annex_b->ring_cond, NULL);

  annex_b->iIOBufferSize = IOBUFFERSIZE * sizeof(byte);
  annex_b->iobuffer = malloc(annex_b->iIOBufferSize);
  if (NULL == annex_b->iobuffer)
    error("open_annex_b_ring: cannot allocate IO buffer", 500);

  annex_b->is_eof = FALSE;
  // Don't call getChunk here - ring is empty, decoder thread will pull when ready
}

/*!
 ************************************************************************
 * \brief
 *    Feed data into the ring buffer (called from producer/FFmpeg thread).
 *    Blocks if ring buffer is full. Returns bytes written.
 ************************************************************************
 */
int annex_b_ring_feed(ANNEXB_t *annex_b, const byte *data, int size)
{
  int written = 0;
  int mask = annex_b->ring_size - 1;

  while (written < size)
  {
    pthread_mutex_lock(&annex_b->ring_mutex);

    // Wait until space is available
    int64_t space = annex_b->ring_size - (annex_b->ring_write - annex_b->ring_read);
    while (space <= 0)
    {
      pthread_cond_wait(&annex_b->ring_cond, &annex_b->ring_mutex);
      space = annex_b->ring_size - (annex_b->ring_write - annex_b->ring_read);
    }

    int to_write = size - written;
    if (to_write > (int)space) to_write = (int)space;

    // Copy to ring buffer (may wrap around)
    int wpos = (int)(annex_b->ring_write & mask);
    int first = annex_b->ring_size - wpos;
    if (to_write <= first)
    {
      memcpy(annex_b->ring_buf + wpos, data + written, to_write);
    }
    else
    {
      memcpy(annex_b->ring_buf + wpos, data + written, first);
      memcpy(annex_b->ring_buf, data + written + first, to_write - first);
    }
    annex_b->ring_write += to_write;
    written += to_write;

    // Signal consumer that data is available
    pthread_cond_signal(&annex_b->ring_cond);
    pthread_mutex_unlock(&annex_b->ring_mutex);
  }

  return written;
}

/*!
 ************************************************************************
 * \brief
 *    Non-blocking feed: write as much as fits into the ring buffer
 *    without waiting. Returns number of bytes actually written.
 ************************************************************************
 */
int annex_b_ring_try_feed(ANNEXB_t *annex_b, const byte *data, int size)
{
  int mask = annex_b->ring_size - 1;

  pthread_mutex_lock(&annex_b->ring_mutex);

  int64_t space = annex_b->ring_size - (annex_b->ring_write - annex_b->ring_read);
  if (space <= 0)
  {
    pthread_mutex_unlock(&annex_b->ring_mutex);
    return 0;
  }

  int to_write = size;
  if (to_write > (int)space) to_write = (int)space;

  int wpos = (int)(annex_b->ring_write & mask);
  int first = annex_b->ring_size - wpos;
  if (to_write <= first)
  {
    memcpy(annex_b->ring_buf + wpos, data, to_write);
  }
  else
  {
    memcpy(annex_b->ring_buf + wpos, data, first);
    memcpy(annex_b->ring_buf, data + first, to_write - first);
  }
  annex_b->ring_write += to_write;

  pthread_cond_signal(&annex_b->ring_cond);
  pthread_mutex_unlock(&annex_b->ring_mutex);

  return to_write;
}

/*!
 ************************************************************************
 * \brief
 *    Signal EOF on the ring buffer (no more data will be fed).
 ************************************************************************
 */
void annex_b_ring_signal_eof(ANNEXB_t *annex_b)
{
  pthread_mutex_lock(&annex_b->ring_mutex);
  annex_b->ring_eof = 1;
  pthread_cond_signal(&annex_b->ring_cond);
  pthread_mutex_unlock(&annex_b->ring_mutex);
}

void close_annex_b(ANNEXB_t *annex_b)
{
  if (annex_b->BitStreamFile == -2)
  {
    // Ring buffer mode
    pthread_mutex_destroy(&annex_b->ring_mutex);
    pthread_cond_destroy(&annex_b->ring_cond);
    free(annex_b->ring_buf);
    annex_b->ring_buf = NULL;
    annex_b->BitStreamFile = -1;
  }
  else if (annex_b->BitStreamFile != -1)
  {
    close(annex_b->BitStreamFile);
    annex_b->BitStreamFile = -1;
  }
  free(annex_b->iobuffer);
  annex_b->iobuffer = NULL;
}


void reset_annex_b(ANNEXB_t *annex_b)
{
  annex_b->is_eof = FALSE;
  annex_b->bytesinbuffer = 0;
  annex_b->iobufferread = annex_b->iobuffer;
}
