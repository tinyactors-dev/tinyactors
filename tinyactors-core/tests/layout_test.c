/*
 * The layout builder (src/layout.h), and the blocks core lays out with it.
 *
 * Claims:
 *
 *  1. An empty layout is 0 bytes aligned to 1, and a single entry starts
 *     at offset 0 and takes size * count bytes.
 *  2. Each entry starts at the first multiple of its alignment at or after
 *     the end of the one before, for powers of two and for other
 *     alignments alike; the layout's alignment is the largest entry's, and
 *     its bytes end at the last entry, with no padding after it.
 *  3. An entry of no bytes (a count or a size of 0) still gets an aligned
 *     offset, moves the end up to it and raises the alignment.
 *  4. An offset may be left unread (NULL), with the same layout.
 *  5. An entry whose bytes, aligned offset or end would exceed SIZE_MAX
 *     answers TA_ERR_NOMEM, and an alignment of 0 TA_ERR_INVALID; either
 *     way the layout and the offset are unchanged, and the next entry
 *     lays out as if the failed one had not been tried. An entry that
 *     ends exactly at SIZE_MAX fits.
 *  6. An actor block (record, session storage, data-model instance) has
 *     the offsets, size and alignment the hand-written sum gave, for
 *     definitions of several shapes, compiled ones included; a refused
 *     layout leaves the definition's fields alone.
 *  7. A continuation has the region offsets and size the hand-written sum
 *     gave, its accessors point at them, and its block is aligned to
 *     TA_CONTINUATION_ALIGN.
 *  8. An operator transition takes the bytes the hand-written sum gave.
 *  9. The system's interpreter regions and its table of nested regions
 *     have the offsets and sizes the hand-written sum gave, and the
 *     system lays its arenas there.
 * 10. The interpreter scratch a definition needs is what the hand-written
 *     arena walk gave, for synthetic and compiled definitions.
 *
 * Claims 6 to 10 compare each block with a copy of the arithmetic it
 * replaced (the `sum_*` functions below) over a range of shapes, and pin
 * the values that arithmetic computed on a 64-bit build. The pinned
 * values depend on the sizes of core's structs, so they are checked only
 * while those sizes are the ones they were recorded with; the comparison
 * with the copied sums runs always.
 *
 * The test includes the amalgamation so it can reach private units; it
 * uses the C library, the core does not.
 */
#include "../include/tinyactors.c"
#include "test_allocator.h"

#include <stdio.h>
#include <string.h>

#define S(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

static ta_layout_entry entry(size_t size, size_t alignment, size_t count) {
    return (ta_layout_entry){ .size = size, .alignment = alignment, .count = count };
}

/* Adds an entry that must fit, and answers its offset. */
static size_t add(ta_layout *layout, ta_layout_entry item) {
    size_t offset = (size_t)-1;
    CHECK(ta_layout_add(layout, &item, &offset) == TA_OK);
    return offset;
}

/* Adds an entry that must be refused with `expected`, and checks it changed nothing. */
static void refused(ta_layout *layout, ta_layout_entry item, ta_result expected) {
    ta_layout before = *layout;
    size_t offset = 12345u;
    CHECK(ta_layout_add(layout, &item, &offset) == expected);
    CHECK(layout->bytes == before.bytes && layout->alignment == before.alignment);
    CHECK(offset == 12345u);
}

/* ------------------------------------------------------------------------
 * The struct sizes the pinned values were recorded with (64-bit, clang).
 */

static bool pinned_sizes(void) {
    return sizeof(void *) == 8u && sizeof(ta_actor) == 232u && _Alignof(ta_actor) == 8u &&
           sizeof(ta_continuation) == 48u && sizeof(ta_exec_frame) == 40u && _Alignof(ta_exec_frame) == 8u &&
           sizeof(ta_arena_block) == 40u && sizeof(ta_nested_region) == 16u &&
           sizeof(struct ta_entry_frame) == 48u && sizeof(struct ta_trace_condition) == 24u;
}

/* ------------------------------------------------------------------------
 * Claim 1: the empty layout and one entry.
 */

static void test_empty_and_one(void) {
    ta_layout layout = { .bytes = 99, .alignment = 64 };
    ta_layout_init(&layout);
    CHECK(layout.bytes == 0 && layout.alignment == 1);

    CHECK(add(&layout, entry(12, 4, 3)) == 0);
    CHECK(layout.bytes == 36 && layout.alignment == 4);

    ta_layout_init(&layout);
    CHECK(add(&layout, *TA_LAYOUT_ENTRY(uint64_t, 5)) == 0);
    CHECK(layout.bytes == 5u * sizeof(uint64_t) && layout.alignment == _Alignof(uint64_t));
}

/* ------------------------------------------------------------------------
 * Claim 2: padding before each entry, the largest alignment, no tail.
 */

static void test_padding(void) {
    ta_layout layout;
    ta_layout_init(&layout);
    CHECK(add(&layout, entry(1, 1, 3)) == 0);   /* [0, 3) */
    CHECK(add(&layout, entry(4, 4, 1)) == 4);   /* padded from 3 */
    CHECK(add(&layout, entry(8, 8, 2)) == 8);   /* already aligned */
    CHECK(add(&layout, entry(2, 2, 1)) == 24);
    CHECK(add(&layout, entry(16, 16, 1)) == 32); /* padded from 26 */
    CHECK(add(&layout, entry(1, 1, 1)) == 48);
    CHECK(layout.bytes == 49);                  /* not rounded to 16 */
    CHECK(layout.alignment == 16);

    /* A smaller alignment after a larger one does not lower it. */
    CHECK(add(&layout, entry(2, 2, 1)) == 50);
    CHECK(layout.alignment == 16 && layout.bytes == 52);

    /* An alignment that is not a power of two rounds up to its multiple. */
    ta_layout_init(&layout);
    CHECK(add(&layout, entry(5, 1, 1)) == 0);
    CHECK(add(&layout, entry(3, 3, 2)) == 6);
    CHECK(add(&layout, entry(1, 7, 1)) == 14);
    CHECK(layout.bytes == 15 && layout.alignment == 7);

    /* Large alignments. */
    ta_layout_init(&layout);
    CHECK(add(&layout, entry(1, 1, 1)) == 0);
    CHECK(add(&layout, entry(1, 4096, 1)) == 4096);
    CHECK(layout.bytes == 4097 && layout.alignment == 4096);
}

/* ------------------------------------------------------------------------
 * Claim 3: entries of no bytes.
 */

static void test_empty_entries(void) {
    ta_layout layout;
    ta_layout_init(&layout);
    CHECK(add(&layout, entry(8, 8, 0)) == 0);
    CHECK(layout.bytes == 0 && layout.alignment == 8);

    CHECK(add(&layout, entry(3, 1, 1)) == 0);
    CHECK(add(&layout, entry(8, 8, 0)) == 8); /* aligned, and the end moves to it */
    CHECK(layout.bytes == 8);
    CHECK(add(&layout, entry(1, 1, 1)) == 8);

    CHECK(add(&layout, entry(0, 32, 7)) == 32); /* size 0, count 7 */
    CHECK(layout.bytes == 32 && layout.alignment == 32);

    CHECK(add(&layout, entry(0, 1, 0)) == 32);
    CHECK(layout.bytes == 32 && layout.alignment == 32);
}

/* ------------------------------------------------------------------------
 * Claim 4: an offset left unread.
 */

static void test_null_offset(void) {
    ta_layout with;
    ta_layout without;
    ta_layout_init(&with);
    ta_layout_init(&without);
    const ta_layout_entry items[] = { entry(3, 1, 1), entry(4, 4, 5), entry(16, 16, 0), entry(2, 2, 3) };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(items); index += 1u) {
        (void)add(&with, items[index]);
        CHECK(ta_layout_add(&without, &items[index], NULL) == TA_OK);
        CHECK(with.bytes == without.bytes && with.alignment == without.alignment);
    }
}

/* ------------------------------------------------------------------------
 * Claim 5: overflow and a zero alignment.
 */

static void test_failures(void) {
    ta_layout layout;
    ta_layout_init(&layout);

    /* size * count does not fit */
    refused(&layout, entry(2, 1, SIZE_MAX / 2u + 1u), TA_ERR_NOMEM);
    refused(&layout, entry(SIZE_MAX, 1, 2), TA_ERR_NOMEM);
    refused(&layout, entry((size_t)1 << (sizeof(size_t) * 4u), 1, (size_t)1 << (sizeof(size_t) * 4u)),
            TA_ERR_NOMEM);
    /* ... but SIZE_MAX bytes as one entry, or as SIZE_MAX of one byte, do */
    ta_layout fits;
    ta_layout_init(&fits);
    CHECK(add(&fits, entry(1, 1, SIZE_MAX)) == 0 && fits.bytes == SIZE_MAX);
    ta_layout_init(&fits);
    CHECK(add(&fits, entry(SIZE_MAX, 1, 1)) == 0 && fits.bytes == SIZE_MAX);
    /* and a huge count of nothing is nothing */
    ta_layout_init(&fits);
    CHECK(add(&fits, entry(0, 8, SIZE_MAX)) == 0 && fits.bytes == 0);

    /* the aligned offset does not fit */
    ta_layout_init(&layout);
    (void)add(&layout, entry(1, 1, SIZE_MAX - 2u)); /* ends at SIZE_MAX - 2 */
    refused(&layout, entry(1, 4, 0), TA_ERR_NOMEM);   /* the next multiple of 4 is SIZE_MAX + 1 */
    refused(&layout, entry(0, SIZE_MAX / 2u + 1u, 1), TA_ERR_NOMEM);
    /* an alignment whose next multiple is still a size fits, so close to the top */
    ta_layout top = layout;
    CHECK(add(&top, entry(0, SIZE_MAX, 0)) == SIZE_MAX && top.bytes == SIZE_MAX);
    /* and one the end already meets adds no padding */
    CHECK(add(&layout, entry(0, SIZE_MAX - 2u, 0)) == SIZE_MAX - 2u); /* not a power of two */
    CHECK(layout.alignment == SIZE_MAX - 2u);
    CHECK(add(&layout, entry(1, 1, 2)) == SIZE_MAX - 2u && layout.bytes == SIZE_MAX);

    /* the end does not fit */
    ta_layout_init(&layout);
    (void)add(&layout, entry(1, 1, 100));
    refused(&layout, entry(1, 4, SIZE_MAX - 99u), TA_ERR_NOMEM); /* starts at 100, one byte too many */
    CHECK(add(&layout, entry(1, 4, SIZE_MAX - 100u)) == 100 && layout.bytes == SIZE_MAX);
    refused(&layout, entry(1, 1, 1), TA_ERR_NOMEM);
    CHECK(add(&layout, entry(0, 1, 1)) == SIZE_MAX); /* no bytes still fit at the very end */

    /* a zero alignment */
    ta_layout_init(&layout);
    (void)add(&layout, entry(3, 1, 1));
    refused(&layout, entry(4, 0, 1), TA_ERR_INVALID);
    refused(&layout, entry(0, 0, 0), TA_ERR_INVALID);

    /* the next entry lays out as if nothing had been refused */
    ta_layout clean;
    ta_layout_init(&clean);
    (void)add(&clean, entry(3, 1, 1));
    CHECK(add(&layout, entry(8, 8, 2)) == add(&clean, entry(8, 8, 2)));
    CHECK(layout.bytes == clean.bytes && layout.alignment == clean.alignment);
}

/* ------------------------------------------------------------------------
 * Claim 6: the actor block.
 */

typedef struct {
    size_t session;
    size_t instance;
    size_t bytes;
    size_t alignment;
} actor_block;

/* The sum the actor layout was written as before it used the builder. */
static actor_block sum_actor(const ta_definition *definition) {
    size_t instance_alignment = definition->datamodel_instance_alignment;
    size_t alignment = _Alignof(ta_actor);
    if (alignment < _Alignof(uint64_t)) {
        alignment = _Alignof(uint64_t);
    }
    if (alignment < instance_alignment) {
        alignment = instance_alignment;
    }
    actor_block block;
    block.session = ta_align_up(sizeof(ta_actor), _Alignof(uint64_t));
    block.instance = ta_align_up(block.session + ta_definition_session_bytes(definition), instance_alignment);
    block.bytes = block.instance + definition->datamodel_instance_size;
    block.alignment = alignment;
    return block;
}

static ta_definition actor_shape(uint16_t config_bytes, uint32_t history_bytes, uint32_t size, uint32_t alignment) {
    ta_definition definition = { 0 };
    definition.configuration.bytes = config_bytes;
    definition.history.storage_bytes = history_bytes;
    definition.datamodel_instance_size = size;
    definition.datamodel_instance_alignment = alignment;
    return definition;
}

static bool actor_matches(const ta_definition *definition, actor_block expected) {
    return definition->actor_session_offset == expected.session &&
           definition->actor_instance_offset == expected.instance && definition->actor_block_bytes == expected.bytes &&
           definition->actor_block_alignment == expected.alignment;
}

/* Two regions of two states each, a guarded transition, a deep history
 * state and an <if> nested in an <onentry>: every count the interpreter's
 * scratch is sized from is non-zero. */
static const ta_str chart_to_a2[] = { S("a2"), { 0 } };
static const ta_str chart_to_b1[] = { S("b1"), { 0 } };
static const ta_scxml_transition_spec chart_a1_out[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = S("go"),
      .condition = { .kind = TA_EXPRESSION_SOURCE, .as.source = S("In('b1')") }, .targets = chart_to_a2 },
    { 0 },
};
static const ta_scxml_transition_spec chart_history_default[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = chart_to_b1 },
    { 0 },
};
static const ta_scxml_action_spec chart_then[] = { { .kind = TA_SCXML_ACTION_LOG, .as.log = { .label = S("in") } },
                                                   { 0 } };
