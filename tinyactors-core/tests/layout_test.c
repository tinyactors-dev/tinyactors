/*
 * Block A2 (src/layout.h): the layout builder. Arithmetic, alignment, the
 * start offset, zero counts and a failure at every edge of size_t, which is
 * the arithmetic a 32-bit host (WebAssembly) meets with 32-bit counts; the
 * builder never allocates, so no allocator is needed here.
 *
 * Includes the amalgamation, because the builder is private to core.
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

static size_t add(ta_layout *layout, size_t size, size_t alignment, size_t count) {
    return ta_layout_add(layout, &(ta_layout_entry){ .size = size, .alignment = alignment, .count = count });
}

static void test_empty(void) {
    ta_layout layout;
    size_t bytes = 99;
    size_t alignment = 99;
    ta_layout_init(&layout, 0);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_OK);
    CHECK(bytes == 0 && alignment == 1u);
    ta_layout_init(&layout, 24);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_OK);
    CHECK(bytes == 24u && alignment == 1u);
}

static void test_single_and_mixed(void) {
    ta_layout layout;
    size_t bytes = 0;
    size_t alignment = 0;
    ta_layout_init(&layout, 0);
    CHECK(add(&layout, 4, 4, 3) == 0);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_OK);
    CHECK(bytes == 12u && alignment == 4u);

    /* 1, 4, 8, 16 in turn: each offset is the end rounded up. */
    ta_layout_init(&layout, 0);
    CHECK(add(&layout, 1, 1, 3) == 0);   /* end 3 */
    CHECK(add(&layout, 4, 4, 1) == 4u);  /* end 8 */
    CHECK(add(&layout, 1, 1, 1) == 8u);  /* end 9 */
    CHECK(add(&layout, 8, 8, 2) == 16u); /* end 32 */
    CHECK(add(&layout, 1, 1, 1) == 32u); /* end 33 */
    CHECK(add(&layout, 16, 16, 1) == 48u); /* end 64 */
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_OK);
    CHECK(bytes == 64u && alignment == 16u);

    /* The total is the unpadded end: it is not rounded to the alignment. */
    ta_layout_init(&layout, 0);
    CHECK(add(&layout, 8, 8, 1) == 0);
    CHECK(add(&layout, 1, 1, 1) == 8u);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_OK);
    CHECK(bytes == 9u && alignment == 8u);
    CHECK(ta_layout_bytes(&layout) == 9u);
}

static void test_start_offset(void) {
    ta_layout layout;
    size_t bytes = 0;
    ta_layout_init(&layout, 5);
    CHECK(add(&layout, 8, 8, 1) == 8u); /* the first byte is at 5, an 8-aligned entry goes to 8 */
    CHECK(add(&layout, 2, 2, 1) == 16u);
    CHECK(ta_layout_finish(&layout, &bytes, NULL) == TA_OK);
    CHECK(bytes == 18u);
}

static void test_zero_count(void) {
    ta_layout layout;
    size_t bytes = 0;
    size_t alignment = 0;
    ta_layout_init(&layout, 0);
    CHECK(add(&layout, 1, 1, 3) == 0);
    /* An empty array still has an aligned place, and still counts toward the alignment. */
    CHECK(add(&layout, 8, 8, 0) == 8u);
    CHECK(add(&layout, 1, 1, 1) == 8u);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_OK);
    CHECK(bytes == 9u && alignment == 8u);
    /* A zero-size item takes no bytes however many there are, even SIZE_MAX of them. */
    ta_layout_init(&layout, 0);
    CHECK(add(&layout, 0, 1, SIZE_MAX) == 0);
    CHECK(ta_layout_finish(&layout, &bytes, NULL) == TA_OK && bytes == 0);
}

static void test_overflow(void) {
    ta_layout layout;
    size_t bytes = 77;
    size_t alignment = 77;

    /* count * size wraps. */
    ta_layout_init(&layout, 0);
    add(&layout, SIZE_MAX / 2u + 1u, 1, 2);
    CHECK(layout.overflowed);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_ERR_NOMEM);
    CHECK(bytes == 77u && alignment == 77u); /* outputs untouched */
    CHECK(ta_layout_bytes(&layout) == 0);

    /* The product fits but the sum with the end does not. */
    ta_layout_init(&layout, 0);
    add(&layout, 1, 1, SIZE_MAX);
    CHECK(!layout.overflowed && layout.end == SIZE_MAX);
    add(&layout, 1, 1, 1);
    CHECK(layout.overflowed);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_ERR_NOMEM);

    /* Rounding up wraps: the end is SIZE_MAX - 1 and the next alignment is 16. */
    ta_layout_init(&layout, SIZE_MAX - 1u);
    add(&layout, 0, 16, 0);
    CHECK(layout.overflowed);

    /* An end that is already aligned near SIZE_MAX is not an overflow. */
    ta_layout_init(&layout, SIZE_MAX - (SIZE_MAX % 16u));
    CHECK(add(&layout, 0, 16, 1) == SIZE_MAX - (SIZE_MAX % 16u));
    CHECK(!layout.overflowed);

    /* An alignment of zero is refused, not divided by. */
    ta_layout_init(&layout, 0);
    add(&layout, 4, 0, 1);
    CHECK(layout.overflowed);

    /* The failure is sticky: a later small entry does not clear it. */
    ta_layout_init(&layout, 0);
    add(&layout, SIZE_MAX, 1, 2);
    CHECK(add(&layout, 1, 1, 1) == 0);
    CHECK(layout.overflowed);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_ERR_NOMEM);
    CHECK(bytes == 77u && alignment == 77u);
}

static void test_checked_primitives(void) {
    size_t out = 5;
    CHECK(ta_checked_mul(0, SIZE_MAX, &out) && out == 0);
    CHECK(ta_checked_mul(SIZE_MAX, 1, &out) && out == SIZE_MAX);
    out = 5;
    CHECK(!ta_checked_mul(SIZE_MAX, 2, &out) && out == 5u);
    CHECK(!ta_checked_mul(SIZE_MAX / 2u + 1u, 2, &out) && out == 5u);
    CHECK(ta_checked_mul(SIZE_MAX / 2u, 2, &out) && out == SIZE_MAX - 1u);
    CHECK(ta_checked_add(SIZE_MAX - 1u, 1, &out) && out == SIZE_MAX);
    out = 5;
    CHECK(!ta_checked_add(SIZE_MAX, 1, &out) && out == 5u);
    CHECK(!ta_checked_add(1, SIZE_MAX, &out) && out == 5u);
}

int main(void) {
    test_empty();
    test_single_and_mixed();
    test_start_offset();
    test_zero_count();
    test_overflow();
    test_checked_primitives();
    if (failures != 0) {
        fprintf(stderr, "layout_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("layout_test: ok");
    return 0;
}
