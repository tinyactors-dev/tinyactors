#include "host.h"
#include <stdint.h>

static int cleanup_log[16];
static int cleanup_count;
static void cleanup(void *arg) { cleanup_log[cleanup_count++] = (int)(uintptr_t)arg; }

int main(void)
{
    ta_test_allocator_t backing;
    ta_chunk_allocator_t callbacks;
    ta_arena_options_t root_options = { 1u, 4096u, 16384u, 2048u };
    ta_arena_options_t child_options = { 128u, 512u, 2048u, 256u };
    ta_arena_t *root = NULL, *a = NULL, *b = NULL;
    ta_memory_stats_t before, after;
    ta_layout_t byte_layout = { 13u, 1u };
    ta_layout_t wide_layout = { 17u, 256u };
    void *memory = NULL;
    ta_test_allocator_init(&backing, 64u, 0u);
    callbacks = ta_test_allocator_callbacks(&backing);
    if (ta_arena_create(&callbacks, &root_options, &root) != TA_OK) return 1;
    ta_arena_stats(root, &before);
    if (before.chunk_count != 1u || before.descendant_count != 0u) return 2;
    if (ta_arena_alloc(root, &wide_layout, &memory) != TA_OK || ((uintptr_t)memory & 255u) != 0u) return 3;
    if (ta_arena_open(root, &child_options, &a) != TA_OK || a == NULL) return 4;
    ta_arena_stats(a, &after);
    if (after.reserved_bytes > child_options.maximum_chunk_bytes) return 4;
    if (ta_arena_alloc(a, &byte_layout, &memory) != TA_OK) return 5;
    if (ta_arena_defer(a, cleanup, (void *)(uintptr_t)1u) != TA_OK || ta_arena_open(root, &child_options, &b) != TA_OK) return 6;
    ta_arena_stats(b, &after);
    if (after.reserved_bytes > child_options.maximum_chunk_bytes) return 6;
    if (ta_arena_defer(b, cleanup, (void *)(uintptr_t)2u) != TA_OK) return 7;
    ta_arena_close(a);
    if (cleanup_count != 1 || cleanup_log[0] != 1) return 8;
    ta_arena_stats(root, &after);
    if (after.descendant_count != 1u) return 9;
    ta_arena_close(root);
    if (cleanup_count != 2 || cleanup_log[1] != 2 || !ta_test_allocator_releases_ok(&backing)) return 10;
    ta_test_allocator_reset(&backing);

    ta_test_allocator_init(&backing, 512u, 0u);
    callbacks = ta_test_allocator_callbacks(&backing);
    { ta_arena_options_t o = { 64u, 512u, 300u, 256u };
      root = NULL;
      if (ta_arena_create(&callbacks, &o, &root) != TA_LIMIT || root != NULL) return 11;
      if (!ta_test_allocator_releases_ok(&backing)) return 12;
    }
    ta_test_allocator_reset(&backing);

    ta_test_allocator_init(&backing, 0u, 2u);
    callbacks = ta_test_allocator_callbacks(&backing);
    { ta_arena_options_t fail_options = { 128u, 512u, 4096u, 512u };
      ta_layout_t fail_layout = { 300u, 1u };
      if (ta_arena_create(&callbacks, &fail_options, &root) != TA_OK) return 13;
      if (ta_arena_alloc(root, &fail_layout, &memory) != TA_NO_MEMORY) return 14;
    }
    ta_arena_stats(root, &after);
    if (after.failed_allocations == 0u) return 15;
    ta_arena_close(root);
    ta_test_allocator_reset(&backing);
    return 0;
}
