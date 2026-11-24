/*
 *  TwoLAME: an optimized MPEG Audio Layer Two encoder
 *
 *  Copyright (C) 2001-2004 Michael Cheng
 *  Copyright (C) 2004-2018 The TwoLAME Project
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/*
 *  Bounded chunked output.
 *
 *  Consumers that can only accept a small piece of MP2 data at a
 *  time (e.g. a browser exporter) enable chunked output with
 *  twolame_set_out_chunk_size(), feed PCM with
 *  twolame_encode_chunk() / twolame_encode_chunk_interleaved(),
 *  finish with twolame_flush_chunk() and drain the result with
 *  twolame_chunk_read().  Each chunk read back is bounded by the
 *  configured chunk size, and the concatenation of all chunks is
 *  byte-for-byte identical to a one-shot encode of the same input
 *  with the same settings.
 *
 *  Encoded audio is committed to the output queue only after a
 *  complete, successful encode pass, so an error never leaves a
 *  half-written MPEG audio frame in the queue.  After the flush,
 *  a single empty (zero-length) chunk is delivered exactly once
 *  as the end-of-stream marker.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "twolame.h"
#include "common.h"
#include "mem.h"
#include "util.h"


/* Make sure the output queue can hold 'extra' more bytes.
   Returns 0 on success, -1 on allocation failure (queue unchanged). */
static int chunk_queue_reserve(twolame_options * glopts, int extra)
{
    int newsize;
    unsigned char *newbuf;

    if (extra <= 0)
        return 0;
    if (glopts->chunk_len + extra <= glopts->chunk_buf_size)
        return 0;

    newsize = (glopts->chunk_buf_size > 0) ? glopts->chunk_buf_size : 4096;
    while (newsize < glopts->chunk_len + extra)
        newsize *= 2;

    newbuf = (unsigned char *) TWOLAME_MALLOC(newsize);
    if (newbuf == NULL)
        return -1;
    if (glopts->chunk_len > 0)
        memcpy(newbuf, glopts->chunk_buf, glopts->chunk_len);
    TWOLAME_FREE(glopts->chunk_buf);

    glopts->chunk_buf = newbuf;
    glopts->chunk_buf_size = newsize;
    return 0;
}


/* Append a fully encoded frame sequence to the output queue.
   Returns 0 on success, -1 on failure (queue unchanged). */
static int chunk_queue_append(twolame_options * glopts, const unsigned char *data, int len)
{
    if (len <= 0)
        return 0;
    if (chunk_queue_reserve(glopts, len) < 0)
        return -1;
    memcpy(glopts->chunk_buf + glopts->chunk_len, data, len);
    glopts->chunk_len += len;
    return 0;
}


/* Size of a temporary buffer able to hold the encoded output
   for 'num_samples' input samples. */
static int chunk_tmp_size(twolame_options * glopts, int num_samples)
{
    int frame_len = twolame_get_framelength(glopts);
    int max_frames = num_samples / TWOLAME_SAMPLES_PER_FRAME + 2;

    if (frame_len <= 0)
        frame_len = 4096;
    /* one extra frame of slack for padding/ancillary bytes */
    return max_frames * (frame_len + 8);
}


/* Common encode-and-queue path.  The audio is encoded into a
   temporary buffer first; the queue is only updated when the
   encode succeeds, so a failure can never commit a partial
   MPEG audio frame. */
static int encode_chunk_common(twolame_options * glopts,
                               const short int leftpcm[],
                               const short int rightpcm[],
                               const short int pcm[], int num_samples)
{
    unsigned char *tmp;
    int tmp_size;
    int bytes;

    if (glopts == NULL) {
        return -1;
    }
    if (glopts->chunk_size <= 0) {
        fprintf(stderr,
                "twolame_encode_chunk: call twolame_set_out_chunk_size() with a non-zero size first.\n");
        return -1;
    }
    if (glopts->chunk_flushed) {
        fprintf(stderr,
                "twolame_encode_chunk: cannot encode after twolame_flush_chunk().\n");
        return -1;
    }
    if (num_samples < 0) {
        fprintf(stderr, "twolame_encode_chunk: invalid number of samples: %i\n", num_samples);
        return -1;
    }
    if (num_samples == 0)
        return 0;

    tmp_size = chunk_tmp_size(glopts, num_samples);
    tmp = (unsigned char *) TWOLAME_MALLOC(tmp_size);
    if (tmp == NULL)
        return -1;

    if (pcm != NULL)
        bytes = twolame_encode_buffer_interleaved(glopts, pcm, num_samples, tmp, tmp_size);
    else
        bytes = twolame_encode_buffer(glopts, leftpcm, rightpcm, num_samples, tmp, tmp_size);

    if (bytes < 0) {
        /* Encode failed: the output queue is left untouched */
        TWOLAME_FREE(tmp);
        return bytes;
    }

    if (chunk_queue_append(glopts, tmp, bytes) < 0) {
        TWOLAME_FREE(tmp);
        return -1;
    }

    TWOLAME_FREE(tmp);
    return 0;
}