static const ta_scxml_action_spec chart_entry[] = {
    { .kind = TA_SCXML_ACTION_IF,
      .as.conditional = { .condition = { .kind = TA_EXPRESSION_SOURCE, .as.source = S("In('a1')") },
                          .then_actions = chart_then } },
    { 0 },
};
static const ta_scxml_handler_spec chart_onentry[] = { { .actions = chart_entry }, { 0 } };
static const ta_scxml_state_spec chart_a[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a1"), .transitions = chart_a1_out, .onentry = chart_onentry },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a2") },
    { 0 },
};
static const ta_scxml_state_spec chart_b[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = S("b1") },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = S("b2") },
    { .kind = TA_SCXML_STATE_HISTORY_DEEP, .id = S("bh"), .transitions = chart_history_default },
    { 0 },
};
static const ta_scxml_state_spec chart_regions[] = {
    { .kind = TA_SCXML_STATE_COMPOUND, .id = S("a"), .states = chart_a },
    { .kind = TA_SCXML_STATE_COMPOUND, .id = S("b"), .states = chart_b },
    { 0 },
};
static const ta_scxml_state_spec chart_top[] = {
    { .kind = TA_SCXML_STATE_PARALLEL, .id = S("p"), .states = chart_regions },
    { 0 },
};
static const ta_scxml_spec chart_spec = { .name = S("layout"), .states = chart_top };

static ta_definition *compile_chart(ta_system *system, ta_allocator allocator) {
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = &chart_spec,
                                                                             .datamodel = ta_null_datamodel(),
                                                                             .allocator = allocator,
                                                                             .out = &definition }) == TA_OK);
    return definition;
}

static void test_actor_block(void) {
    /* The shapes the values below were recorded for. */
    static const struct {
        uint16_t config;
        uint32_t history;
        uint32_t size;
        uint32_t alignment;
        actor_block pinned;
    } shapes[] = {
        { 8, 0, 0, 1, { 232, 248, 248, 8 } },     /* the null data model, no history */
        { 8, 16, 1, 1, { 232, 264, 265, 8 } },
        { 16, 32, 24, 16, { 232, 304, 328, 16 } }, /* an instance aligned past the record */
        { 8, 8, 40, 8, { 232, 256, 296, 8 } },
        { 8, 0, 5, 3, { 232, 249, 254, 8 } },      /* an alignment that is not a power of two */
        { 24, 0, 7, 64, { 232, 320, 327, 64 } },
    };
    bool pin = pinned_sizes();
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(shapes); index += 1u) {
        ta_definition definition =
            actor_shape(shapes[index].config, shapes[index].history, shapes[index].size, shapes[index].alignment);
        CHECK(ta_actor_layout(&definition) == TA_OK);
        CHECK(actor_matches(&definition, sum_actor(&definition)));
        if (pin) {
            CHECK(actor_matches(&definition, shapes[index].pinned));
        }
    }
    /* A sweep of shapes against the sum. */
    static const uint32_t alignments[] = { 1, 2, 3, 4, 8, 16, 24, 64, 4096 };
    for (uint16_t config = 0; config <= 64; config += 8u) {
        for (uint32_t history = 0; history <= 96; history += 24u) {
            for (uint32_t size = 0; size <= 33; size += 11u) {
                for (uint32_t index = 0; index < TA_ARRAY_COUNT(alignments); index += 1u) {
                    ta_definition definition = actor_shape(config, history, size, alignments[index]);
                    CHECK(ta_actor_layout(&definition) == TA_OK);
                    CHECK(actor_matches(&definition, sum_actor(&definition)));
                }
            }
        }
    }

    /* A refused layout leaves the fields alone. */
    ta_definition refused_shape = actor_shape(8, 0, 4, 0);
    refused_shape.actor_session_offset = 1;
    refused_shape.actor_instance_offset = 2;
    refused_shape.actor_block_bytes = 3;
    refused_shape.actor_block_alignment = 4;
    CHECK(ta_actor_layout(&refused_shape) == TA_ERR_INVALID);
    CHECK(actor_matches(&refused_shape, (actor_block){ 1, 2, 3, 4 }));
#if SIZE_MAX <= UINT32_MAX
    /* Only a 32-bit size_t can overflow: a session and an instance near 4 GiB. */
    refused_shape = actor_shape(8, UINT32_MAX - 1024u, UINT32_MAX - 1024u, 8);
    refused_shape.actor_block_bytes = 3;
    CHECK(ta_actor_layout(&refused_shape) == TA_ERR_NOMEM);
    CHECK(refused_shape.actor_block_bytes == 3);
