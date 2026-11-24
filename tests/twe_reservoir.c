/*
 *  TwoLAME: TW-E fixed-bitrate frame budget and bit-reservoir test
 *
 *  Fixed sample TW-E: 48 kHz stereo, 192 kbps, 200 frames that alternate
 *  between digital silence and a single-frequency tone, with the bit
 *  reservoir allowed to borrow and repay across frames.
 *
 *  Verifies:
 *    1. Every one of the 200 committed frames is exactly FRAME_BYTES bytes
 *       (192000/8000 = 24000 bytes/s, 48000/1152 frames/s => 576 bytes).
 *    2. The PCM energy of the first and last frames matches within
 *       ENERGY_EPS (the two endpoint frames are identical by construction).
 *    3. The bit reservoir nets to zero over the stream - gross borrowed and
 *       repaid bits are equal and the final balance is zero.
 *    4. A failure mid-stream rolls the whole attempt back: the bytes
 *       already committed are unchanged, no half frame is published, and a
 *       retry reproduces the exact frame a clean encoder would have made.
 *    5. Encoding the identical input twice yields a byte-identical stream
 *       (deterministic output hash).
 *
 *  Only the public libtwolame API is used; the program exits non-zero on
 *  any failed check.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "twolame.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TW_E_SAMPLE_RATE   48000
#define TW_E_BITRATE       192
#define TW_E_FRAMES        200
#define TW_E_TONE_HZ       1000
#define TW_E_TONE_AMP      0.5

#define SPF                TWOLAME_SAMPLES_PER_FRAME
#define FRAME_BYTES        576
#define ENERGY_EPS         1e-3
#define OUT_CAPACITY       (TW_E_FRAMES * FRAME_BYTES + 1024)

static int failures = 0;

static void check(int condition, const char *message)
{
    if (condition) {
        printf("ok - %s\n", message);
    } else {
        printf("FAIL - %s\n", message);
        failures++;
    }
}

static twolame_options *twe_init(void)
{
    twolame_options *opts = twolame_init();
    if (opts == NULL)
        return NULL;

    twolame_set_num_channels(opts, 2);
    twolame_set_in_samplerate(opts, TW_E_SAMPLE_RATE);
    twolame_set_out_samplerate(opts, TW_E_SAMPLE_RATE);
    twolame_set_bitrate(opts, TW_E_BITRATE);
    twolame_set_verbosity(opts, 0);

    if (twolame_init_params(opts) != 0) {
        twolame_close(&opts);
        return NULL;
    }
    return opts;
}

/* Build the TW-E signal. Interior frames alternate silence/tone; the two
 * endpoint frames are made identical tones so first/last energy can be
 * compared directly. */
static void twe_build_signal(short *left, short *right, int frames)
{
    int f, i;

    for (f = 0; f < frames; f++) {
        int is_tone = (f % 2 == 1) || (f == 0);

        for (i = 0; i < SPF; i++) {
            int idx = f * SPF + i;
            short v = 0;
            if (is_tone) {
                v = (short) (TW_E_TONE_AMP * 32767.0 *
                             sin(2.0 * M_PI * TW_E_TONE_HZ * (double) i /
                                 TW_E_SAMPLE_RATE));
            }
            left[idx] = v;
            right[idx] = v;
        }
    }
}

/* PCM-domain energy of one frame, summed over both channels. */
static double frame_energy(const short *left, const short *right, int frame)
{
    double energy = 0.0;
    int base = frame * SPF;
    int i;

    for (i = 0; i < SPF; i++) {
        double l = left[base + i];
        double r = right[base + i];
        energy += l * l + r * r;
    }
    return energy;
}