int twolame_encode_chunk(twolame_options * glopts,
                         const short int leftpcm[],
                         const short int rightpcm[], int num_samples)
{
    if (leftpcm == NULL || (glopts != NULL && glopts->num_channels_in == 2 && rightpcm == NULL)) {
        fprintf(stderr, "twolame_encode_chunk: NULL sample buffer.\n");
        return -1;
    }
    return encode_chunk_common(glopts, leftpcm, rightpcm, NULL, num_samples);
}


int twolame_encode_chunk_interleaved(twolame_options * glopts,
                                     const short int pcm[], int num_samples)
{
    if (pcm == NULL && num_samples > 0) {
        fprintf(stderr, "twolame_encode_chunk_interleaved: NULL sample buffer.\n");
        return -1;
    }
    return encode_chunk_common(glopts, NULL, NULL, pcm, num_samples);
}


int twolame_flush_chunk(twolame_options * glopts)
{
    unsigned char *tmp;
    int tmp_size;
    int bytes;

    if (glopts == NULL) {
        return -1;
    }
    if (glopts->chunk_size <= 0) {
        fprintf(stderr,
                "twolame_flush_chunk: call twolame_set_out_chunk_size() with a non-zero size first.\n");
        return -1;
    }
    /* Idempotent: a second flush adds nothing, so the trailing
       empty end-of-stream chunk is produced exactly once. */
    if (glopts->chunk_flushed)
        return 0;

    tmp_size = chunk_tmp_size(glopts, TWOLAME_SAMPLES_PER_FRAME);
    tmp = (unsigned char *) TWOLAME_MALLOC(tmp_size);
    if (tmp == NULL)
        return -1;

    bytes = twolame_encode_flush(glopts, tmp, tmp_size);
    if (bytes < 0) {
        /* Flush failed: the output queue is left untouched */
        TWOLAME_FREE(tmp);
        return bytes;
    }

    if (chunk_queue_append(glopts, tmp, bytes) < 0) {
        TWOLAME_FREE(tmp);
        return -1;
    }

    TWOLAME_FREE(tmp);
    glopts->chunk_flushed = 1;
    return 0;
}


int twolame_chunk_pending(twolame_options * glopts)
{
    if (glopts == NULL)
        return -1;
    return glopts->chunk_len;
}


int twolame_chunk_read(twolame_options * glopts, unsigned char *chunk, int chunk_buffer_size)
{
    int n;

    if (glopts == NULL || chunk == NULL || chunk_buffer_size <= 0)
        return -1;
    if (glopts->chunk_size <= 0) {
        fprintf(stderr,
                "twolame_chunk_read: call twolame_set_out_chunk_size() with a non-zero size first.\n");
        return -1;
    }
    /* The end-of-stream marker has already been delivered */
    if (glopts->chunk_eof_sent)
        return -1;

    if (glopts->chunk_len > 0) {
        n = glopts->chunk_len;
        if (n > glopts->chunk_size)
            n = glopts->chunk_size;
        if (n > chunk_buffer_size)
            n = chunk_buffer_size;
        memcpy(chunk, glopts->chunk_buf, n);
        glopts->chunk_len -= n;
        if (glopts->chunk_len > 0)
            memmove(glopts->chunk_buf, glopts->chunk_buf + n, glopts->chunk_len);
        return n;
    }

    if (glopts->chunk_flushed) {
        /* Deliver the single trailing empty chunk */
        glopts->chunk_eof_sent = 1;
        return 0;
    }

    /* No data available yet */
    return -1;
}

// vim:ts=4:sw=4:nowrap:
