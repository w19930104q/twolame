/*
 *  TwoLAME: an optimized MPEG Audio Layer Two encoder
 *
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
 *  Fixed sample TW-D: persistent frame/sample ledger.
 *
 *  Encodes 50000 mono samples at 44100 Hz, flushes, and checks the
 *  ledger: 44 frames of 1152 samples, 50000 input samples and
 *  44*1152-50000 = 688 tail padding samples.  The ledger is then
 *  saved, the session closed and reopened, and the same accounting
 *  must be reported.  Encoding after a flush must be rejected and
 *  must not change the committed frame count.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <twolame.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SAMPLE_RATE     44100
#define TOTAL_SAMPLES   50000
#define CHUNK_SAMPLES   4096
#define MP2_BUFFER_SIZE 8192

#define EXPECT_FRAMES   44
#define EXPECT_SAMPLES  50000
#define EXPECT_PADDING  (EXPECT_FRAMES * TWOLAME_SAMPLES_PER_FRAME - TOTAL_SAMPLES)

#define LEDGER_FILE     "ledger_twd.ledger"

static int failures = 0;

static void check_ulong(const char *what, unsigned long got, unsigned long want)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got %lu, expected %lu\n", what, got, want);
        failures++;
    } else {
        printf("ok: %s = %lu\n", what, got);
    }
}

static void check_int(const char *what, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got %d, expected %d\n", what, got, want);
        failures++;
    } else {
        printf("ok: %s = %d\n", what, got);
    }
}

int main(void)
{
    twolame_options *opts;
    short *pcm;
    unsigned char mp2buffer[MP2_BUFFER_SIZE];
    int fed;
    int r;

    pcm = (short *) malloc(sizeof(short) * TOTAL_SAMPLES);
    if (pcm == NULL) {
        fprintf(stderr, "FAIL: out of memory\n");
        return 1;
    }

    /* Synthetic mono input: 50000 samples of a 1 kHz sine at 44100 Hz */
    for (int i = 0; i < TOTAL_SAMPLES; i++)
        pcm[i] = (short) (12000.0 * sin(2.0 * M_PI * 1000.0 * i / SAMPLE_RATE));

    /* --- Encoding session --- */
    opts = twolame_init();
    if (opts == NULL) {
        fprintf(stderr, "FAIL: twolame_init() failed\n");
        return 1;
    }
    twolame_set_num_channels(opts, 1);
    twolame_set_in_samplerate(opts, SAMPLE_RATE);
    twolame_set_verbosity(opts, 0);
    if (twolame_init_params(opts) != 0) {
        fprintf(stderr, "FAIL: twolame_init_params() failed\n");
        return 1;
    }

    /* Feed all 50000 samples in chunks */
    for (fed = 0; fed < TOTAL_SAMPLES; fed += CHUNK_SAMPLES) {
        int chunk = TOTAL_SAMPLES - fed;
        if (chunk > CHUNK_SAMPLES)
            chunk = CHUNK_SAMPLES;
        r = twolame_encode_buffer(opts, &pcm[fed], NULL, chunk, mp2buffer, MP2_BUFFER_SIZE);
        if (r < 0) {
            fprintf(stderr, "FAIL: twolame_encode_buffer() returned %d\n", r);
            return 1;
        }
    }

    /* Flush the tail frame */
    r = twolame_encode_flush(opts, mp2buffer, MP2_BUFFER_SIZE);
    if (r <= 0) {
        fprintf(stderr, "FAIL: twolame_encode_flush() returned %d\n", r);
        return 1;
    }

    /* Ledger after encoding: 44 frames, 50000 samples, 688 padding */
    check_ulong("frames after flush", twolame_get_frame_count(opts), EXPECT_FRAMES);
    check_ulong("samples after flush", twolame_get_sample_count(opts), EXPECT_SAMPLES);
    check_ulong("padding after flush", twolame_get_padding_count(opts), EXPECT_PADDING);

    /* A second flush must not count the padding (or anything) again */
    r = twolame_encode_flush(opts, mp2buffer, MP2_BUFFER_SIZE);
    check_int("second flush returns 0", r, 0);
    check_ulong("frames after second flush", twolame_get_frame_count(opts), EXPECT_FRAMES);
    check_ulong("padding after second flush", twolame_get_padding_count(opts), EXPECT_PADDING);

    /* Encoding more samples after a flush must be rejected
       and must not change the committed frame count */
    r = twolame_encode_buffer(opts, pcm, NULL, CHUNK_SAMPLES, mp2buffer, MP2_BUFFER_SIZE);
    if (r >= 0) {
        fprintf(stderr, "FAIL: encoding after flush was not rejected (returned %d)\n", r);
        failures++;
    } else {
        printf("ok: encoding after flush rejected (%d)\n", r);
    }
    check_ulong("frames after rejected encode", twolame_get_frame_count(opts), EXPECT_FRAMES);
    check_ulong("samples after rejected encode", twolame_get_sample_count(opts), EXPECT_SAMPLES);

    /* Persist the ledger and close the session */
    if (twolame_save_ledger(opts, LEDGER_FILE) != 0) {
        fprintf(stderr, "FAIL: twolame_save_ledger() failed\n");
        return 1;
    }
    twolame_close(&opts);

    /* --- Reopen the session and query the ledger --- */
    opts = twolame_init();
    if (opts == NULL) {
        fprintf(stderr, "FAIL: twolame_init() failed on reopen\n");
        return 1;
    }
    if (twolame_load_ledger(opts, LEDGER_FILE) != 0) {
        fprintf(stderr, "FAIL: twolame_load_ledger() failed\n");
        return 1;
    }

    /* Closing and reopening must not increase the frame count */
    check_ulong("frames after reopen", twolame_get_frame_count(opts), EXPECT_FRAMES);
    check_ulong("samples after reopen", twolame_get_sample_count(opts), EXPECT_SAMPLES);
    check_ulong("padding after reopen", twolame_get_padding_count(opts), EXPECT_PADDING);

    /* Loading the ledger again must be idempotent */
    if (twolame_load_ledger(opts, LEDGER_FILE) != 0) {
        fprintf(stderr, "FAIL: second twolame_load_ledger() failed\n");
        return 1;
    }
    check_ulong("frames after second load", twolame_get_frame_count(opts), EXPECT_FRAMES);
    check_ulong("samples after second load", twolame_get_sample_count(opts), EXPECT_SAMPLES);
    check_ulong("padding after second load", twolame_get_padding_count(opts), EXPECT_PADDING);

    twolame_close(&opts);
    free(pcm);
    remove(LEDGER_FILE);

    if (failures) {
        fprintf(stderr, "ledger_twd: %d check(s) failed\n", failures);
        return 1;
    }
    printf("ledger_twd: all checks passed\n");
    return 0;
}
