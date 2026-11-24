/*
 *  TwoLAME: TW-C entry point equivalence regression test
 *
 *  Fixed sample TW-C: stereo, 48 kHz, 192 kbps.
 *    Left channel values:  0, 1024, -2048
 *    Right channel values: -512, 2048, -4096
 *
 *  Verifies that the four public encode entry points
 *  (integer planar, integer interleaved, float planar, float interleaved)
 *  produce the same frame sequence for identical audio, that the channel
 *  order stays left-then-right, and that a sub-frame tail is padded out
 *  exactly once by twolame_encode_flush().
 *
 *  The test only uses the public libtwolame API and exits non-zero on failure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "twolame.h"

/* MPEG-1 Layer II, 48 kHz / 192 kbps: 1152 samples per channel per frame,
 * 192000/48000*1152/8 = 576 bytes per frame. */
#define TW_C_SAMPLE_RATE 48000
#define TW_C_BITRATE 192
#define FRAME_BYTES 576
#define SAMPLES_PER_FRAME TWOLAME_SAMPLES_PER_FRAME
#define FIRST_FRAMES 3

/* The fixed TW-C sample values. */
static const short tw_c_left[3] = { 0, 1024, -2048 };
static const short tw_c_right[3] = { -512, 2048, -4096 };

#define OUT_CAPACITY (256 * 1024)

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

static twolame_options *tw_c_init(void)
{
    twolame_options *opts = twolame_init();
    if (opts == NULL)
        return NULL;

    twolame_set_num_channels(opts, 2);
    twolame_set_in_samplerate(opts, TW_C_SAMPLE_RATE);
    twolame_set_out_samplerate(opts, TW_C_SAMPLE_RATE);
    twolame_set_bitrate(opts, TW_C_BITRATE);
    twolame_set_verbosity(opts, 0);

    if (twolame_init_params(opts) != 0) {
        twolame_close(&opts);
        return NULL;
    }
    return opts;
}

struct stream {
    unsigned char data[OUT_CAPACITY];
    int size;
};

/* Feed a whole per-channel sample array through one entry point, using the
 * given chunk size (in samples per channel) to exercise buffering state,
 * then flush. */
static void encode_planar_short(const short *left, const short *right,
                                int total, int chunk, struct stream *out)
{
    twolame_options *opts = tw_c_init();
    int off = 0;
    out->size = 0;

    while (off < total) {
        int n = total - off;
        if (n > chunk)
            n = chunk;
        out->size += twolame_encode_buffer(opts, left + off, right + off, n,
                                           out->data + out->size,
                                           OUT_CAPACITY - out->size);
        off += n;
    }
    out->size += twolame_encode_flush(opts, out->data + out->size,
                                      OUT_CAPACITY - out->size);
    twolame_close(&opts);
}

static void encode_interleaved_short(const short *interleaved, int total,
                                     int chunk, struct stream *out)
{
    twolame_options *opts = tw_c_init();
    int off = 0;
    out->size = 0;

    while (off < total) {
        int n = total - off;
        if (n > chunk)
            n = chunk;
        out->size += twolame_encode_buffer_interleaved(opts,
                                                       interleaved + 2 * off, n,
                                                       out->data + out->size,
                                                       OUT_CAPACITY - out->size);
        off += n;
    }
    out->size += twolame_encode_flush(opts, out->data + out->size,
                                      OUT_CAPACITY - out->size);
    twolame_close(&opts);
}

static void encode_planar_float(const float *left, const float *right,
                                int total, int chunk, struct stream *out)
{
    twolame_options *opts = tw_c_init();
    int off = 0;
    out->size = 0;

    while (off < total) {
        int n = total - off;
        if (n > chunk)
            n = chunk;
        out->size += twolame_encode_buffer_float32(opts, left + off, right + off,
                                                   n, out->data + out->size,
                                                   OUT_CAPACITY - out->size);
        off += n;
    }
    out->size += twolame_encode_flush(opts, out->data + out->size,
                                      OUT_CAPACITY - out->size);
    twolame_close(&opts);
}