#endif

    /* A compiled definition: the null data model's instance takes nothing. */
    test_allocator heap = { 0 };
    ta_allocator allocator = test_allocator_of(&heap);
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    ta_definition *definition = compile_chart(system, allocator);
    if (definition != NULL) {
        CHECK(actor_matches(definition, sum_actor(definition)));
        CHECK(definition->configuration.bytes == 8 && definition->history.storage_bytes == 8);
        if (pin) {
            CHECK(actor_matches(definition, (actor_block){ 232, 256, 256, 8 }));
        }
        ta_definition_destroy(definition);
    }
    ta_system_destroy(system);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 7: the continuation.
 */

typedef struct {
    size_t sets;
    size_t decls;
    size_t history;
    size_t enabled;
    size_t frames;
    size_t end;
} continuation_block;

/* The sum the continuation was written as before it used the builder. */
static continuation_block sum_continuation(const ta_continuation *shape) {
    continuation_block block;
    block.sets = ta_align_up(sizeof(ta_continuation), _Alignof(uint64_t));
    block.decls = block.sets + (size_t)shape->words * 4u * sizeof(uint64_t);
    block.history = ta_align_up(block.decls + (size_t)shape->decl_words * sizeof(uint64_t), _Alignof(ta_state_id));
    block.enabled = ta_align_up(block.history + (size_t)shape->state_count * sizeof(ta_state_id),
                                _Alignof(ta_transition_id));
    block.frames = ta_align_up(block.enabled + (size_t)shape->enabled_count * sizeof(ta_transition_id),
                               _Alignof(ta_exec_frame));
    block.end = block.frames + (size_t)shape->frame_count * sizeof(ta_exec_frame);
    return block;
}

/* Where the accessors of a continuation of `shape`, allocated with the
 * bytes it needs, point, as offsets from its start. */
static continuation_block accessed_continuation(const ta_continuation *shape, ta_allocator allocator) {
    continuation_block block = { 0 };
    size_t bytes = 0;
    CHECK(ta_continuation_bytes(shape, &bytes) == TA_OK);
    ta_continuation *continuation = ta_mem_alloc(allocator, bytes, TA_CONTINUATION_ALIGN);
    CHECK(continuation != NULL);
    if (continuation == NULL) {
        return block;
    }
    *continuation = *shape;
    continuation->bytes = bytes;
    unsigned char *base = (unsigned char *)continuation;
    block.sets = (size_t)((unsigned char *)ta_continuation_sets(continuation) - base);
    block.decls = (size_t)((unsigned char *)ta_continuation_decls(continuation) - base);
    block.history = (size_t)((unsigned char *)ta_continuation_history(continuation) - base);
    block.enabled = (size_t)((unsigned char *)ta_continuation_enabled(continuation) - base);
    block.frames = (size_t)((unsigned char *)ta_continuation_frames(continuation) - base);
    block.end = bytes;
    /* Every region can be written to its end without leaving the block. */
    memset(base + block.sets, 0xa5, bytes - block.sets);
    ta_mem_free(allocator, continuation, &(ta_allocator_free_params){ .size = bytes, .alignment = TA_CONTINUATION_ALIGN });
    return block;
}

static bool continuation_equal(continuation_block a, continuation_block b) {
    return a.sets == b.sets && a.decls == b.decls && a.history == b.history && a.enabled == b.enabled &&
           a.frames == b.frames && a.end == b.end;
}

