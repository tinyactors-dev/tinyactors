/*
 * text_writer.h: the bounded text writer, its renderer, and the decimal and
 * hex formatters that trace contexts, fingerprints, compile errors and
 * generated IDs are written with.
 *
 * Claims proven here, in order:
 *
 * 1. ta_text_format_u64 writes a value's decimal digits, most significant
 *    first, with no leading zeros ("0" for zero), answers their count, from
 *    1 to TA_TEXT_U64_DIGITS for UINT64_MAX, and writes nothing beyond it;
 * 2. ta_text_format_hex writes exactly `count` lowercase hex digits of the
 *    value's low bits, most significant first and zero-padded; a count of
 *    0 writes nothing, and a count above 16 pads with zeros;
 * 3. ta_text_put stores the bytes that fall in the writer's window and
 *    counts all of them: with no buffer it only counts, with a small
 *    buffer it stores the start, and with a skip it stores a later window;
 * 4. ta_text_put_u64 puts the digits ta_text_format_u64 writes;
 * 5. ta_text_finish NUL-terminates the text when it and its NUL fit and
 *    reports the bytes needed either way, answering TA_ERR_NOMEM when they
 *    do not fit;
 * 6. ta_text_render allocates exactly the text and its NUL, answers
 *    TA_ERR_NOT_FOUND when there is no text, and on an allocation failure
 *    answers TA_ERR_NOMEM, leaves the output alone and leaks nothing.
 *
 * The test includes the amalgamation so it can reach the private units; it
 * uses the C library, the core does not.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

#define S(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

/* ------------------------------------------------------------------------
 * A counting allocator that fails on request.
 */

typedef struct {
    uint32_t live;
    uint32_t allocations;
    size_t last_size;
    bool fail;
} counting_allocator;

static void *counting_alloc(void *self, size_t size, size_t alignment) {
    counting_allocator *counter = self;
    if (counter->fail) {
        return NULL;
    }
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size_t rounded = (size + alignment - 1u) / alignment * alignment;
    void *ptr = aligned_alloc(alignment, rounded == 0 ? alignment : rounded);
    if (ptr != NULL) {
        counter->live += 1;
        counter->allocations += 1;
        counter->last_size = size;
    }
    return ptr;
}

static void counting_free(void *self, const ta_allocator_free_params *params) {
    counting_allocator *counter = self;
    if (params->ptr != NULL) {
        counter->live -= 1;
        free(params->ptr);
    }
}

static const ta_allocator_vtable counting_vtable = { .alloc = counting_alloc, .free = counting_free };

/* ------------------------------------------------------------------------
 * Claim 1: decimal.
 */

static void check_u64(uint64_t value, const char *expected) {
    char out[TA_TEXT_U64_DIGITS + 1u];
    memset(out, '#', sizeof(out));
    uint32_t count = ta_text_format_u64(out, value);
    CHECK(count == strlen(expected));
    CHECK(memcmp(out, expected, count) == 0);
    for (uint32_t index = count; index < sizeof(out); index += 1) {
        CHECK(out[index] == '#');
    }
}

static void test_format_u64(void) {
    check_u64(0, "0");
    check_u64(7, "7");
    check_u64(9, "9");
    check_u64(10, "10");
    check_u64(100, "100");
    check_u64(1234567890, "1234567890");
    check_u64(UINT32_MAX, "4294967295");
    check_u64((uint64_t)UINT32_MAX + 1u, "4294967296");
    check_u64(10000000000000000000ull, "10000000000000000000");
    check_u64(UINT64_MAX, "18446744073709551615");
    CHECK(strlen("18446744073709551615") == TA_TEXT_U64_DIGITS);
}

/* ------------------------------------------------------------------------
 * Claim 2: hex.
 */

static void check_hex(uint64_t value, uint32_t count, const char *expected) {
    char out[40];
    memset(out, '#', sizeof(out));
    ta_text_format_hex(out, value, count);
    CHECK(strlen(expected) == count);
    CHECK(memcmp(out, expected, count) == 0);
    for (uint32_t index = count; index < sizeof(out); index += 1) {
        CHECK(out[index] == '#');
    }
}

static void test_format_hex(void) {
    check_hex(0xabu, 0, "");
    check_hex(0, 1, "0");
    check_hex(0xfu, 1, "f");
    check_hex(0x1fu, 1, "f");
    check_hex(0x0au, 2, "0a");
    check_hex(0xc3u, 2, "c3");
    check_hex(0xffu, 2, "ff");
    check_hex(0x01u, 2, "01");
    check_hex(0x00f067aa0ba902b7ull, 16, "00f067aa0ba902b7");
    check_hex(0xa94684120f4c8b1aull, 16, "a94684120f4c8b1a");
    check_hex(UINT64_MAX, 16, "ffffffffffffffff");
    check_hex(0, 16, "0000000000000000");
    check_hex(0xabcdefull, 4, "cdef");
    check_hex(0x1234u, 20, "00000000000000001234");
}

/* ------------------------------------------------------------------------
 * Claim 3: the window.
 */