static void encode_interleaved_float(const float *interleaved, int total,
                                     int chunk, struct stream *out)
{
    twolame_options *opts = tw_c_init();
    int off = 0;
    out->size = 0;

    while (off < total) {
        int n = total - off;
        if (n > chunk)
            n = chunk;
        out->size += twolame_encode_buffer_float32_interleaved(opts,
                interleaved + 2 * off, n,
                out->data + out->size, OUT_CAPACITY - out->size);
        off += n;
    }
    out->size += twolame_encode_flush(opts, out->data + out->size,
                                      OUT_CAPACITY - out->size);
    twolame_close(&opts);
}

static int streams_equal(const struct stream *a, const struct stream *b)
{
    return a->size == b->size && memcmp(a->data, b->data, a->size) == 0;
}

int main(void)
{
    /* Enough audio for three full frames plus a three-sample tail. The TW-C
     * values are cycled so every frame is built from the fixed sample. */
    const int total = FIRST_FRAMES * SAMPLES_PER_FRAME + 3;
    short *left_s = malloc(total * sizeof(short));
    short *right_s = malloc(total * sizeof(short));
    short *inter_s = malloc(total * 2 * sizeof(short));
    float *left_f = malloc(total * sizeof(float));
    float *right_f = malloc(total * sizeof(float));
    float *inter_f = malloc(total * 2 * sizeof(float));
    struct stream sp, si, fp, fi;
    int i;

    if (!left_s || !right_s || !inter_s || !left_f || !right_f || !inter_f) {
        fprintf(stderr, "allocation failure\n");
        return 2;
    }

    for (i = 0; i < total; i++) {
        left_s[i] = tw_c_left[i % 3];
        right_s[i] = tw_c_right[i % 3];
        inter_s[2 * i] = left_s[i];
        inter_s[2 * i + 1] = right_s[i];
        /* Float representation of the same integer PCM value. */
        left_f[i] = (float) left_s[i] / 32768.0f;
        right_f[i] = (float) right_s[i] / 32768.0f;
        inter_f[2 * i] = left_f[i];
        inter_f[2 * i + 1] = right_f[i];
    }

    /* Awkward chunk sizes deliberately cross 1152-sample frame boundaries. */
    encode_planar_short(left_s, right_s, total, 100, &sp);
    encode_interleaved_short(inter_s, total, 100, &si);
    encode_planar_float(left_f, right_f, total, 100, &fp);
    encode_interleaved_float(inter_f, total, 100, &fi);

    check(sp.size == (FIRST_FRAMES + 1) * FRAME_BYTES,
          "integer planar entry yields three frames plus one padded tail");
    check(si.size == sp.size,
          "integer interleaved entry yields the same number of frames");
    check(fp.size == sp.size,
          "float planar entry yields the same number of frames");
    check(fi.size == sp.size,
          "float interleaved entry yields the same number of frames");

    /* The core requirement: first three frames are byte-for-byte identical. */
    check(memcmp(sp.data, si.data, FIRST_FRAMES * FRAME_BYTES) == 0,
          "integer planar and interleaved first three frames are byte identical");
    check(memcmp(sp.data, fp.data, FIRST_FRAMES * FRAME_BYTES) == 0,
          "integer and float planar first three frames are byte identical");
    check(memcmp(sp.data, fi.data, FIRST_FRAMES * FRAME_BYTES) == 0,
          "integer planar and float interleaved first three frames are byte identical");

    /* Stronger: the whole stream, padded tail included, must match. */
    check(streams_equal(&sp, &si),
          "integer planar and interleaved streams are fully identical");
    check(streams_equal(&sp, &fp),
          "integer and float planar streams are fully identical");
    check(streams_equal(&sp, &fi),
          "integer planar and float interleaved streams are fully identical");

    /* Exact TW-C call shape: three samples per channel in a single call. */
    {
        short inter3[6];
        float lf3[3], rf3[3], fi3[6];
        struct stream a, b, c, d;
        for (i = 0; i < 3; i++) {
            inter3[2 * i] = tw_c_left[i];
            inter3[2 * i + 1] = tw_c_right[i];
            lf3[i] = (float) tw_c_left[i] / 32768.0f;
            rf3[i] = (float) tw_c_right[i] / 32768.0f;
            fi3[2 * i] = lf3[i];
            fi3[2 * i + 1] = rf3[i];
        }
        encode_planar_short(tw_c_left, tw_c_right, 3, 3, &a);
        encode_interleaved_short(inter3, 3, 3, &b);
        encode_planar_float(lf3, rf3, 3, 3, &c);
        encode_interleaved_float(fi3, 3, 3, &d);

        check(a.size == FRAME_BYTES && b.size == FRAME_BYTES &&
              c.size == FRAME_BYTES && d.size == FRAME_BYTES,
              "exact TW-C three-sample input produces one padded frame per entry");
        check(streams_equal(&a, &b) && streams_equal(&a, &c) &&
              streams_equal(&a, &d),
              "exact TW-C input is identical across all four entry points");
    }

    /* Channel order: a signal on the left only must map the same way through
       the planar and interleaved entries (even samples -> left channel). */
    {
        const int n = SAMPLES_PER_FRAME;
        short *lonly_p_l = calloc(n, sizeof(short));
        short *lonly_p_r = calloc(n, sizeof(short));
        short *lonly_i = calloc(n * 2, sizeof(short));
        struct stream pa, ia;
        lonly_p_l[0] = 1024;
        lonly_p_l[n / 2] = -2048;
        lonly_i[0] = 1024;
        lonly_i[2 * (n / 2)] = -2048;

        encode_planar_short(lonly_p_l, lonly_p_r, n, n, &pa);
        encode_interleaved_short(lonly_i, n, n, &ia);
        check(streams_equal(&pa, &ia),
              "left-only signal keeps left channel order in interleaved entry");

        free(lonly_p_l);
        free(lonly_p_r);
        free(lonly_i);
    }

    /* Channel order for the right channel (odd samples -> right channel). */
    {
        const int n = SAMPLES_PER_FRAME;
        short *ronly_p_l = calloc(n, sizeof(short));
        short *ronly_p_r = calloc(n, sizeof(short));
        short *ronly_i = calloc(n * 2, sizeof(short));
        struct stream pa, ia;
        ronly_p_r[1] = 2048;
        ronly_p_r[n / 3] = -4096;
        ronly_i[2 * 1 + 1] = 2048;
        ronly_i[2 * (n / 3) + 1] = -4096;

        encode_planar_short(ronly_p_l, ronly_p_r, n, n, &pa);
        encode_interleaved_short(ronly_i, n, n, &ia);
        check(streams_equal(&pa, &ia),
              "right-only signal keeps right channel order in interleaved entry");

        free(ronly_p_l);
        free(ronly_p_r);
        free(ronly_i);
    }

    /* A sub-frame tail is padded exactly once: flush emits one frame, a
     * second flush emits nothing. */
    {
        twolame_options *opts = tw_c_init();
        unsigned char buffer[OUT_CAPACITY];
        int first, second;
        twolame_encode_buffer(opts, tw_c_left, tw_c_right, 3,
                              buffer, sizeof(buffer));
        first = twolame_encode_flush(opts, buffer, sizeof(buffer));
        second = twolame_encode_flush(opts, buffer, sizeof(buffer));
        check(first == FRAME_BYTES, "flush pads the tail into one frame");
        check(second == 0, "a second flush does not pad the tail again");
        twolame_close(&opts);
    }

    /* Invalid arguments are rejected instead of reading out of bounds. */
    {
        twolame_options *opts = tw_c_init();
        unsigned char buffer[64];
        short pair[2] = { 1, 2 };
        check(twolame_encode_buffer_interleaved(opts, pair, -1, buffer,
                                                sizeof(buffer)) == -1,
              "negative num_samples is rejected");
        check(twolame_encode_buffer_interleaved(opts, NULL, 2, buffer,
                                                sizeof(buffer)) == -1,
              "NULL PCM with positive count is rejected");
        check(twolame_encode_buffer_interleaved(opts, pair, 2, buffer, 0) == -1,
              "zero-sized output buffer is rejected");
        check(twolame_encode_flush(opts, NULL, 64) == -1,
              "flush with NULL output buffer is rejected");
        twolame_close(&opts);
    }

    free(left_s);
    free(right_s);
    free(inter_s);
    free(left_f);
    free(right_f);
    free(inter_f);

    if (failures == 0) {
        printf("\nAll TW-C entry point tests passed.\n");
        return 0;
    }
    printf("\n%d TW-C test(s) failed.\n", failures);
    return 1;
}