static void test_continuation(void) {
    test_allocator heap = { 0 };
    ta_allocator allocator = test_allocator_of(&heap);
    static const struct {
        uint32_t words;
        uint32_t decl_words;
        uint32_t state_count;
        uint32_t enabled_count;
        uint32_t frame_count;
        continuation_block pinned;
    } shapes[] = {
        { 1, 1, 1, 2, 1, { 48, 80, 88, 92, 104, 144 } },
        { 0, 1, 0, 1, 1, { 48, 48, 56, 56, 64, 104 } },
        { 2, 1, 65, 66, 5, { 48, 112, 120, 380, 648, 848 } },
        { 3, 2, 129, 130, 7, { 48, 144, 160, 676, 1200, 1480 } },
        { 1, 1, 3, 4, 3, { 48, 80, 88, 100, 120, 240 } }, /* padding before the frames */
    };
    bool pin = pinned_sizes();
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(shapes); index += 1u) {
        ta_continuation shape = { .words = shapes[index].words, .decl_words = shapes[index].decl_words,
                                  .state_count = shapes[index].state_count,
                                  .enabled_count = shapes[index].enabled_count,
                                  .frame_count = shapes[index].frame_count };
        continuation_block accessed = accessed_continuation(&shape, allocator);
        CHECK(continuation_equal(accessed, sum_continuation(&shape)));
        if (pin) {
            CHECK(continuation_equal(accessed, shapes[index].pinned));
        }
    }
    /* A sweep of shapes against the sum, and the block's alignment. */
    for (uint32_t words = 0; words <= 3; words += 1u) {
        for (uint32_t states = 0; states <= 9; states += 1u) {
            for (uint32_t frames = 0; frames <= 4; frames += 1u) {
                ta_continuation shape = { .words = words, .decl_words = 1u + states / 4u, .state_count = states,
                                          .enabled_count = states + 1u, .frame_count = frames };
                CHECK(continuation_equal(accessed_continuation(&shape, allocator), sum_continuation(&shape)));
                size_t offsets[TA_CONTINUATION_REGIONS];
                ta_layout layout;
                CHECK(ta_continuation_lay_out(&shape, offsets, &layout) == TA_OK);
                CHECK(layout.alignment == TA_CONTINUATION_ALIGN);
            }
        }
    }
#if SIZE_MAX <= UINT32_MAX
    /* Only a 32-bit size_t can overflow. */
    size_t bytes = 7;
    ta_continuation huge = { .words = UINT32_MAX, .decl_words = 1, .frame_count = 1 };
    CHECK(ta_continuation_bytes(&huge, &bytes) == TA_ERR_NOMEM);
    CHECK(bytes == 7);
#endif
    CHECK(test_allocator_finish(&heap) == 0);
}

/* ------------------------------------------------------------------------
 * Claim 8: the operator transition.
 */

static void test_operator_jump(void) {
    static const struct {
        uint32_t count;
        size_t pinned;
    } counts[] = { { 0, 8 }, { 1, 12 }, { 2, 16 }, { 3, 20 }, { 1000, 4008 } };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(counts); index += 1u) {
        size_t bytes = 0;
        CHECK(ta_operator_jump_bytes(counts[index].count, &bytes) == TA_OK);
        CHECK(bytes == offsetof(ta_operator_jump, targets) + (size_t)counts[index].count * sizeof(ta_state_id));
        CHECK(bytes == counts[index].pinned); /* depends on no pointer size */
    }
    size_t bytes = 0;
#if SIZE_MAX <= UINT32_MAX
    bytes = 7;
    CHECK(ta_operator_jump_bytes(UINT32_MAX, &bytes) == TA_ERR_NOMEM);
    CHECK(bytes == 7);
#else
    CHECK(ta_operator_jump_bytes(UINT32_MAX, &bytes) == TA_OK);
    CHECK(bytes == offsetof(ta_operator_jump, targets) + (size_t)UINT32_MAX * sizeof(ta_state_id));
#endif
}

/* ------------------------------------------------------------------------
 * Claim 9: the system's interpreter regions.
 */

typedef struct {
    size_t render;
    size_t region;
    size_t nested;
    size_t arena;
} system_block;

/* The sums the system's regions were written as before they used the builder. */
static system_block sum_system(uint32_t interpreter_bytes, uint32_t render_bytes, uint32_t nesting) {
    system_block block;
    block.render = TA_ALIGN_UP(ta_arena_fixed_bytes(interpreter_bytes), (size_t)_Alignof(ta_arena_block));
    block.region = block.render + ta_arena_fixed_bytes(render_bytes);
    block.nested = TA_ALIGN_UP(block.region, (size_t)_Alignof(ta_nested_region));
    block.arena = block.nested + (size_t)(nesting - 1u) * sizeof(ta_nested_region);
    return block;
}

