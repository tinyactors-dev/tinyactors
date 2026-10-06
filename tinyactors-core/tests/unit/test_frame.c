/* ta_frame: length-prefixed link frames on byte streams (docs/protocol.md,
 * Framing) — the C helper for embedded hosts; the JS bindings' codec
 * (encodeFrame / FrameDecoder) is tested in bindings/node/test/frames.test.js. */
#include "../harness/harness.h"
#include <tinyactors/frame.h>

#include <string.h>

/* frames of `texts` back to back into out; returns the length */
static uint32_t encode(const char *const *texts, uint32_t n, uint8_t *out) {
    uint32_t at = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t length = (uint32_t)strlen(texts[i]);
        ta_frame_header(length, out + at);
        memcpy(out + at + TA_FRAME_HEADER, texts[i], length);
        at += TA_FRAME_HEADER + length;
    }
    return at;
}

typedef struct seen {
    char frames[8][64];
    uint32_t refused[8];
    uint32_t count, refused_count;
} seen;

/* the stream in pieces of `step` bytes (0: all at once) */
static void feed(ta_frame_reader *reader, uint8_t *buffer, const uint8_t *in, uint32_t n,
                 uint32_t step, seen *s) {
    memset(s, 0, sizeof *s);
    for (uint32_t at = 0; at < n;) {
        uint32_t piece = step && n - at > step ? step : n - at;
        const uint8_t *p = in + at;
        at += piece;
        while (piece) {
            uint32_t event, k = ta_frame_read(reader, buffer, p, piece, &event);
            CHECK(k <= piece);
            p += k;
            piece -= k;
            if (event == TA_FRAME_READY && s->count < 8)
                memcpy(s->frames[s->count++], buffer, reader->length);
            if (event == TA_FRAME_REFUSED && s->refused_count < 8)
                s->refused[s->refused_count++] = reader->length;
            if (event == TA_FRAME_MORE) CHECK_EQUAL(piece, 0);
        }
    }
}

static void test_header(void) {
    uint8_t h[4];
    ta_frame_header(10, h);
    CHECK(h[0] == 0 && h[1] == 0 && h[2] == 0 && h[3] == 10);
    ta_frame_header(70000, h);
    CHECK(h[0] == 0 && h[1] == 1 && h[2] == 0x11 && h[3] == 0x70);
    ta_frame_header(0xA1B2C3D4u, h);
    CHECK(h[0] == 0xA1 && h[1] == 0xB2 && h[2] == 0xC3 && h[3] == 0xD4);
}

static void test_partial_and_coalesced(void) {
    const char *texts[] = {"{\"t\":\"hello\",\"v\":1}", "", "{\"t\":\"ev\",\"name\":\"\xc3\xbc\"}",
                           "{\"t\":\"ping\",\"at\":42}"};
    uint8_t stream[256], buffer[64];
    uint32_t n = encode(texts, 4, stream);
    /* all at once (several frames in one chunk), byte by byte, and odd splits */
    uint32_t steps[] = {0, 1, 2, 3, 5, 7, 23};
    for (uint32_t s = 0; s < sizeof steps / sizeof steps[0]; s++) {
        ta_frame_reader reader;
        seen got;
        ta_frame_reader_init(&reader, sizeof buffer);
        CHECK_EQUAL(reader.size, sizeof reader);
        feed(&reader, buffer, stream, n, steps[s], &got);
        CHECK_EQUAL(got.count, 4);
        CHECK_EQUAL(got.refused_count, 0);
        for (uint32_t i = 0; i < 4; i++)
            CHECK(strcmp(got.frames[i], texts[i]) == 0);
    }
    /* one call returns at the end of each frame: the rest is left for the next */
    ta_frame_reader reader;
    uint32_t event;
    ta_frame_reader_init(&reader, sizeof buffer);
    uint32_t k = ta_frame_read(&reader, buffer, stream, n, &event);
    CHECK_EQUAL(event, TA_FRAME_READY);
    CHECK_EQUAL(k, TA_FRAME_HEADER + strlen(texts[0]));
    CHECK_EQUAL(reader.length, strlen(texts[0]));
}

static void test_oversize_refused(void) {
    char big[40];
    memset(big, 'y', sizeof big - 1);
    big[sizeof big - 1] = 0;
    const char *texts[] = {"ok", big, "after", "123456789"};
    uint8_t stream[256], buffer[8];
    uint32_t n = encode(texts, 4, stream);
    uint32_t steps[] = {0, 1, 3, 6};
    for (uint32_t s = 0; s < sizeof steps / sizeof steps[0]; s++) {
        ta_frame_reader reader;
        seen got;
        ta_frame_reader_init(&reader, sizeof buffer); /* max_frame 8 */
        feed(&reader, buffer, stream, n, steps[s], &got);
        /* the frames above 8 bytes are refused (answer: malformed), the others read */
        CHECK_EQUAL(got.count, 2);
        CHECK(strcmp(got.frames[0], "ok") == 0);
        CHECK(strcmp(got.frames[1], "after") == 0);
        CHECK_EQUAL(got.refused_count, 2);
        CHECK_EQUAL(got.refused[0], 39);
        CHECK_EQUAL(got.refused[1], 9);
    }
    /* refused from the prefix alone: a 4 GiB frame costs nothing to skip */
    ta_frame_reader reader;
    uint32_t event;
    const uint8_t huge[] = {0xff, 0xff, 0xff, 0xff, 1, 2, 3};
    ta_frame_reader_init(&reader, TA_FRAME_MAX);
    CHECK_EQUAL(ta_frame_read(&reader, buffer, huge, sizeof huge, &event), 4);
    CHECK_EQUAL(event, TA_FRAME_REFUSED);
    CHECK_EQUAL(reader.length, 0xffffffffu);
    CHECK_EQUAL(ta_frame_read(&reader, buffer, huge + 4, 3, &event), 3);
    CHECK_EQUAL(event, TA_FRAME_MORE);
    CHECK_EQUAL(reader.have, 3);
}

int main(void) {
    RUN(test_header);
    RUN(test_partial_and_coalesced);
    RUN(test_oversize_refused);
    TEST_MAIN_END();
}
