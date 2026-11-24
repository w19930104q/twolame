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
 *  Fixed-sample test "TW-B" for bounded chunked output.
 *
 *  Sample: 48000 Hz stereo, 192 kbps, 3456 16-bit samples,
 *  chunk cap 4096 bytes, input fed in frames of 576 samples.
 *
 *  Verifies:
 *   - chunked encoding is byte-for-byte identical to one-shot encoding
 *   - every emitted chunk is at most 4096 bytes
 *   - total byte count and MPEG audio frame count match one-shot
 *   - the trailing empty (end-of-stream) chunk appears exactly once
 *   - an injected encode error commits no partial frame to the queue
 *   - replaying the streaming session twice yields the same hash
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "twolame.h"

/* TW-B fixed sample parameters */
#define TWB_SAMPLE_RATE   48000
#define TWB_CHANNELS      2
#define TWB_BITRATE       192
#define TWB_TOTAL_SAMPLES 3456
#define TWB_FEED_SAMPLES  576
#define TWB_CHUNK_CAP     4096

#define MAX_OUTPUT_BYTES  (TWB_TOTAL_SAMPLES * 4)
#define MAX_CHUNKS        256

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        printf("ok - %s\n", msg); \
    } else { \
        printf("FAIL - %s\n", msg); \
        failures++; \
    } \
} while (0)


/* Deterministic TW-B input signal (16-bit interleaved stereo) */
static void gen_twb_pcm(short int *pcm, int total_samples)
{
    int n;

    for (n = 0; n < total_samples; n++) {
        double t = (double) n / TWB_SAMPLE_RATE;
        pcm[2 * n] = (short int) (sin(2.0 * M_PI * 440.0 * t) * 12000.0);
        pcm[2 * n + 1] = (short int) (sin(2.0 * M_PI * 660.0 * t + 0.5) * 9000.0);
    }
}


/* FNV-1a 64-bit hash, used to compare session replays */
static unsigned long long fnv1a64(const unsigned char *data, int len)
{
    unsigned long long h = 1469598103934665603ULL;
    int i;

    for (i = 0; i < len; i++) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}


static twolame_options *new_twb_options(void)
{
    twolame_options *opts = twolame_init();

    twolame_set_verbosity(opts, 0);
    twolame_set_num_channels(opts, TWB_CHANNELS);
    twolame_set_in_samplerate(opts, TWB_SAMPLE_RATE);
    twolame_set_out_samplerate(opts, TWB_SAMPLE_RATE);
    twolame_set_mode(opts, TWOLAME_STEREO);
    twolame_set_bitrate(opts, TWB_BITRATE);
    if (twolame_init_params(opts) != 0) {
        fprintf(stderr, "twolame_init_params() failed\n");
        twolame_close(&opts);
        exit(1);
    }
    return opts;
}


/* Count MPEG audio frames by walking the sync words */
static int count_mp2_frames(const unsigned char *data, int len, int frame_len)
{
    int frames = 0;
    int pos = 0;

    while (pos + 2 <= len) {
        if (data[pos] != 0xFF || (data[pos + 1] & 0xE0) != 0xE0) {
            fprintf(stderr, "lost MPEG sync at offset %d\n", pos);
            return -1;
        }
        frames++;
        pos += frame_len;
    }
    if (pos != len) {
        fprintf(stderr, "trailing %d bytes after last frame\n", len - pos);
        return -1;
    }
    return frames;
}


/* One-shot reference encode: whole input in a single call */
static int encode_one_shot(const short int *pcm, unsigned char *out)
{
    twolame_options *opts = new_twb_options();
    int total = 0;
    int r;

    r = twolame_encode_buffer_interleaved(opts, pcm, TWB_TOTAL_SAMPLES,
                                          out, MAX_OUTPUT_BYTES);
    if (r < 0) {
        fprintf(stderr, "one-shot encode failed\n");
        exit(1);
    }
    total += r;
    r = twolame_encode_flush(opts, out + total, MAX_OUTPUT_BYTES - total);
    if (r < 0) {
        fprintf(stderr, "one-shot flush failed\n");
        exit(1);
    }
    total += r;
    twolame_close(&opts);
    return total;
}


/*
 *  Streaming session: feed TWB_FEED_SAMPLES samples per call and
 *  drain bounded chunks.  When inject_error is non-zero, an invalid
 *  encode call is injected half-way through the session; the queue
 *  must be left untouched (no half frame committed).
 *
 *  Returns the total number of output bytes.  Chunk statistics are
 *  returned via the pointer arguments.
 */