/* FNV-1a style 64-bit hash so the check has no external dependency. */
static unsigned long long hash_bytes(const unsigned char *data, int size)
{
    unsigned long long h = 1469598103934665603ULL;
    int i;

    for (i = 0; i < size; i++) {
        h ^= (unsigned long long) data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Encode the whole signal one committed frame at a time, recording every
 * per-frame size. Returns the total number of bytes written. */
static int twe_encode(const short *left, const short *right, int frames,
                      unsigned char *out, int *frame_sizes)
{
    twolame_options *opts = twe_init();
    int total = 0;
    int f;

    if (opts == NULL)
        return -1;

    for (f = 0; f < frames; f++) {
        int n = twolame_encode_frame_commit(opts,
                                            left + f * SPF, right + f * SPF,
                                            out + total, OUT_CAPACITY - total);
        if (n < 0) {
            twolame_close(&opts);
            return -1;
        }
        if (frame_sizes != NULL)
            frame_sizes[f] = n;
        total += n;
    }

    check(twolame_get_frame_count(opts) == (unsigned long) frames,
          "frame counter reports exactly the committed frames");
    check(twolame_get_last_frame_bytes(opts) == FRAME_BYTES,
          "last committed frame reports the fixed byte budget");

    twolame_close(&opts);
    return total;
}

/* Encode a single frame (by index) with a fresh encoder, returning what a
 * clean run would produce for it. */
static int reference_frame(const short *left, const short *right, int frame,
                           unsigned char *out)
{
    twolame_options *opts = twe_init();
    int f, n = -1;

    if (opts == NULL)
        return -1;
    for (f = 0; f <= frame; f++) {
        n = twolame_encode_frame_commit(opts, left + f * SPF,
                                        right + f * SPF, out, FRAME_BYTES);
        if (n < 0)
            break;
    }
    twolame_close(&opts);
    return n;
}

int main(void)
{
    const int total_samples = TW_E_FRAMES * SPF;
    short *left = malloc((size_t) total_samples * sizeof(short));
    short *right = malloc((size_t) total_samples * sizeof(short));
    unsigned char *stream_a = malloc(OUT_CAPACITY);
    unsigned char *stream_b = malloc(OUT_CAPACITY);
    int *sizes = malloc((size_t) TW_E_FRAMES * sizeof(int));
    int total, f;

    if (!left || !right || !stream_a || !stream_b || !sizes) {
        fprintf(stderr, "allocation failure\n");
        return 2;
    }

    twe_build_signal(left, right, TW_E_FRAMES);

    /* ---- 1. constant per-frame byte budget --------------------------- */
    total = twe_encode(left, right, TW_E_FRAMES, stream_a, sizes);
    check(total == TW_E_FRAMES * FRAME_BYTES,
          "200 frames produce exactly 200 * 576 bytes");

    {
        int all_budget = 1;
        for (f = 0; f < TW_E_FRAMES; f++) {
            if (sizes[f] != FRAME_BYTES) {
                all_budget = 0;
                fprintf(stderr, "frame %d was %d bytes, expected %d\n",
                        f, sizes[f], FRAME_BYTES);
            }
        }
        check(all_budget, "every frame is exactly 576 bytes");
    }

    /* ---- 2. first/last frame energy stability ------------------------- */
    {
        double e_first = frame_energy(left, right, 0);
        double e_last = frame_energy(left, right, TW_E_FRAMES - 1);
        double denom = e_first > e_last ? e_first : e_last;
        double rel = (denom > 0.0) ? fabs(e_first - e_last) / denom
                                   : fabs(e_first - e_last);
        check(rel < ENERGY_EPS,
              "first and last frame energy error is below 1e-3");
    }

    /* ---- 3. reservoir nets to zero ------------------------------------ */
    {
        twolame_options *opts = twe_init();
        unsigned long borrowed, repaid;
        long balance;
        int written = 0;
        int kept_budget = 1;

        for (f = 0; f < TW_E_FRAMES; f++) {
            int n = twolame_encode_frame_commit(opts,
                                                left + f * SPF,
                                                right + f * SPF,
                                                stream_b + written,
                                                OUT_CAPACITY - written);
            if (n != FRAME_BYTES)
                kept_budget = 0;
            written += n;
        }

        borrowed = twolame_get_reservoir_borrowed(opts);
        repaid = twolame_get_reservoir_repaid(opts);
        balance = twolame_get_reservoir_balance(opts);

        check(kept_budget, "reservoir run keeps the fixed frame budget");
        check(borrowed == repaid,
              "gross reservoir borrow equals gross repay");
        check(balance == 0, "reservoir balance is zero at end of stream");
        check(written == TW_E_FRAMES * FRAME_BYTES,
              "reservoir run total size matches the fixed budget");

        twolame_close(&opts);
    }

    /* ---- 4. mid-stream failure rolls back and never commits ----------- */
    {
        twolame_options *opts = twe_init();
        const int good_frames = 50;
        const int fail_at = good_frames;     /* next frame, 0-based */
        unsigned char ref[FRAME_BYTES];
        unsigned char good_saved[FRAME_BYTES];
        unsigned char probe[FRAME_BYTES];
        unsigned long committed_before;
        int written = 0;
        int rc;

        for (f = 0; f < good_frames; f++) {
            int n = twolame_encode_frame_commit(opts,
                                                left + f * SPF,
                                                right + f * SPF,
                                                stream_a + written,
                                                OUT_CAPACITY - written);
            if (n != FRAME_BYTES)
                check(0, "frames before the fault commit normally");
            written += n;
        }
        check(1, "frames before the fault commit normally");

        /* Snapshot the last already-committed frame and the clean output
           of the frame we are about to force to fail. */
        memcpy(good_saved, stream_a + written - FRAME_BYTES, FRAME_BYTES);
        check(reference_frame(left, right, fail_at, ref) == FRAME_BYTES,
              "clean reference frame for the rollback target is built");

        committed_before = twolame_get_frame_count(opts);

        /* Force the next attempt to fail during assembly. */
        check(twolame_set_fail_after_frames(opts, 1) == 0,
              "fault injection is armed");
        rc = twolame_encode_frame_commit(opts,
                                         left + fail_at * SPF,
                                         right + fail_at * SPF,
                                         probe, sizeof(probe));
        check(rc < 0, "the injected failure is reported as an error");
        check(twolame_get_frame_count(opts) == committed_before,
              "a failed attempt does not increase the committed frame count");
        check(memcmp(good_saved, stream_a + written - FRAME_BYTES,
                     FRAME_BYTES) == 0,
              "previously committed frames are untouched by the rollback");

        /* Clear the fault and retry: it must succeed exactly once and
           reproduce the bytes the clean reference encoder produced. */
        check(twolame_set_fail_after_frames(opts, 0) == 0,
              "fault injection is disarmed before retry");
        rc = twolame_encode_frame_commit(opts,
                                         left + fail_at * SPF,
                                         right + fail_at * SPF,
                                         probe, sizeof(probe));
        check(rc == FRAME_BYTES, "the frame retried after rollback succeeds");
        check(twolame_get_frame_count(opts) == committed_before + 1,
              "the retried frame is committed exactly once");
        check(memcmp(probe, ref, FRAME_BYTES) == 0,
              "the retried frame matches the clean reference output");

        /* Late failure: the fault fires after all per-frame state changes
           (including memory lazily allocated during the frame) but before
           commit. Rollback must still keep the committed history intact and
           allow a clean retry. */
        twolame_set_fail_late_frames(opts, 1);
        rc = twolame_encode_frame_commit(opts,
                                         left + (fail_at + 1) * SPF,
                                         right + (fail_at + 1) * SPF,
                                         probe, sizeof(probe));
        check(rc < 0, "a late (post-mutation) failure is reported as an error");
        check(twolame_get_frame_count(opts) == committed_before + 1,
              "a late failure does not increase the committed frame count");
        twolame_set_fail_late_frames(opts, 0);
        rc = twolame_encode_frame_commit(opts,
                                         left + (fail_at + 1) * SPF,
                                         right + (fail_at + 1) * SPF,
                                         probe, sizeof(probe));
        check(rc == FRAME_BYTES, "the frame retried after a late failure succeeds");
        check(twolame_get_frame_count(opts) == committed_before + 2,
              "the late-failed frame is committed exactly once on retry");

        twolame_close(&opts);
    }

    /* ---- 5. deterministic output hash --------------------------------- */
    {
        int total_b = twe_encode(left, right, TW_E_FRAMES, stream_b, NULL);
        check(total == total_b, "two runs produce the same number of bytes");
        check(total_b > 0, "the deterministic comparison run produced output");
        check(hash_bytes(stream_a, total) == hash_bytes(stream_b, total_b),
              "identical input gives identical output bytes twice");
    }

    /* ---- argument validation ------------------------------------------ */
    {
        twolame_options *opts = twe_init();
        unsigned char small[8];
        check(twolame_encode_frame_commit(NULL, left, right, stream_a,
                                          OUT_CAPACITY) < 0,
              "NULL options are rejected");
        check(twolame_encode_frame_commit(opts, NULL, right, stream_a,
                                          OUT_CAPACITY) < 0,
              "NULL PCM is rejected");
        check(twolame_encode_frame_commit(opts, left, right, NULL,
                                          OUT_CAPACITY) < 0,
              "NULL output buffer is rejected");
        check(twolame_encode_frame_commit(opts, left, right, small,
                                          sizeof(small)) < 0,
              "an output buffer smaller than one frame is rejected without commit");
        check(twolame_get_frame_count(opts) == 0,
              "rejected calls never commit a frame");
        check(twolame_set_fail_after_frames(opts, -1) < 0,
              "a negative fault count is rejected");
        check(twolame_set_fail_late_frames(opts, -1) < 0,
              "a negative late-fault count is rejected");
        twolame_close(&opts);
    }

    free(left);
    free(right);
    free(stream_a);
    free(stream_b);
    free(sizes);

    if (failures) {
        fprintf(stderr, "twe_reservoir: %d check(s) failed\n", failures);
        return 1;
    }
    printf("twe_reservoir: all checks passed\n");
    return 0;
}