static void test_put_window(void) {
    ta_text_writer measure = {0};
    ta_text_put(&measure, S("hello"));
    ta_text_put(&measure, (ta_str){0});
    CHECK(measure.length == 5);

    char small[3];
    memset(small, '#', sizeof(small));
    ta_text_writer start = { .buffer = small, .capacity = 3 };
    ta_text_put(&start, S("he"));
    ta_text_put(&start, S("llo"));
    CHECK(start.length == 5);
    CHECK(memcmp(small, "hel", 3) == 0);

    char window[3];
    memset(window, '#', sizeof(window));
    ta_text_writer later = { .buffer = window, .capacity = 3, .skip = 2 };
    ta_text_put(&later, S("hello world"));
    CHECK(later.length == 11);
    CHECK(memcmp(window, "llo", 3) == 0);

    char tail[4];
    memset(tail, '#', sizeof(tail));
    ta_text_writer past = { .buffer = tail, .capacity = 4, .skip = 9 };
    ta_text_put(&past, S("hello world"));
    CHECK(past.length == 11);
    CHECK(memcmp(tail, "ld##", 4) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 4: decimal through the writer.
 */

static void test_put_u64(void) {
    const uint64_t values[] = { 0, 1, 42, 4294967295u, UINT64_MAX };
    for (uint32_t index = 0; index < sizeof(values) / sizeof(values[0]); index += 1) {
        char digits[TA_TEXT_U64_DIGITS];
        uint32_t count = ta_text_format_u64(digits, values[index]);
        char buffer[32];
        ta_text_writer writer = { .buffer = buffer, .capacity = sizeof(buffer) };
        ta_text_put(&writer, S("n="));
        ta_text_put_u64(&writer, values[index]);
        CHECK(writer.length == 2u + count);
        CHECK(memcmp(buffer, "n=", 2) == 0 && memcmp(buffer + 2, digits, count) == 0);
    }
    ta_text_writer measure = {0};
    ta_text_put_u64(&measure, UINT64_MAX);
    CHECK(measure.length == TA_TEXT_U64_DIGITS);
}

/* ------------------------------------------------------------------------
 * Claim 5: finishing.
 */

static void test_finish(void) {
    char buffer[6];
    ta_text_writer fits = { .buffer = buffer, .capacity = sizeof(buffer) };
    ta_text_put(&fits, S("hello"));
    uint32_t needed = 0;
    CHECK(ta_text_finish(&fits, &needed) == TA_OK);
    CHECK(needed == 6 && strcmp(buffer, "hello") == 0);

    ta_text_writer empty = { .buffer = buffer, .capacity = 1 };
    CHECK(ta_text_finish(&empty, &needed) == TA_OK);
    CHECK(needed == 1 && buffer[0] == '\0');

    char short_buffer[5];
    ta_text_writer too_short = { .buffer = short_buffer, .capacity = sizeof(short_buffer) };
    ta_text_put(&too_short, S("hello"));
    CHECK(ta_text_finish(&too_short, &needed) == TA_ERR_NOMEM);
    CHECK(needed == 6);

    ta_text_writer measure = {0};
    ta_text_put(&measure, S("hello"));
    CHECK(ta_text_finish(&measure, &needed) == TA_ERR_NOMEM);
    CHECK(needed == 6);
}

/* ------------------------------------------------------------------------
 * Claim 6: rendering.
 */

static bool write_number(const void *context, ta_text_writer *writer) {
    ta_text_put(writer, S("id."));
    ta_text_put_u64(writer, *(const uint64_t *)context);
    return true;
}

static bool write_nothing(const void *context, ta_text_writer *writer) {
    (void)context, (void)writer;
    return false;
}

static void test_render(void) {
    counting_allocator counter = {0};
    ta_allocator allocator = { .self = &counter, .v = &counting_vtable };
    uint64_t number = 1234;
    ta_str out = S("untouched");

    counter.fail = true;
    CHECK(ta_text_render(allocator, &(ta_text_render_params){ .write = write_number, .context = &number,
                                                              .out = &out }) == TA_ERR_NOMEM);
    CHECK(out.len == 9 && memcmp(out.ptr, "untouched", 9) == 0);
    CHECK(counter.live == 0 && counter.allocations == 0);

    counter.fail = false;
    CHECK(ta_text_render(allocator, &(ta_text_render_params){ .write = write_nothing, .context = NULL,
                                                              .out = &out }) == TA_ERR_NOT_FOUND);
    CHECK(out.len == 9 && counter.allocations == 0);

    CHECK(ta_text_render(allocator, &(ta_text_render_params){ .write = write_number, .context = &number,
                                                              .out = &out }) == TA_OK);
    CHECK(out.len == 7 && strcmp(out.ptr, "id.1234") == 0);
    CHECK(counter.live == 1 && counter.last_size == 8);
    ta_mem_free(allocator, (void *)out.ptr, &(ta_allocator_free_params){ .size = out.len + 1u, .alignment = 1 });
    CHECK(counter.live == 0);
}

int main(void) {
    test_format_u64();
    test_format_hex();
    test_put_window();
    test_put_u64();
    test_finish();
    test_render();
    if (failures != 0) {
        fprintf(stderr, "text writer test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core text writer test passed");
    return 0;
}