static int encode_chunked(const short int *pcm, unsigned char *out,
                          int inject_error,
                          int *num_chunks, int *max_chunk, int *empty_chunks)
{
    twolame_options *opts = new_twb_options();
    unsigned char chunk[TWB_CHUNK_CAP];
    int total = 0;
    int fed = 0;
    int r;

    *num_chunks = 0;
    *max_chunk = 0;
    *empty_chunks = 0;

    if (twolame_set_out_chunk_size(opts, TWB_CHUNK_CAP) != 0) {
        fprintf(stderr, "twolame_set_out_chunk_size failed\n");
        exit(1);
    }
    if (twolame_get_out_chunk_size(opts) != TWB_CHUNK_CAP) {
        fprintf(stderr, "twolame_get_out_chunk_size mismatch\n");
        exit(1);
    }

    while (fed < TWB_TOTAL_SAMPLES) {
        r = twolame_encode_chunk_interleaved(opts, pcm + 2 * fed, TWB_FEED_SAMPLES);
        if (r != 0) {
            fprintf(stderr, "chunked encode failed at sample %d\n", fed);
            exit(1);
        }
        fed += TWB_FEED_SAMPLES;

        /* Inject an error half-way through the session */
        if (inject_error && fed == TWB_TOTAL_SAMPLES / 2) {
            int pending_before = twolame_chunk_pending(opts);
            int pending_after;

            r = twolame_encode_chunk_interleaved(opts, pcm, -1);
            CHECK(r < 0, "error injection: invalid encode call is rejected");
            pending_after = twolame_chunk_pending(opts);
            CHECK(pending_before == pending_after,
                  "error injection: no partial frame committed to the queue");
        }

        /* Drain whatever is available so far */
        while ((r = twolame_chunk_read(opts, chunk, sizeof(chunk))) > 0) {
            if (r > TWB_CHUNK_CAP) {
                fprintf(stderr, "chunk of %d bytes exceeds cap\n", r);
                exit(1);
            }
            if (r > *max_chunk)
                *max_chunk = r;
            memcpy(out + total, chunk, r);
            total += r;
            (*num_chunks)++;
        }
    }

    /* Flush twice: the second call must be a no-op */
    if (twolame_flush_chunk(opts) != 0) {
        fprintf(stderr, "twolame_flush_chunk failed\n");
        exit(1);
    }
    if (twolame_flush_chunk(opts) != 0) {
        fprintf(stderr, "second twolame_flush_chunk failed\n");
        exit(1);
    }

    /* Drain until the end-of-stream marker (the empty chunk) */
    for (;;) {
        r = twolame_chunk_read(opts, chunk, sizeof(chunk));
        if (r > 0) {
            if (r > TWB_CHUNK_CAP) {
                fprintf(stderr, "chunk of %d bytes exceeds cap\n", r);
                exit(1);
            }
            if (r > *max_chunk)
                *max_chunk = r;
            memcpy(out + total, chunk, r);
            total += r;
            (*num_chunks)++;
        } else if (r == 0) {
            (*empty_chunks)++;
            break;
        } else {
            fprintf(stderr, "no end-of-stream marker after flush\n");
            exit(1);
        }
    }

    /* The end-of-stream marker must be delivered exactly once */
    r = twolame_chunk_read(opts, chunk, sizeof(chunk));
    CHECK(r < 0, "no further chunk after the end-of-stream marker");

    twolame_close(&opts);
    return total;
}


int main(void)
{
    short int *pcm;
    unsigned char *one_shot, *streamed, *replayed;
    int one_shot_len, streamed_len, replayed_len;
    int num_chunks, max_chunk, empty_chunks;
    int frame_len, one_shot_frames, streamed_frames;
    unsigned long long hash_one_shot, hash_streamed, hash_replayed;

    pcm = malloc(sizeof(short int) * TWB_CHANNELS * TWB_TOTAL_SAMPLES);
    one_shot = malloc(MAX_OUTPUT_BYTES);
    streamed = malloc(MAX_OUTPUT_BYTES);
    replayed = malloc(MAX_OUTPUT_BYTES);
    if (!pcm || !one_shot || !streamed || !replayed) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    gen_twb_pcm(pcm, TWB_TOTAL_SAMPLES);

    /* Reference one-shot encode */
    one_shot_len = encode_one_shot(pcm, one_shot);
    printf("one-shot: %d bytes\n", one_shot_len);

    /* Chunked streaming encode, with error injection */
    streamed_len = encode_chunked(pcm, streamed, 1,
                                  &num_chunks, &max_chunk, &empty_chunks);
    printf("chunked: %d bytes in %d chunks (max chunk %d bytes)\n",
           streamed_len, num_chunks, max_chunk);

    /* Byte-for-byte identical output */
    CHECK(streamed_len == one_shot_len, "total byte count matches one-shot");
    CHECK(memcmp(one_shot, streamed, one_shot_len) == 0,
          "chunked output is byte-for-byte identical to one-shot");

    /* Bounded chunks */
    CHECK(max_chunk <= TWB_CHUNK_CAP, "every chunk is at most 4096 bytes");

    /* Same number of MPEG audio frames */
    {
        twolame_options *opts = new_twb_options();
        frame_len = twolame_get_framelength(opts);
        twolame_close(&opts);
    }
    one_shot_frames = count_mp2_frames(one_shot, one_shot_len, frame_len);
    streamed_frames = count_mp2_frames(streamed, streamed_len, frame_len);
    CHECK(one_shot_frames > 0 && one_shot_frames == streamed_frames,
          "MPEG audio frame count matches one-shot");
    printf("frames: %d (frame length %d bytes)\n", one_shot_frames, frame_len);

    /* Exactly one trailing empty chunk */
    CHECK(empty_chunks == 1, "trailing empty chunk appears exactly once");

    /* Replay the whole streaming session: the hash must be identical */
    replayed_len = encode_chunked(pcm, replayed, 0,
                                  &num_chunks, &max_chunk, &empty_chunks);
    hash_one_shot = fnv1a64(one_shot, one_shot_len);
    hash_streamed = fnv1a64(streamed, streamed_len);
    hash_replayed = fnv1a64(replayed, replayed_len);
    printf("hash: one-shot=%016llx streamed=%016llx replay=%016llx\n",
           hash_one_shot, hash_streamed, hash_replayed);
    CHECK(replayed_len == streamed_len && hash_replayed == hash_streamed,
          "replayed session produces the same hash");
    CHECK(hash_streamed == hash_one_shot, "streamed hash matches one-shot hash");

    free(pcm);
    free(one_shot);
    free(streamed);
    free(replayed);

    if (failures) {
        printf("FAILED: %d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: all TW-B chunked output checks passed\n");
    return 0;
}