static void test_system_regions(void) {
    static const struct {
        uint32_t interpreter_bytes;
        uint32_t render_bytes;
        uint32_t nesting;
        system_block pinned;
    } shapes[] = {
        { TA_PARAM_DEFAULT_INTERPRETER_BYTES, TA_PARAM_DEFAULT_RENDER_BYTES, TA_PARAM_DEFAULT_INTERPRETER_NESTING,
          { 65576, 82000, 82000, 82048 } },
        { 1024, 256, 1, { 1064, 1360, 1360, 1360 } },        /* the smallest system: no nested regions */
        { 1025, 257, 2, { 1072, 1369, 1376, 1392 } },        /* padding after each arena */
        { 4093, 999, 64, { 4136, 5175, 5176, 6184 } },
    };
    bool pin = pinned_sizes() && TA_PARAM_DEFAULT_INTERPRETER_BYTES == 65536u &&
               TA_PARAM_DEFAULT_RENDER_BYTES == 16384u && TA_PARAM_DEFAULT_INTERPRETER_NESTING == 4u;
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(shapes); index += 1u) {
        uint32_t params[TA_PARAM_COUNT] = { 0 };
        params[TA_PARAM_INTERPRETER_BYTES] = shapes[index].interpreter_bytes;
        params[TA_PARAM_RENDER_BYTES] = shapes[index].render_bytes;
        params[TA_PARAM_INTERPRETER_NESTING] = shapes[index].nesting;
        system_block expected = sum_system(shapes[index].interpreter_bytes, shapes[index].render_bytes,
                                           shapes[index].nesting);
        if (pin) {
            CHECK(memcmp(&expected, &shapes[index].pinned, sizeof expected) == 0);
        }
        ta_interpreter_region region = { 0 };
        CHECK(ta_interpreter_region_lay_out(params, &region) == TA_OK);
        CHECK(region.render == expected.render && region.bytes == expected.region);

        /* The system lays its arenas over exactly that. */
        test_allocator heap = { 0 };
        ta_allocator allocator = test_allocator_of(&heap);
        ta_param_value values[] = {
            { TA_PARAM_INTERPRETER_BYTES, shapes[index].interpreter_bytes },
            { TA_PARAM_RENDER_BYTES, shapes[index].render_bytes },
            { TA_PARAM_INTERPRETER_NESTING, shapes[index].nesting },
        };
        ta_system *system = NULL;
        CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = values, .param_count = 3 },
                               &system) == TA_OK);
        if (system == NULL) {
            continue;
        }
        unsigned char *memory = system->arena_memory;
        CHECK(system->arena_bytes == expected.arena);
        CHECK((unsigned char *)system->scratch.head == memory);
        CHECK(system->scratch.head->allocation_size == expected.render);
        CHECK((unsigned char *)system->render.head == memory + expected.render);
        CHECK(system->render.head->allocation_size == expected.region - expected.render);
        CHECK((unsigned char *)system->nested == memory + expected.nested);
        if (shapes[index].nesting > 1u) {
            ta_arena scratch;
            ta_arena render;
            /* A region opens only once a safe point reserved it. */
            CHECK(ta_system_nested_region(system, 1, &scratch, &render) == TA_ERR_NOMEM);
            CHECK(ta_system_nested_reserve(system) == TA_OK);
            CHECK(ta_system_nested_region(system, 1, &scratch, &render) == TA_OK);
            CHECK(system->nested[0].bytes == expected.region);
            CHECK((unsigned char *)scratch.head == system->nested[0].memory);
            CHECK(scratch.head->allocation_size == expected.render);
            CHECK((unsigned char *)render.head == (unsigned char *)system->nested[0].memory + expected.render);
            /* Opening it again reuses the region. */
            uint64_t allocations = heap.allocations;
            CHECK(ta_system_nested_region(system, 1, &scratch, &render) == TA_OK);
            CHECK(heap.allocations == allocations);
        }
        ta_system_destroy(system);
        CHECK(test_allocator_finish(&heap) == 0);
    }
    /* The largest parameters lay out as the sums did. */
    uint32_t params[TA_PARAM_COUNT] = { 0 };
    params[TA_PARAM_INTERPRETER_BYTES] = 1073741824u;
    params[TA_PARAM_RENDER_BYTES] = 1073741824u;
    params[TA_PARAM_INTERPRETER_NESTING] = 64u;
    ta_interpreter_region region = { 0 };
    CHECK(ta_interpreter_region_lay_out(params, &region) == TA_OK);
    system_block expected = sum_system(1073741824u, 1073741824u, 64u);
    CHECK(region.render == expected.render && region.bytes == expected.region);
}

/* ------------------------------------------------------------------------
 * Claim 10: the interpreter scratch a definition needs.
 */

static void sum_take(size_t *at, size_t size, size_t alignment) {
    *at = TA_ALIGN_UP(*at, alignment) + size;
}

/* The arena walk the need was written as before it used the builder. */
static size_t sum_scratch_need(const ta_definition *definition) {
    size_t start = offsetof(ta_arena_block, data);
    size_t at = start;
    uint32_t words = ta_bits_words(definition->state_count);
    for (uint32_t index = 0; index < 11u; index += 1u) {
        sum_take(&at, (size_t)words * sizeof(uint64_t), _Alignof(uint64_t));
    }
    size_t capacity = (size_t)definition->state_count + 1u;
    sum_take(&at, capacity * sizeof(ta_state_id), _Alignof(ta_state_id));
    sum_take(&at, capacity * sizeof(ta_transition_id), _Alignof(ta_transition_id));
    sum_take(&at, capacity * sizeof(ta_transition_id), _Alignof(ta_transition_id));
    sum_take(&at, capacity * sizeof(bool), _Alignof(bool));
    sum_take(&at, (size_t)ta_entry_capacity(definition) * sizeof(struct ta_entry_frame),
             _Alignof(struct ta_entry_frame));
    sum_take(&at, ((size_t)definition->history.slot_count + 1u) * sizeof(ta_state_id), _Alignof(ta_state_id));
    sum_take(&at, (size_t)ta_exec_capacity(definition) * sizeof(ta_exec_frame), _Alignof(ta_exec_frame));
    sum_take(&at, (size_t)ta_decl_words(definition) * sizeof(uint64_t), _Alignof(uint64_t));
    sum_take(&at, (3u * (size_t)definition->state_count + 1u) * sizeof(ta_state_id), _Alignof(ta_state_id));
    sum_take(&at, ((size_t)ta_trace_guard_capacity(definition) + 1u) * sizeof(struct ta_trace_condition),
             _Alignof(struct ta_trace_condition));
    return at - start;
}

static void test_scratch_need(void) {
    /* Definitions with no states, so nothing but these counts is read. */
    static const struct {
        uint32_t max_state_depth;
        uint32_t history_slots;
        uint32_t max_action_depth;
        uint32_t data_decls;
        size_t pinned;
    } shapes[] = {
        { 0, 0, 0, 0, 292 },
        { 1, 0, 0, 0, 436 },
        { 3, 2, 4, 5, 1348 },
        { 10, 7, 3, 130, 3028 },
    };
    bool pin = pinned_sizes();
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(shapes); index += 1u) {
        ta_definition definition = { 0 };
        definition.max_state_depth = shapes[index].max_state_depth;
        definition.history.slot_count = shapes[index].history_slots;
        definition.max_action_depth = shapes[index].max_action_depth;
        definition.data_decl_count = shapes[index].data_decls;
        size_t need = 0;
        CHECK(ta_interpreter_scratch_need(&definition, &need) == TA_OK);
        CHECK(need == sum_scratch_need(&definition));
        if (pin) {
            CHECK(need == shapes[index].pinned);
        }
    }
    for (uint32_t depth = 0; depth <= 6; depth += 1u) {
        for (uint32_t slots = 0; slots <= 3; slots += 1u) {
            for (uint32_t actions = 0; actions <= 5; actions += 1u) {
                ta_definition definition = { 0 };
                definition.max_state_depth = depth;
                definition.history.slot_count = slots;
                definition.max_action_depth = actions;
                definition.data_decl_count = depth * 23u;
                size_t need = 0;
                CHECK(ta_interpreter_scratch_need(&definition, &need) == TA_OK);
                CHECK(need == sum_scratch_need(&definition));
            }
        }
    }

    /* A compiled definition, whose states, guards and history count. */
    test_allocator heap = { 0 };
    ta_allocator allocator = test_allocator_of(&heap);
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    ta_definition *definition = compile_chart(system, allocator);
    if (definition != NULL) {
        CHECK(definition->state_count == 8 && definition->max_action_depth == 2);
        CHECK(ta_trace_guard_capacity(definition) == 1 && ta_entry_capacity(definition) == 16);
        size_t need = 0;
        CHECK(ta_interpreter_scratch_need(definition, &need) == TA_OK);
        CHECK(need == sum_scratch_need(definition));
        size_t continuation = 0;
        CHECK(ta_continuation_capacity(definition, &continuation) == TA_OK);
        if (pin) {
            CHECK(need == 1340);
            CHECK(continuation == 360);
        }
        ta_definition_destroy(definition);
    }
    ta_system_destroy(system);
    CHECK(test_allocator_finish(&heap) == 0);
}

int main(void) {
    test_empty_and_one();   /* claim 1 */
    test_padding();         /* claim 2 */
    test_empty_entries();   /* claim 3 */
    test_null_offset();     /* claim 4 */
    test_failures();        /* claim 5 */
    test_actor_block();     /* claim 6 */
    test_continuation();    /* claim 7 */
    test_operator_jump();   /* claim 8 */
    test_system_regions();  /* claim 9 */
    test_scratch_need();    /* claim 10 */
    if (!pinned_sizes()) {
        fprintf(stderr, "layout_test: struct sizes differ from the pinned ones; compared with the sums only\n");
    }
    if (failures != 0) {
        fprintf(stderr, "layout_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("layout_test: ok\n");
    return 0;
}
