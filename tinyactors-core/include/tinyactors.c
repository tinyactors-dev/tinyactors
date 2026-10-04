/*
 * GENERATED FILE. Do not edit; run `mise run amalgamate` in tinyactors-core.
 * The public API is include/tinyactors.h. Private module headers and sources
 * follow in the order listed by src/internal.h.
 */
#include "tinyactors.h"

/*
 * Functions not declared in tinyactors.h are private to this file. Private
 * helpers may be unused in a given build, so they do not warn.
 */
#ifndef TA_PRIVATE
#if defined(__GNUC__) || defined(__clang__)
#define TA_PRIVATE static __attribute__((unused))
#else
#define TA_PRIVATE static
#endif
#endif


/* Private base header: every implementation module reaches the public API here. */

#define TA_ARRAY_COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

typedef struct {
    uint32_t offset;
    uint32_t length;
} ta_span32;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} ta_actor_key;

/* Freestanding memory helpers over an injected ta_allocator. */
TA_PRIVATE void *ta_mem_alloc(ta_allocator allocator, size_t size, size_t alignment);
TA_PRIVATE void ta_mem_free(ta_allocator allocator, void *ptr, const ta_allocator_free_params *layout);
TA_PRIVATE void ta_mem_copy(void *dst, const void *src, size_t size);
TA_PRIVATE void ta_mem_zero(void *dst, size_t size);
TA_PRIVATE bool ta_str_equal(ta_str left, ta_str right);

/* Bitsets of 64-bit words, used for state sets. */
TA_PRIVATE inline uint32_t ta_bits_words(uint32_t bits) { return (bits + 63u) / 64u; }
TA_PRIVATE inline bool ta_bits_get(const uint64_t *words, uint32_t bit) {
    return (words[bit / 64u] >> (bit % 64u)) & 1u;
}
TA_PRIVATE inline void ta_bits_put(uint64_t *words, uint32_t bit, bool value) {
    uint64_t mask = (uint64_t)1u << (bit % 64u);
    if (value) {
        words[bit / 64u] |= mask;
    } else {
        words[bit / 64u] &= ~mask;
    }
}

/*
 * Bounded text output: writes while there is room and always counts the full
 * length, so one pass both sizes and fills a caller's buffer.
 */
typedef struct {
    char *buffer;
    uint32_t capacity;
    uint32_t length;
    /* Bytes of the text before the window `buffer` holds: a writer with
     * skip s stores text bytes s to s + capacity - 1, so a long text is
     * rendered through a small buffer a window at a time (ADR 0090). */
    uint32_t skip;
} ta_text_writer;

TA_PRIVATE void ta_text_put(ta_text_writer *writer, ta_str text);
TA_PRIVATE void ta_text_put_u64(ta_text_writer *writer, uint64_t value);
/* NUL-terminates; TA_ERR_NOMEM (and nothing useful written) when it did not fit. */
TA_PRIVATE ta_result ta_text_finish(ta_text_writer *writer, uint32_t *needed);
/*
 * Writes text of unknown length into memory from `allocator`: `write` runs
 * once to measure and once to fill. It answers false when there is no text,
 * which becomes TA_ERR_NOT_FOUND.
 */
typedef bool (*ta_text_write_fn)(const void *context, ta_text_writer *writer);
typedef struct {
    ta_text_write_fn write;
    const void *context;
    ta_str *out;
} ta_text_render_params;
TA_PRIVATE ta_result ta_text_render(ta_allocator allocator, const ta_text_render_params *params);
/*
 * ta_str_from_cstr views a NUL-terminated string as a ta_str (block D6's
 * cstr helper, kept beside the type it builds). It copies nothing and
 * allocates nothing: the view lives as long as text does, and core uses it
 * on string literals only. text must not be NULL.
 */
TA_PRIVATE inline ta_str ta_str_from_cstr(const char *text) {
    ta_str out = { .ptr = text, .len = 0 };
    while (text[out.len] != '\0') {
        out.len += 1;
    }
    return out;
}

/* Growable tables (ta_vec, ta_columns) are used throughout core. Included last so vec.h sees all of this header. */



/*
 * Package hash is block D5 of the ring protocol's catalog: the three hash
 * functions core uses, in one place.
 *
 * What it is for. Core hashes in three situations and wants each one to have
 * exactly one implementation:
 *
 *   - bucket keys for tables (atoms, actor addresses, the event-name trie):
 *     ta_fnv1a32, 32 bits, cheap, any distribution that spreads text;
 *   - the content hash of a compiled definition, which is persisted as
 *     definition->content_hash and, when the chart names no revision, written
 *     out as the 16-digit revision fingerprint: ta_fnv1a64;
 *   - integer scrambling: runtime session IDs differ mostly in their low
 *     slot bits, and span IDs are derived from step keys, so both need every
 *     input bit to reach every output bit: ta_mix64.
 *
 * Contract. All three are pure functions of their arguments. They keep no
 * state between calls and read no global. ta_fnv1a32 and ta_fnv1a64 take a
 * running state, not a seed: the result of one call is the state for the
 * next, so hashing "foo" and then "bar" gives the same value as hashing
 * "foobar" in one call. Start from TA_FNV1A32_BASIS or TA_FNV1A64_BASIS.
 * This is why a caller can hash a structure field by field, as the
 * definition compiler does, without building a buffer first.
 *
 * Lifetime. Call. A function owns nothing and keeps no pointer after it
 * returns; "bytes" is only read, and only during the call. The running state
 * a caller threads through is the caller's own value and lives as long as the
 * caller's object: the compiler's state lives for one compile and its final
 * value becomes definition->content_hash (definition lifetime); an atom's
 * hash is stored with the atom entry (system lifetime); a session hash is
 * recomputed on every lookup and stored nowhere.
 *
 * Allocation. Never. There is no allocator parameter, no static mutable
 * state, no failure result. The functions are safe at any point, including
 * inside a macrostep and from the render arena's paths.
 *
 * Invariants. The values are fixed forever. ta_fnv1a64 is persisted
 * (content_hash, revision fingerprints) and ta_mix64 derives span IDs that
 * every host must reproduce, so neither may change; tests/hash_format_test.c
 * pins vectors captured from the code this block replaced. ta_fnv1a32 over
 * integers hashes their in-memory bytes, so its value depends on endianness
 * and is only for in-process tables, never written out.
 *
 * Complexity. ta_fnv1a32 and ta_fnv1a64 are O(size), one xor and one multiply
 * per byte. ta_mix64 is O(1), two multiplies and three shifts.
 *
 * Not here. Neither FNV-1a nor Murmur's finalizer is cryptographic or
 * resistant to chosen input; nothing in core relies on that. Where core
 * needs a pseudo-random sequence rather than a hash (ta_splitmix64 in
 * trace.c) it stays with its one user.
 *
 * Example:
 *
 *     uint32_t bucket = ta_fnv1a32(TA_FNV1A32_BASIS, name.ptr, name.len);
 *
 *     uint64_t state = TA_FNV1A64_BASIS;
 *     state = ta_fnv1a64(state, &length, sizeof(length));
 *     state = ta_fnv1a64(state, text, length);        // same as one call
 *
 *     uint32_t slot = (uint32_t)ta_mix64(session_id);  // spread low bits
 */

/* TA_FNV1A32_BASIS is the FNV-1a offset basis of the 32-bit variant: the state to start a hash from. */
#define TA_FNV1A32_BASIS 2166136261u
/* TA_FNV1A64_BASIS is the FNV-1a offset basis of the 64-bit variant: the state to start a hash from. */
#define TA_FNV1A64_BASIS 14695981039346656037ull

/*
 * ta_fnv1a32 continues a 32-bit FNV-1a hash over size bytes and returns the
 * new state. Start from TA_FNV1A32_BASIS; pass the result back as state to
 * add more bytes. size 0 returns state unchanged, and bytes may be NULL
 * then. It reads bytes[0..size) during the call, keeps nothing, never
 * allocates. The value of multi-byte integers depends on endianness, so do
 * not persist it.
 */
TA_PRIVATE uint32_t ta_fnv1a32(uint32_t state, const void *bytes, size_t size);

/*
 * ta_fnv1a64 is ta_fnv1a32 in 64 bits. Its value is persisted as a
 * definition's content hash and revision fingerprint, so it is fixed
 * forever. size 0 returns state unchanged; bytes may be NULL then.
 */
TA_PRIVATE uint64_t ta_fnv1a64(uint64_t state, const void *bytes, size_t size);

/*
 * ta_mix64 is the finalizer of MurmurHash3's 64-bit variant (fmix64): every
 * input bit reaches every output bit, and ta_mix64(0) is 0. Fixed, because
 * span IDs derive from it and every host must agree. Truncate to 32 bits
 * with a cast for a table index; the low bits are as good as any.
 */
TA_PRIVATE uint64_t ta_mix64(uint64_t value);



/*
 * Package handle is block E1 of the ring protocol's catalog: the one place
 * that knows how a generation and a slot become a 64-bit ID.
 *
 * What it is for. Actors and definitions live in slot tables that reuse
 * freed slots. A bare index would then name a different occupant after a
 * reuse, so every ID carries the slot's generation too:
 *
 *     handle = (generation << 32) | slot
 *
 * ta_actor_id and ta_definition_id are both uint64_t and both use this
 * layout (ADR 0059, ADR 0043), so they pass as a ta_handle without a cast.
 * The layout is persisted in traces and snapshots and visible to every
 * host; it never changes. The public ta_actor_id_slot, _generation and
 * _make are one-line calls into this block, so the layout exists once.
 *
 * Contract. Every function is pure and O(1). Any (slot, generation) pair is
 * representable. Generation 0 is never issued, so the zero handle, and any
 * handle with generation 0, was never a real ID. Slot 0 is a valid slot.
 *
 * Staleness. Freeing a slot stores ta_handle_next_generation(current) in it,
 * so every handle issued before the free stops matching: the table answers
 * NOT_FOUND (actors) or NULL (definitions) through ta_handle_matches. A
 * stale handle is still a perfectly good value that unpacks to the same slot
 * and generation; only the table can say it is stale. A slot can be reused
 * 2^32 - 2 times before its generation wraps back to a value an old handle
 * may hold (ADR 0059 accepts this).
 *
 * Lifetime. A handle is a value with call lifetime: it owns nothing and
 * points at nothing, and is copied freely, stored wherever its holder keeps
 * it. The slot it names lives as long as its table, which is system
 * lifetime for the actor and definition tables; a slot is never returned to
 * the allocator before ta_system_destroy, only recycled with a new
 * generation.
 *
 * Allocation. Never. No function takes an allocator, none can fail, none
 * touches the heap, a ta_layout or any global. They are safe at any point,
 * including inside a macrostep.
 *
 * Example:
 *
 *     ta_handle id = ta_handle_make(slot, entry->generation);   // issue
 *     if (ta_handle_matches(id, entry->generation)) { ... }     // look up
 *     entry->generation = ta_handle_next_generation(entry->generation); // free
 *     // ta_handle_matches(id, entry->generation) is now false: id is stale.
 */

typedef uint64_t ta_handle;

/*
 * ta_handle_make packs a slot and the generation that slot holds. Any pair
 * is representable; (0, 0) is the zero handle, which is never valid.
 */
TA_PRIVATE ta_handle ta_handle_make(uint32_t slot, uint32_t generation);

/* ta_handle_slot returns the low 32 bits: the index of the slot in its table. */
TA_PRIVATE uint32_t ta_handle_slot(ta_handle handle);

/* ta_handle_generation returns the high 32 bits: the generation the slot had when the handle was issued. */
TA_PRIVATE uint32_t ta_handle_generation(ta_handle handle);

/*
 * ta_handle_valid reports whether the handle can have been issued: its
 * generation is not 0. Slot 0 with generation 1 is valid. It says nothing
 * about whether the slot still holds the thing; only the table knows that.
 */
TA_PRIVATE bool ta_handle_valid(ta_handle handle);

/*
 * ta_handle_matches reports whether the handle is valid and its generation
 * equals current_generation, the one now stored in its slot, i.e. the handle
 * is not stale. The caller has already checked that the slot is in range.
 */
TA_PRIVATE bool ta_handle_matches(ta_handle handle, uint32_t current_generation);

/*
 * ta_handle_next_generation returns the generation a slot takes when it is
 * freed: current + 1, except that UINT32_MAX (and 0) go to 1, so a slot is
 * never issued generation 0. The caller stores it at the moment it frees the
 * slot, which is what makes older handles stale.
 */
TA_PRIVATE uint32_t ta_handle_next_generation(uint32_t current_generation);



/*
 * Package format is block D6 of the ring protocol's catalog: the small
 * formatters core uses to write numbers as text, in one place.
 *
 * What it is for. Core writes decimal numbers (send IDs, counters in error
 * messages), lowercase hex (trace and span IDs, revision fingerprints, \xHH
 * escapes), uppercase hex (percent-encoding in URIs), and reads a hex digit
 * back (percent-decoding). Each used to be written out locally, three times
 * for decimal and four for hex. This block is the one copy. It also owns
 * reading a hex digit, ta_hex_digit_value, so writer and reader live side
 * by side.
 *
 * Contract. Every function works on a buffer the caller supplies and sizes.
 * Nothing here writes a terminating NUL, formats a sign, or pads, except
 * where a function says so. The functions measure and write separately, so a
 * caller can measure, reserve exactly that many bytes, then fill them:
 *
 *     uint32_t digits = ta_dec_len(serial);
 *     char *text = ta_arena_alloc(&arena, prefix + digits, 1);
 *     if (text == NULL) { ... }             // the caller's failure path
 *     ta_dec_write(text + prefix, serial);
 *
 * Lifetime. Call. A function writes into the caller's buffer and keeps no
 * pointer after it returns. The destination decides how long the text
 * lives: a stack buffer (the call), the render arena (until the macrostep's
 * next render reset) or a definition arena (the definition's lifetime).
 *
 * Allocation. Never. There is no allocator parameter and no failure result;
 * the only allocation near these functions is the caller's, which keeps its
 * own safe point and its own failure path. Safe at any point, including
 * inside a macrostep.
 *
 * Invariants. A buffer shorter than the documented count is a caller bug,
 * not a runtime error; the documented upper bound for decimal is
 * TA_DEC_U64_DIGITS, and a caller that cannot know the value in advance
 * sizes a stack buffer by it. Output is deterministic and independent of the
 * locale. Hex output is lowercase unless the caller asks for uppercase.
 *
 * Complexity. O(digits) for decimal (a division per digit), O(count) for
 * hex, O(1) for the digit helpers.
 *
 * Not here. The traceparent readers in trace.c (ta_hex_read and friends) are
 * tuned word-at-a-time parsers for one fixed format (ADR 0073) and stay with
 * their user. The decimal parsers stay with the lexers that own their error
 * handling. ta_str_from_cstr, which turns a NUL-terminated literal into a
 * ta_str, is the block's third helper and lives in types.h beside the type
 * it builds.
 *
 * Example:
 *
 *     char digits[TA_DEC_U64_DIGITS];
 *     uint32_t count = ta_dec_write(digits, value);
 *     ta_text_put(writer, (ta_str){ .ptr = digits, .len = count });
 *
 *     char span[16];
 *     ta_hex_write(span, span_id, 16u);        // "00f067aa0ba902b7"
 */

/* TA_DEC_U64_DIGITS is the number of decimal digits of UINT64_MAX: the size of a buffer that holds any value ta_dec_write can write. */
#define TA_DEC_U64_DIGITS 20u

/*
 * ta_dec_len returns the number of decimal digits of value, 1 for 0. It lets
 * a caller measure, reserve exactly that many bytes, then fill with
 * ta_dec_write. Never allocates.
 */
TA_PRIVATE uint32_t ta_dec_len(uint64_t value);

/*
 * ta_dec_write writes value in decimal at out and returns the number of
 * bytes written, which is exactly ta_dec_len(value) and at most
 * TA_DEC_U64_DIGITS. It writes no NUL and touches no byte past that count.
 * out must have room for it; out is only written during the call.
 */
TA_PRIVATE uint32_t ta_dec_write(char *out, uint64_t value);

/*
 * ta_hex_digit returns the ASCII digit for a nibble, lowercase or, when
 * upper is true, uppercase. Only the low four bits of nibble are used, so a
 * caller may pass an unmasked byte shifted down.
 */
TA_PRIVATE char ta_hex_digit(uint32_t nibble, bool upper);

/*
 * ta_hex_write writes the low count hex digits of value at out, most
 * significant first, lowercase, keeping leading zeros: exactly count bytes,
 * no NUL. count must be 1 to 16; out must have room for it.
 */
TA_PRIVATE void ta_hex_write(char *out, uint64_t value, uint32_t count);

/*
 * ta_hex_digit_value returns the value 0 to 15 of a hex digit in either
 * case, or -1 for any other character. It is the inverse of ta_hex_digit
 * for the characters that function writes.
 */
TA_PRIVATE int ta_hex_digit_value(char digit);



/*
 * Package layout computes where the regions of one allocation go.
 *
 * Several things in core are one allocation holding a header and then
 * arrays or sub-objects behind it: the actor block (the actor, its session
 * state, the data model's instance), a continuation (sets, declarations,
 * history, enabled transitions, frames), the interpreter's scratch (eleven
 * state sets and a handful of stacks), a system's arena region (structural
 * arena, render arena, table of nested regions), an operator jump (header
 * and its targets). Each needs the same arithmetic: round the running end
 * up to the next region's alignment, add the region's `count * size`, and
 * remember the strictest alignment seen. This unit is that arithmetic, once,
 * with every step checked, so that a size an input can influence is either
 * correct or reported as a failure, and is never a number that wrapped
 * around and is smaller than the data it must hold.
 *
 * Lifetime: call. A ta_layout is a small value on the caller's stack. It
 * owns nothing, points at nothing and never allocates, so there is nothing
 * to free and no failure that leaves state behind. Only the numbers computed
 * from it (offsets, a total, an alignment) outlive it, and they are stored
 * by the thing they describe, once, where it is allocated:
 *   - the actor block: offsets in the definition (the offsets are fixed at
 *     compile time and the sized free at ta_actor_destroy depends on them);
 *   - a continuation: its array offsets and `bytes` in its header
 *     (ta_continuation_bytes, called by ta_reserve_stop and the restore of
 *     an image);
 *   - an operator jump: `bytes` in its header (ta_operator_jump_bytes);
 *   - a system's arena region: the total in the system, for the sized free
 *     at ta_system_destroy.
 * Nothing recomputes such a number later from counts, in a function that
 * could not report that the layout failed.
 *
 * Contract:
 *   - Entries are placed in the order they are added. Each entry's offset is
 *     the running end rounded up to a multiple of the entry's alignment;
 *     the running end then moves by `size * count`.
 *   - The total is the unpadded end of the last entry. It is not rounded up
 *     to the layout's strictest alignment. That is deliberate: it is what
 *     the hand-written calculators this unit replaced returned, and the
 *     size of an actor block, a continuation or a scratch figure is a sized
 *     free (ta_mem_free checks the size it is given) and a per-actor cost
 *     that must not grow. A caller that needs a padded total asks for the
 *     alignment too and pads it itself (ta_layout_finish gives both).
 *   - A zero count takes no bytes, but the entry's offset still honours its
 *     alignment, as an array of length zero at that place would.
 *   - An alignment is a non-zero number; the arithmetic is a remainder, so
 *     it needs no power of two, but every alignment in core is one.
 *   - Offsets are measured from the layout's origin, and the first byte is
 *     at `start` (a block header, or the data offset of an arena block).
 *     The origin is assumed to be aligned to the strictest alignment of the
 *     layout, which is what allocating the result with that alignment makes
 *     true.
 *
 * Failure: every size computation is checked. When one does not fit in a
 * size_t (on a 32-bit host such as WebAssembly that is 4 GiB, and counts
 * are 32-bit values times element sizes, so it is reachable), or an
 * alignment is zero, the layout is marked failed, and the mark is sticky:
 * later entries cannot clear it. ta_layout_add keeps returning a number so
 * that a caller can build all its offsets in straight-line code, but that
 * number must not be used until ta_layout_finish has answered TA_OK for the
 * whole layout. Checking once at the end is what makes a long sequence of
 * entries as easy to write as the unchecked arithmetic was.
 *
 * "Overflowed" is representable only as a ta_result. There is no function
 * that answers a size with 0 or SIZE_MAX for failure: the only way to read a
 * total from a layout is ta_layout_finish, which answers TA_ERR_NOMEM and
 * writes nothing on failure. It is called where the allocation is made, a
 * safe point that can fail cleanly, and its result is always used. The
 * fields of a ta_layout are private to layout.c (their names start with
 * `internal_`). `mise run check-layout` enforces this, with ast-grep rules
 * (mise/ast-grep/rules/layout-*.yml):
 *   - a function that calls ta_layout_add also calls ta_layout_finish;
 *   - the result of ta_layout_finish is never discarded;
 *   - no code outside layout.c reads or writes a ta_layout's fields;
 *   - nothing is allocated, copied or freed between the adds and the finish
 *     of a function, and ta_layout_add is not used as an operand.
 *
 * Allocation: none, ever. A function that builds a layout may therefore be
 * called at any point, including where allocation is forbidden. The
 * allocation a layout describes is made by the caller, once, at a safe point
 * (with ta_mem_alloc, with the finished size and alignment), and freed with
 * the same size and alignment.
 *
 * Complexity: O(1) per entry, O(entries) per layout, O(1) space.
 *
 * Usage, an object of a header, an array of 16-bit counters and a table of
 * 64-bit words, allocated as one block:
 *
 *	ta_layout layout;
 *	ta_layout_init(&layout, 0);
 *	size_t header = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(header_t), _Alignof(header_t), 1 });
 *	size_t counters = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(uint16_t), _Alignof(uint16_t), n });
 *	size_t words = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(uint64_t), _Alignof(uint64_t), m });
 *	size_t bytes, alignment;
 *	if (ta_layout_finish(&layout, &bytes, &alignment) != TA_OK) {
 *		return TA_ERR_NOMEM;
 *	}
 *	unsigned char *block = ta_mem_alloc(allocator, bytes, alignment);
 *	// header, counters and words are now valid offsets into block.
 */

/*
 * A running layout. Its fields are for layout.c: callers use the functions,
 * so that the checks cannot be skipped, and the names are unique so that a
 * static check can find any other use (see the package comment). It is a
 * value, copyable (a caller may finish a copy to learn the size of a prefix
 * and go on adding to the original), with no pointer in it.
 */
typedef struct ta_layout {
    size_t internal_end;       /* bytes laid out so far, counted from the layout's origin */
    size_t internal_alignment; /* the strictest alignment any entry asked for; at least 1 */
    bool internal_overflowed;  /* a size computation did not fit, or an alignment was zero; sticky */
} ta_layout;

/*
 * One region: `count` items of `size` bytes each, placed at a multiple of
 * `alignment`. A passed entry is read during the call and not kept.
 */
typedef struct ta_layout_entry {
    size_t size;      /* bytes of one item */
    size_t alignment; /* never 0 */
    size_t count;     /* items; 0 is allowed and takes no bytes, but the offset still honours alignment */
} ta_layout_entry;

/*
 * ta_layout_init starts a layout whose first free byte is at offset `start`
 * from the origin. A block header goes here (`sizeof` of it) or the data
 * offset of an arena block; 0 for a layout that begins at the origin. It
 * never fails and never allocates.
 */
TA_PRIVATE void ta_layout_init(ta_layout *layout, size_t start);

/*
 * ta_layout_add appends one entry and returns its offset from the origin:
 * the running end rounded up to the entry's alignment. It then moves the
 * end past `size * count` bytes and raises the layout's alignment to the
 * entry's if that is stricter. When any step does not fit in a size_t, or
 * the alignment is zero, it marks the layout failed and returns 0; 0 is also
 * a real offset, so the caller must not use any offset until
 * ta_layout_finish has answered TA_OK.
 * It never allocates and never reads memory behind the entry.
 */
TA_PRIVATE size_t ta_layout_add(ta_layout *layout, const ta_layout_entry *entry);

/*
 * ta_layout_finish reports the unpadded total in `*bytes` and the strictest
 * alignment in `*alignment`; an output may be NULL if the caller has no use
 * for it. It answers TA_ERR_NOMEM and writes neither output if any step of
 * the layout failed, so a caller that propagates the result leaves no
 * half-built state: nothing was allocated, and nothing was written. The
 * result must be used: it is the only place a failed layout shows.
 */
TA_PRIVATE ta_result ta_layout_finish(const ta_layout *layout, size_t *bytes, size_t *alignment);

/*
 * ta_checked_mul stores `a * b` in `*out` and answers true, or answers false
 * and leaves `*out` untouched when the product does not fit in a size_t.
 * It is the multiplication underneath ta_layout_add, exported for the size
 * estimates that sum several layouts (session_image.c).
 */
TA_PRIVATE bool ta_checked_mul(size_t a, size_t b, size_t *out);

/*
 * ta_checked_add stores `a + b` in `*out` and answers true, or answers false
 * and leaves `*out` untouched when the sum does not fit in a size_t.
 */
TA_PRIVATE bool ta_checked_add(size_t a, size_t b, size_t *out);



/*
 * Package vec holds core's two growable tables (building block B6): ta_vec,
 * one array of fixed-size elements, and ta_columns, several parallel arrays
 * that share one count and one capacity and live in a single allocation.
 *
 * Why two. A ta_vec is a 48-byte value (allocator, pointer, count, capacity,
 * element size and alignment). That is the right price for a handful of
 * tables per system (the page tables, the definitions list, the atom table)
 * and for a per-actor table that is allocated only by an uncommon facility,
 * but it is the wrong price for a table a per-actor record carries twice.
 * A ta_columns is 16 bytes (block, count, capacity): the allocator comes
 * from the caller (the system already owns it) and the element shapes come
 * from a ta_columns_layout that is static const data, one per kind of table,
 * not one per instance. The active-invoke table of an actor session is the
 * user: its public records (ta_active_invoke, which ta_actor_inspect hands
 * to the host as a contiguous array) and its private records
 * (ta_invoke_meta) are two columns of one table.
 *
 * What is deliberately not here. The queue of blocks that one export call
 * builds (export.c) is not a growable array: it is carved once from the
 * call's arena at a size computed up front and checked, and it never grows.
 * It is not a ta_vec either, because a ta_vec frees with the allocator and
 * arena memory is never freed one block at a time. The timer lists and the
 * event-block queue grow by doubling too, but they are deleted by a later
 * phase of the ring-proto plan, so they keep their own code.
 *
 * Lifetime. Both tables are owned by whatever record embeds them and live
 * exactly as long as it does:
 *   - the active-invoke table (a ta_columns): session. It is created lazily
 *     at the first <invoke>, grows only when an invoke starts, and is
 *     released with the session's sparse state (ta_invoke_release);
 *   - pending_invokes and the activation effects (ta_vec): session;
 *   - the page tables of the session, actor and address tables, the atom
 *     table and the definitions list (ta_vec): system, freed by the system's
 *     destroy;
 *   - the exit frames of the interpreter (ta_vec): call.
 * A table never outlives its owner, and nothing but its owner frees it.
 *
 * Moving storage. Growing a table replaces its block. Every pointer into the
 * old block (ta_vec_at, ta_columns_at, or `items`) is dangling afterwards,
 * so a caller keeps an index across anything that can grow the table and
 * re-derives the pointer. The one pointer that is handed out on purpose,
 * ta_actor_inspection.invokes, is valid until the actor's next macrostep.
 *
 * When it allocates. Only ta_vec_reserve, ta_vec_extend, ta_vec_push and
 * ta_columns_grow allocate, and only the owner's safe points call them:
 * the end of a macrostep, loading or restoring a session, building a
 * definition, or creating a system. No unit of executable content (an
 * action, a guard, an expression) may call them: it could not report the
 * failure without leaving the macrostep half done. Every other function in
 * this unit never allocates, including ta_columns_at and the byte counts.
 *
 * Failure. Every size is computed with ta_checked_mul / ta_checked_add or
 * with ta_layout, so a count an input can influence either fits or fails;
 * nothing wraps. An allocation that fails, or a size that does not fit,
 * answers TA_ERR_NOMEM and leaves the table exactly as it was: same block,
 * same count, same capacity, same contents. A grow allocates the new block
 * first, copies, and only then frees the old one, so there is no moment at
 * which the table owns nothing.
 *
 * Sized frees. ta_mem_free is told the size and alignment of the block it
 * frees. Both are recomputed from the capacity by the same function that
 * sized the allocation (ta_vec_bytes, ta_columns_bytes), so the free
 * matches the allocation exactly; a strict test allocator reports any
 * mismatch as a bad free. Nothing else in core computes these sizes.
 *
 * Growth policy. A ta_vec starts at 8 elements and doubles; a ta_columns
 * starts at its layout's first_capacity and doubles. Doubling keeps pushes
 * amortised O(1) and never leaves more than half the capacity unused.
 *
 * Complexity. ta_vec_at is O(1). ta_columns_at is O(columns) (it places the
 * columns with ta_layout; a table has two or three). A grow is O(count) time
 * and O(capacity) space for the new block.
 *
 * Invariants. count <= capacity; capacity == 0 exactly when the table owns
 * no block; the zero value of a ta_columns is an empty table, valid for
 * every function here; a ta_vec must have been through ta_vec_init.
 *
 * Usage, a table of two parallel arrays (an identifier and a counter):
 *
 *	static const ta_column columns[] = {
 *		{ sizeof(uint32_t), _Alignof(uint32_t) },
 *		{ sizeof(uint16_t), _Alignof(uint16_t) },
 *	};
 *	static const ta_columns_layout layout = { columns, 2, 4 };
 *
 *	ta_columns table = { 0 };
 *	if (ta_columns_grow(allocator, &layout, &table) != TA_OK) {
 *		return TA_ERR_NOMEM;       // table is untouched
 *	}
 *	uint32_t index = table.count;
 *	table.count += 1;
 *	((uint32_t *)ta_columns_at(&layout, &table, 0))[index] = id;
 *	((uint16_t *)ta_columns_at(&layout, &table, 1))[index] = 0;
 *	...
 *	ta_columns_release(allocator, &layout, &table);
 */

/*
 * A growable array of fixed-size elements. `items` is reallocated as it
 * grows, so callers keep indices, never element pointers, across a push.
 */
typedef struct {
    ta_allocator allocator;
    void *items;
    uint32_t count;
    uint32_t capacity;
    size_t element_size;
    size_t element_alignment;
} ta_vec;

#define TA_VEC_LAYOUT(T) (&(ta_tree_list_layout){ .element_size = sizeof(T), .element_alignment = _Alignof(T) })

/*
 * ta_vec_init makes an empty vector of elements shaped by `layout`, taking
 * memory from `allocator` for as long as the vector lives. It allocates
 * nothing and cannot fail; `layout` is read during the call and not kept.
 */
TA_PRIVATE void ta_vec_init(ta_vec *vec, ta_allocator allocator, const ta_tree_list_layout *layout);

/*
 * ta_vec_destroy frees the vector's block with the exact size and alignment
 * it was allocated with, and leaves an empty vector that may be used again.
 * It never allocates. Calling it on an empty vector is a no-op.
 */
TA_PRIVATE void ta_vec_destroy(ta_vec *vec);

/*
 * ta_vec_reserve makes room for `count` elements in total (not `count` more),
 * doubling the capacity from 8. It allocates, so only a safe point may call
 * it. On TA_ERR_NOMEM (the allocator refused, or the size does not fit in a
 * size_t) the vector is unchanged, with its old block, count and contents.
 * Any pointer into the old block is invalid after a successful grow.
 */
TA_PRIVATE ta_result ta_vec_reserve(ta_vec *vec, uint32_t count);

/*
 * ta_vec_extend appends `count` zeroed elements and stores the index of the
 * first in `*out` (which may be NULL). It allocates through
 * ta_vec_reserve and has the same failure behaviour: on TA_ERR_NOMEM
 * nothing was appended and `*out` was not written.
 */
TA_PRIVATE ta_result ta_vec_extend(ta_vec *vec, uint32_t count, uint32_t *out);

/*
 * ta_vec_push copies one element, of the vector's element size, to the end.
 * It allocates through ta_vec_extend; on failure nothing was appended.
 */
TA_PRIVATE ta_result ta_vec_push(ta_vec *vec, const void *element);

/*
 * ta_vec_at returns the address of element `index`. It never allocates and
 * does not check the index: it must be below capacity. The address is valid
 * until the next call that can grow the vector.
 */
TA_PRIVATE void *ta_vec_at(const ta_vec *vec, uint32_t index);

/*
 * ta_vec_bytes is the size of the vector's block (capacity * element size),
 * 0 when it owns none. It is the size ta_vec_destroy frees, and what
 * ta_actor_inspect reports as the vector's cost. The product of a vector
 * that grew fits in a size_t, because the grow checked it.
 */
TA_PRIVATE size_t ta_vec_bytes(const ta_vec *vec);

/*
 * One column of a ta_columns table: the size and alignment of one item.
 */
typedef struct {
    size_t size;
    size_t alignment;
} ta_column;

/*
 * The shape of a kind of ta_columns table. It is static const data shared by
 * every table of that kind: it is not stored in the table, which is why a
 * table costs 16 bytes. `first_capacity` is the capacity of the first block
 * and must not be 0. The columns sit in the block in order, each at a
 * multiple of its alignment, so column 0 is at offset 0.
 */
typedef struct {
    const ta_column *columns;
    uint32_t column_count;
    uint32_t first_capacity;
} ta_columns_layout;

/*
 * A table of parallel arrays in one allocation. `block` is column 0 as well
 * as the start of the allocation (it may be read as a pointer to column 0's
 * items, as ta_actor_sparse_state does for `invokes`). Fields are public so
 * that the owner can read count and bump it after filling in an item. The
 * zero value is an empty table.
 */
typedef struct {
    void *block;
    uint32_t count;
    uint32_t capacity;
} ta_columns;

/*
 * ta_columns_grow makes room for one more item: it does nothing when
 * count < capacity, and otherwise doubles the capacity (from the layout's
 * first_capacity). It allocates one block, copies the first `count` items
 * of every column, then frees the old block with its exact size. Only a
 * safe point may call it. On TA_ERR_NOMEM (the allocator refused, or a size
 * does not fit) `*table` is untouched. Pointers from ta_columns_at are
 * invalid after a successful grow that reallocated. (One item at a time,
 * not "n more": core's rule of at most three parameters leaves no room for
 * a count, and the only user adds one invoke at a time.)
 */
TA_PRIVATE ta_result ta_columns_grow(ta_allocator allocator, const ta_columns_layout *layout, ta_columns *table);

/*
 * ta_columns_at returns the first item of column `index`. The offset is
 * recomputed from the capacity (O(columns)), so a function that uses a
 * column in a loop reads it once into a local. It never allocates. It
 * answers NULL for an empty table or a column out of range. The pointer is
 * valid until the next ta_columns_grow or ta_columns_release.
 */
TA_PRIVATE void *ta_columns_at(const ta_columns_layout *layout, const ta_columns *table, uint32_t index);

/*
 * ta_columns_bytes is the size of the table's block, 0 when empty: what
 * ta_columns_release frees and what an allocated-bytes figure adds up. It
 * never allocates.
 */
TA_PRIVATE size_t ta_columns_bytes(const ta_columns_layout *layout, const ta_columns *table);

/*
 * ta_columns_release frees the block with the size and alignment it was
 * allocated with and leaves the empty table. It never allocates, and calling
 * it on an empty table is a no-op. The owner has already dealt with the
 * items (cancelled an invoke, released a child): this frees memory only.
 */
TA_PRIVATE void ta_columns_release(ta_allocator allocator, const ta_columns_layout *layout, ta_columns *table);



struct ta_arena_block {
    struct ta_arena_block *next;
    size_t allocation_size;
    size_t alignment;
    size_t capacity;
    size_t used;
    unsigned char data[];
};

/* Copies text into the arena. An empty string yields a non-null empty ta_str. */
TA_PRIVATE ta_result ta_arena_copy_str(ta_arena *arena, ta_str text, ta_str *out);
/*
 * An arena over `bytes` of memory the caller owns, aligned for
 * ta_arena_block, which never grows (ADR 0092): an allocation that does not
 * fit answers NULL. Reset and destroy work as for any arena; destroy frees
 * nothing, since the memory is the caller's.
 */
TA_PRIVATE void ta_arena_init_fixed(ta_arena *arena, void *memory, size_t bytes);
/*
 * ta_arena_fixed_add adds to `layout` the memory one fixed arena needs to
 * offer `capacity` bytes (its block header, then the data), and returns the
 * offset where it starts. The arena block's size is a fact of this unit, so
 * the layout's checked arithmetic covers the sum: nothing adds a capacity to
 * a header by hand. As for ta_layout_add, the offset means something only
 * after ta_layout_finish has answered TA_OK. Never allocates.
 */
TA_PRIVATE size_t ta_arena_fixed_add(ta_layout *layout, size_t capacity);
/* Bytes the arena holds from its backing allocator, block headers included. */
TA_PRIVATE size_t ta_arena_allocated_bytes(const ta_arena *arena);



/*
 * Storage behind the public ta_tree / ta_tree_list (include/tinyactors.h).
 *
 * An importer authors a specification tree bottom-up: the transitions of a
 * state are opened, filled and closed while the state list is still open. A
 * single bump region cannot serve that, because the inner list's elements
 * would land between the outer list's. Each open list therefore stages its
 * elements in a chain of chunks of its own, and ta_tree_close() copies the
 * chain into one contiguous, zero-terminated array. Every allocation, the
 * staging chunks included, comes from one arena the tree owns, so
 * ta_tree_destroy() returns all of it and no list needs a matching close.
 */

typedef struct ta_tree_chunk {
    struct ta_tree_chunk *next;
    unsigned char *elements;
    size_t count;
    size_t capacity;
} ta_tree_chunk;

/* One open list: its chunks in insertion order, and what it has staged. */
typedef struct {
    ta_tree_chunk *first;
    ta_tree_chunk *last;
    size_t count;
    size_t next_capacity;
} ta_tree_list_state;

typedef struct {
    ta_arena arena;
} ta_tree_state;



/* Atom text lives in arena blocks, so a returned ta_str never moves. */
typedef struct {
    uint32_t hash;
    uint32_t next;
    ta_str text;
} ta_atom_entry;

typedef struct {
    ta_allocator allocator;
    ta_vec entries;
    uint32_t *buckets;
    uint32_t bucket_count;
    ta_arena strings;
} ta_atom_table;

#define TA_ATOM_NONE UINT32_MAX

TA_PRIVATE ta_result ta_atom_table_init(ta_atom_table *table, ta_allocator allocator);
TA_PRIVATE void ta_atom_table_destroy(ta_atom_table *table);
TA_PRIVATE ta_result ta_atom_intern(ta_atom_table *table, ta_str text, ta_atom *out);
TA_PRIVATE ta_str ta_atom_text(const ta_atom_table *table, ta_atom atom);
/* Looks text up without interning it; TA_ERR_NOT_FOUND when it is not an atom. */
TA_PRIVATE ta_result ta_atom_find(const ta_atom_table *table, ta_str text, ta_atom *out);
TA_PRIVATE bool ta_atom_valid(const ta_atom_table *table, ta_atom atom);



/*
 * Interned named addresses (ADR 0065). A system keeps one record per
 * distinct (family, name) pair that anyone holds; a record is freed with its
 * last reference.
 *
 * The table is a linear hash table (Litwin): it grows one bucket at a time,
 * each insert splitting at most one bucket, so no single operation's cost
 * grows with the number of addresses. Buckets live in fixed pages found
 * through a small page directory, as the actor table's slots do (ADR 0063),
 * so growing never copies the buckets. A failed page allocation only skips
 * a split; the only allocation that can fail an intern is the record's own.
 */

#define TA_ADDRESS_PAGE_BUCKETS 1024u
#define TA_ADDRESS_INITIAL_BUCKETS 16u

typedef struct ta_address_table ta_address_table;

struct ta_address {
    /* Null for a detached record: a session image's own copy (see below). */
    ta_address_table *table;
    struct ta_address *next;
    /* The unloaded session at this address (ADR 0066), or null. */
    struct ta_session_node *unloaded;
    uint32_t references;
    uint32_t hash;
    uint32_t family_len;
    uint32_t name_len;
    /* The family, a NUL, the name, a NUL. */
    char text[];
};

struct ta_address_table {
    ta_allocator allocator;
    ta_vec pages; /* ta_address ** pages of TA_ADDRESS_PAGE_BUCKETS buckets */
    /* Linear hashing: `base` buckets hashed with base - 1, and the first
     * `split` of them already split with 2 * base - 1. */
    uint32_t base;
    uint32_t split;
    uint32_t count;
};

TA_PRIVATE void ta_address_table_init(ta_address_table *table, ta_allocator allocator);
/* Frees every record, held or not; references held past this are dangling. */
TA_PRIVATE void ta_address_table_destroy(ta_address_table *table);
TA_PRIVATE ta_result ta_address_table_intern(ta_address_table *table, const ta_actor_address *address,
                                  ta_address **out);

/*
 * A detached copy in `arena`: the same strings, no table, no counting.
 * Retain and release do nothing on it. Session images carry their addresses
 * this way, so an image does not depend on the system that captured it
 * (ADR 0028); loading interns them again.
 */
TA_PRIVATE ta_result ta_address_detach(ta_arena *arena, const ta_address *address, ta_address **out);
TA_PRIVATE bool ta_address_in(const ta_address *address, const ta_address_table *table);



/*
 * Stored URIs of I/O origins (ADR 0068): SCOPED origins name a record in the
 * system's scope table by index and generation; MESSAGE origins point at a
 * reference-counted record in the entry table. Records are fixed-size, live
 * in pages that never move, and are reused through a free list, so a URI of
 * up to TA_IO_ORIGIN_INLINE bytes costs no allocation after warm-up.
 */
#define TA_IO_ORIGIN_INLINE 48u
#define TA_IO_ORIGIN_PAGE 256u

typedef struct ta_io_origin_table ta_io_origin_table;

struct ta_io_origin_entry {
    /* MESSAGE: references held by events. SCOPED: the record's own index. */
    uint32_t refs;
    /* SCOPED: incremented when the scope closes, so stale handles miss. */
    uint32_t generation;
    /* The owning table; null for a detached copy in a session image. */
    ta_io_origin_table *table;
    /* The processor's registry index; unused in a detached copy. */
    uint32_t processor;
    /* The URI's length; UINT32_MAX while the record is free. */
    uint32_t length;
    /* A URI longer than the inline buffer; the next free record while free. */
    char *spill;
    /* The URI when it fits; a detached copy keeps its processor type text here. */
    char text[TA_IO_ORIGIN_INLINE];
};

struct ta_io_origin_table {
    ta_allocator allocator;
    /* A scope table keeps each record's index in `refs`; an entry table counts references there. */
    bool scoped;
    ta_vec pages; /* ta_io_origin_entry *, TA_IO_ORIGIN_PAGE records each */
    uint32_t issued;
    uint32_t live;
    ta_io_origin_entry *free;
};

TA_PRIVATE void ta_io_origin_table_init(ta_io_origin_table *table, ta_allocator allocator, bool scoped);
TA_PRIVATE void ta_io_origin_table_destroy(ta_io_origin_table *table);

/* A live record holding a copy of `uri`, with its index in `*index`; null when out of memory. */
typedef struct {
    uint32_t processor;
    ta_str uri;
    uint32_t *index;
} ta_io_origin_take_params;
TA_PRIVATE ta_io_origin_entry *ta_io_origin_take(ta_io_origin_table *table, const ta_io_origin_take_params *params);
/* Frees a record's spill and returns it to its table's free list. */
TA_PRIVATE void ta_io_origin_give(ta_io_origin_entry *entry);
/* The live record at `index`, or null. */
TA_PRIVATE ta_io_origin_entry *ta_io_origin_at(const ta_io_origin_table *table, uint32_t index);

/* MESSAGE references; both ignore null and detached records. */
TA_PRIVATE void ta_io_origin_entry_retain(ta_io_origin_entry *entry);
TA_PRIVATE void ta_io_origin_entry_release(ta_io_origin_entry *entry);

/* The stored URI. */
TA_PRIVATE ta_str ta_io_origin_entry_uri(const ta_io_origin_entry *entry);

/* A detached copy for a session image: the processor type text and the URI, both in `arena`. */
typedef struct {
    ta_str type;
    ta_str uri;
    ta_io_origin_entry **out;
} ta_io_origin_detach_params;
TA_PRIVATE ta_result ta_io_origin_detach(ta_arena *arena, const ta_io_origin_detach_params *params);
TA_PRIVATE bool ta_io_origin_entry_detached(const ta_io_origin_entry *entry);
/* The processor type text of a detached copy. */
TA_PRIVATE ta_str ta_io_origin_detached_type(const ta_io_origin_entry *entry);



/*
 * Sessions whose ID is not a resident actor's runtime ID (ADR 0066). A
 * session's ID is the runtime ID it was spawned with, so a never-paged
 * session is found through the actor table and has no entry here. An entry
 * is either a reloaded resident session (`actor` set) or an unloaded named
 * one (`actor` null, `address` held). Lookups prefer a resident entry.
 *
 * The table is a linear hash table over fixed pages, like the address table
 * (ADR 0065): each insert splits at most one bucket, growing never copies
 * the table, and a failed page allocation only postpones a split. Its first
 * page is allocated at init, so inserting never fails.
 */

#define TA_SESSION_PAGE_BUCKETS 1024u
#define TA_SESSION_INITIAL_BUCKETS 16u

typedef struct ta_session_node {
    uint64_t id;
    struct ta_session_node *next;
    /* The resident actor holding the session, or null when it is unloaded. */
    struct ta_actor *actor;
    /* An unloaded session's address, one reference held; null otherwise. */
    ta_address *address;
} ta_session_node;

typedef struct {
    ta_allocator allocator;
    ta_vec pages; /* ta_session_node ** pages of TA_SESSION_PAGE_BUCKETS buckets */
    uint32_t base;
    uint32_t split;
    uint32_t count;
} ta_session_table;

TA_PRIVATE ta_result ta_session_table_init(ta_session_table *table, ta_allocator allocator);
/* Frees every entry left, releasing the addresses unloaded entries hold. */
TA_PRIVATE void ta_session_table_destroy(ta_session_table *table);

TA_PRIVATE ta_result ta_session_node_new(ta_session_table *table, ta_session_node **out);
/* Frees an entry that is not in the table, releasing its address. */
TA_PRIVATE void ta_session_node_free(ta_session_table *table, ta_session_node *node);
TA_PRIVATE void ta_session_insert(ta_session_table *table, ta_session_node *node);
TA_PRIVATE void ta_session_remove(ta_session_table *table, ta_session_node *node);
/* The entry for `id`: a resident one if there is one, else an unloaded one. */
TA_PRIVATE const ta_session_node *ta_session_find(const ta_session_table *table, uint64_t id);

/* Whether a resident session holds `id`: a never-paged actor or a reloaded one. */
TA_PRIVATE bool ta_session_resident(const ta_system *system, uint64_t id);
/* The session ID of the resident actor `runtime`, or `runtime` itself. */
TA_PRIVATE uint64_t ta_session_id_of(const ta_system *system, ta_actor_id runtime);
/*
 * Where an SCXML send to `ref` goes (ADR 0066 section 3): a runtime ID that
 * is not a resident actor's is looked up as a session ID. A reloaded session
 * gives its current runtime ID; an unloaded one gives its address, retained,
 * in which case this returns true and the caller releases *out. Otherwise
 * *out is `ref` and this returns false.
 */
TA_PRIVATE bool ta_session_route(const ta_system *system, const ta_actor_ref *ref, ta_actor_ref *out);



typedef struct {
    ta_event_name_id parent;
    ta_atom segment;
    uint16_t depth;
    uint16_t flags;
    uint32_t hash_next;
} ta_event_name_node;

typedef struct {
    ta_allocator allocator;
    ta_atom_table *atoms;
    ta_vec nodes;
    uint32_t *buckets;
    uint32_t bucket_count;
    /* Segments a name may have: the system's eventNameDepth (ADR 0092),
     * at most TA_EVENT_NAME_MAX_DEPTH, which sizes callers' arrays. */
    uint32_t max_depth;
} ta_event_name_table;

enum {
    TA_EVENT_NAME_ROOT = 0u,
    TA_EVENT_NAME_ERROR = 1u,
    TA_EVENT_NAME_ERROR_EXECUTION = 2u,
    TA_EVENT_NAME_ERROR_COMMUNICATION = 3u,
    TA_EVENT_NAME_ERROR_PLATFORM = 4u,
    TA_EVENT_NAME_DONE = 5u,
    TA_EVENT_NAME_DONE_STATE = 6u,
    TA_EVENT_NAME_DONE_INVOKE = 7u
};

/* The ceiling of eventNameDepth (ADR 0092): it sizes segment arrays. */
#define TA_EVENT_NAME_MAX_DEPTH 64u

TA_PRIVATE ta_result ta_event_name_table_init(ta_event_name_table *table,
                                   ta_allocator allocator,
                                   ta_atom_table *atoms);
TA_PRIVATE void ta_event_name_table_destroy(ta_event_name_table *table);
TA_PRIVATE ta_result ta_event_name_intern_atoms_internal(ta_event_name_table *table,
                                              const ta_atom_list *segments,
                                              ta_event_name_id *out);
/* Interns one more segment below `parent`. */
TA_PRIVATE ta_result ta_event_name_child(ta_event_name_table *table,
                              ta_event_name_id parent,
                              ta_atom segment,
                              ta_event_name_id *out);
/* Interns a dotted name; used by the compiler for authored names only. */
TA_PRIVATE ta_result ta_event_name_intern_dotted(ta_event_name_table *table, ta_str dotted, ta_event_name_id *out);
TA_PRIVATE bool ta_event_name_matches_internal(const ta_event_name_table *table,
                                    ta_event_name_id event,
                                    ta_event_name_id prefix);
TA_PRIVATE bool ta_event_name_valid(const ta_event_name_table *table, ta_event_name_id name);
/* Writes the name's segments root-first; returns the segment count. */
TA_PRIVATE uint32_t ta_event_name_segments(const ta_event_name_table *table,
                                ta_event_name_id name,
                                ta_atom out[TA_EVENT_NAME_MAX_DEPTH]);



/* Events, faults, and queued events are fully public. */

/* ta_event_retain() and ta_event_release() are public (ADR 0050, ADR 0065). */



/*
 * Tracing (ADR 0067): the sink a system writes records to, and the rules
 * every call site shares. The interpreter builds the records; this module
 * decides which ones are built at all.
 */

TA_PRIVATE inline bool ta_trace_context_empty(const ta_trace_context *context) {
    return context->trace_id_high == 0 && context->trace_id_low == 0;
}

TA_PRIVATE inline bool ta_trace_parent_empty(const ta_trace_parent *parent) {
    return ta_trace_context_empty(&parent->context) && parent->context.flags == 0 && parent->cause.session == 0 &&
           parent->cause.macrostep == 0 && parent->cause.microstep == 0;
}

/*
 * The kinds a macrostep under `context` builds (ADR 0073 section 4):
 * `unsampled_kinds` when it has a trace ID that is not sampled, otherwise
 * `watched_kinds` when it is marked watched, and `kinds` when not.
 */
TA_PRIVATE uint32_t ta_trace_macrostep_kinds(const ta_trace_sink *sink, const ta_trace_context *context);

/* Whether the sink wants lifecycle record `kind` under `context`: from
 * `watched_kinds` when the context is watched, else from `kinds`. Lifecycle
 * records are not sampled (ADR 0073 section 4). */
TA_PRIVATE inline bool ta_trace_lifecycle_wanted(const ta_trace_sink *sink, ta_trace_kind kind,
                                             const ta_trace_context *context) {
    uint32_t kinds = (context->detail & TA_TRACE_DETAIL_WATCHED) != 0 ? sink->watched_kinds : sink->kinds;
    return (kinds & TA_TRACE_BIT(kind)) != 0;
}

/* Every kind any macrostep may build: what scratch must be reserved for. */
TA_PRIVATE inline uint32_t ta_trace_any_kinds(const ta_trace_sink *sink) {
    return sink->kinds | sink->watched_kinds | sink->unsampled_kinds;
}

/* A record with every ID set to NONE and everything else zero. */
TA_PRIVATE ta_trace_record ta_trace_record_blank(ta_trace_kind kind);

struct ta_actor;

/*
 * Writes the lifecycle record `kind` for `actor` (SPAWNED, LOADED,
 * UNLOADED or DESTROYED), with `parent` as its context and cause when
 * there is one. Lifecycle records are outside macrosteps and never sampled.
 */
TA_PRIVATE void ta_trace_lifecycle(ta_system *system, struct ta_actor *actor, ta_trace_kind kind,
                        const ta_trace_parent *parent);

/*
 * The key of the actor's next lifecycle record: (session, 0, n), where n
 * counts the session's lifecycle records from 1. It moves whether or not a
 * sink wants the record, so keys are the same with tracing on or off.
 */
TA_PRIVATE ta_step_key ta_trace_lifecycle_key(struct ta_actor *actor);

/* Writes a record the caller built because `sink.kinds` asked for it. */
TA_PRIVATE inline void ta_trace_write(const ta_trace_sink *sink, const ta_trace_record *record) {
    sink->v->write(sink->self, record);
}



/*
 * The SCXML Event I/O Processor target URI grammar (ADR 0044).
 *
 * Core owns it in both directions, so a literal `target`, an evaluated
 * `targetexpr`, and the text core writes for `$_event.origin` all mean the
 * same thing. `ta_scxml_target_parse()` and `ta_scxml_target_format()` are
 * the public entry points; this is the writer the data-model host view
 * shares with them.
 */

/* Writes the URI of `target`; false when its kind has no URI (SELF, INVALID). */
TA_PRIVATE bool ta_scxml_uri_write(const ta_system *system, ta_text_writer *writer, const ta_scxml_target *target);
/* `#_actor_<family>/<name>` with both parts percent-encoded (ADR 0096). */
TA_PRIVATE void ta_actor_uri_write(ta_text_writer *writer, const ta_actor_address *address);

/* A session ID's text, <incarnation>.<session>.<family> (ADR 0088). */
typedef struct {
    uint64_t incarnation;
    uint64_t session;
    ta_str family; /* borrowed from the parsed text */
} ta_session_text;

/* False when `text` is not in that form (a bare number is not). */
TA_PRIVATE bool ta_session_text_parse(ta_str text, ta_session_text *out);
/* Writes a session of this system as text. */
TA_PRIVATE void ta_session_text_write(const ta_system *system, ta_text_writer *writer, uint64_t session, ta_str family);
/* Writes a runtime reference's session: as text when its family is known,
 * from the reference or its live actor, otherwise as a bare number. */
TA_PRIVATE void ta_session_ref_write(const ta_system *system, ta_text_writer *writer, const ta_actor_ref *ref);



/* External delivery and the router boundary are fully public. */

/*
 * A named ta_actor_ref is a reference to an interned address (ADR 0065).
 * An owner retains it and releases it; both ignore a runtime ref.
 */
TA_PRIVATE void ta_actor_ref_retain(const ta_actor_ref *ref);
TA_PRIVATE void ta_actor_ref_release(ta_actor_ref *ref);

/*
 * Makes a named ref one of this system's handles: a detached address (from a
 * session image) is interned and the ref then holds a new reference. A ref
 * that is already this system's, or a runtime ref, is left alone.
 */
TA_PRIVATE ta_result ta_actor_ref_localize(ta_system *system, ta_actor_ref *ref);
/* Detaches a named ref into `arena`, for an image; a runtime ref is left alone. */
TA_PRIVATE ta_result ta_actor_ref_detach(ta_arena *arena, ta_actor_ref *ref);
/* The origin of an event, when it is an actor. */
TA_PRIVATE ta_actor_ref *ta_event_origin_ref(ta_event *event);



typedef struct {
    ta_time now;
} ta_clock;

TA_PRIVATE inline ta_time ta_clock_now(const ta_clock *clock) { return clock->now; }
TA_PRIVATE inline void ta_clock_set(ta_clock *clock, ta_time now) { clock->now = now; }



/*
 * Configuration is the set of active states of one actor session.
 *
 * A configuration is a bitset with one bit per state ID. Only atomic and
 * final states are ever set; ancestors are derived from the definition when
 * In(state) is asked, so the set stays small and a parallel's regions are
 * simply several bits set at once. The bitset is stored in whole 64-bit
 * words. There is one layout, so there is no kind to choose and none to
 * switch on.
 *
 * Lifetime. The layout is part of the definition and lives as long as it
 * does (definition lifetime); the compiler fills it once and nothing writes
 * it afterwards. The storage is carved out of the actor's own block at
 * creation (session lifetime) and freed with that block, so a
 * ta_configuration holds borrowed pointers: it is valid while its actor is
 * and its definition is retained, and must not outlive either.
 *
 * Allocation. Nothing in this unit allocates or frees. Every function works
 * on storage the caller already owns, so none can fail for lack of memory.
 *
 * Invariants. layout->bytes is ta_bits_words(atomic_state_count) * 8, and
 * storage points at that many bytes, aligned for uint64_t. A state ID at or
 * beyond atomic_state_count is never active: reads report false and writes
 * are ignored, so a stale or foreign ID cannot touch memory past the set.
 *
 * Complexity. Reading or writing one state is O(1); clearing is O(bytes).
 *
 * Example:
 *
 *	ta_configuration config = { .storage = words, .layout = &definition->configuration };
 *	ta_configuration_clear(&config);
 *	ta_configuration_set_atomic(&config, leaf, true);
 *	bool active = ta_configuration_contains_atomic(&config, leaf);
 */

/*
 * ta_configuration_layout describes the storage of a configuration. `bytes`
 * is its size in whole 64-bit words, in bytes; `atomic_state_count` is the
 * number of addressable bits, which is the definition's state count.
 */
typedef struct {
    uint16_t bytes;
    uint32_t atomic_state_count;
} ta_configuration_layout;

/*
 * ta_configuration is a view of one actor's active states: `storage` is
 * borrowed from the actor block and `layout` from its definition.
 */
typedef struct ta_configuration ta_configuration;
struct ta_configuration {
    void *storage;
    const ta_configuration_layout *layout;
};

/* ta_configuration_contains_atomic reports whether `atomic_state` is active.
 * An ID beyond the layout's state count is not active. Never allocates. */
TA_PRIVATE bool ta_configuration_contains_atomic(const ta_configuration *config,
                                      ta_state_id atomic_state);
/* ta_configuration_set_atomic marks `atomic_state` active or inactive. An ID
 * beyond the layout's state count is ignored. Never allocates. */
TA_PRIVATE void ta_configuration_set_atomic(ta_configuration *config,
                                 ta_state_id atomic_state,
                                 bool active);
/* ta_configuration_clear makes every state inactive by zeroing layout->bytes
 * bytes of storage. Never allocates. */
TA_PRIVATE void ta_configuration_clear(ta_configuration *config);



/*
 * One slot per history pseudo-state: a state bitmap of `bytes` at `offset`
 * within the history storage. An empty bitmap means never recorded.
 */
typedef struct {
    uint32_t offset;
    uint16_t bytes;
    uint16_t flags;
} ta_history_slot;

typedef struct {
    ta_history_slot *slots;
    uint32_t slot_count;
    uint32_t storage_bytes;
} ta_history_layout;

typedef struct ta_history_state ta_history_state;
struct ta_history_state {
    void *storage;
    const ta_history_layout *layout;
};

TA_PRIVATE uint64_t *ta_history_slot_words(const ta_history_state *history, uint32_t slot);



typedef enum {
    TA_EXEC_FRAME_ACTIONS = 0,
    TA_EXEC_FRAME_FOREACH
} ta_exec_frame_kind;

/*
 * One open block of executable content, or one running <foreach> (ADR 0060).
 * ACTIONS: `actions` is the block and `cursor` the next action in it.
 * FOREACH: `actions` is the body, `cursor` the <foreach> action itself, and
 * the snapshot, position and count are the loop's (ADR 0033).
 */
typedef struct {
    ta_exec_frame_kind kind;
    ta_span32 actions;
    uint32_t cursor;
    uint32_t iteration;
    uint32_t iteration_count;
    ta_value iteration_snapshot;
} ta_exec_frame;

/*
 * Where a run stands between two steps it can stop at (ADR 0061 section 2,
 * ADR 0070). NONE: nothing is open, and the actor holds all its state. The
 * microstep's phases are START, EXIT, TRANSITIONS and ENTER. START: its
 * transitions are selected, its sets computed and its history recorded,
 * and nothing of it has run; a run stops there only when its caller asked
 * (ADR 0086). The others are the stretches outside a microstep that run
 * chart code or start services, and so can run out of memory after
 * something is visible:
 *
 *   DATA      the bootstrap's data: declaring (stage 0, `handler` the next
 *             declaration), then initializing (stage 1, `state` 0 for the
 *             top-level data and s + 1 for state s's, `handler` the next
 *             declaration)
 *   SCRIPT    the bootstrap's top-level <script>; the initial microstep
 *             follows
 *   FINALIZE  an external event was taken: its <finalize> (stage 0), then
 *             autoforwarding (stage 1, `state` the next invocation)
 *   SELECT    the taken external event's transitions are still to be chosen
 *   INVOKES   starting the invocations of the states the macrostep entered:
 *             `state` the state, `handler` the next of its <invoke>s
 *   FINISH    a top-level <final> was entered: exiting every state (stage
 *             0, `state` as in EXIT), then ending the session (stage 1)
 */
typedef enum {
    TA_PHASE_NONE = 0,
    TA_PHASE_START,
    TA_PHASE_EXIT,
    TA_PHASE_TRANSITIONS,
    TA_PHASE_ENTER,
    TA_PHASE_DATA,
    TA_PHASE_SCRIPT,
    TA_PHASE_FINALIZE,
    TA_PHASE_SELECT,
    TA_PHASE_INVOKES,
    TA_PHASE_FINISH
} ta_run_phase;

/* The stages of one state inside a phase. */
enum {
    TA_EXIT_HANDLERS = 0,
    TA_EXIT_HANDLERS_DONE = 1
};
enum {
    TA_ENTER_ACTIVATE = 0,
    TA_ENTER_DATA = 1,
    TA_ENTER_HANDLERS = 2,
    TA_ENTER_INITIAL = 3,
    TA_ENTER_HISTORY = 4,
    TA_ENTER_DONE = 5
};
enum {
    TA_DATA_DECLARE = 0,
    TA_DATA_INITIALIZE = 1
};
enum {
    TA_FINALIZE_BLOCK = 0,
    TA_FINALIZE_FORWARD = 1
};
enum {
    TA_FINISH_EXIT = 0,
    TA_FINISH_END = 1
};

typedef struct {
    ta_run_phase phase;
    /*
     * EXIT: the states still to exit are the exit set below this ID, highest
     * first. TRANSITIONS: the index of the enabled transition whose content
     * runs. ENTER: the next state to consider, lowest first.
     */
    uint32_t state;
    uint32_t stage;
    /* The <onentry>/<onexit> handler being run; in ENTER's DATA stage, the
     * next data declaration to initialize. */
    uint32_t handler;
    /* The executor's frames hold an open block that the current step resumes. */
    bool block_open;
} ta_micro_position;

/*
 * Where the arrays that follow a continuation's header start, in bytes from
 * the start of the allocation. The state sets come first and start at
 * sizeof(ta_continuation), so they need no field (see the static assertion in
 * continuation.c). The offsets are 32-bit: ta_continuation_bytes refuses a
 * shape whose end does not fit, which keeps the header at 64 bytes where
 * size_t offsets would make it 88.
 */
typedef struct ta_continuation_offsets {
    uint32_t decls;
    uint32_t history;
    uint32_t enabled;
    uint32_t frames;
} ta_continuation_offsets;

/*
 * A run stopped inside a phase, by a pause (ADR 0061) or by running out of
 * memory (ADR 0070), moved out of the interpreter's scratch into one
 * allocation the actor owns. The header is followed, at the offsets it
 * stores, by the four state sets (active, to exit, to enter, default entry;
 * `words` each), the data declarations the binding supplies (`decl_words`,
 * while bootstrapping), the default-history map (`state_count`), the enabled
 * transitions, and the executor's frames. `bytes` is the allocation, which
 * may be larger than this shape needs: the system keeps one spare, sized for
 * the largest definition it ran. It holds no pointers, so copying `bytes`
 * copies it; the frames' snapshots are values a copy must retain.
 *
 * Lifetime: parked. The actor owns its paused continuation, and the system
 * owns the one spare; either frees it with `bytes` and TA_CONTINUATION_ALIGN.
 *
 * `offsets` are written once, where the allocation is made or loaded
 * (ta_reserve_stop, and the restore of an image), from the CAPACITY shape of
 * the definition (ta_continuation_capacity in interpreter.c): every array
 * sits in a slot sized for the most the definition can save. A stop writes
 * fewer enabled transitions and frames than the slots hold (`enabled_count`,
 * `frame_count`), never more, so each array stays inside its slot and no two
 * overlap. `words`, `decl_words` and `state_count` always equal the capacity
 * values. The accessors read the offsets and nothing can fail after the
 * allocation. The offsets are valid for one definition's shape: a spare that
 * is reused for another definition gets them rewritten, and an actor's
 * paused continuation keeps them because an actor's definition does not
 * change while it is paused.
 */
typedef struct ta_continuation {
    size_t bytes;
    ta_micro_position at;
    uint32_t words;
    uint32_t decl_words;
    uint32_t state_count;
    uint32_t enabled_count;
    uint32_t frame_count;
    ta_continuation_offsets offsets;
} ta_continuation;

/*
 * ta_continuation_bytes answers, in `*bytes`, the bytes a continuation of
 * this shape takes (`shape` has the counts set) and, in `*offsets` when it
 * is not NULL, where each array goes. It answers TA_ERR_NOMEM and writes
 * neither output when the shape does not fit in a size_t, or when its end
 * exceeds UINT32_MAX (every offset is at most the end, so one bound covers
 * them all). It never allocates. It is called only where a continuation is
 * allocated or loaded, which is a safe point that can fail cleanly; what is
 * needed afterwards is stored in the header.
 */
TA_PRIVATE ta_result ta_continuation_bytes(const ta_continuation *shape, size_t *bytes, ta_continuation_offsets *offsets);
/* Whether the phase is one of a microstep's (START, EXIT, TRANSITIONS, ENTER). */
TA_PRIVATE bool ta_run_phase_in_microstep(ta_run_phase phase);
/*
 * The accessors answer where each array of an existing continuation starts:
 * the allocation's address plus the stored offset. They read the header and
 * never fail or allocate. The pointers are valid until the continuation is
 * freed or recycled, and each array holds at most the count its name says
 * (`words` per set, `decl_words`, `state_count`, `enabled_count`,
 * `frame_count`).
 */
TA_PRIVATE uint64_t *ta_continuation_sets(ta_continuation *continuation);
TA_PRIVATE uint64_t *ta_continuation_decls(ta_continuation *continuation);
TA_PRIVATE ta_state_id *ta_continuation_history(ta_continuation *continuation);
TA_PRIVATE ta_transition_id *ta_continuation_enabled(ta_continuation *continuation);
TA_PRIVATE ta_exec_frame *ta_continuation_frames(ta_continuation *continuation);
/* The alignment every continuation allocation needs. */
#define TA_CONTINUATION_ALIGN _Alignof(ta_exec_frame)

/* Frees a continuation and releases its frames' snapshots; null is fine. */
TA_PRIVATE void ta_continuation_free(ta_allocator allocator, ta_continuation **continuation);
/* Releases the snapshots of a continuation's frames, and nothing else. */
TA_PRIVATE void ta_continuation_release(ta_continuation *continuation);
/* Retains the snapshots of a continuation's frames, for a copy of it. */
TA_PRIVATE void ta_continuation_retain(ta_continuation *continuation);



/*
 * Delayed sends (ADR 0026, ADR 0054).
 *
 * A send is stored with its owner: each owner has a ta_timer_list, one array
 * of public ta_delayed_send_info sorted by (due, timer id) plus a parallel
 * array of private fields. The system keeps a min-heap of the lists that hold
 * at least one send, keyed by each list's front. The heap's top list's front
 * is the globally earliest send, so dispatch order is exactly (due, id).
 *
 * A list's address must stay stable while it holds sends: the heap points at
 * it. Actors keep theirs in their sparse state; the sends whose session is
 * gone live in the heap's own `orphans` list.
 */

/*
 * An evaluated delayed send, the flat form used to schedule a send, to hand a
 * due send to dispatch, and to carry sends in a session image. For the
 * built-in SCXML processor `destination` is the resolved external target;
 * custom processors receive `target`.
 */
typedef struct {
    ta_timer_id id;
    ta_actor_id owner;
    ta_send_id send_id;
    ta_time due;
    ta_event_name_id event;
    ta_atom io_type;
    ta_actor_ref destination;
    ta_value target;
    ta_value data;
    /*
     * A send to a host I/O processor with literal <content>: the content's ID
     * in the owner's definition, not its bytes, so a session image restores
     * it against its exact revision (ADR 0053, ADR 0038). The processor gets
     * the bytes when the send is dispatched.
     */
    bool has_content;
    ta_content_id content;
    /*
     * A send to a host I/O processor with a literal `target`: its content ID
     * in the owner's definition, for the same reason. `target` is then
     * absent. Public, in ta_delayed_send_info, so inspection can name it.
     */
    bool has_target_text;
    ta_content_id target_text;
    /*
     * Already sent: the router accepted nothing and the send waits here to be
     * offered again. <cancel> and session termination leave it in place.
     */
    bool submitted;
    /* Submitted sends only: the envelope, which outlives the sending session. */
    ta_actor_ref origin;
    ta_invoke_id invoke_id;
    /* The sending step (ADR 0067): a send that fires later still names it. */
    ta_trace_parent trace;
} ta_delayed_send;

/* The private fields of one stored send, parallel to ta_timer_list.sends. */
typedef struct {
    ta_timer_id id;
    ta_actor_id owner;
    ta_actor_ref origin;
    ta_invoke_id invoke_id;
} ta_delayed_send_meta;

/* One owner's pending sends, sorted by (due, id). Zero-initialized is empty. */
typedef struct {
    ta_delayed_send_info *sends;
    ta_delayed_send_meta *meta;
    uint32_t count;
    uint32_t capacity;
    /* Position in the owner heap; meaningful only while `indexed`. */
    uint32_t heap_index;
    /* In the owner heap: true exactly when count != 0. */
    bool indexed;
} ta_timer_list;

/*
 * The owner heap's slots live in fixed pages of TA_TIMER_PAGE_OWNERS, found
 * through a page directory, as the actor table's do (ADR 0063). Growing adds
 * a page and never copies the heap; only the directory of page pointers
 * doubles. Pages live as long as the system.
 */
#define TA_TIMER_PAGE_OWNERS 8192u

typedef struct {
    ta_allocator allocator;
    /* A binary min-heap of non-empty lists, keyed by each list's front. */
    ta_vec pages; /* ta_timer_list **, one per page of owner slots */
    uint32_t count;
    ta_timer_id next_id;
    /* Submitted sends whose sending session is gone (ADR 0054). */
    ta_timer_list orphans;
    /* Set while the system is destroyed: orphaning releases instead of keeping. */
    bool closing;
} ta_timer_heap;

TA_PRIVATE ta_result ta_timer_heap_init(ta_timer_heap *heap, ta_allocator allocator);
/* Releases every send still indexed, the orphans included. */
TA_PRIVATE void ta_timer_heap_destroy(ta_timer_heap *heap);
/* Makes the next `additional` schedules into list allocation-free. */
TA_PRIVATE ta_result ta_timer_reserve(ta_timer_heap *heap, ta_timer_list *list, uint32_t additional);
/* Takes ownership of the send's values on TA_OK; assigns send->id when it is zero. */
TA_PRIVATE ta_result ta_timer_schedule(ta_timer_heap *heap, ta_timer_list *list, const ta_delayed_send *send);
/* Removes and releases every not yet submitted send in list with send_id. */
TA_PRIVATE ta_result ta_timer_cancel(ta_timer_heap *heap, ta_timer_list *list, ta_send_id send_id);
/* Like ta_timer_cancel(), but also removes submitted sends. */
TA_PRIVATE ta_result ta_timer_withdraw(ta_timer_heap *heap, ta_timer_list *list, ta_send_id send_id);
/* Removes and releases every not yet submitted send in list (session termination). */
TA_PRIVATE void ta_timer_cancel_unsubmitted(ta_timer_heap *heap, ta_timer_list *list);
/*
 * Makes ta_timer_orphan(list) allocation-free: reserves room in the orphans
 * for the list's submitted sends.
 */
TA_PRIVATE ta_result ta_timer_reserve_orphans(ta_timer_heap *heap, const ta_timer_list *list);
/*
 * The sending session is gone: releases the list's unsubmitted sends and
 * moves its submitted ones to the orphans. Call ta_timer_reserve_orphans()
 * first. Leaves the list empty and unindexed; its storage stays allocated.
 */
TA_PRIVATE void ta_timer_orphan(ta_timer_heap *heap, ta_timer_list *list);
/* Releases every send in list, unindexes it, and frees its storage. */
TA_PRIVATE void ta_timer_list_destroy(ta_timer_heap *heap, ta_timer_list *list);
/* Bytes the list's storage occupies. */
TA_PRIVATE size_t ta_timer_list_bytes(const ta_timer_list *list);
/*
 * Copies the list's not yet submitted sends in (due, id) order, and returns
 * how many there are; a null `out` only counts. These are the sends a
 * session belongs to — a submitted send outlives it and is the system's to
 * re-offer. The copies borrow their values; the caller takes its own
 * references.
 */
TA_PRIVATE uint32_t ta_timer_copy_unsubmitted(const ta_timer_list *list, ta_delayed_send *out);
/* The earliest due time of any indexed send; false when there is none. */
TA_PRIVATE bool ta_timer_next_due(const ta_timer_heap *heap, ta_time *out);
/* Removes the earliest send and transfers ownership of its values to *out. */
TA_PRIVATE ta_result ta_timer_pop(ta_timer_heap *heap, ta_delayed_send *out);



typedef struct ta_actor ta_actor;

/* The data-model host view resolves everything through the calling actor. */
typedef struct {
    ta_system *system;
    ta_actor *actor;
} ta_dm_host_impl;

struct ta_dm_host {
    const ta_dm_host_impl *impl;
};



/*
 * The SCXML Null data model (SCXML B.1, ADR 0057), declared publicly as
 * ta_null_datamodel(). Its whole expression language is the `In(state)`
 * predicate in a condition; everything that needs data is an error. See
 * docs/internal/datamodel-null-profile.md.
 */



typedef struct {
    ta_atom type;
    ta_io io;
} ta_io_registration;

/*
 * Rendering origins (ADR 0068). `session_id` and `address` describe the
 * session the origin was delivered to, for a COMPUTED origin's `text`
 * callback. Stored URIs come back as views; everything else is written into
 * `arena`.
 */
typedef struct {
    const ta_origin *origin;
    uint64_t session_id;
    ta_address *address;
    ta_allocator arena;
    ta_str *out;
} ta_io_origin_render_params;
TA_PRIVATE ta_result ta_io_origin_uri(const ta_system *system, const ta_io_origin_render_params *params);
/* The processor type of an origin; TA_ERR_NOT_FOUND for none. */
TA_PRIVATE ta_result ta_io_origin_type(const ta_system *system, const ta_origin *origin, ta_str *out);



typedef struct {
    ta_atom type;
    ta_invoker invoker;
} ta_invoker_registration;

/*
 * Private bookkeeping for one active invocation, kept parallel to the public
 * ta_active_invoke array of the actor's sparse state. `child` is set when the
 * service spawned an SCXML session with this invocation as its parent.
 */
typedef struct {
    ta_invoke_decl_id decl;
    ta_state_id state;
    ta_invoker invoker;
    ta_actor_id child;
    bool has_child;
    /* Started while its actor replayed (ADR 0089): no invoker was called, so
     * none is cancelled or forwarded to, and nothing runs behind it. */
    bool replayed;
} ta_invoke_meta;

/*
 * ta_invoke_table_bytes is the size of the block behind an actor's invoke
 * table (0 when it has none): the figure ta_actor_inspect adds to the
 * actor's allocated bytes. It never allocates.
 */
TA_PRIVATE size_t ta_invoke_table_bytes(const ta_columns *table);

/*
 * ta_invoke_table_decl is the declaration (index into the definition's
 * invokes) of the invoke at `index`, which must be below the table's count.
 * It never allocates.
 */
TA_PRIVATE ta_invoke_decl_id ta_invoke_table_decl(const ta_columns *table, uint32_t index);

/* The send ID of a child's queued done.invoke completion record. */
#define TA_SEND_ID_COMPLETION (UINT64_MAX - 1u)

/*
 * Retires an invocation whose service did not survive a session image, and
 * queues its error.communication.invoke.<id>.unavailable event.
 */
TA_PRIVATE ta_result ta_invoke_report_lost(ta_system *system, ta_actor *actor, ta_invoke_id invoke);
/*
 * Publishing a replaying actor retires its active invocations (ADR 0089
 * section 6). The first makes room for their unavailable events, and
 * changes nothing visible when it fails; the second then queues them and
 * forgets the invocations, and cannot fail. It answers whether it queued one.
 */
/*
 * A replaying actor restored from a checkpoint (ADR 0091) takes back its
 * active invocations as replayed ones: active in core, with no invoker
 * called, so the services' later events still match them.
 */
typedef struct {
    ta_invoke_id id;
    ta_atom type;
    ta_invoke_decl_id decl;
} ta_invoke_restore_params;
TA_PRIVATE ta_result ta_invoke_restore(ta_system *system, ta_actor *actor, const ta_invoke_restore_params *params);
/* Whether `invoke` is one of the actor's active invocations. */
TA_PRIVATE bool ta_invoke_is_active(const ta_actor *actor, ta_invoke_id invoke);
TA_PRIVATE ta_result ta_invoke_prepare_retire(ta_system *system, ta_actor *actor);
TA_PRIVATE bool ta_invoke_retire_all(ta_system *system, ta_actor *actor);

/* The origin of an event from `invoke`: its ID and its invoker's registry index (ADR 0068). */
TA_PRIVATE ta_origin ta_invoke_origin_of(const ta_system *system, const ta_active_invoke *invoke);
/* The origin of an event from the actor's active invocation `invoke`; false when there is none. */
TA_PRIVATE bool ta_invoke_origin_for(const ta_system *system, const ta_actor *actor, ta_invoke_id invoke, ta_origin *out);



/* Functions over compiled definitions, shared by the compiler and interpreter. */

typedef enum {
    TA_TRANSITION_EXTERNAL,
    TA_TRANSITION_INTERNAL
} ta_transition_type;

typedef enum {
    TA_BINDING_EARLY,
    TA_BINDING_LATE
} ta_datamodel_binding_mode;

typedef enum {
    TA_VALUE_SOURCE_NONE,
    TA_VALUE_SOURCE_EXPR,
    TA_VALUE_SOURCE_LOCATION,
    TA_VALUE_SOURCE_CONTENT
} ta_value_source_kind;

typedef struct {
    ta_value_source_kind kind;
    union {
        ta_expr_id expr;
        ta_expr_id location;
        ta_content_id content;
    } as;
} ta_value_source;

typedef enum {
    TA_EVENT_SOURCE_NAME,
    TA_EVENT_SOURCE_EXPR
} ta_event_source_kind;

typedef struct {
    ta_event_source_kind kind;
    union {
        ta_event_name_id name;
        ta_expr_id expr;
    } as;
} ta_event_source;

/*
 * A compiled `x` / `xexpr` attribute pair. ATOM holds interned literal text;
 * NONE is used only where the attribute is optional.
 */
typedef enum {
    TA_TEXT_SOURCE_NONE,
    TA_TEXT_SOURCE_ATOM,
    TA_TEXT_SOURCE_EXPR
} ta_text_source_kind;

typedef struct {
    ta_text_source_kind kind;
    union {
        ta_atom atom;
        ta_expr_id expr;
    } as;
} ta_text_source;

/*
 * A compiled <send>/<invoke> `id` / `idlocation`. LITERAL holds the author's
 * interned ID. GENERATED and LOCATION make the runtime generate an ID on each
 * execution; LOCATION also assigns it to the location.
 */
typedef enum {
    TA_ID_SOURCE_GENERATED,
    TA_ID_SOURCE_LITERAL,
    TA_ID_SOURCE_LOCATION
} ta_id_source_kind;

typedef struct {
    ta_id_source_kind kind;
    union {
        ta_atom literal;
        ta_expr_id location;
    } as;
} ta_id_source;

/*
 * Index into ta_definition.authored_texts: authored text the compiler
 * normalizes and the definition keeps for ta_definition_export().
 */
typedef uint32_t ta_authored_text_id;
#define TA_AUTHORED_TEXT_NONE UINT32_MAX

/*
 * LITERAL is a duration parsed at compile time, in ta_time milliseconds;
 * `text` is the authored duration (e.g. "2.5s"), which the parse discards.
 */
typedef enum {
    TA_DELAY_SOURCE_NONE,
    TA_DELAY_SOURCE_LITERAL,
    TA_DELAY_SOURCE_EXPR
} ta_delay_source_kind;

typedef struct {
    ta_delay_source_kind kind;
    ta_authored_text_id text;
    union {
        ta_time literal;
        ta_expr_id expr;
    } as;
} ta_delay_source;

typedef enum {
    TA_ACTION_RAISE,
    TA_ACTION_SEND,
    TA_ACTION_CANCEL,
    TA_ACTION_ASSIGN,
    TA_ACTION_SCRIPT,
    TA_ACTION_LOG,
    TA_ACTION_IF,
    TA_ACTION_FOREACH,
    TA_ACTION_INVOKE
} ta_action_kind;

typedef struct {
    ta_event_name_id event;
    ta_value_source data;
} ta_raise_action;

/*
 * A NONE type selects the built-in SCXML Event I/O Processor. A literal
 * target is TA_VALUE_SOURCE_CONTENT.
 */
typedef struct {
    ta_id_source id;
    ta_text_source type;
    ta_event_source event;
    ta_value_source target;
    ta_delay_source delay;
    ta_value_source data;
    ta_span32 params;
} ta_send_action;

/* Never NONE: SCXML requires exactly one of sendid or sendidexpr. */
typedef struct {
    ta_text_source sendid;
} ta_cancel_action;

typedef struct {
    ta_atom label;
    bool has_label;
    ta_expr_id expr;
    bool has_expr;
} ta_log_action;

typedef struct {
    ta_expr_id location;
    ta_value_source value;
} ta_assign_action;

typedef struct {
    ta_expr_id condition;
    ta_span32 then_actions;
    ta_span32 else_actions;
} ta_if_action;

typedef struct {
    ta_expr_id array;
    ta_atom item;
    ta_atom index;
    bool has_index;
    ta_span32 body_actions;
} ta_foreach_action;

typedef struct {
    ta_action_kind kind;
    union {
        ta_raise_action raise;
        ta_send_action send;
        ta_cancel_action cancel;
        ta_assign_action assign;
        ta_expr_id script;
        ta_log_action log;
        ta_if_action conditional;
        ta_foreach_action foreach;
        ta_invoke_decl_id invoke;
    } as;
} ta_action_def;

typedef struct {
    ta_atom name;
    ta_value_source value;
} ta_param_def;

typedef struct {
    ta_atom name;
    ta_state_id owner_state;
    ta_initializer_id initializer;
} ta_data_decl;

typedef struct {
    ta_value_source value;
} ta_initializer_def;

typedef struct {
    ta_value_source content;
    ta_span32 params;
} ta_done_data_def;

/*
 * Literal content, and the text of a literal <send target> or <invoke src>.
 * `media_type` is what the front end said the bytes are (ADR 0053); it is
 * empty for literal text, and hashed into the revision either way.
 */
typedef struct {
    ta_str media_type;
    ta_bytes bytes;
} ta_content_def;


/* A literal src is TA_VALUE_SOURCE_CONTENT. */
typedef struct {
    ta_id_source id;
    ta_text_source type;
    ta_value_source src;
    ta_value_source data;
    ta_span32 params;
    ta_span32 finalize_actions;
    bool autoforward;
    /* The state whose entry starts this invocation. */
    ta_state_id owner;
} ta_invoke_decl;

typedef struct {
    ta_atom type;
    ta_bytes payload;
} ta_extension_def;

typedef struct {
    ta_atom name;
    ta_state_id parent;
    ta_state_kind kind;
    ta_span32 children;
    ta_span32 transitions;
    ta_initial_transition_id initial;
    /* Ranges of definition.handlers: one action block per <onentry>/<onexit>. */
    ta_span32 onentry_handlers;
    ta_span32 onexit_handlers;
    ta_event_name_id done_event;
    ta_done_data_id done_data;
    /* Index into history.slots for history states, else UINT32_MAX. */
    uint32_t history_slot;
} ta_state_def;

typedef struct {
    ta_span32 segments;
} ta_event_pattern;

typedef struct {
    ta_atom atom;
    uint32_t flags;
} ta_event_pattern_segment;

enum {
    TA_EVENT_PATTERN_SEGMENT_WILDCARD = 1u << 0
};

typedef struct {
    const ta_event_pattern_segment *segments;
    uint32_t segment_count;
} ta_event_pattern_spec;

typedef struct {
    ta_state_id source;
    ta_span32 targets;
    ta_span32 event_patterns;
    ta_expr_id condition;
    ta_transition_type type;
    ta_span32 actions;
    uint32_t document_order;
    /*
     * The authored event descriptor text, which event_patterns normalize
     * (whitespace, ".*" forms); TA_AUTHORED_TEXT_NONE when absent.
     */
    ta_authored_text_id event_text;
} ta_transition_def;

typedef struct {
    ta_state_id owner;
    ta_span32 targets;
    ta_span32 actions;
    /* False when `targets` is the SCXML default (the first child), not authored. */
    bool authored_targets;
} ta_initial_transition_def;

/*
 * Where an action sits (ADR 0071): its block, the block's owner, which of
 * the owner's blocks of that kind, and its place in the block. The public
 * ta_action_info, less the element kind, which the action itself holds.
 */
typedef struct {
    ta_action_block block;
    uint32_t owner;
    uint32_t block_index;
    uint32_t index;
} ta_action_place;

struct ta_definition {
    ta_allocator allocator;
    /* Owns every array and string below. */
    ta_arena arena;
    /* Counters and state occupancy (ADR 0072): bookkeeping about the
     * definition's actors, the only fields that change after registration.
     * `occupancy` has `state_count` entries, from registration on. */
    ta_counters counters;
    uint32_t *occupancy;
    /* The microstep limit (ADR 0075): 0 takes the system's,
     * TA_MICROSTEP_LIMIT_NONE is none. Bookkeeping, like the counters. */
    uint32_t microstep_limit;
    /* The system whose atoms and event names this definition uses. */
    ta_system *system;
    bool registered;
    /* Intrusive list of compiled definitions not yet registered. */
    ta_definition *unregistered_previous;
    ta_definition *unregistered_next;
    /* ta_state_def.children ranges index this array. */
    ta_state_id *children;
    uint32_t child_count;
    ta_definition_id id;
    /* identity.family as a system atom: what a runtime reference to one of
     * its actors names (ADR 0088). */
    ta_atom family;
    ta_definition_identity identity;
    /* The <scxml name> attribute, exposed as _name; TA_ATOM_NONE when absent. */
    ta_atom document_name;
    ta_configuration_layout configuration;
    ta_history_layout history;
    ta_state_def *states;
    uint32_t state_count;
    /* The deepest nesting of states: 1 for a chart of root states only.
     * Sizes the interpreter's entry-set frames (ADR 0060). */
    uint32_t max_state_depth;
    /* The deepest nesting of blocks of executable content: 1 for blocks with
     * no <if> or <foreach> in them, 0 for a chart with no blocks at all.
     * Sizes the executor's frames (ADR 0060). */
    uint32_t max_action_depth;
    ta_transition_def *transitions;
    uint32_t transition_count;
    ta_initial_transition_def *initial_transitions;
    uint32_t initial_transition_count;
    ta_initial_transition_id root_initial;
    /*
     * The <scxml> <script>: a block of zero or one TA_ACTION_SCRIPT in the
     * action stream, run once per session before the root initial transition.
     * A block, not a bare ta_expr_id, so it shares executable-content error
     * semantics (SCXML 4.9) and continuation frames with every other block.
     */
    ta_span32 global_script_actions;
    ta_event_pattern *event_patterns;
    uint32_t event_pattern_count;
    ta_event_pattern_segment *event_pattern_segments;
    uint32_t event_pattern_segment_count;
    ta_state_id *targets;
    uint32_t target_count;
    /* Action blocks are ta_span32 ranges of action_defs. */
    ta_action_def *action_defs;
    uint32_t action_count;
    /* Where each action sits, indexed like action_defs; computed once the
     * definition is sealed (ta_definition_place_actions). */
    ta_action_place *action_places;
    /*
     * The action block of each <onentry>/<onexit> handler. SCXML runs every
     * handler as a separate block (3.8, 3.9), so the handlers of a state are
     * not merged into one block.
     */
    ta_span32 *handlers;
    uint32_t handler_count;
    ta_param_def *params;
    uint32_t param_count;
    ta_data_decl *data_decls;
    uint32_t data_decl_count;
    ta_initializer_def *initializers;
    uint32_t initializer_count;
    ta_content_def *contents;
    uint32_t content_count;
    ta_done_data_def *done_data;
    uint32_t done_data_count;
    ta_invoke_decl *invokes;
    uint32_t invoke_decl_count;
    ta_extension_def *extensions;
    uint32_t extension_count;
    ta_expression *expressions;
    ta_expr_role *expression_roles; /* by expression, for prepare_definition (ADR 0076) */
    uint32_t expression_count;
    /*
     * Authored input the compiler normalizes, kept so ta_definition_export()
     * rebuilds a spec with the same fingerprint (ADR 0042).
     */
    ta_str *authored_texts;
    uint32_t authored_text_count;
    /*
     * Where each element was written (ADR 0085), when the spec knew for at
     * least one; null otherwise. One span per element, by kind and then by
     * ID: the states, the transitions (initial ones included), the actions
     * and the expressions (ta_definition_span_base()). Unknown spans are
     * zero. `sources` are the names they point into. All of it is one
     * allocation of `span_bytes` from `span_allocator`, starting at `spans`,
     * not the arena, so that a registration answered by this definition can
     * hand it newer spans (ta_definition_take_spans()).
     */
    ta_source_span *spans;
    uint32_t span_count;
    ta_scxml_source *sources;
    uint32_t source_count;
    size_t span_bytes;
    ta_allocator span_allocator;
    /* The authored <scxml binding>; DEFAULT and EARLY both compile to early. */
    ta_scxml_binding_kind authored_binding;
    /* The authored <scxml datamodel>; empty when absent. */
    ta_str authored_datamodel;
    /* True when `identity.revision` is the compiler's fingerprint, not an explicit identity. */
    bool fingerprinted;
    ta_datamodel_binding_mode binding_mode;
    ta_datamodel datamodel;
    void *datamodel_shared;
    bool datamodel_prepared;
    uint32_t datamodel_instance_size;
    uint32_t datamodel_instance_alignment;
    /* One actor of this definition is one block (ADR 0063; ta_actor_layout()). */
    size_t actor_block_bytes;
    size_t actor_block_alignment;
    size_t actor_session_offset;
    size_t actor_session_bytes; /* the session storage's size, stored with the layout; ta_actor_layout() */
    size_t actor_instance_offset;
    /* Resident actors whose `definition` points here; see ta_actor_free(). */
    uint32_t actor_count;
    /*
     * Holds (ADR 0077): each ta_system_register_definition() takes one and
     * ta_system_release_definition() gives one back. `holders` counts the
     * definitions that nest this one for an inline <invoke>. The system
     * frees the definition once holds, holders and actor_count are all zero.
     */
    uint32_t holds;
    uint32_t holders;
    /*
     * Set by ta_system_retire_definition(): no new actors, no match by
     * identity, and its holds dropped (ADR 0043, ADR 0077).
     */
    bool retired;
    /* Registration order; see ta_system_find_definition(). */
    uint64_t registration_serial;
    /* The compiler's content hash, whatever the revision (ADR 0077). */
    uint64_t content_hash;
    /*
     * The chart has a host expression, which the content hash marks but
     * cannot see into: registration answers it only with a definition that
     * holds the same handles (ADR 0087).
     */
    bool host_expressions;
    /*
     * The definition each <invoke> nests, by invoke declaration, or 0; null
     * until the first ta_definition_nest(). Allocated in `arena`.
     */
    ta_definition_id *nested;
    /* The system's free list while a release cascades (ADR 0060: no recursion). */
    ta_definition *free_next;
};

/* A registered definition, retired or not; NULL for an unknown or stale ID. */
TA_PRIVATE const ta_definition *ta_definition_lookup(const ta_system *system, ta_definition_id id);
/*
 * ta_definition_session_bytes answers, in `*bytes`, the bytes of one actor's
 * session storage: two configurations and the history. It answers
 * TA_ERR_NOMEM, with `*bytes` untouched, when the sum does not fit in a
 * size_t. It never allocates. Only ta_actor_layout calls it, at compile
 * time, and stores the result in `actor_session_bytes` (definition lifetime).
 */
TA_PRIVATE ta_result ta_definition_session_bytes(const ta_definition *definition, size_t *bytes);
/*
 * Fills `action_places` from the sealed blocks, in the definition's arena.
 * Every action belongs to exactly one block, so one pass over the top-level
 * blocks and one over the actions (for the blocks nested in <if> and
 * <foreach>) place them all, with no recursion (ADR 0060).
 */
TA_PRIVATE ta_result ta_definition_place_actions(ta_definition *definition);
/*
 * The layout of `spans` (ADR 0085): elements of `kind` are numbered from
 * ta_definition_span_base(), and there are ta_definition_element_count() of
 * them. ta_definition_export() numbers the exported spec's spans the same
 * way.
 */
TA_PRIVATE uint32_t ta_definition_element_count(const ta_definition *definition, ta_element_kind kind);
TA_PRIVATE uint32_t ta_definition_span_base(const ta_definition *definition, ta_element_kind kind);
/*
 * Registration by identity answered `definition` with `live` (ADR 0077):
 * when `definition` knows where its elements are written, and not exactly
 * where `live` says, `live` takes its spans, and `definition`, about to be
 * destroyed, takes `live`'s old ones with it (ADR 0085). Allocates nothing,
 * so it cannot fail.
 */
TA_PRIVATE void ta_definition_take_spans(ta_definition *live, ta_definition *definition);
TA_PRIVATE bool ta_definition_is_atomic(const ta_definition *definition, ta_state_id state);
TA_PRIVATE bool ta_definition_is_descendant(const ta_definition *definition, ta_state_id state, ta_state_id ancestor);
TA_PRIVATE bool ta_event_pattern_matches(const ta_definition *definition,
                              const ta_event_pattern *pattern,
                              ta_event_name_id event);



/* The declarative specification tree and ta_tree are fully public. */

/* Parses an SCXML duration ("500ms", "2.5s") into logical milliseconds. */
TA_PRIVATE ta_result ta_duration_parse(ta_str text, ta_time *out);



/*
 * ta_definition_export() rebuilds a ta_scxml_spec from a compiled definition.
 * It reads the compiled tables and the authored text the compiler keeps for
 * it (ta_definition.authored_texts and the other `authored_*` fields); see
 * docs/internal/definition-inspection.md.
 */

/*
 * The export behind ta_definition_export() and ta_definition_encode().
 * An element with a known span always indexes the spans table (ADR 0085):
 * the element of `kind` with ID `id` has `span`
 * ta_definition_span_base(kind) + id + 1. `numbered` gives every element
 * that `span`, and the spec a table even when the definition keeps none, so
 * an encode can report where each element landed.
 */
typedef struct {
    ta_definition_id definition;
    ta_arena *arena;
    bool numbered;
    const ta_scxml_spec **out;
} ta_export_request;

TA_PRIVATE ta_result ta_export_spec(const ta_system *system, const ta_export_request *request);



/*
 * ta_definition_import() and ta_definition_encode() (ADR 0053 section 4):
 * the two functions core expresses over a chart codec. Core links no parser;
 * the codec is the embedder's.
 */



typedef struct {
    ta_actor_id id;
    ta_definition_id definition;
    ta_actor_ref self;
    ta_actor_residency residency;
} ta_actor_record;

typedef struct {
    ta_delayed_send send;
    bool immediate;
} ta_activation_send;

typedef enum {
    TA_ACTIVATION_SEND,
    TA_ACTIVATION_LOG
} ta_activation_effect_kind;

typedef struct {
    ta_activation_effect_kind kind;
    union {
        ta_activation_send send;
        ta_log_record log;
    } as;
} ta_activation_effect;

typedef struct ta_activation_effects {
    ta_vec entries; /* ta_activation_effect, in authored order */
    /* A replaying actor (ADR 0089): it crosses no border until published,
     * and `clock` is its own logical time, moved by each input's time (ADR 0094). */
    bool replaying;
    ta_time clock;
    /* QUEUED records of the checkpoint the replay started from (ADR 0091)
     * still to hand back, although their seq is the checkpoint's. */
    uint32_t checkpoint_owed;
} ta_activation_effects;

/* Where an actor's external event is (ADR 0069). */
typedef enum {
    TA_EXTERNAL_NONE = 0,
    TA_EXTERNAL_WAITING, /* delivered, behind any internal events */
    TA_EXTERNAL_TAKEN    /* taken by the running macrostep, kept until it ends */
} ta_external_state;

/* What `_event` names (ADR 0069). */
typedef enum {
    TA_BOUND_NONE = 0,
    TA_BOUND_EXTERNAL, /* the external event */
    TA_BOUND_INTERNAL, /* the head of the internal queue, which stays queued while bound */
    TA_BOUND_KEPT      /* the kept event */
} ta_event_binding;

/*
 * An operator transition delivered with the external event (ADR 0082), in
 * one allocation the event block owns until the macrostep takes the event.
 * Its targets are checked when it is delivered, against the configuration
 * it will run from: the event it came with is the first step taken.
 *
 * Lifetime: input. The jump lives in the event block until the macrostep
 * takes the event (ta_jump_free), and an image keeps a copy in its arena.
 * `bytes` is the size the allocation was made with, stored at that safe
 * point (ta_operator_jump_bytes) so that every free reads it and nothing
 * recomputes a size from `target_count` where a failure could not be
 * reported.
 */
typedef struct ta_operator_jump {
    ta_state_id source; /* TA_STATE_NONE: the root */
    uint32_t target_count;
    uint32_t bytes; /* the allocation's size, set where it was made; every free reads it */
    ta_state_id targets[];
} ta_operator_jump;

/*
 * ta_operator_jump_bytes answers, in `*bytes`, the size of an operator
 * transition with `count` targets: the header and then `count` state IDs.
 * It answers TA_ERR_NOMEM and leaves `*bytes` untouched when the total does
 * not fit in a size_t or in the header's 32-bit `bytes` (a jump of a billion
 * targets is refused rather than stored truncated). It never allocates. It
 * is called only where a jump is allocated (ta_actor_deliver, and the
 * restore of an image), which is a safe point that can fail cleanly; the
 * result is stored in `bytes` and read from there afterwards.
 */
TA_PRIVATE ta_result ta_operator_jump_bytes(uint32_t count, uint32_t *bytes);

/*
 * An actor's events other than its queued internal ones (ADR 0069). Taking
 * an event does not move it: `bound` names where it is. An event leaves
 * only when `_event` moves on, and it moves into `kept_event` when
 * something may still read it:
 * - a taken queue head, when the next event is taken, so that a retry can
 *   bind it again;
 * - the last event of an invoked session, when its macrostep ends, so that
 *   the session's <onexit> sees it if the parent cancels it (SCXML 6.4).
 */
typedef struct {
    uint8_t external;     /* ta_external_state */
    uint8_t bound;        /* ta_event_binding */
    uint8_t bound_before; /* what `bound` was before the last take, for a retry (ADR 0047) */
    bool has_kept;
    /* The external event enabled no transition (ADR 0089 section 8); read
     * when its macrostep ends, cleared with the event. Fills padding. */
    bool external_ignored;
    /* `time` holds the running macrostep's logical time (ADR 0089 section
     * 9), taken by the first ta_actor_resume() of the macrostep. */
    bool timed;
    ta_time time;
    /* The external event of the running macrostep. Inspection reports it
     * while the macrostep runs (ADR 0047). */
    ta_event external_event;
    ta_event kept_event;
    /* The operator transition that takes the waiting external event, or
     * null (ADR 0082). */
    ta_operator_jump *jump;
    /*
     * What the running macrostep runs under (ADR 0067): its delivery's, its
     * spawn's, or the failed send's that queued its first event. Set when a
     * macrostep starts, and kept while it runs, so a block that holds only a
     * parent stays with its actor until the macrostep ends.
     */
    ta_trace_parent trace;
} ta_event_slots;

/*
 * An actor's event block (ADR 0064, ADR 0069): its events and its internal
 * queue, oldest first, with the queue's capacity, in one allocation. It is
 * lent to an actor from ta_actor_deliver(), or the first internal event,
 * until the macrostep ends; an actor keeps it longer only while events are
 * queued, a microstep is paused, or it keeps an event. An idle actor that
 * nobody invoked holds none, so it carries no event.
 */
typedef struct ta_event_block {
    uint32_t count;
    uint32_t capacity; /* 0 for a header only, which grows with the first internal event */
    /* While the block is in the system's cache, the next cached block. */
    struct ta_event_block *next_cached;
    ta_event_slots slots;
    ta_queued_event events[];
} ta_event_block;

/* Allocated only for actors that use one of these uncommon facilities. */
typedef struct {
    /* The invoke table (session lifetime; see invoke.c and vec.h): one
     * ta_columns block holding ta_active_invoke records and, parallel to
     * them, ta_invoke_meta records (invoke.h). `invokes` is column 0, the
     * start of the block, so readers keep the plain array view. */
    union {
        ta_columns invoke_table;
        struct {
            ta_active_invoke *invokes;
            uint32_t invoke_count;
            uint32_t invoke_capacity;
        };
    };
    /* States entered in this macrostep whose invokes start when it ends. */
    ta_vec pending_invokes;
    ta_activation_effects *activation_effects;
    ta_actor_parent parent;
    ta_actor_completion completion;
    /* The top-level <final> whose entry ended the session (ADR 0056); set
     * with `completion`, and meaningful only while the actor is finished. */
    ta_state_id final_state;
    /* This actor's pending delayed sends, indexed by the system's owner heap. */
    ta_timer_list timers;
    bool has_parent;
    bool completion_pending;
    /* A microstep paused inside executable content (ADR 0061), or null. */
    ta_continuation *paused;
} ta_actor_sparse_state;

struct ta_actor {
    ta_actor_record record;
    const ta_definition *definition;
    /* One definition-sized allocation backs configuration, history, and late-binding bits. */
    void *session_storage;
    size_t session_bytes;
    ta_configuration configuration;
    ta_history_state history;
    /* Late binding: one bit per state whose data has been initialized. */
    uint64_t *late_initialized;
    void *datamodel_instance;
    bool datamodel_initialized;
    ta_actor_sparse_state *sparse;
    /* Its event block (ADR 0064), or null. */
    ta_event_block *event_block;
    ta_actor_id replacement_of;
    /* The session this actor holds (ADR 0066), and its entry in the session
     * table when that ID is not this actor's runtime ID. */
    uint64_t session_id;
    ta_session_node *session_node;
    uint64_t session_epoch;
    bool published;
    /* Lifecycle records written for this session, from 1 (ADR 0067; napi
     * ADR 0016): a lifecycle record's key is (session, 0, this count), so
     * each has its own span ID. Sits in the padding after `published`. */
    uint32_t trace_lifecycle;
    ta_dm_host_impl host_impl;
    ta_dm_host host;

    /* Execution state between microsteps; a macrostep resumes from here. */
    bool bootstrapped;
    bool macrostep;
    bool finished;
    /* The running macrostep has written MACROSTEP_BEGIN (ADR 0067): taking
     * its event can no longer be undone, and its key is `trace_macrostep`. */
    bool trace_begun;
    /* The step counters (ADR 0067 section 3): the current or last macrostep,
     * from 1, and the current or last microstep within it, from 1. They run
     * whether or not anything traces, and move only when a step commits. */
    uint32_t trace_microstep;
    uint64_t trace_macrostep;
    uint64_t next_send_id;
    /*
     * While the interpreter runs, the full active set it is updating (ancestors
     * included), so In() sees states as SCXML defines them mid-microstep.
     */
    const uint64_t *live_active;
};

typedef struct {
    ta_actor *actor;
    uint32_t generation;
    /* While free: the next free slot, or UINT32_MAX. Fits the padding the
     * generation already left, so the table costs no more per actor. */
    uint32_t next_free;
} ta_actor_slot;

/*
 * The actor table (ADR 0063): slots live in fixed pages of
 * TA_ACTOR_PAGE_SLOTS, found through a page directory. Slot `s` is in page
 * `s / TA_ACTOR_PAGE_SLOTS`. A page is allocated only when the last one is
 * full, and pages never move, so no spawn copies the table; only the
 * directory of page pointers doubles. Pages live as long as the system.
 */
#define TA_ACTOR_PAGE_SLOTS 4096u
typedef struct {
    ta_vec pages;   /* ta_actor_slot *, one per page */
    uint32_t count; /* slots issued so far */
} ta_actor_table;

/* The entry for `slot`, or NULL when that slot was never issued. */
TA_PRIVATE ta_actor_slot *ta_actor_slot_at(const ta_system *system, uint32_t slot);
TA_PRIVATE void ta_actor_table_destroy(ta_actor_table *table, ta_allocator allocator);
/*
 * Lays out one actor of `definition` as a single allocation (ADR 0063): the
 * ta_actor record, then the session storage, then the data-model instance,
 * each at its alignment. Called once, when the definition is compiled, so
 * the layout has the definition's lifetime and the sized free in
 * ta_actor_destroy depends on it never changing. It never allocates. When a
 * size does not fit in a size_t it answers TA_ERR_NOMEM and leaves the four
 * layout fields as they were.
 */
TA_PRIVATE ta_result ta_actor_layout(ta_definition *definition);

TA_PRIVATE ta_actor *ta_actor_lookup(const ta_system *system, ta_actor_id id);
/* The family atom of the resident actor `id`, or 0 when there is none (ADR 0088). */
TA_PRIVATE ta_atom ta_actor_family_of(const ta_system *system, ta_actor_id id);
/* A runtime reference to `id` that names its family (ADR 0088). */
TA_PRIVATE ta_actor_ref ta_actor_runtime_ref(const ta_system *system, ta_actor_id id);
TA_PRIVATE bool ta_actor_state_active(const ta_actor *actor, ta_state_id state);
/* Runs an invoked child's exitInterpreter path with isolated scratch storage. */
TA_PRIVATE void ta_actor_exit_invoked(ta_system *system, ta_actor_id id);

/*
 * A blank, unbootstrapped actor of `definition` with its slot claimed; the
 * data-model instance is allocated but not initialized. ta_actor_spawn()
 * bootstraps it, ta_actor_reconstruct() fills it from a session image.
 */
TA_PRIVATE ta_result ta_actor_new(ta_system *system, ta_definition *definition, ta_actor **out);
/* Tears the actor down completely and uncounts it against its definition. */
TA_PRIVATE void ta_actor_free(ta_system *system, ta_actor *actor);
/* Frees the slot an actor was given, so a failed construction reuses it. */
TA_PRIVATE void ta_actor_release_slot(ta_system *system, ta_actor_id id);

/* The host directory's compare-and-swaps, which every residency change uses. */
TA_PRIVATE ta_result ta_actor_directory_swap(ta_system *system, const ta_actor_directory_replace_params *params);
TA_PRIVATE ta_result ta_actor_directory_drop(ta_system *system, const ta_actor_directory_remove_params *params);

/* The external event the actor holds, waiting or taken, or null. */
TA_PRIVATE const ta_event *ta_actor_external_event(const ta_actor *actor);
/* The event `_event` names, or null (ADR 0069). The pointer is valid until
 * the actor's event block grows or the event is released. */
TA_PRIVATE const ta_event *ta_actor_bound_event(const ta_actor *actor);
/* Internal events waiting to be taken: the queue without a taken head. */
TA_PRIVATE uint32_t ta_actor_waiting_internal(const ta_actor *actor);

/* The actor's delayed sends, or null when it has no sparse state (so none). */
TA_PRIVATE ta_timer_list *ta_actor_timers(const ta_actor *actor);
/* Stores a delayed send with its owner; takes the send's values on TA_OK. */
TA_PRIVATE ta_result ta_actor_schedule_send(ta_system *system, ta_actor *actor, const ta_delayed_send *send);
/* Makes room to store one more delayed send, so that storing it cannot fail
 * (ADR 0070). */
TA_PRIVATE ta_result ta_actor_reserve_send(ta_system *system, ta_actor *actor);
/* The list a send owned by `owner` belongs in: the actor's, or the orphans when it is gone. */
TA_PRIVATE ta_result ta_actor_timer_home(ta_system *system, ta_actor_id owner, ta_timer_list **out);

/* True for a replaying actor (ADR 0089): prepared with `at`, not yet published. */
TA_PRIVATE bool ta_actor_replaying(const ta_actor *actor);
/* The time the actor lives at: a replaying actor's own clock, otherwise the
 * system's (ADR 0089 section 2). */
TA_PRIVATE ta_time ta_actor_now(const ta_system *system, const ta_actor *actor);
/* Whether `time` may be an input's time for `actor` (ADR 0094 section 1). */
TA_PRIVATE bool ta_actor_input_time_valid(const ta_system *system, const ta_actor *actor, ta_time time);
/* Moves a replaying actor's clock to a checked `time`, dropping held delayed
 * sends due by then (ADR 0089 section 2); nothing for any other actor. */
TA_PRIVATE void ta_actor_move_clock(ta_actor *actor, ta_time time);

/* Materialized effects of an unpublished replacement's bootstrap. */
TA_PRIVATE ta_result ta_actor_hold_send(ta_system *system, ta_actor *actor, const ta_activation_send *send);
TA_PRIVATE ta_result ta_actor_hold_log(ta_system *system, ta_actor *actor, const ta_log_record *record);
TA_PRIVATE bool ta_actor_cancel_held_send(ta_actor *actor, ta_send_id send_id);

/*
 * Advances the session epoch: the actor has changed in a way a session image
 * captured before the change no longer describes. ta_actor_unload() compares
 * epochs to refuse a stale image (ADR 0014), so this marks every semantic
 * mutation and nothing else — a read, an inspection, or a capture must not
 * move it.
 */
TA_PRIVATE inline void ta_actor_touch(ta_actor *actor) { actor->session_epoch += 1u; }



/* Session images are opaque; struct ta_session_image is private to session_image.c. */



/*
 * Tinyactors core deliberately defines no persistence-store protocol.  This
 * compatibility module remains empty while durable checkpointing, command
 * sourcing, and storage adapters live in optional higher-level components.
 */



/*
 * A nested interpreter's memory (ADR 0092 section 3): its structural region
 * and its render area, one allocation made the first time an invocation is
 * cancelled at this depth, then kept for reuse until the system goes.
 */
typedef struct {
    void *memory;
    size_t bytes;
} ta_nested_region;

typedef struct {
    ta_definition *definition;
    uint32_t generation;
    /* While free: the next free slot, or UINT32_MAX. */
    uint32_t next_free;
} ta_definition_slot;

struct ta_system {
    ta_allocator allocator;
    ta_clock clock;
    /* The first part of every session ID's text (ADR 0088). */
    uint64_t incarnation;
    ta_atom_table atoms;
    /* Interned named addresses (ADR 0065). */
    ta_address_table addresses;
    /* Reloaded and unloaded sessions, by session ID (ADR 0066). */
    ta_session_table sessions;
    ta_event_name_table event_names;

    /*
     * ta_definition_slot; a definition ID is (generation << 32) | slot, as
     * for actors. Freed slots form a list through `next_free`.
     */
    ta_vec definitions;
    uint32_t free_definition_slot;
    /* Compiled definitions still owned by callers, reclaimed at teardown. */
    ta_definition *unregistered_definitions;
    /* Orders registrations, so the newest identity match wins after slot reuse. */
    uint64_t next_definition_serial;

    /*
     * ta_actor_slot; an actor ID is ta_actor_id_make(slot, generation). Freed slots
     * form a list through `next_free`, as definitions' do: a spawn takes the
     * head instead of scanning the table, so it costs the same against four
     * million actors as against four (invariant 17).
     */
    ta_actor_table actors;
    uint32_t free_actor_slot;

    ta_timer_heap timers;

    /* The I/O processor registry (ADR 0068): registry index i is element i - 1; 0 is SCXML. */
    ta_vec io_processors; /* ta_io_registration */
    /* Invokers, whose element index is their registry index. */
    ta_vec invokers;      /* ta_invoker_registration */
    /* Stored URIs of SCOPED and MESSAGE I/O origins (ADR 0068). */
    ta_io_origin_table io_scopes;
    ta_io_origin_table io_messages;

    ta_actor_directory directory;
    ta_external_event_router external_events;
    ta_log_sink log_sink;
    /* Never null: ta_null_trace_sink() when the host installed none (ADR 0067). */
    ta_trace_sink trace;
    /* Never null: ta_null_definition_sink() when the host installed none (ADR 0079). */
    ta_definition_sink definition_sink;
    /* The trace-ID generator's state and the sampler's threshold (ADR 0073). */
    uint64_t trace_rng[4];
    uint64_t trace_threshold;
    /* Marks the bytes this system offloads (ADR 0074), drawn from the ID
     * generator at the first offload; 0 until then. */
    uint64_t offload_token;
    /* Every definition's counters, freed ones included (ADR 0072). */
    ta_counters counters;
    /* The system's parameters (ADR 0092), fixed at creation. */
    uint32_t params[TA_PARAM_COUNT];
    /* Live actors and definitions, for their quotas (ADR 0092 section 4). */
    uint32_t live_actors;
    uint32_t live_definitions;
    /*
     * The memory allocated at creation for the buffers every run needs (ADR
     * 0092 section 3): interpreter region 0 and its render area, and the
     * table of nested regions. `scratch` and `render` are arenas over it
     * that never grow.
     */
    void *arena_memory;
    size_t arena_bytes;
    ta_arena render;
    ta_nested_region *nested; /* interpreterNesting - 1 entries */
    ta_atom scxml_io_type;
    /* The short form "scxml" also selects the built-in processor. */
    ta_atom scxml_io_alias;
    /* Invoke: the canonical SCXML invoker type. */
    ta_atom scxml_invoke_type;
    ta_arena scratch;
    /* Empty event blocks, lent to the next actor that needs one (ADR 0064),
     * newest first, linked through `next_cached`; at most `block_cache_limit`
     * of them (ADR 0069). Empty at creation: a block is made when a delivery
     * finds none, and kept when released while there is room (ADR 0093). */
    ta_event_block *block_cache;
    uint32_t block_cache_count;
    uint32_t block_cache_limit;
    /*
     * What a run that stops inside a phase takes (ADR 0070): a continuation
     * at least as large as the running definition needs, and a sparse state
     * for an actor that has none. ta_interpreter_resume reserves both before
     * it runs anything, so stopping never allocates. Null when taken.
     */
    ta_continuation *spare_continuation;
    ta_actor_sparse_state *spare_sparse;
};

/*
 * The registered definition behind a live ID, retired or not; NULL for an
 * unknown or stale ID.
 */
TA_PRIVATE ta_definition *ta_system_definition(const ta_system *system, ta_definition_id id);
/*
 * A nested interpreter's two arenas (ADR 0092 section 3): region `depth`,
 * from 1, allocated the first time it is needed and kept. TA_ERR_UNSUPPORTED
 * past interpreterNesting, TA_ERR_NOMEM when it cannot be allocated.
 */
TA_PRIVATE ta_result ta_system_nested_region(ta_system *system, uint32_t depth, ta_arena *scratch, ta_arena *render);
/* False, counting the refusal, when a new actor would pass maxActors. */
TA_PRIVATE bool ta_system_quota_actor(ta_system *system);
/* Frees a registered definition and its slot if nothing holds or uses it,
 * and what that frees in turn (ADR 0077). */
TA_PRIVATE void ta_system_settle_definition(ta_system *system, ta_definition *definition);
/* Tells the definition sink of one lifecycle event (ADR 0079). */
TA_PRIVATE void ta_system_tell_definition(ta_system *system, const ta_definition *definition, ta_definition_event_kind kind);
/* The adapter registered for `type`, or null (not for the SCXML processor). */
TA_PRIVATE const ta_io *ta_system_io_of(const ta_system *system, ta_atom type);
/* The registration at registry index `index`, or null; index 0 (SCXML) has none. */
TA_PRIVATE const ta_io_registration *ta_system_io_at(const ta_system *system, uint32_t index);
/* The registry index of the invoker for `type`; UINT32_MAX when none. */
TA_PRIVATE uint32_t ta_system_invoker_index(const ta_system *system, ta_atom type);
/* The type of the invoker at registry index `index`; TA_ATOM_NONE when none. */
TA_PRIVATE ta_atom ta_system_invoker_type(const ta_system *system, uint32_t index);
TA_PRIVATE bool ta_system_is_scxml_io(const ta_system *system, ta_atom type);

/*
 * Counts one occurrence for `definition` and for the system (ADR 0072). A
 * definition's counters are bookkeeping, not part of what it compiled to,
 * so they change through a definition core otherwise treats as read-only.
 */
#define TA_COUNT(system, definition, field)                                                                   \
    do {                                                                                                      \
        ((ta_definition *)(definition))->counters.field += 1u;                                               \
        (system)->counters.field += 1u;                                                                       \
    } while (0)



/* A generated send ID (`#_send.<n>`, ADR 0048) is this bit and the session's counter n. */
#define TA_GENERATED_SEND_ID_BIT ((ta_send_id)1u << 63)

/*
 * Per-call interpreter scratch. Between steps the actor holds all state:
 * configuration, history, internal queue, and pending event. A run that
 * stops inside a phase, paused (ADR 0061) or out of memory (ADR 0070),
 * moves the rest, the position, the sets and the executor's frames, into
 * the actor's continuation.
 */
typedef struct {
    ta_system *system;
    ta_actor *actor;
    const ta_definition *definition;
    uint32_t budget;
    uint32_t steps;
    uint32_t words;
    uint64_t *active;
    uint64_t *to_exit;
    uint64_t *to_enter;
    uint64_t *default_entry;
    uint64_t *domain_targets;
    uint64_t *entry_targets;
    uint64_t *exit_left;
    uint64_t *exit_right;
    ta_state_id *default_history;
    ta_transition_id *enabled;
    uint32_t enabled_count;
    /* The operator transition being taken, while its microstep begins
     * (ADR 0082); null otherwise. */
    const struct ta_operator_jump *jump;
    ta_transition_id *filtered;
    bool *remove;
    /*
     * Scratch for the state-tree queries (ADR 0060 section 4), sized from
     * the definition when the interpreter starts, so no query allocates.
     */
    struct ta_entry_frame *entry_frames;
    uint32_t entry_capacity;
    ta_state_id *history_stack;
    uint64_t *history_seen;
    uint64_t *history_busy;
    uint64_t *final_states;
    /*
     * The executor's frames (ADR 0060 section 2): one per open block and one
     * per running <foreach>, sized from the definition's deepest nesting of
     * blocks when the interpreter starts, so running actions never allocates
     * frames.
     */
    ta_exec_frame *exec_frames;
    uint32_t exec_count;
    uint32_t exec_capacity;
    /*
     * Where the run stands inside a phase (ADR 0061, ADR 0070), the units of
     * executable content this call may run and has run, whether the block
     * running now is one a pause may stop, and whether a microstep this
     * call begins pauses at its start (ADR 0086).
     */
    ta_micro_position at;
    uint32_t action_budget;
    uint32_t actions;
    bool pausable;
    bool in_microstep;
    bool pause_at_start;
    /* The continuation this call resumed from, which the actor no longer
     * holds: the run stops into it again, or gives it to the system. */
    ta_continuation *resumed;
    /* The invocations of the macrostep's states just started: eventless
     * transitions are selected again only if that queued an internal event. */
    bool after_invokes;
    /* The data declarations the instance's binding already supplies, which
     * the bootstrap does not initialize (`decl_words` words). */
    uint64_t *bound_decls;
    uint32_t decl_words;
    /*
     * Tracing (ADR 0067). `trace_kinds` is what the running macrostep builds,
     * from the sink's mask and the macrostep's context. `trace_action` is the
     * unit of executable content running now. `trace_states` is scratch for
     * a record's state lists (3 * state_count), and `conditions` holds the
     * guards of the selection in progress, written only once it completes;
     * both are null when the sink wants no record that needs them.
     */
    uint32_t trace_kinds;
    ta_trace_context trace_context;
    ta_action_id trace_action;
    ta_state_id *trace_states;
    struct ta_trace_condition *conditions;
    uint32_t condition_count;
    uint32_t condition_capacity;
    /* Microsteps the running macrostep may run (ADR 0075); 0 for no limit. */
    uint32_t microstep_limit;
    /* The macrostep this call ended took an external event that enabled no
     * transition (ADR 0089 section 8). */
    bool ended_ignored;
} ta_interpreter;

TA_PRIVATE ta_time ta_interpreter_macrostep_time(const ta_system *system, const ta_actor *actor);

TA_PRIVATE ta_result ta_interpreter_resume(ta_system *system, ta_actor *actor, const ta_actor_resume_params *params);
/*
 * Whether an operator transition is well formed for the definition (ADR
 * 0082): at least one target, known states, and targets that can form a
 * legal configuration. Whether its source is active is the caller's check.
 */
TA_PRIVATE bool ta_interpreter_jump_valid(const ta_definition *definition, const ta_operator_transition *transition);
/* ta_actor_abandon() for a resident actor (ADR 0082). */
TA_PRIVATE ta_result ta_interpreter_abandon(ta_system *system, ta_actor *actor);
/*
 * Runs SCXML exitInterpreter for a cancelled invoked session, without
 * done.invoke, and for every session it invoked in turn, without recursion
 * (ADR 0060). A session that is gone or finished is skipped.
 */
TA_PRIVATE void ta_interpreter_exit_invoked(ta_system *system, ta_actor_id id);
/* Takes ownership of the event's data. */
TA_PRIVATE ta_result ta_interpreter_enqueue_internal(ta_system *system, ta_actor *actor, ta_event *event);
/* Dispatches a due delayed send; RETRY leaves the send owned by the caller. */
/*
 * `in_run` says the sender's own run dispatches it (an immediate <send>):
 * a failure is then raised in that run, and the unit that sent it counts
 * and records it. Otherwise (a timer, a held send released) the failure is
 * raised outside a run, and counted and recorded here (ADR 0072).
 */
TA_PRIVATE ta_external_event_submit_status ta_interpreter_dispatch_delayed(ta_system *system, ta_delayed_send *send, bool in_run);
/*
 * A literal content as a host adapter receives it (ADR 0053): its media type
 * and its bytes. The bytes' pointer is non-null even for empty content, so an
 * adapter can tell empty content from none.
 */
TA_PRIVATE void ta_interpreter_literal_content(const ta_definition *definition, ta_content_id content, ta_str *media_type,
                                    ta_bytes *bytes);
TA_PRIVATE void ta_interpreter_release_actor_state(ta_system *system, ta_actor *actor);
/*
 * The interpreter scratch a definition needs (ADR 0092 section 3), in
 * `*out`. A pure computation with call lifetime that never allocates; it
 * answers TA_ERR_NOMEM, and leaves `*out` alone, when the figure does not
 * fit in a size_t.
 */
TA_PRIVATE ta_result ta_interpreter_scratch_need(const ta_definition *definition, size_t *out);
/* The fault of a temporary that does not fit the render area (ADR 0092). */
TA_PRIVATE void ta_interpreter_render_fault(ta_fault *fault);
/* ta_interpreter_block_cache_trim frees cached event blocks until the cache
 * holds no more than the system's block_cache_limit (ADR 0069); with a limit
 * of 0 it empties the cache, which is what ta_system_destroy does. The cache
 * fills on demand, never at creation (ADR 0093). It never allocates. */
TA_PRIVATE void ta_interpreter_block_cache_trim(ta_system *system);
/* The largest continuation a run of the definition can save: its shape with
 * the offsets of its arrays, and its size; TA_ERR_NOMEM, outputs untouched,
 * when that does not fit. Never allocates. */
TA_PRIVATE ta_result ta_continuation_capacity(const ta_definition *definition, ta_continuation *shape, size_t *bytes);

/* Frees the continuation and sparse state the system keeps for a run that
 * stops (ADR 0070), at teardown. */
TA_PRIVATE void ta_interpreter_release_spares(ta_system *system);
/* Lends the actor an event block if it holds none: a cached one, else a
 * header only. TA_ERR_NOMEM changes nothing (ADR 0069). */
TA_PRIVATE ta_result ta_interpreter_hold_block(ta_system *system, ta_actor *actor);
/* The bytes an event block with room for this many internal events takes. */
TA_PRIVATE size_t ta_event_block_bytes(uint32_t capacity);

/* Send IDs as text: literal IDs are their author's text; generated ones round-trip. */
TA_PRIVATE void ta_send_id_write(const ta_system *system, ta_send_id id, ta_text_writer *writer);
/* Returns 0 when the text names no send ID. */
TA_PRIVATE ta_send_id ta_send_id_from_text(const ta_system *system, ta_str text);
/* ------------------------------------------------------------------------
 * Interpreter services used by invoke.c
 */

typedef struct {
    ta_value_source source;
    ta_value *out;
    ta_fault *fault;
} ta_interpreter_value_params;

TA_PRIVATE ta_actor_sparse_state *ta_interpreter_sparse(ta_system *system, ta_actor *actor);
TA_PRIVATE ta_op_status ta_interpreter_eval(ta_interpreter *it, const ta_interpreter_value_params *params);
/*
 * Replaces *value, which is about to leave the session, with the data
 * model's copy_out of it (ADR 0097). An absent value stays as it is, and so
 * does every value of a data model without copy_out, which shares. On
 * failure *value is unchanged and still the caller's.
 */
TA_PRIVATE ta_op_status ta_interpreter_copy_out(ta_interpreter *it, ta_value *value, ta_fault *fault);
/* Queues error.execution; returns TA_OK unless queueing itself failed. */
TA_PRIVATE ta_result ta_interpreter_execution_error(ta_interpreter *it, ta_fault *fault);
/* Makes sure the actor's queue has room for one more internal event, so
 * queueing it cannot fail (ADR 0070). */
TA_PRIVATE ta_result ta_interpreter_queue_room(ta_system *system, ta_actor *actor);
/* Room for `count` more internal events, so that queueing them cannot fail. */
TA_PRIVATE ta_result ta_interpreter_queue_reserve(ta_system *system, ta_actor *actor, uint32_t count);
/* How many events the actor's queue holds, a taken head included: a mark
 * a step's RAISED records start from. */
TA_PRIVATE uint32_t ta_interpreter_queue_mark(const ta_actor *actor);
/* Writes a RAISED record for every event queued after `mark`, once the step
 * that queued them has committed (ADR 0067). */
TA_PRIVATE void ta_interpreter_trace_raised(ta_interpreter *it, uint32_t mark);
/* Writes an INVOKED or INVOKE_CANCELLED record for `invoke`. */
TA_PRIVATE void ta_interpreter_trace_invoke(ta_interpreter *it, ta_trace_kind kind, ta_invoke_id invoke);
/* Records that `event` was autoforwarded to `invoke` (ADR 0067 open point 3). */
TA_PRIVATE void ta_interpreter_trace_forward(ta_interpreter *it, ta_invoke_id invoke, const ta_event *event);
/* What a step running now passes to what it produces: the macrostep's
 * context, and the step's own key as the cause (ADR 0067 section 6). */
TA_PRIVATE ta_trace_parent ta_interpreter_trace_here(const ta_interpreter *it);
/* The parent the actor's current macrostep runs under; all zero for none. */
TA_PRIVATE ta_trace_parent ta_actor_macrostep_parent(const ta_actor *actor);
/* The key of the step the actor stands in: its macrostep and microstep
 * while a macrostep has begun, the session alone otherwise (ADR 0067). */
TA_PRIVATE ta_step_key ta_actor_step(const ta_actor *actor);
/* Where the actor's stopped macrostep stands, as its last PAUSED or STOPPED
 * record said (ADR 0083); nowhere when no phase is open. */
TA_PRIVATE ta_trace_position ta_actor_position(const ta_actor *actor);
/* Adds the actor's active states to its definition's occupancy, or takes
 * them away (ADR 0072), when it becomes resident or stops being so. */
TA_PRIVATE void ta_interpreter_occupancy(ta_actor *actor, bool arrive);
/* Counts and records an event core queued outside a run (ADR 0072), with
 * `parent` as its cause when there is one. */
TA_PRIVATE void ta_interpreter_raised_outside(ta_system *system, ta_actor *actor, ta_event_name_id name,
                                   const ta_trace_parent *parent);
/*
 * Makes an idle actor run a macrostep for work queued outside any run, such
 * as a failed send's error, under `trace` (null: none). An actor already in
 * a macrostep is left alone.
 */
TA_PRIVATE void ta_interpreter_start_macrostep(ta_actor *actor, const ta_trace_parent *trace);
TA_PRIVATE bool ta_interpreter_internal_pending(const ta_actor *actor);

/* ------------------------------------------------------------------------
 * Invoke hooks, implemented in invoke.c
 */

/* Interns an SCXML invoke ID's text; the runtime ID is its atom + 1. */
TA_PRIVATE ta_result ta_invoke_id_from_text(ta_system *system, ta_str text, ta_invoke_id *out);
TA_PRIVATE ta_str ta_invoke_id_text(const ta_system *system, ta_invoke_id invoke);
TA_PRIVATE ta_result ta_invoke_state_entered(ta_interpreter *it, ta_state_id state);
TA_PRIVATE ta_result ta_invoke_state_exited(ta_interpreter *it, ta_state_id state);
/* An invocation taken out of an actor's table, to be cancelled. */
typedef struct {
    ta_active_invoke active;
    ta_invoke_meta meta;
} ta_invoke_taken;
/* Forgets the not-yet-started invocations of `state`. */
TA_PRIVATE void ta_invoke_forget_pending(ta_actor *actor, ta_state_id state);
/* Takes the last invocation of `state` below index `*cursor` out of the
 * actor's table; false when there is none. */
TA_PRIVATE bool ta_invoke_take(ta_actor *actor, ta_state_id state, uint32_t *cursor, ta_invoke_taken *out);
/* Tells the invoker to cancel the service of a taken invocation. */
TA_PRIVATE void ta_invoke_cancel_service(const ta_invoke_taken *taken);
/* Whether states the macrostep entered have invocations still to start. */
TA_PRIVATE bool ta_invoke_has_pending(const ta_actor *actor);
/*
 * Starts the invocations of the states entered during the finished
 * macrostep, from state `*state` and <invoke> declaration `*decl` on. On
 * TA_ERR_NOMEM they name the invocation that did not start, and nothing of
 * it happened; the next call starts from there (ADR 0070).
 */
TA_PRIVATE ta_result ta_invoke_start_pending(ta_interpreter *it, uint32_t *state, uint32_t *decl);
/* Whether <finalize> and autoforwarding apply to an external event just
 * taken: not for an event of an invocation cancelled since it was queued. */
TA_PRIVATE bool ta_invoke_before_external(const ta_actor *actor, const ta_event *event);
/* The <finalize> actions of the invocation `event` came from; false when it
 * came from none that is still active. */
TA_PRIVATE bool ta_invoke_finalize_actions(const ta_actor *actor, const ta_event *event, ta_span32 *out);
/* Autoforwards `event` to every invocation that asks for it, from index
 * `*cursor` on; on TA_ERR_NOMEM `*cursor` names the one to forward next. */
TA_PRIVATE ta_result ta_invoke_autoforward(ta_interpreter *it, const ta_event *event, uint32_t *cursor);
/* Cancels every invocation and publishes done.invoke to the parent, if any. */
TA_PRIVATE ta_result ta_invoke_finished(ta_interpreter *it);
TA_PRIVATE bool ta_invoke_destination(const ta_actor *actor, ta_invoke_id invoke, ta_actor_ref *out);
/* The invoke ID an event from `sender` to `destination` carries: set for child-to-parent events. */
TA_PRIVATE ta_invoke_id ta_invoke_parent_id(const ta_actor *sender, const ta_actor_ref *destination);
/* False after a parent has retired the invocation that identifies this child. */
TA_PRIVATE bool ta_invoke_parent_accepts(const ta_system *system,
                              const ta_actor *sender,
                              const ta_actor_ref *destination);
TA_PRIVATE void ta_invoke_attach_child(ta_system *system, const ta_actor_parent *parent, ta_actor_id child);
/* Cancels the actor's services and frees its invoke bookkeeping. */
TA_PRIVATE void ta_invoke_release(ta_system *system, ta_actor *actor);


TA_API uint32_t ta_version(void) {
    return TA_VERSION;
}

TA_PRIVATE void *ta_mem_alloc(ta_allocator allocator, size_t size, size_t alignment) {
    if (allocator.v == NULL || allocator.v->alloc == NULL || size == 0) {
        return NULL;
    }
    return allocator.v->alloc(allocator.self, size, alignment);
}

TA_PRIVATE void ta_mem_free(ta_allocator allocator, void *ptr, const ta_allocator_free_params *layout) {
    if (ptr == NULL || allocator.v == NULL || allocator.v->free == NULL) {
        return;
    }
    ta_allocator_free_params params = *layout;
    params.ptr = ptr;
    allocator.v->free(allocator.self, &params);
}

TA_PRIVATE void ta_mem_copy(void *dst, const void *src, size_t size) {
    unsigned char *out = dst;
    const unsigned char *in = src;
    for (size_t index = 0; index < size; index += 1) {
        out[index] = in[index];
    }
}

TA_PRIVATE void ta_mem_zero(void *dst, size_t size) {
    unsigned char *out = dst;
    for (size_t index = 0; index < size; index += 1) {
        out[index] = 0;
    }
}

TA_PRIVATE bool ta_str_equal(ta_str left, ta_str right) {
    if (left.len != right.len) {
        return false;
    }
    for (uint32_t index = 0; index < left.len; index += 1) {
        if (left.ptr[index] != right.ptr[index]) {
            return false;
        }
    }
    return true;
}

TA_PRIVATE void ta_text_put(ta_text_writer *writer, ta_str text) {
    for (uint32_t index = 0; index < text.len; index += 1) {
        if (writer->buffer != NULL && writer->length >= writer->skip &&
            writer->length - writer->skip < writer->capacity) {
            writer->buffer[writer->length - writer->skip] = text.ptr[index];
        }
        writer->length += 1;
    }
}

TA_PRIVATE void ta_text_put_u64(ta_text_writer *writer, uint64_t value) {
    char digits[TA_DEC_U64_DIGITS];
    uint32_t count = ta_dec_write(digits, value);
    ta_text_put(writer, (ta_str){ .ptr = digits, .len = count });
}

TA_PRIVATE ta_result ta_text_finish(ta_text_writer *writer, uint32_t *needed) {
    *needed = writer->length + 1u;
    if (writer->buffer == NULL || writer->capacity < writer->length + 1u) {
        return TA_ERR_NOMEM;
    }
    writer->buffer[writer->length] = '\0';
    return TA_OK;
}

TA_PRIVATE ta_result ta_text_render(ta_allocator allocator, const ta_text_render_params *params) {
    ta_text_writer measure = {0};
    if (!params->write(params->context, &measure)) {
        return TA_ERR_NOT_FOUND;
    }
    char *buffer = ta_mem_alloc(allocator, (size_t)measure.length + 1u, 1);
    if (buffer == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_text_writer writer = { .buffer = buffer, .capacity = measure.length + 1u };
    (void)params->write(params->context, &writer);
    buffer[measure.length] = '\0';
    *params->out = (ta_str){ .ptr = buffer, .len = measure.length };
    return TA_OK;
}

/*
 * Implementation of block D5 (hash). The contract, lifetime and usage are in
 * hash.h; this comment records what the code relies on.
 *
 * Lifetime and allocation. Call lifetime only. Nothing here allocates, frees
 * or keeps a pointer: no function takes an allocator, so none can fail.
 *
 * Constants. The FNV prime for 32 bits is 16777619 and for 64 bits
 * 1099511628211. The bases live in hash.h. The mixer is the fixed 64-bit
 * Murmur3 finalizer (shifts of 33, multipliers 0xff51afd7ed558ccd and
 * 0xc4ceb9fe1a85ec53). These values are persisted or visible: content
 * hashes become revision fingerprints, and span and trace IDs derive from
 * ta_mix64, so changing any constant changes IDs that tests compare
 * bit for bit (span_test).
 *
 * Byte order. FNV-1a consumes bytes one at a time in memory order, through
 * an unsigned char pointer, so the result does not depend on the host's
 * endianness or alignment. Multiplication wraps modulo 2^32 or 2^64 by
 * definition of the unsigned types; that wrap is intended, and it is the only
 * arithmetic in the file, so there is no size computation to check.
 *
 * Complexity. ta_fnv1a32 and ta_fnv1a64 are O(size); ta_mix64 is O(1).
 */


TA_PRIVATE uint32_t ta_fnv1a32(uint32_t state, const void *bytes, size_t size) {
    const unsigned char *in = bytes;
    for (size_t index = 0; index < size; index += 1) {
        state ^= in[index];
        state *= 16777619u;
    }
    return state;
}

TA_PRIVATE uint64_t ta_fnv1a64(uint64_t state, const void *bytes, size_t size) {
    const unsigned char *in = bytes;
    for (size_t index = 0; index < size; index += 1) {
        state ^= in[index];
        state *= 1099511628211ull;
    }
    return state;
}

TA_PRIVATE uint64_t ta_mix64(uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}


/*
 * Implementation of block E1 (handle). The contract, lifetime and usage are
 * in handle.h. Nothing here allocates, can fail or keeps state; the layout
 * is fixed by ADR 0059 and persisted, so the shifts and masks below are the
 * only place it is written down.
 */

TA_PRIVATE ta_handle ta_handle_make(uint32_t slot, uint32_t generation) {
    return ((ta_handle)generation << 32) | slot;
}

TA_PRIVATE uint32_t ta_handle_slot(ta_handle handle) {
    return (uint32_t)(handle & 0xffffffffu);
}

TA_PRIVATE uint32_t ta_handle_generation(ta_handle handle) {
    return (uint32_t)(handle >> 32);
}

TA_PRIVATE bool ta_handle_valid(ta_handle handle) {
    return ta_handle_generation(handle) != 0u;
}

TA_PRIVATE bool ta_handle_matches(ta_handle handle, uint32_t current_generation) {
    return ta_handle_valid(handle) && ta_handle_generation(handle) == current_generation;
}

TA_PRIVATE uint32_t ta_handle_next_generation(uint32_t current_generation) {
    return current_generation == UINT32_MAX || current_generation == 0u ? 1u : current_generation + 1u;
}

/*
 * Implementation of block D6 (format). The contract, lifetime and usage are
 * in format.h; this comment records what the code relies on.
 *
 * Lifetime and allocation. Call lifetime only. Nothing here allocates,
 * frees or keeps a pointer. The writers fill a buffer the caller owns and
 * sized beforehand; they never look past the count they were given or
 * return, and write no terminator. The caller sizes the buffer with
 * ta_dec_len (decimal) or from the digit count it passes (hex), so no size
 * arithmetic happens here and none can overflow.
 *
 * Ordering. Both writers fill from the last digit backwards, so the most
 * significant digit lands first and no scratch buffer or reversal is needed.
 * Decimal peels one digit per division by 10; hex peels one nibble per shift
 * by 4. ta_hex_write keeps only the low "count" digits of the value and
 * pads on the left with zeros, which is what fixed-width fingerprints and
 * IDs need.
 *
 * Case. ta_hex_write always writes lower case, which is the form persisted
 * in IDs and fingerprints. ta_hex_digit takes the case as an argument for
 * the callers that print upper case. ta_hex_digit_value accepts both and
 * returns -1 for anything else, so parsing is case-insensitive while output
 * is canonical.
 *
 * Complexity. Each function is O(digits); ta_hex_digit and
 * ta_hex_digit_value are O(1).
 */


TA_PRIVATE uint32_t ta_dec_len(uint64_t value) {
    uint32_t count = 1;
    while (value >= 10u) {
        value /= 10u;
        count += 1;
    }
    return count;
}

TA_PRIVATE uint32_t ta_dec_write(char *out, uint64_t value) {
    uint32_t count = ta_dec_len(value);
    for (uint32_t index = count; index > 0; index -= 1) {
        out[index - 1u] = (char)('0' + value % 10u);
        value /= 10u;
    }
    return count;
}

TA_PRIVATE char ta_hex_digit(uint32_t nibble, bool upper) {
    static const char lower[] = "0123456789abcdef";
    static const char capital[] = "0123456789ABCDEF";
    return (upper ? capital : lower)[nibble & 0xfu];
}

TA_PRIVATE void ta_hex_write(char *out, uint64_t value, uint32_t count) {
    for (uint32_t index = count; index > 0; index -= 1) {
        out[index - 1u] = ta_hex_digit((uint32_t)(value & 0xfu), false);
        value >>= 4;
    }
}

TA_PRIVATE int ta_hex_digit_value(char digit) {
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }
    return -1;
}


/*
 * The implementation of package layout (see layout.h for the contract).
 * Everything here is arithmetic on size_t values passed in; nothing is
 * allocated, read from memory or kept.
 */

TA_PRIVATE bool ta_checked_mul(size_t a, size_t b, size_t *out) {
    /* Dividing, not multiplying and comparing: the division cannot wrap. */
    if (a != 0 && b > SIZE_MAX / a) {
        return false;
    }
    *out = a * b;
    return true;
}

TA_PRIVATE bool ta_checked_add(size_t a, size_t b, size_t *out) {
    if (b > SIZE_MAX - a) {
        return false;
    }
    *out = a + b;
    return true;
}

TA_PRIVATE void ta_layout_init(ta_layout *layout, size_t start) {
    *layout = (ta_layout){ .internal_end = start, .internal_alignment = 1u, .internal_overflowed = false };
}

TA_PRIVATE size_t ta_layout_add(ta_layout *layout, const ta_layout_entry *entry) {
    size_t offset = 0;
    size_t bytes = 0;
    size_t end = 0;
    if (layout->internal_overflowed) {
        return 0;
    }
    if (entry->alignment == 0) {
        layout->internal_overflowed = true;
        return 0;
    }
    /* Round the end up by the gap to the next multiple, adding the gap
     * rather than the whole alignment so that an end already aligned (and
     * one close to SIZE_MAX) is not reported as an overflow. */
    size_t remainder = layout->internal_end % entry->alignment;
    size_t gap = remainder == 0 ? 0 : entry->alignment - remainder;
    if (!ta_checked_add(layout->internal_end, gap, &offset) || !ta_checked_mul(entry->size, entry->count, &bytes) ||
        !ta_checked_add(offset, bytes, &end)) {
        layout->internal_overflowed = true;
        return 0;
    }
    layout->internal_end = end;
    if (entry->alignment > layout->internal_alignment) {
        layout->internal_alignment = entry->alignment;
    }
    return offset;
}

TA_PRIVATE ta_result ta_layout_finish(const ta_layout *layout, size_t *bytes, size_t *alignment) {
    if (layout->internal_overflowed) {
        return TA_ERR_NOMEM;
    }
    if (bytes != NULL) {
        *bytes = layout->internal_end;
    }
    if (alignment != NULL) {
        *alignment = layout->internal_alignment;
    }
    return TA_OK;
}


/* The package comment is in vec.h. */

TA_PRIVATE size_t ta_vec_bytes(const ta_vec *vec) {
    /* ta_vec_reserve checked this product before it allocated the block. */
    return (size_t)vec->capacity * vec->element_size;
}

TA_PRIVATE void ta_vec_init(ta_vec *vec, ta_allocator allocator, const ta_tree_list_layout *layout) {
    *vec = (ta_vec){
        .allocator = allocator,
        .element_size = layout->element_size,
        .element_alignment = layout->element_alignment,
    };
}

TA_PRIVATE void ta_vec_destroy(ta_vec *vec) {
    if (vec->items != NULL) {
        ta_mem_free(vec->allocator, vec->items,
                    &(ta_allocator_free_params){
                        .size = ta_vec_bytes(vec),
                        .alignment = vec->element_alignment,
                    });
    }
    vec->items = NULL;
    vec->count = 0;
    vec->capacity = 0;
}

TA_PRIVATE ta_result ta_vec_reserve(ta_vec *vec, uint32_t count) {
    if (count <= vec->capacity) {
        return TA_OK;
    }
    uint32_t capacity = vec->capacity == 0 ? 8u : vec->capacity;
    while (capacity < count) {
        if (capacity > UINT32_MAX / 2u) {
            return TA_ERR_NOMEM;
        }
        capacity *= 2u;
    }
    size_t bytes = 0;
    if (!ta_checked_mul(capacity, vec->element_size, &bytes)) {
        return TA_ERR_NOMEM;
    }
    void *items = ta_mem_alloc(vec->allocator, bytes, vec->element_alignment);
    if (items == NULL) {
        return TA_ERR_NOMEM;
    }
    if (vec->items != NULL) {
        /* count <= old capacity, whose product was checked when it was allocated. */
        ta_mem_copy(items, vec->items, (size_t)vec->count * vec->element_size);
        ta_mem_free(vec->allocator, vec->items,
                    &(ta_allocator_free_params){
                        .size = ta_vec_bytes(vec),
                        .alignment = vec->element_alignment,
                    });
    }
    vec->items = items;
    vec->capacity = capacity;
    return TA_OK;
}

TA_PRIVATE ta_result ta_vec_extend(ta_vec *vec, uint32_t count, uint32_t *out) {
    if (count > UINT32_MAX - vec->count) {
        return TA_ERR_NOMEM;
    }
    ta_result result = ta_vec_reserve(vec, vec->count + count);
    if (result != TA_OK) {
        return result;
    }
    if (out != NULL) {
        *out = vec->count;
    }
    if (count != 0) {
        ta_mem_zero((unsigned char *)vec->items + (size_t)vec->count * vec->element_size,
                    (size_t)count * vec->element_size);
    }
    vec->count += count;
    return TA_OK;
}

TA_PRIVATE ta_result ta_vec_push(ta_vec *vec, const void *element) {
    uint32_t index = 0;
    ta_result result = ta_vec_extend(vec, 1, &index);
    if (result == TA_OK) {
        ta_mem_copy(ta_vec_at(vec, index), element, vec->element_size);
    }
    return result;
}

TA_PRIVATE void *ta_vec_at(const ta_vec *vec, uint32_t index) {
    return (unsigned char *)vec->items + (size_t)index * vec->element_size;
}

/*
 * Where column `column` goes in a block of `capacity` items, and how big and
 * how aligned the whole block is. The rest of the table is placed too,
 * because the total needs every column. It never allocates, and answers
 * TA_ERR_NOMEM, writing nothing, when a size does not fit.
 */
typedef struct {
    uint32_t capacity;
    uint32_t column;
} ta_columns_query;

typedef struct {
    size_t offset;
    size_t bytes;
    size_t alignment;
} ta_columns_placement;

static ta_result ta_columns_place(const ta_columns_layout *layout, ta_columns_query query,
                                  ta_columns_placement *out) {
    ta_layout block;
    ta_layout_init(&block, 0);
    size_t offset = 0;
    for (uint32_t index = 0; index < layout->column_count; index += 1u) {
        const ta_column *column = &layout->columns[index];
        size_t at = ta_layout_add(&block, &(ta_layout_entry){ .size = column->size,
                                                             .alignment = column->alignment,
                                                             .count = query.capacity });
        offset = index == query.column ? at : offset;
    }
    size_t bytes = 0;
    size_t alignment = 0;
    ta_result result = ta_layout_finish(&block, &bytes, &alignment);
    if (result == TA_OK) {
        *out = (ta_columns_placement){ .offset = offset, .bytes = bytes, .alignment = alignment };
    }
    return result;
}

TA_PRIVATE size_t ta_columns_bytes(const ta_columns_layout *layout, const ta_columns *table) {
    ta_columns_placement placement = { 0 };
    if (table->block == NULL ||
        ta_columns_place(layout, (ta_columns_query){ .capacity = table->capacity }, &placement) != TA_OK) {
        return 0;
    }
    return placement.bytes;
}

TA_PRIVATE void *ta_columns_at(const ta_columns_layout *layout, const ta_columns *table, uint32_t index) {
    ta_columns_placement placement = { 0 };
    if (table->block == NULL || index >= layout->column_count ||
        ta_columns_place(layout, (ta_columns_query){ .capacity = table->capacity, .column = index },
                         &placement) != TA_OK) {
        return NULL;
    }
    return (unsigned char *)table->block + placement.offset;
}

TA_PRIVATE void ta_columns_release(ta_allocator allocator, const ta_columns_layout *layout, ta_columns *table) {
    ta_columns_placement placement = { 0 };
    /* A block exists only because this same placement fit when it was allocated. */
    if (table->block != NULL &&
        ta_columns_place(layout, (ta_columns_query){ .capacity = table->capacity }, &placement) == TA_OK) {
        ta_mem_free(allocator, table->block,
                    &(ta_allocator_free_params){ .size = placement.bytes, .alignment = placement.alignment });
    }
    *table = (ta_columns){ 0 };
}

TA_PRIVATE ta_result ta_columns_grow(ta_allocator allocator, const ta_columns_layout *layout, ta_columns *table) {
    if (table->count < table->capacity) {
        return TA_OK;
    }
    uint32_t capacity = layout->first_capacity;
    if (table->capacity != 0) {
        if (table->capacity > UINT32_MAX / 2u) {
            return TA_ERR_NOMEM;
        }
        capacity = table->capacity * 2u;
    }
    ta_columns_placement next = { 0 };
    if (ta_columns_place(layout, (ta_columns_query){ .capacity = capacity }, &next) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    unsigned char *block = ta_mem_alloc(allocator, next.bytes, next.alignment);
    if (block == NULL) {
        return TA_ERR_NOMEM;
    }
    /* From here nothing fails: the old block's placement fit when it was made. */
    for (uint32_t index = 0; table->block != NULL && index < layout->column_count; index += 1u) {
        ta_columns_placement from = { 0 };
        ta_columns_placement to = { 0 };
        (void)ta_columns_place(layout, (ta_columns_query){ .capacity = table->capacity, .column = index }, &from);
        (void)ta_columns_place(layout, (ta_columns_query){ .capacity = capacity, .column = index }, &to);
        ta_mem_copy(block + to.offset, (unsigned char *)table->block + from.offset,
                    (size_t)table->count * layout->columns[index].size);
    }
    uint32_t count = table->count;
    ta_columns_release(allocator, layout, table);
    *table = (ta_columns){ .block = block, .count = count, .capacity = capacity };
    return TA_OK;
}


static bool ta_arena_valid_alignment(size_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1u)) == 0;
}

static size_t ta_arena_max_size(size_t left, size_t right) {
    return left > right ? left : right;
}

static void *ta_arena_allocate_from_block(ta_arena_block *block,
                                          size_t size,
                                          size_t alignment) {
    uintptr_t base = (uintptr_t)block->data;
    uintptr_t current = base + block->used;
    uintptr_t aligned = (current + alignment - 1u) & ~(uintptr_t)(alignment - 1u);
    size_t offset = (size_t)(aligned - base);
    if (offset > block->capacity || size > block->capacity - offset) {
        return NULL;
    }
    block->used = offset + size;
    return (void *)aligned;
}

TA_API ta_result ta_arena_init(ta_arena *arena, ta_allocator backing, size_t block_size) {
    if (arena == NULL || backing.v == NULL || backing.v->alloc == NULL ||
        backing.v->free == NULL || block_size == 0) {
        return TA_ERR_INVALID;
    }
    *arena = (ta_arena){
        .backing = backing,
        .block_size = block_size,
    };
    return TA_OK;
}

TA_API void ta_arena_reset(ta_arena *arena) {
    if (arena == NULL) {
        return;
    }
    for (ta_arena_block *block = arena->head; block != NULL; block = block->next) {
        block->used = 0;
    }
}

TA_API void ta_arena_destroy(ta_arena *arena) {
    if (arena == NULL || arena->backing.v == NULL) {
        return;
    }
    ta_arena_block *block = arena->head;
    while (block != NULL) {
        ta_arena_block *next = block->next;
        arena->backing.v->free(
            arena->backing.self,
            &(ta_allocator_free_params){
                .ptr = block,
                .size = block->allocation_size,
                .alignment = block->alignment,
            });
        block = next;
    }
    *arena = (ta_arena){0};
}

TA_API void *ta_arena_alloc(ta_arena *arena, size_t size, size_t alignment) {
    if (arena == NULL || !ta_arena_valid_alignment(alignment) || size == 0) {
        return NULL;
    }
    for (ta_arena_block *block = arena->head; block != NULL; block = block->next) {
        void *result = ta_arena_allocate_from_block(block, size, alignment);
        if (result != NULL) {
            return result;
        }
    }

    if (size > SIZE_MAX - (alignment - 1u)) {
        return NULL;
    }
    size_t capacity = ta_arena_max_size(arena->block_size, size + alignment - 1u);
    if (capacity > SIZE_MAX - offsetof(ta_arena_block, data)) {
        return NULL;
    }
    size_t allocation_size = offsetof(ta_arena_block, data) + capacity;
    size_t block_alignment = ta_arena_max_size(_Alignof(ta_arena_block), alignment);
    ta_arena_block *block = arena->backing.v->alloc(arena->backing.self,
                                                    allocation_size,
                                                    block_alignment);
    if (block == NULL) {
        return NULL;
    }
    *block = (ta_arena_block){
        .next = arena->head,
        .allocation_size = allocation_size,
        .alignment = block_alignment,
        .capacity = capacity,
    };
    arena->head = block;
    return ta_arena_allocate_from_block(block, size, alignment);
}

TA_PRIVATE size_t ta_arena_allocated_bytes(const ta_arena *arena) {
    size_t total = 0;
    for (const ta_arena_block *block = arena->head; block != NULL; block = block->next) {
        total += block->allocation_size;
    }
    return total;
}

TA_PRIVATE ta_result ta_arena_copy_str(ta_arena *arena, ta_str text, ta_str *out) {
    char *copy = ta_arena_alloc(arena, (size_t)text.len + 1u, 1);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    if (text.len != 0) {
        ta_mem_copy(copy, text.ptr, text.len);
    }
    copy[text.len] = '\0';
    *out = (ta_str){ .ptr = copy, .len = text.len };
    return TA_OK;
}

static void *ta_arena_allocator_alloc(void *self, size_t size, size_t alignment) {
    return ta_arena_alloc(self, size, alignment);
}

static void ta_arena_allocator_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    (void)params;
}

static const ta_allocator_vtable ta_arena_allocator_vtable = {
    .alloc = ta_arena_allocator_alloc,
    .free = ta_arena_allocator_free,
};

TA_API ta_allocator ta_arena_allocator(ta_arena *arena) {
    return (ta_allocator){ .self = arena, .v = &ta_arena_allocator_vtable };
}

/* A fixed arena's backing: it never gives memory, and its blocks are not its own. */
static void *ta_arena_fixed_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    (void)size;
    (void)alignment;
    return NULL;
}

static void ta_arena_fixed_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    (void)params;
}

static const ta_allocator_vtable ta_arena_fixed_vtable = {
    .alloc = ta_arena_fixed_alloc,
    .free = ta_arena_fixed_free,
};

TA_PRIVATE size_t ta_arena_fixed_add(ta_layout *layout, size_t capacity) {
    size_t offset = ta_layout_add(layout, &(ta_layout_entry){ .size = offsetof(ta_arena_block, data),
                                                              .alignment = _Alignof(ta_arena_block), .count = 1u });
    /* The data follows the block header directly, as ta_arena_init_fixed lays it out. */
    ta_layout_add(layout, &(ta_layout_entry){ .size = 1u, .alignment = 1u, .count = capacity });
    return offset;
}

TA_PRIVATE void ta_arena_init_fixed(ta_arena *arena, void *memory, size_t bytes) {
    ta_arena_block *block = memory;
    *block = (ta_arena_block){
        .next = NULL,
        .allocation_size = bytes,
        .alignment = _Alignof(ta_arena_block),
        .capacity = bytes - offsetof(ta_arena_block, data),
        .used = 0,
    };
    /* block_size is never used to grow: the backing refuses. */
    *arena = (ta_arena){ .backing = { .self = NULL, .v = &ta_arena_fixed_vtable }, .head = block, .block_size = 1 };
}


/* The arena block every tree starts from, as elsewhere in core. */
#define TA_TREE_BLOCK_SIZE 4096u
/* Elements in a list's first staging chunk; every next chunk doubles. */
#define TA_TREE_LIST_FIRST_CHUNK 8u

static bool ta_tree_power_of_two(size_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1u)) == 0;
}

/* Records the first failure only: an importer reports one result at its
 * outer boundary, and a later, smaller allocation must not clear it. */
static void ta_tree_fail(ta_tree *tree, ta_result result) {
    if (tree != NULL && tree->result == TA_OK) {
        tree->result = result;
    }
}

TA_API void ta_tree_init(ta_tree *tree, ta_allocator allocator) {
    if (tree == NULL) {
        return;
    }
    *tree = (ta_tree){ .allocator = allocator, .result = TA_OK, .impl = NULL };
    if (allocator.v == NULL || allocator.v->alloc == NULL || allocator.v->free == NULL) {
        tree->result = TA_ERR_INVALID;
        return;
    }
    ta_tree_state *state = ta_mem_alloc(allocator, sizeof(ta_tree_state), _Alignof(ta_tree_state));
    if (state == NULL) {
        tree->result = TA_ERR_NOMEM;
        return;
    }
    *state = (ta_tree_state){0};
    ta_result result = ta_arena_init(&state->arena, allocator, TA_TREE_BLOCK_SIZE);
    if (result != TA_OK) {
        ta_mem_free(allocator, state,
                    &(ta_allocator_free_params){ .size = sizeof(ta_tree_state),
                                                 .alignment = _Alignof(ta_tree_state) });
        tree->result = result;
        return;
    }
    tree->impl = state;
}

TA_API void ta_tree_destroy(ta_tree *tree) {
    if (tree == NULL) {
        return;
    }
    ta_tree_state *state = tree->impl;
    if (state != NULL) {
        ta_arena_destroy(&state->arena);
        ta_mem_free(tree->allocator, state,
                    &(ta_allocator_free_params){ .size = sizeof(ta_tree_state),
                                                 .alignment = _Alignof(ta_tree_state) });
    }
    *tree = (ta_tree){0};
}

TA_API void *ta_tree_alloc(ta_tree *tree, size_t size, size_t alignment) {
    if (tree == NULL || tree->impl == NULL || tree->result != TA_OK) {
        return NULL;
    }
    if (!ta_tree_power_of_two(alignment)) {
        ta_tree_fail(tree, TA_ERR_INVALID);
        return NULL;
    }
    ta_tree_state *state = tree->impl;
    /* A zero-sized request still has to yield its own address. */
    void *block = ta_arena_alloc(&state->arena, size == 0 ? 1u : size, alignment);
    if (block == NULL) {
        ta_tree_fail(tree, TA_ERR_NOMEM);
        return NULL;
    }
    return block;
}

TA_API ta_result ta_tree_result(const ta_tree *tree) {
    return tree == NULL ? TA_ERR_INVALID : tree->result;
}

TA_API void ta_tree_list_init(ta_tree_list *list, ta_tree *tree, const ta_tree_list_layout *layout) {
    if (list == NULL) {
        return;
    }
    *list = (ta_tree_list){ .tree = tree };
    if (tree == NULL) {
        return;
    }
    if (layout == NULL || layout->element_size == 0 || !ta_tree_power_of_two(layout->element_alignment)) {
        ta_tree_fail(tree, TA_ERR_INVALID);
        return;
    }
    list->element_size = layout->element_size;
    list->element_alignment = layout->element_alignment;
    ta_tree_list_state *state =
        ta_tree_alloc(tree, sizeof(ta_tree_list_state), _Alignof(ta_tree_list_state));
    if (state == NULL) {
        return;
    }
    *state = (ta_tree_list_state){ .next_capacity = TA_TREE_LIST_FIRST_CHUNK };
    list->impl = state;
}

/* Appends one more staging chunk, twice the size of the last. */
static ta_tree_chunk *ta_tree_list_grow(ta_tree_list *list, ta_tree_list_state *state) {
    size_t capacity = state->next_capacity == 0 ? TA_TREE_LIST_FIRST_CHUNK : state->next_capacity;
    if (capacity > SIZE_MAX / list->element_size) {
        ta_tree_fail(list->tree, TA_ERR_NOMEM);
        return NULL;
    }
    ta_tree_chunk *chunk = ta_tree_alloc(list->tree, sizeof(ta_tree_chunk), _Alignof(ta_tree_chunk));
    if (chunk == NULL) {
        return NULL;
    }
    unsigned char *elements =
        ta_tree_alloc(list->tree, capacity * list->element_size, list->element_alignment);
    if (elements == NULL) {
        return NULL;
    }
    *chunk = (ta_tree_chunk){ .elements = elements, .capacity = capacity };
    if (state->last == NULL) {
        state->first = chunk;
    } else {
        state->last->next = chunk;
    }
    state->last = chunk;
    state->next_capacity = capacity > SIZE_MAX / 2u ? capacity : capacity * 2u;
    return chunk;
}

TA_API void ta_tree_put(ta_tree_list *list, const void *element) {
    if (list == NULL || list->impl == NULL || element == NULL) {
        return;
    }
    if (list->tree == NULL || list->tree->result != TA_OK) {
        return;
    }
    ta_tree_list_state *state = list->impl;
    ta_tree_chunk *chunk = state->last;
    if (chunk == NULL || chunk->count == chunk->capacity) {
        chunk = ta_tree_list_grow(list, state);
        if (chunk == NULL) {
            return;
        }
    }
    ta_mem_copy(chunk->elements + chunk->count * list->element_size, element, list->element_size);
    chunk->count += 1u;
    state->count += 1u;
}

TA_API const void *ta_tree_close(ta_tree_list *list) {
    if (list == NULL || list->impl == NULL) {
        return NULL;
    }
    ta_tree_list_state *state = list->impl;
    /* A closed list is spent: a put after it must not reach the chunks the
     * returned array was copied from. */
    list->impl = NULL;
    if (list->tree == NULL || list->tree->result != TA_OK) {
        return NULL;
    }
    /* Every child list is terminated by a zero-initialized element. */
    size_t total = state->count + 1u;
    if (total > SIZE_MAX / list->element_size) {
        ta_tree_fail(list->tree, TA_ERR_NOMEM);
        return NULL;
    }
    unsigned char *array =
        ta_tree_alloc(list->tree, total * list->element_size, list->element_alignment);
    if (array == NULL) {
        return NULL;
    }
    size_t at = 0;
    for (const ta_tree_chunk *chunk = state->first; chunk != NULL; chunk = chunk->next) {
        ta_mem_copy(array + at, chunk->elements, chunk->count * list->element_size);
        at += chunk->count * list->element_size;
    }
    ta_mem_zero(array + at, list->element_size);
    return array;
}


static ta_result ta_atom_rehash(ta_atom_table *table, uint32_t bucket_count) {
    uint32_t *buckets = ta_mem_alloc(table->allocator, bucket_count * sizeof(uint32_t),
                                     _Alignof(uint32_t));
    if (buckets == NULL) {
        return TA_ERR_NOMEM;
    }
    for (uint32_t index = 0; index < bucket_count; index += 1) {
        buckets[index] = TA_ATOM_NONE;
    }
    for (uint32_t atom = 0; atom < table->entries.count; atom += 1) {
        ta_atom_entry *entry = ta_vec_at(&table->entries, atom);
        uint32_t bucket = entry->hash & (bucket_count - 1u);
        entry->next = buckets[bucket];
        buckets[bucket] = atom;
    }
    ta_mem_free(table->allocator, table->buckets,
                &(ta_allocator_free_params){ .size = table->bucket_count * sizeof(uint32_t),
                                             .alignment = _Alignof(uint32_t) });
    table->buckets = buckets;
    table->bucket_count = bucket_count;
    return TA_OK;
}

TA_PRIVATE ta_result ta_atom_table_init(ta_atom_table *table, ta_allocator allocator) {
    *table = (ta_atom_table){ .allocator = allocator };
    ta_vec_init(&table->entries, allocator, TA_VEC_LAYOUT(ta_atom_entry));
    ta_result result = ta_arena_init(&table->strings, allocator, 4096);
    if (result == TA_OK) {
        result = ta_atom_rehash(table, 64);
    }
    /* Atom 0 is the empty text, which no family is, so a zeroed family
     * atom names none (ADR 0088). */
    ta_atom empty = TA_ATOM_NONE;
    if (result == TA_OK) {
        result = ta_atom_intern(table, (ta_str){ .ptr = "", .len = 0 }, &empty);
    }
    if (result != TA_OK) {
        ta_atom_table_destroy(table);
    }
    return result;
}

TA_PRIVATE void ta_atom_table_destroy(ta_atom_table *table) {
    ta_vec_destroy(&table->entries);
    ta_mem_free(table->allocator, table->buckets,
                &(ta_allocator_free_params){ .size = table->bucket_count * sizeof(uint32_t),
                                             .alignment = _Alignof(uint32_t) });
    ta_arena_destroy(&table->strings);
    table->buckets = NULL;
    table->bucket_count = 0;
}

TA_PRIVATE ta_result ta_atom_intern(ta_atom_table *table, ta_str text, ta_atom *out) {
    if (text.ptr == NULL && text.len != 0) {
        return TA_ERR_INVALID;
    }
    uint32_t hash = ta_fnv1a32(TA_FNV1A32_BASIS, text.ptr, text.len);
    for (uint32_t atom = table->buckets[hash & (table->bucket_count - 1u)]; atom != TA_ATOM_NONE;) {
        const ta_atom_entry *entry = ta_vec_at(&table->entries, atom);
        if (entry->hash == hash && ta_str_equal(entry->text, text)) {
            *out = atom;
            return TA_OK;
        }
        atom = entry->next;
    }
    if (table->entries.count >= TA_ATOM_NONE - 1u) {
        return TA_ERR_NOMEM;
    }
    ta_atom_entry entry = { .hash = hash };
    ta_result result = ta_arena_copy_str(&table->strings, text, &entry.text);
    if (result != TA_OK) {
        return result;
    }
    uint32_t atom = table->entries.count;
    uint32_t bucket = hash & (table->bucket_count - 1u);
    entry.next = table->buckets[bucket];
    result = ta_vec_push(&table->entries, &entry);
    if (result != TA_OK) {
        return result;
    }
    table->buckets[bucket] = atom;
    if (table->entries.count > table->bucket_count) {
        /* A failed rehash leaves the old, still-correct buckets. */
        (void)ta_atom_rehash(table, table->bucket_count * 2u);
    }
    *out = atom;
    return TA_OK;
}

TA_PRIVATE bool ta_atom_valid(const ta_atom_table *table, ta_atom atom) {
    return atom < table->entries.count;
}

TA_PRIVATE ta_str ta_atom_text(const ta_atom_table *table, ta_atom atom) {
    if (!ta_atom_valid(table, atom)) {
        return (ta_str){0};
    }
    const ta_atom_entry *entry = ta_vec_at(&table->entries, atom);
    return entry->text;
}

TA_PRIVATE ta_result ta_atom_find(const ta_atom_table *table, ta_str text, ta_atom *out) {
    if (text.ptr == NULL && text.len != 0) {
        return TA_ERR_INVALID;
    }
    uint32_t hash = ta_fnv1a32(TA_FNV1A32_BASIS, text.ptr, text.len);
    for (uint32_t atom = table->buckets[hash & (table->bucket_count - 1u)]; atom != TA_ATOM_NONE;) {
        const ta_atom_entry *entry = ta_vec_at(&table->entries, atom);
        if (entry->hash == hash && ta_str_equal(entry->text, text)) {
            *out = atom;
            return TA_OK;
        }
        atom = entry->next;
    }
    return TA_ERR_NOT_FOUND;
}


/* The hash chains family, a NUL separator and name through one running FNV-1a
 * state, so ("a", "bc") and ("ab", "c") differ. */
static uint32_t ta_address_hash(const ta_actor_address *address) {
    const char separator = 0;
    uint32_t hash = ta_fnv1a32(TA_FNV1A32_BASIS, address->family.name.ptr, address->family.name.len);
    hash = ta_fnv1a32(hash, &separator, 1);
    return ta_fnv1a32(hash, address->name.ptr, address->name.len);
}

static uint32_t ta_address_bucket_count(const ta_address_table *table) {
    return table->base + table->split;
}

static ta_address **ta_address_bucket(const ta_address_table *table, uint32_t index) {
    ta_address **page = *(ta_address ***)ta_vec_at(&table->pages, index / TA_ADDRESS_PAGE_BUCKETS);
    return &page[index % TA_ADDRESS_PAGE_BUCKETS];
}

static uint32_t ta_address_index(const ta_address_table *table, uint32_t hash) {
    uint32_t index = hash & (table->base - 1u);
    return index < table->split ? hash & (2u * table->base - 1u) : index;
}

static size_t ta_address_bytes(uint32_t family_len, uint32_t name_len) {
    return sizeof(ta_address) + (size_t)family_len + (size_t)name_len + 2u;
}

/* Makes sure the page holding bucket `index` exists; false when it cannot. */
static bool ta_address_page_for(ta_address_table *table, uint32_t index) {
    uint32_t page_index = index / TA_ADDRESS_PAGE_BUCKETS;
    if (page_index < table->pages.count) {
        return true;
    }
    ta_address **page = ta_mem_alloc(table->allocator, TA_ADDRESS_PAGE_BUCKETS * sizeof(ta_address *),
                                     _Alignof(ta_address *));
    if (page == NULL) {
        return false;
    }
    for (uint32_t bucket = 0; bucket < TA_ADDRESS_PAGE_BUCKETS; bucket += 1u) {
        page[bucket] = NULL;
    }
    if (ta_vec_push(&table->pages, &page) != TA_OK) {
        ta_mem_free(table->allocator, page,
                    &(ta_allocator_free_params){ .size = TA_ADDRESS_PAGE_BUCKETS * sizeof(ta_address *),
                                                 .alignment = _Alignof(ta_address *) });
        return false;
    }
    return true;
}

/* Splits one bucket when the table is fuller than one record per bucket. */
static void ta_address_grow(ta_address_table *table) {
    uint32_t added = ta_address_bucket_count(table);
    if (table->count <= added || added == UINT32_MAX || !ta_address_page_for(table, added)) {
        return; /* a failed page only postpones the split */
    }
    uint32_t wide = 2u * table->base - 1u;
    ta_address **from = ta_address_bucket(table, table->split);
    ta_address **to = ta_address_bucket(table, added);
    ta_address *chain = *from;
    *from = NULL;
    while (chain != NULL) {
        ta_address *next = chain->next;
        ta_address **home = (chain->hash & wide) == table->split ? from : to;
        chain->next = *home;
        *home = chain;
        chain = next;
    }
    table->split += 1u;
    if (table->split == table->base) {
        table->base *= 2u;
        table->split = 0;
    }
}

TA_PRIVATE void ta_address_table_init(ta_address_table *table, ta_allocator allocator) {
    *table = (ta_address_table){ .allocator = allocator, .base = TA_ADDRESS_INITIAL_BUCKETS };
    ta_vec_init(&table->pages, allocator, TA_VEC_LAYOUT(ta_address **));
}

static void ta_address_free(ta_address_table *table, ta_address *address) {
    ta_mem_free(table->allocator, address,
                &(ta_allocator_free_params){ .size = ta_address_bytes(address->family_len, address->name_len),
                                             .alignment = _Alignof(ta_address) });
}

TA_PRIVATE void ta_address_table_destroy(ta_address_table *table) {
    uint32_t buckets = table->pages.count == 0 ? 0u : ta_address_bucket_count(table);
    for (uint32_t index = 0; index < buckets; index += 1u) {
        ta_address *chain = *ta_address_bucket(table, index);
        while (chain != NULL) {
            ta_address *next = chain->next;
            ta_address_free(table, chain);
            chain = next;
        }
    }
    for (uint32_t page = 0; page < table->pages.count; page += 1u) {
        ta_mem_free(table->allocator, *(ta_address ***)ta_vec_at(&table->pages, page),
                    &(ta_allocator_free_params){ .size = TA_ADDRESS_PAGE_BUCKETS * sizeof(ta_address *),
                                                 .alignment = _Alignof(ta_address *) });
    }
    ta_vec_destroy(&table->pages);
    *table = (ta_address_table){0};
}

static bool ta_address_matches(const ta_address *record, const ta_actor_address *address, uint32_t hash) {
    ta_str family = { .ptr = record->text, .len = record->family_len };
    ta_str name = { .ptr = record->text + record->family_len + 1u, .len = record->name_len };
    return record->hash == hash && ta_str_equal(family, address->family.name) && ta_str_equal(name, address->name);
}

static ta_address *ta_address_record(ta_allocator allocator, const ta_actor_address *address, uint32_t hash) {
    uint32_t family_len = address->family.name.len;
    uint32_t name_len = address->name.len;
    ta_address *record = ta_mem_alloc(allocator, ta_address_bytes(family_len, name_len), _Alignof(ta_address));
    if (record == NULL) {
        return NULL;
    }
    record->table = NULL;
    record->next = NULL;
    record->unloaded = NULL;
    record->references = 0;
    record->hash = hash;
    record->family_len = family_len;
    record->name_len = name_len;
    if (family_len != 0) {
        ta_mem_copy(record->text, address->family.name.ptr, family_len);
    }
    record->text[family_len] = 0;
    if (name_len != 0) {
        ta_mem_copy(record->text + family_len + 1u, address->name.ptr, name_len);
    }
    record->text[family_len + 1u + name_len] = 0;
    return record;
}

TA_PRIVATE ta_result ta_address_table_intern(ta_address_table *table, const ta_actor_address *address,
                                  ta_address **out) {
    uint32_t hash = ta_address_hash(address);
    if (table->pages.count != 0) {
        for (ta_address *record = *ta_address_bucket(table, ta_address_index(table, hash)); record != NULL;
             record = record->next) {
            if (ta_address_matches(record, address, hash)) {
                record->references += 1u;
                *out = record;
                return TA_OK;
            }
        }
    } else if (!ta_address_page_for(table, 0)) {
        return TA_ERR_NOMEM;
    }
    ta_address *record = ta_address_record(table->allocator, address, hash);
    if (record == NULL) {
        return TA_ERR_NOMEM;
    }
    record->table = table;
    record->references = 1;
    ta_address **bucket = ta_address_bucket(table, ta_address_index(table, hash));
    record->next = *bucket;
    *bucket = record;
    table->count += 1u;
    ta_address_grow(table);
    *out = record;
    return TA_OK;
}

TA_PRIVATE ta_result ta_address_detach(ta_arena *arena, const ta_address *address, ta_address **out) {
    size_t bytes = ta_address_bytes(address->family_len, address->name_len);
    ta_address *copy = ta_arena_alloc(arena, bytes, _Alignof(ta_address));
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, address, bytes);
    copy->table = NULL;
    copy->next = NULL;
    copy->unloaded = NULL;
    copy->references = 0;
    *out = copy;
    return TA_OK;
}

TA_PRIVATE bool ta_address_in(const ta_address *address, const ta_address_table *table) {
    return address != NULL && address->table == table;
}

/* ------------------------------------------------------------------------
 * Public
 */

TA_API ta_result ta_address_intern(ta_system *system, const ta_actor_address *address, ta_address **out) {
    if (system == NULL || address == NULL || out == NULL ||
        (address->family.name.ptr == NULL && address->family.name.len != 0) ||
        (address->name.ptr == NULL && address->name.len != 0)) {
        return TA_ERR_INVALID;
    }
    return ta_address_table_intern(&system->addresses, address, out);
}

TA_API void ta_address_retain(ta_address *address) {
    if (address != NULL && address->table != NULL) {
        address->references += 1u;
    }
}

TA_API void ta_address_release(ta_address *address) {
    if (address == NULL || address->table == NULL) {
        return;
    }
    ta_address *record = address;
    record->references -= 1u;
    if (record->references != 0) {
        return;
    }
    ta_address_table *table = record->table;
    for (ta_address **link = ta_address_bucket(table, ta_address_index(table, record->hash)); *link != NULL;
         link = &(*link)->next) {
        if (*link == record) {
            *link = record->next;
            break;
        }
    }
    table->count -= 1u;
    ta_address_free(table, record);
}

TA_API ta_actor_address ta_address_view(const ta_address *address) {
    if (address == NULL) {
        return (ta_actor_address){0};
    }
    return (ta_actor_address){
        .family = { .name = { .ptr = address->text, .len = address->family_len } },
        .name = { .ptr = address->text + address->family_len + 1u, .len = address->name_len },
    };
}


#define TA_IO_ORIGIN_FREE UINT32_MAX

static ta_allocator_free_params ta_io_origin_page_layout(void) {
    return (ta_allocator_free_params){ .size = (size_t)TA_IO_ORIGIN_PAGE * sizeof(ta_io_origin_entry),
                                       .alignment = _Alignof(ta_io_origin_entry) };
}

TA_PRIVATE void ta_io_origin_table_init(ta_io_origin_table *table, ta_allocator allocator, bool scoped) {
    *table = (ta_io_origin_table){ .allocator = allocator, .scoped = scoped };
    ta_vec_init(&table->pages, allocator, TA_VEC_LAYOUT(ta_io_origin_entry *));
}

static void ta_io_origin_free_spill(ta_io_origin_entry *entry) {
    if (entry->length > TA_IO_ORIGIN_INLINE && entry->spill != NULL) {
        ta_mem_free(entry->table->allocator, entry->spill,
                    &(ta_allocator_free_params){ .size = entry->length, .alignment = 1 });
    }
    entry->spill = NULL;
}

/*
 * Frees every page and every spill still held. Live records at this point
 * belong to events a host has not released yet; like addresses (ADR 0065),
 * a host releases every event before destroying the system.
 */
TA_PRIVATE void ta_io_origin_table_destroy(ta_io_origin_table *table) {
    for (uint32_t page = 0; page < table->pages.count; page += 1) {
        ta_io_origin_entry *records = *(ta_io_origin_entry **)ta_vec_at(&table->pages, page);
        uint32_t used = table->issued - page * TA_IO_ORIGIN_PAGE;
        used = used > TA_IO_ORIGIN_PAGE ? TA_IO_ORIGIN_PAGE : used;
        for (uint32_t index = 0; index < used; index += 1) {
            if (records[index].length != TA_IO_ORIGIN_FREE) {
                ta_io_origin_free_spill(&records[index]);
            }
        }
        ta_allocator_free_params layout = ta_io_origin_page_layout();
        ta_mem_free(table->allocator, records, &layout);
    }
    ta_vec_destroy(&table->pages);
    *table = (ta_io_origin_table){0};
}

TA_PRIVATE ta_io_origin_entry *ta_io_origin_at(const ta_io_origin_table *table, uint32_t index) {
    if (index >= table->issued) {
        return NULL;
    }
    ta_io_origin_entry *records = *(ta_io_origin_entry **)ta_vec_at(&table->pages, index / TA_IO_ORIGIN_PAGE);
    ta_io_origin_entry *entry = &records[index % TA_IO_ORIGIN_PAGE];
    return entry->length == TA_IO_ORIGIN_FREE ? NULL : entry;
}

/* A record off the free list, or the next unissued one, with its index. */
static ta_io_origin_entry *ta_io_origin_slot(ta_io_origin_table *table, uint32_t *index) {
    if (table->free != NULL) {
        ta_io_origin_entry *entry = table->free;
        table->free = (ta_io_origin_entry *)(void *)entry->spill;
        entry->spill = NULL;
        *index = entry->refs;
        return entry;
    }
    if (table->issued % TA_IO_ORIGIN_PAGE == 0) {
        ta_allocator_free_params layout = ta_io_origin_page_layout();
        ta_io_origin_entry *page = ta_mem_alloc(table->allocator, layout.size, layout.alignment);
        if (page == NULL) {
            return NULL;
        }
        if (ta_vec_push(&table->pages, &page) != TA_OK) {
            ta_mem_free(table->allocator, page, &layout);
            return NULL;
        }
    }
    *index = table->issued;
    ta_io_origin_entry *records = *(ta_io_origin_entry **)ta_vec_at(&table->pages, *index / TA_IO_ORIGIN_PAGE);
    table->issued += 1;
    ta_io_origin_entry *entry = &records[*index % TA_IO_ORIGIN_PAGE];
    *entry = (ta_io_origin_entry){ .generation = 1, .table = table, .length = TA_IO_ORIGIN_FREE };
    return entry;
}

/* Puts a record on the free list; a free record keeps its index in `refs`. */
static void ta_io_origin_park(ta_io_origin_table *table, ta_io_origin_entry *entry, uint32_t index) {
    entry->refs = index;
    entry->length = TA_IO_ORIGIN_FREE;
    entry->spill = (char *)(void *)table->free;
    table->free = entry;
}

TA_PRIVATE ta_io_origin_entry *ta_io_origin_take(ta_io_origin_table *table, const ta_io_origin_take_params *params) {
    uint32_t index = 0;
    ta_io_origin_entry *entry = ta_io_origin_slot(table, &index);
    if (entry == NULL) {
        return NULL;
    }
    ta_str uri = params->uri;
    char *spill = NULL;
    if (uri.len > TA_IO_ORIGIN_INLINE) {
        spill = ta_mem_alloc(table->allocator, uri.len, 1);
        if (spill == NULL) {
            ta_io_origin_park(table, entry, index);
            return NULL;
        }
        ta_mem_copy(spill, uri.ptr, uri.len);
    } else if (uri.len != 0) {
        ta_mem_copy(entry->text, uri.ptr, uri.len);
    }
    entry->refs = table->scoped ? index : 1u;
    entry->table = table;
    entry->processor = params->processor;
    entry->length = uri.len;
    entry->spill = spill;
    table->live += 1;
    if (params->index != NULL) {
        *params->index = index;
    }
    return entry;
}

TA_PRIVATE void ta_io_origin_give(ta_io_origin_entry *entry) {
    if (entry == NULL || entry->table == NULL) {
        return;
    }
    ta_io_origin_table *table = entry->table;
    ta_io_origin_free_spill(entry);
    entry->generation += 1;
    table->live -= 1;
    /* Only scopes are found by index; an entry's index is never read. */
    ta_io_origin_park(table, entry, table->scoped ? entry->refs : 0u);
}

TA_PRIVATE void ta_io_origin_entry_retain(ta_io_origin_entry *entry) {
    if (entry != NULL && entry->table != NULL) {
        entry->refs += 1;
    }
}

TA_PRIVATE void ta_io_origin_entry_release(ta_io_origin_entry *entry) {
    if (entry == NULL || entry->table == NULL) {
        return;
    }
    entry->refs -= 1;
    if (entry->refs == 0) {
        ta_io_origin_give(entry);
    }
}

TA_PRIVATE ta_str ta_io_origin_entry_uri(const ta_io_origin_entry *entry) {
    if (entry == NULL || entry->length == TA_IO_ORIGIN_FREE) {
        return (ta_str){0};
    }
    if (entry->table == NULL || entry->length > TA_IO_ORIGIN_INLINE) {
        return (ta_str){ .ptr = entry->spill, .len = entry->length };
    }
    return (ta_str){ .ptr = entry->text, .len = entry->length };
}

TA_PRIVATE ta_result ta_io_origin_detach(ta_arena *arena, const ta_io_origin_detach_params *params) {
    ta_io_origin_entry *entry = ta_arena_alloc(arena, sizeof(ta_io_origin_entry), _Alignof(ta_io_origin_entry));
    if (entry == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_str type;
    ta_str uri;
    ta_result result = ta_arena_copy_str(arena, params->type, &type);
    if (result == TA_OK) {
        result = ta_arena_copy_str(arena, params->uri, &uri);
    }
    if (result != TA_OK) {
        return result;
    }
    *entry = (ta_io_origin_entry){ .spill = (char *)uri.ptr, .length = uri.len };
    ta_mem_copy(entry->text, &type, sizeof(type));
    *params->out = entry;
    return TA_OK;
}

TA_PRIVATE bool ta_io_origin_entry_detached(const ta_io_origin_entry *entry) {
    return entry != NULL && entry->table == NULL;
}

TA_PRIVATE ta_str ta_io_origin_detached_type(const ta_io_origin_entry *entry) {
    ta_str type;
    ta_mem_copy(&type, entry->text, sizeof(type));
    return type;
}


/* Runtime IDs differ mostly in their low slot bits, so the bucket hash is the
 * 64-bit finalizer (D5) cut to 32 bits. */
static uint32_t ta_session_hash(uint64_t id) {
    return (uint32_t)ta_mix64(id);
}

static uint32_t ta_session_bucket_count(const ta_session_table *table) {
    return table->base + table->split;
}

static ta_session_node **ta_session_bucket(const ta_session_table *table, uint32_t index) {
    ta_session_node **page = *(ta_session_node ***)ta_vec_at(&table->pages, index / TA_SESSION_PAGE_BUCKETS);
    return &page[index % TA_SESSION_PAGE_BUCKETS];
}

static uint32_t ta_session_index(const ta_session_table *table, uint32_t hash) {
    uint32_t index = hash & (table->base - 1u);
    return index < table->split ? hash & (2u * table->base - 1u) : index;
}

static void ta_session_page_free(ta_allocator allocator, ta_session_node **page) {
    ta_mem_free(allocator, page,
                &(ta_allocator_free_params){ .size = TA_SESSION_PAGE_BUCKETS * sizeof(ta_session_node *),
                                             .alignment = _Alignof(ta_session_node *) });
}

/* Makes sure the page holding bucket `index` exists; false when it cannot. */
static bool ta_session_page_for(ta_session_table *table, uint32_t index) {
    if (index / TA_SESSION_PAGE_BUCKETS < table->pages.count) {
        return true;
    }
    ta_session_node **page = ta_mem_alloc(table->allocator, TA_SESSION_PAGE_BUCKETS * sizeof(ta_session_node *),
                                          _Alignof(ta_session_node *));
    if (page == NULL) {
        return false;
    }
    for (uint32_t bucket = 0; bucket < TA_SESSION_PAGE_BUCKETS; bucket += 1u) {
        page[bucket] = NULL;
    }
    if (ta_vec_push(&table->pages, &page) != TA_OK) {
        ta_session_page_free(table->allocator, page);
        return false;
    }
    return true;
}

/* Splits one bucket when the table holds more than one entry per bucket. */
static void ta_session_grow(ta_session_table *table) {
    uint32_t added = ta_session_bucket_count(table);
    if (table->count <= added || added == UINT32_MAX || !ta_session_page_for(table, added)) {
        return; /* a failed page only postpones the split */
    }
    uint32_t wide = 2u * table->base - 1u;
    ta_session_node **from = ta_session_bucket(table, table->split);
    ta_session_node **to = ta_session_bucket(table, added);
    ta_session_node *chain = *from;
    *from = NULL;
    while (chain != NULL) {
        ta_session_node *next = chain->next;
        ta_session_node **home = (ta_session_hash(chain->id) & wide) == table->split ? from : to;
        chain->next = *home;
        *home = chain;
        chain = next;
    }
    table->split += 1u;
    if (table->split == table->base) {
        table->base *= 2u;
        table->split = 0;
    }
}

TA_PRIVATE ta_result ta_session_table_init(ta_session_table *table, ta_allocator allocator) {
    *table = (ta_session_table){ .allocator = allocator, .base = TA_SESSION_INITIAL_BUCKETS };
    ta_vec_init(&table->pages, allocator, TA_VEC_LAYOUT(ta_session_node **));
    return ta_session_page_for(table, 0) ? TA_OK : TA_ERR_NOMEM;
}

TA_PRIVATE void ta_session_table_destroy(ta_session_table *table) {
    uint32_t buckets = table->pages.count == 0 ? 0u : ta_session_bucket_count(table);
    for (uint32_t index = 0; index < buckets; index += 1u) {
        ta_session_node *chain = *ta_session_bucket(table, index);
        while (chain != NULL) {
            ta_session_node *next = chain->next;
            ta_session_node_free(table, chain);
            chain = next;
        }
    }
    for (uint32_t page = 0; page < table->pages.count; page += 1u) {
        ta_session_page_free(table->allocator, *(ta_session_node ***)ta_vec_at(&table->pages, page));
    }
    ta_vec_destroy(&table->pages);
    *table = (ta_session_table){0};
}

TA_PRIVATE ta_result ta_session_node_new(ta_session_table *table, ta_session_node **out) {
    ta_session_node *node = ta_mem_alloc(table->allocator, sizeof(ta_session_node), _Alignof(ta_session_node));
    if (node == NULL) {
        return TA_ERR_NOMEM;
    }
    *node = (ta_session_node){0};
    *out = node;
    return TA_OK;
}

TA_PRIVATE void ta_session_node_free(ta_session_table *table, ta_session_node *node) {
    if (node == NULL) {
        return;
    }
    if (node->address != NULL) {
        if (node->address->unloaded == node) {
            node->address->unloaded = NULL;
        }
        ta_address_release(node->address);
    }
    ta_mem_free(table->allocator, node,
                &(ta_allocator_free_params){ .size = sizeof(ta_session_node), .alignment = _Alignof(ta_session_node) });
}

TA_PRIVATE void ta_session_insert(ta_session_table *table, ta_session_node *node) {
    ta_session_node **bucket = ta_session_bucket(table, ta_session_index(table, ta_session_hash(node->id)));
    node->next = *bucket;
    *bucket = node;
    table->count += 1u;
    ta_session_grow(table);
}

TA_PRIVATE void ta_session_remove(ta_session_table *table, ta_session_node *node) {
    for (ta_session_node **link = ta_session_bucket(table, ta_session_index(table, ta_session_hash(node->id)));
         *link != NULL; link = &(*link)->next) {
        if (*link == node) {
            *link = node->next;
            node->next = NULL;
            table->count -= 1u;
            return;
        }
    }
}

TA_PRIVATE const ta_session_node *ta_session_find(const ta_session_table *table, uint64_t id) {
    if (table->pages.count == 0) {
        return NULL;
    }
    const ta_session_node *unloaded = NULL;
    for (const ta_session_node *node = *ta_session_bucket(table, ta_session_index(table, ta_session_hash(id)));
         node != NULL; node = node->next) {
        if (node->id != id) {
            continue;
        }
        if (node->actor != NULL) {
            return node;
        }
        unloaded = node;
    }
    return unloaded;
}

TA_PRIVATE bool ta_session_resident(const ta_system *system, uint64_t id) {
    const ta_actor *actor = ta_actor_lookup(system, id);
    if (actor != NULL && actor->session_id == id) {
        return true;
    }
    const ta_session_node *node = ta_session_find(&system->sessions, id);
    return node != NULL && node->actor != NULL;
}

TA_PRIVATE uint64_t ta_session_id_of(const ta_system *system, ta_actor_id runtime) {
    const ta_actor *actor = ta_actor_lookup(system, runtime);
    return actor != NULL ? actor->session_id : runtime;
}

/* Whether `actor` is of the family `ref` names; a reference naming none matches any (ADR 0088). */
static bool ta_session_family_matches(const ta_actor_ref *ref, const ta_actor *actor) {
    return ref->family == 0 || actor->definition->family == ref->family;
}

TA_PRIVATE bool ta_session_route(const ta_system *system, const ta_actor_ref *ref, ta_actor_ref *out) {
    *out = *ref;
    if (ref->kind != TA_ACTOR_REF_RUNTIME) {
        return false;
    }
    const ta_actor *resident = ta_actor_lookup(system, ref->as.runtime);
    if (resident != NULL) {
        /* Another family's actor holds the ID: the session named is gone. */
        if (!ta_session_family_matches(ref, resident)) {
            out->as.runtime = 0;
        }
        return false;
    }
    const ta_session_node *node = ta_session_find(&system->sessions, ref->as.runtime);
    if (node == NULL) {
        return false;
    }
    if (node->actor != NULL) {
        *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME,
                               .family = ref->family,
                               .as.runtime = ta_session_family_matches(ref, node->actor) ? node->actor->record.id : 0 };
        return false;
    }
    ta_address_retain(node->address);
    *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_NAMED, .as.named = node->address };
    return true;
}


static const ta_event_name_node *ta_event_name_node_at(const ta_event_name_table *table,
                                                       ta_event_name_id name) {
    return ta_vec_at(&table->nodes, name);
}

static uint32_t ta_event_name_hash(ta_event_name_id parent, ta_atom segment) {
    uint32_t key[2] = { parent, segment };
    /* Offsetting the basis keeps these hashes apart from the atom table's. */
    return ta_fnv1a32(TA_FNV1A32_BASIS ^ 0x9e3779b9u, key, sizeof(key));
}

static ta_result ta_event_name_rehash(ta_event_name_table *table, uint32_t bucket_count) {
    uint32_t *buckets = ta_mem_alloc(table->allocator, bucket_count * sizeof(uint32_t),
                                     _Alignof(uint32_t));
    if (buckets == NULL) {
        return TA_ERR_NOMEM;
    }
    for (uint32_t index = 0; index < bucket_count; index += 1) {
        buckets[index] = UINT32_MAX;
    }
    /* Node 0 is the root and is never looked up as a child. */
    for (uint32_t name = 1; name < table->nodes.count; name += 1) {
        ta_event_name_node *node = ta_vec_at(&table->nodes, name);
        uint32_t bucket = ta_event_name_hash(node->parent, node->segment) & (bucket_count - 1u);
        node->hash_next = buckets[bucket];
        buckets[bucket] = name;
    }
    ta_mem_free(table->allocator, table->buckets,
                &(ta_allocator_free_params){ .size = table->bucket_count * sizeof(uint32_t),
                                             .alignment = _Alignof(uint32_t) });
    table->buckets = buckets;
    table->bucket_count = bucket_count;
    return TA_OK;
}

TA_PRIVATE ta_result ta_event_name_child(ta_event_name_table *table,
                              ta_event_name_id parent,
                              ta_atom segment,
                              ta_event_name_id *out) {
    if (!ta_event_name_valid(table, parent) || !ta_atom_valid(table->atoms, segment)) {
        return TA_ERR_INVALID;
    }
    uint32_t depth = ta_event_name_node_at(table, parent)->depth + 1u;
    if (depth > table->max_depth) {
        return TA_ERR_INVALID;
    }
    uint32_t hash = ta_event_name_hash(parent, segment);
    for (uint32_t name = table->buckets[hash & (table->bucket_count - 1u)]; name != UINT32_MAX;) {
        const ta_event_name_node *node = ta_event_name_node_at(table, name);
        if (node->parent == parent && node->segment == segment) {
            *out = name;
            return TA_OK;
        }
        name = node->hash_next;
    }
    uint32_t name = table->nodes.count;
    ta_event_name_node node = {
        .parent = parent,
        .segment = segment,
        .depth = (uint16_t)depth,
        .hash_next = table->buckets[hash & (table->bucket_count - 1u)],
    };
    ta_result result = ta_vec_push(&table->nodes, &node);
    if (result != TA_OK) {
        return result;
    }
    table->buckets[hash & (table->bucket_count - 1u)] = name;
    if (table->nodes.count > table->bucket_count) {
        (void)ta_event_name_rehash(table, table->bucket_count * 2u);
    }
    *out = name;
    return TA_OK;
}

static ta_result ta_event_name_builtin(ta_event_name_table *table, const char *parent_and_segment) {
    /* "P:segment" where P is the parent's built-in index digit. */
    ta_event_name_id parent = (ta_event_name_id)(parent_and_segment[0] - '0');
    ta_str text = { .ptr = parent_and_segment + 2, .len = 0 };
    while (text.ptr[text.len] != '\0') {
        text.len += 1;
    }
    ta_atom atom = 0;
    ta_event_name_id name = 0;
    ta_result result = ta_atom_intern(table->atoms, text, &atom);
    return result == TA_OK ? ta_event_name_child(table, parent, atom, &name) : result;
}

TA_PRIVATE ta_result ta_event_name_table_init(ta_event_name_table *table,
                                   ta_allocator allocator,
                                   ta_atom_table *atoms) {
    *table = (ta_event_name_table){ .allocator = allocator, .atoms = atoms, .max_depth = TA_EVENT_NAME_MAX_DEPTH };
    ta_vec_init(&table->nodes, allocator, TA_VEC_LAYOUT(ta_event_name_node));
    ta_event_name_node root = { .parent = TA_EVENT_NAME_ROOT, .segment = TA_ATOM_NONE };
    ta_result result = ta_vec_push(&table->nodes, &root);
    if (result == TA_OK) {
        result = ta_event_name_rehash(table, 64);
    }
    /* Order matches the TA_EVENT_NAME_* constants. */
    static const char *const builtins[] = {
        "0:error", "1:execution", "1:communication", "1:platform",
        "0:done", "5:state", "5:invoke",
    };
    for (uint32_t index = 0; result == TA_OK && index < TA_ARRAY_COUNT(builtins); index += 1) {
        result = ta_event_name_builtin(table, builtins[index]);
    }
    if (result != TA_OK) {
        ta_event_name_table_destroy(table);
    }
    return result;
}

TA_PRIVATE void ta_event_name_table_destroy(ta_event_name_table *table) {
    ta_vec_destroy(&table->nodes);
    ta_mem_free(table->allocator, table->buckets,
                &(ta_allocator_free_params){ .size = table->bucket_count * sizeof(uint32_t),
                                             .alignment = _Alignof(uint32_t) });
    table->buckets = NULL;
    table->bucket_count = 0;
}

TA_PRIVATE bool ta_event_name_valid(const ta_event_name_table *table, ta_event_name_id name) {
    return name < table->nodes.count;
}

TA_PRIVATE ta_result ta_event_name_intern_atoms_internal(ta_event_name_table *table,
                                              const ta_atom_list *segments,
                                              ta_event_name_id *out) {
    if (segments == NULL || segments->count == 0 || segments->atoms == NULL ||
        segments->count > table->max_depth) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < segments->count; index += 1) {
        if (!ta_atom_valid(table->atoms, segments->atoms[index])) {
            return TA_ERR_INVALID;
        }
    }
    ta_event_name_id name = TA_EVENT_NAME_ROOT;
    for (uint32_t index = 0; index < segments->count; index += 1) {
        ta_result result = ta_event_name_child(table, name, segments->atoms[index], &name);
        if (result != TA_OK) {
            return result;
        }
    }
    *out = name;
    return TA_OK;
}

TA_PRIVATE ta_result ta_event_name_intern_dotted(ta_event_name_table *table, ta_str dotted, ta_event_name_id *out) {
    if (dotted.ptr == NULL || dotted.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_event_name_id name = TA_EVENT_NAME_ROOT;
    uint32_t start = 0;
    for (uint32_t index = 0; index <= dotted.len; index += 1) {
        if (index < dotted.len && dotted.ptr[index] != '.') {
            continue;
        }
        if (index == start) {
            return TA_ERR_INVALID;
        }
        ta_atom atom = 0;
        ta_result result = ta_atom_intern(table->atoms,
                                          (ta_str){ .ptr = dotted.ptr + start, .len = index - start },
                                          &atom);
        if (result == TA_OK) {
            result = ta_event_name_child(table, name, atom, &name);
        }
        if (result != TA_OK) {
            return result;
        }
        start = index + 1u;
    }
    *out = name;
    return TA_OK;
}

TA_PRIVATE bool ta_event_name_matches_internal(const ta_event_name_table *table,
                                    ta_event_name_id event,
                                    ta_event_name_id prefix) {
    if (!ta_event_name_valid(table, event) || !ta_event_name_valid(table, prefix)) {
        return false;
    }
    uint16_t depth = ta_event_name_node_at(table, prefix)->depth;
    while (ta_event_name_node_at(table, event)->depth > depth) {
        event = ta_event_name_node_at(table, event)->parent;
    }
    return event == prefix;
}

TA_PRIVATE uint32_t ta_event_name_segments(const ta_event_name_table *table,
                                ta_event_name_id name,
                                ta_atom out[TA_EVENT_NAME_MAX_DEPTH]) {
    if (!ta_event_name_valid(table, name)) {
        return 0;
    }
    uint32_t depth = ta_event_name_node_at(table, name)->depth;
    for (uint32_t index = depth; index > 0; index -= 1) {
        const ta_event_name_node *node = ta_event_name_node_at(table, name);
        out[index - 1u] = node->segment;
        name = node->parent;
    }
    return depth;
}


TA_API void ta_event_retain(const ta_event *event, ta_event *out) {
    *out = *event;
    ta_value_retain(&out->data);
    if (out->origin.kind == TA_ORIGIN_ACTOR) {
        ta_actor_ref_retain(&out->origin.as.actor);
    } else if (out->origin.kind == TA_ORIGIN_IO && out->origin.as.io.origin_class == TA_IO_ORIGIN_MESSAGE) {
        ta_io_origin_entry_retain(out->origin.as.io.as.entry);
    }
}

TA_API void ta_event_release(ta_event *event) {
    if (event == NULL) {
        return;
    }
    ta_value_release(&event->data);
    event->data = (ta_value){0};
    ta_origin_release(&event->origin);
}

TA_API void ta_origin_release(ta_origin *origin) {
    if (origin == NULL) {
        return;
    }
    if (origin->kind == TA_ORIGIN_ACTOR) {
        ta_actor_ref_release(&origin->as.actor);
    } else if (origin->kind == TA_ORIGIN_IO && origin->as.io.origin_class == TA_IO_ORIGIN_MESSAGE) {
        ta_io_origin_entry_release(origin->as.io.as.entry);
        origin->as.io.as.entry = NULL;
    }
}


/* ------------------------------------------------------------------------
 * The null sink (ADR 0067 section 2)
 */

static void ta_null_trace_write(void *self, const ta_trace_record *record) {
    (void)self;
    (void)record;
}

static const ta_trace_sink_vtable ta_null_trace_vtable = { .write = ta_null_trace_write };

TA_API ta_trace_sink ta_null_trace_sink(void) {
    return (ta_trace_sink){ .self = NULL, .v = &ta_null_trace_vtable };
}

TA_API ta_result ta_system_set_trace_sink(ta_system *system, ta_trace_sink sink) {
    if (system == NULL || sink.v == NULL || sink.v->write == NULL) {
        return TA_ERR_INVALID;
    }
    system->trace = sink;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Step keys as span IDs (section 6)
 */

TA_API uint64_t ta_step_key_span_id(ta_step_key key) {
    if (key.session == 0) {
        return key.macrostep;
    }
    uint64_t span = ta_mix64(key.session);
    span = ta_mix64(span ^ key.macrostep);
    span = ta_mix64(span ^ ((uint64_t)key.microstep | ((uint64_t)1u << 32)));
    return span == 0 ? 1u : span;
}

/* ------------------------------------------------------------------------
 * What is built
 */

TA_PRIVATE uint32_t ta_trace_macrostep_kinds(const ta_trace_sink *sink, const ta_trace_context *context) {
    if (!ta_trace_context_empty(context) && (context->flags & TA_TRACE_FLAG_SAMPLED) == 0) {
        return sink->unsampled_kinds;
    }
    return (context->detail & TA_TRACE_DETAIL_WATCHED) != 0 ? sink->watched_kinds : sink->kinds;
}

TA_PRIVATE ta_step_key ta_trace_lifecycle_key(ta_actor *actor) {
    actor->trace_lifecycle += 1u;
    return (ta_step_key){ .session = actor->session_id, .microstep = actor->trace_lifecycle,
                          .family = actor->definition->family };
}

TA_PRIVATE void ta_trace_lifecycle(ta_system *system, ta_actor *actor, ta_trace_kind kind,
                        const ta_trace_parent *parent) {
    ta_step_key key = ta_trace_lifecycle_key(actor);
    /* Counters and occupancy (ADR 0072): every residency change passes here. */
    switch (kind) {
    case TA_TRACE_SPAWNED: TA_COUNT(system, actor->definition, spawned); break;
    case TA_TRACE_LOADED:
        TA_COUNT(system, actor->definition, loaded);
        ta_interpreter_occupancy(actor, true);
        break;
    case TA_TRACE_UNLOADED:
        TA_COUNT(system, actor->definition, unloaded);
        ta_interpreter_occupancy(actor, false);
        break;
    case TA_TRACE_DESTROYED:
        TA_COUNT(system, actor->definition, destroyed);
        ta_interpreter_occupancy(actor, false);
        break;
    default: break;
    }
    ta_trace_context none = { 0 };
    if (!ta_trace_lifecycle_wanted(&system->trace, kind, parent != NULL ? &parent->context : &none)) {
        return;
    }
    ta_trace_record record = ta_trace_record_blank(kind);
    record.key = key;
    record.actor = actor->record.id;
    record.definition = actor->definition->id;
    record.now = ta_actor_now(system, actor);
    if (parent != NULL) {
        record.context = parent->context;
        record.cause = parent->cause;
    }
    ta_trace_write(&system->trace, &record);
}

TA_PRIVATE ta_trace_record ta_trace_record_blank(ta_trace_kind kind) {
    return (ta_trace_record){
        .kind = kind,
        .position = { .at = TA_TRACE_AT_NONE, .state = TA_STATE_NONE, .transition = TA_TRANSITION_NONE,
                      .action = TA_ACTION_NONE },
        .state = TA_STATE_NONE,
        .expr = TA_EXPR_ID_NONE,
        .action = TA_ACTION_NONE,
        .invoke = TA_INVOKE_ID_NONE,
    };
}

/* ------------------------------------------------------------------------
 * traceparent text (ADR 0073 section 1)
 */

#define TA_TRACEPARENT_LEN 55u

/*
 * A lowercase hex digit's value plus one, and 0 for anything else, so the
 * table's default (zero) is "not a digit". A traceparent has 52 digits,
 * read on every post that carries one: a lookup and an AND per digit, and
 * one test per field, keep that cheap (ADR 0073, napi ADR 0017 section 2).
 */
static const uint8_t ta_hex_value[256] = {
    ['0'] = 1, ['1'] = 2, ['2'] = 3, ['3'] = 4, ['4'] = 5, ['5'] = 6, ['6'] = 7, ['7'] = 8, ['8'] = 9, ['9'] = 10,
    ['a'] = 11, ['b'] = 12, ['c'] = 13, ['d'] = 14, ['e'] = 15, ['f'] = 16,
};

/* Reads `count` lowercase hex digits (at most 16) into `*out`, one at a time: the 2-digit fields. */
static bool ta_hex_read(const char *text, uint32_t count, uint64_t *out) {
    uint64_t value = 0;
    uint32_t valid = 1;
    for (uint32_t index = 0; index < count; index += 1) {
        uint32_t digit = ta_hex_value[(uint8_t)text[index]];
        valid &= digit != 0;
        value = (value << 4) | (uint64_t)((digit - 1u) & 0xfu);
    }
    *out = value;
    return valid != 0;
}

/*
 * Eight lowercase hex digits at once (SWAR): loaded with shifts, so the
 * result does not depend on byte order, validated with byte-wise range
 * tests, and packed into 32 bits. On 16-digit fields this is several times
 * faster than a digit at a time.
 */
static bool ta_hex_read8(const char *text, uint32_t *out) {
    const unsigned char *bytes = (const unsigned char *)text;
    uint64_t x = 0;
    for (uint32_t index = 0; index < 8u; index += 1u) {
        x |= (uint64_t)bytes[index] << (8u * index);
    }
    const uint64_t ones = 0x0101010101010101u;
    const uint64_t highs = 0x8080808080808080u;
    if ((x & highs) != 0) {
        return false; /* not ASCII; also keeps the additions below inside each byte */
    }
    /* Bit 7 of each byte: b >= lo is (b + 0x80 - lo), b > hi is (b + 0x7f - hi). */
    uint64_t digit = (x + ones * 0x50u) & ~(x + ones * 0x46u);  /* '0'..'9' */
    uint64_t alpha = (x + ones * 0x1fu) & ~(x + ones * 0x19u);  /* 'a'..'f' */
    if (((digit | alpha) & highs) != highs) {
        return false;
    }
    /* A digit's value is its low nibble, plus 9 for a letter (bit 6 set). */
    uint64_t v = (x & (ones * 0x0fu)) + ((x >> 6) & ones) * 9u;
    /* The first character is in the lowest byte and is the most significant nibble. */
    v = ((v << 4) | (v >> 8)) & 0x00ff00ff00ff00ffu;
    v = ((v << 8) | (v >> 16)) & 0x0000ffff0000ffffu;
    v = ((v << 16) | (v >> 32)) & 0x00000000ffffffffu;
    *out = (uint32_t)v;
    return true;
}

/* Reads 16 lowercase hex digits into `*out`. */
static bool ta_hex_read16(const char *text, uint64_t *out) {
    uint32_t high = 0;
    uint32_t low = 0;
    bool high_valid = ta_hex_read8(text, &high);
    bool low_valid = ta_hex_read8(text + 8, &low);
    *out = ((uint64_t)high << 32) | low;
    return high_valid && low_valid;
}

static bool ta_trace_space(char c) {
    return c == ' ' || c == '\t';
}

TA_API ta_result ta_traceparent_parse(const ta_traceparent_parse_params *params) {
    if (params == NULL || params->out == NULL || (params->text.len != 0 && params->text.ptr == NULL)) {
        return TA_ERR_INVALID;
    }
    const char *text = params->text.ptr;
    uint32_t len = params->text.len;
    while (len != 0 && ta_trace_space(text[0])) {
        text += 1;
        len -= 1;
    }
    while (len != 0 && ta_trace_space(text[len - 1u])) {
        len -= 1;
    }
    uint64_t version = 0;
    uint64_t high = 0;
    uint64_t low = 0;
    uint64_t span = 0;
    uint64_t flags = 0;
    if (len < TA_TRACEPARENT_LEN || !ta_hex_read(text, 2u, &version) || version == 0xffu || text[2] != '-' ||
        text[35] != '-' || text[52] != '-' || !ta_hex_read16(text + 3, &high) ||
        !ta_hex_read16(text + 19, &low) || !ta_hex_read16(text + 36, &span) ||
        !ta_hex_read(text + 53, 2u, &flags)) {
        return TA_ERR_INVALID;
    }
    /* Version 00 is exactly 55 characters; a later one may carry more after a dash. */
    if (len != TA_TRACEPARENT_LEN && (version == 0 || text[TA_TRACEPARENT_LEN] != '-')) {
        return TA_ERR_INVALID;
    }
    if ((high == 0 && low == 0) || span == 0) {
        return TA_ERR_INVALID;
    }
    *params->out = (ta_trace_parent){
        .context = { .trace_id_high = high, .trace_id_low = low, .flags = (uint8_t)flags },
        .cause = { .session = 0, .macrostep = span, .microstep = 0 },
    };
    return TA_OK;
}

/* Writes `len` characters and a NUL through the buffer convention. */
static ta_result ta_trace_text_begin(char *buffer, uint32_t capacity, uint32_t *needed, uint32_t len) {
    if (needed == NULL) {
        return TA_ERR_INVALID;
    }
    *needed = len + 1u;
    if (buffer == NULL || capacity < len + 1u) {
        return TA_ERR_NOMEM;
    }
    buffer[len] = '\0';
    return TA_OK;
}

TA_API ta_result ta_traceparent_format(const ta_traceparent_format_params *params) {
    if (params == NULL) {
        return TA_ERR_INVALID;
    }
    uint64_t span = ta_step_key_span_id(params->parent.cause);
    if (ta_trace_context_empty(&params->parent.context) || span == 0) {
        if (params->needed != NULL) {
            *params->needed = 0;
        }
        return TA_ERR_INVALID;
    }
    ta_result result = ta_trace_text_begin(params->buffer, params->capacity, params->needed, TA_TRACEPARENT_LEN);
    if (result != TA_OK) {
        return result;
    }
    char *out = params->buffer;
    out[0] = '0';
    out[1] = '0';
    out[2] = '-';
    ta_hex_write(out + 3, params->parent.context.trace_id_high, 16u);
    ta_hex_write(out + 19, params->parent.context.trace_id_low, 16u);
    out[35] = '-';
    ta_hex_write(out + 36, span, 16u);
    out[52] = '-';
    ta_hex_write(out + 53, params->parent.context.flags, 2u);
    return TA_OK;
}

TA_API ta_result ta_trace_id_format(const ta_trace_id_format_params *params) {
    if (params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_trace_text_begin(params->buffer, params->capacity, params->needed, 32u);
    if (result == TA_OK) {
        ta_hex_write(params->buffer, params->context.trace_id_high, 16u);
        ta_hex_write(params->buffer + 16, params->context.trace_id_low, 16u);
    }
    return result;
}

TA_API ta_result ta_span_id_format(const ta_span_id_format_params *params) {
    if (params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_trace_text_begin(params->buffer, params->capacity, params->needed, 16u);
    if (result == TA_OK) {
        ta_hex_write(params->buffer, params->span_id, 16u);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Sampling (ADR 0073 section 3)
 */

#define TA_TRACE_RANDOM_BITS ((uint64_t)1u << 56)

TA_API uint64_t ta_trace_ratio_threshold(uint32_t parts, uint32_t whole) {
    if (whole == 0 || parts == 0) {
        return TA_TRACE_RANDOM_BITS; /* nothing is sampled */
    }
    if (parts >= whole) {
        return 0;
    }
    /* round(rejected * 2^56 / whole), in 64 bits: 2^56 = q * whole + r. */
    uint64_t rejected = (uint64_t)(whole - parts);
    uint64_t q = TA_TRACE_RANDOM_BITS / whole;
    uint64_t r = TA_TRACE_RANDOM_BITS % whole;
    return rejected * q + (rejected * r + whole / 2u) / whole;
}

TA_API bool ta_trace_sampled_at(const ta_trace_context *context, uint64_t threshold) {
    return context != NULL && (context->trace_id_low & (TA_TRACE_RANDOM_BITS - 1u)) >= threshold;
}

TA_API void ta_system_set_trace_ratio(ta_system *system, uint32_t parts, uint32_t whole) {
    if (system != NULL) {
        system->trace_threshold = ta_trace_ratio_threshold(parts, whole);
    }
}

/* ------------------------------------------------------------------------
 * The generator (ADR 0073 section 2): xoshiro256**, seeded through SplitMix64
 */

static uint64_t ta_splitmix64(uint64_t *state) {
    *state += 0x9e3779b97f4a7c15ull;
    uint64_t z = *state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static uint64_t ta_rotl(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static uint64_t ta_trace_next(ta_system *system) {
    uint64_t *s = system->trace_rng;
    uint64_t result = ta_rotl(s[1] * 5u, 7) * 9u;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = ta_rotl(s[3], 45);
    return result;
}

TA_API void ta_system_seed_trace_ids(ta_system *system, uint64_t seed) {
    if (system == NULL) {
        return;
    }
    uint64_t state = seed;
    for (uint32_t index = 0; index < 4u; index += 1) {
        system->trace_rng[index] = ta_splitmix64(&state);
    }
}

TA_API uint64_t ta_system_new_span_id(ta_system *system) {
    if (system == NULL) {
        return 0;
    }
    uint64_t span = 0;
    while (span == 0) {
        span = ta_trace_next(system);
    }
    return span;
}

TA_API ta_trace_context ta_system_new_trace(ta_system *system) {
    if (system == NULL) {
        return (ta_trace_context){0};
    }
    ta_trace_context context = { .flags = TA_TRACE_FLAG_RANDOM };
    while (context.trace_id_high == 0 && context.trace_id_low == 0) {
        context.trace_id_high = ta_trace_next(system);
        context.trace_id_low = ta_trace_next(system);
    }
    if (ta_trace_sampled_at(&context, system->trace_threshold)) {
        context.flags |= TA_TRACE_FLAG_SAMPLED;
    }
    return context;
}



/*
 * SCXML 6.2 and C.1 define the target URIs of the SCXML Event I/O
 * Processor; ADR 0044 adds `#_actor_<family>/<name>` for the logical
 * address of a named actor (ADR 0037).
 *
 *   #_internal                 the sender's own internal queue
 *   #_parent                   the invoking parent session
 *   #_scxml_<session>          a session by its session ID (ADR 0066), whose
 *                              text is <incarnation>.<n>.<family> (ADR 0088),
 *                              or a bare <n> a document may write
 *   #_actor_<family>/<name>    a named actor by its logical address, each
 *                              part percent-encoded (ADR 0096)
 *   #_<invokeid>               an invocation of the sending session
 *
 * A session URI whose text is not a session ID names no session, which is
 * inaccessible rather than invalid (SCXML C.1): it parses as runtime actor
 * 0, which never exists, so the send reports error.communication. Anything
 * else is an invalid target and the send reports error.execution.
 */

static const char TA_URI_INTERNAL[] = "#_internal";
static const char TA_URI_PARENT[] = "#_parent";
static const char TA_URI_SESSION[] = "#_scxml_";
static const char TA_URI_ADDRESS[] = "#_actor_";
static const char TA_URI_INVOKE[] = "#_";

static bool ta_uri_is(ta_str text, const char *literal) {
    return ta_str_equal(text, ta_str_from_cstr(literal));
}

/* Consumes `prefix` from `text`; false when it does not start with it. */
static bool ta_uri_prefix(ta_str *text, const char *prefix) {
    ta_str candidate = ta_str_from_cstr(prefix);
    if (text->len < candidate.len ||
        !ta_str_equal((ta_str){ .ptr = text->ptr, .len = candidate.len }, candidate)) {
        return false;
    }
    text->ptr += candidate.len;
    text->len -= candidate.len;
    return true;
}

/*
 * ADR 0096: in `#_actor_<family>/<name>` each part is percent-encoded. A
 * byte an RFC 3986 fragment may hold is written as it is, except '/', which
 * separates the parts: the unreserved characters, the sub-delims, ':', '@'
 * and '?'. Every other byte, '/' and '%' among them, is written as %XX
 * with upper-case hex digits.
 */
static bool ta_uri_keeps(uint8_t byte) {
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')) {
        return true;
    }
    switch (byte) {
        case '-': case '.': case '_': case '~':
        case '!': case '$': case '&': case '\'': case '(': case ')': case '*': case '+': case ',': case ';': case '=':
        case ':': case '@': case '?':
            return true;
        default:
            return false;
    }
}

static void ta_uri_put_part(ta_text_writer *writer, ta_str part) {
    if (part.len == 0) {
        return;
    }
    uint32_t run = 0;
    for (uint32_t index = 0; index < part.len; index += 1) {
        uint8_t byte = (uint8_t)part.ptr[index];
        if (ta_uri_keeps(byte)) {
            continue;
        }
        ta_text_put(writer, (ta_str){ .ptr = part.ptr + run, .len = index - run });
        char escape[3] = { '%', ta_hex_digit(byte >> 4u, true), ta_hex_digit(byte, true) };
        ta_text_put(writer, (ta_str){ .ptr = escape, .len = 3 });
        run = index + 1u;
    }
    ta_text_put(writer, (ta_str){ .ptr = part.ptr + run, .len = part.len - run });
}

TA_PRIVATE void ta_actor_uri_write(ta_text_writer *writer, const ta_actor_address *address) {
    ta_text_put(writer, ta_str_from_cstr(TA_URI_ADDRESS));
    ta_uri_put_part(writer, address->family.name);
    ta_text_put(writer, (ta_str){ .ptr = "/", .len = 1 });
    ta_uri_put_part(writer, address->name);
}

/*
 * Decodes a percent-encoded part into `out` (NULL only measures it) and
 * sets `*length` to its decoded length; false for a '%' not followed by two
 * hex digits. Any other byte is taken as it is, so a document may write a
 * name by hand without encoding what needs no decoding.
 */
static bool ta_uri_decode(ta_str part, char *out, uint32_t *length) {
    uint32_t written = 0;
    for (uint32_t index = 0; index < part.len; index += 1) {
        char byte = part.ptr[index];
        if (byte == '%') {
            if (index + 2u >= part.len) {
                return false;
            }
            int high = ta_hex_digit_value(part.ptr[index + 1u]);
            int low = ta_hex_digit_value(part.ptr[index + 2u]);
            if (high < 0 || low < 0) {
                return false;
            }
            byte = (char)((high << 4) | low);
            index += 2u;
        }
        if (out != NULL) {
            out[written] = byte;
        }
        written += 1u;
    }
    *length = written;
    return true;
}

/*
 * "<family>/<name>", both parts non-empty with no further unencoded slash,
 * interned decoded (ADR 0065). A malformed escape is TA_ERR_INVALID.
 */
static ta_result ta_uri_address_intern(ta_system *system, ta_str text, ta_address **out) {
    uint32_t slash = 0;
    bool encoded = false;
    while (slash < text.len && text.ptr[slash] != '/') {
        encoded = encoded || text.ptr[slash] == '%';
        slash += 1;
    }
    if (slash == 0 || slash + 1u >= text.len) {
        return TA_ERR_INVALID;
    }
    ta_str family = { .ptr = text.ptr, .len = slash };
    ta_str name = { .ptr = text.ptr + slash + 1u, .len = text.len - slash - 1u };
    for (uint32_t index = 0; index < name.len; index += 1) {
        if (name.ptr[index] == '/') {
            return TA_ERR_INVALID;
        }
        encoded = encoded || name.ptr[index] == '%';
    }
    if (!encoded) {
        return ta_address_intern(system, &(ta_actor_address){ .family = { .name = family }, .name = name }, out);
    }
    uint32_t family_length = 0;
    uint32_t name_length = 0;
    if (!ta_uri_decode(family, NULL, &family_length) || !ta_uri_decode(name, NULL, &name_length)) {
        return TA_ERR_INVALID;
    }
    size_t size = (size_t)family_length + name_length;
    char *decoded = ta_mem_alloc(system->allocator, size, 1);
    if (decoded == NULL) {
        return TA_ERR_NOMEM;
    }
    (void)ta_uri_decode(family, decoded, &family_length);
    (void)ta_uri_decode(name, decoded + family_length, &name_length);
    ta_result result = ta_address_intern(
        system,
        &(ta_actor_address){ .family = { .name = { .ptr = decoded, .len = family_length } },
                             .name = { .ptr = decoded + family_length, .len = name_length } },
        out);
    ta_mem_free(system->allocator, decoded, &(ta_allocator_free_params){ .ptr = decoded, .size = size, .alignment = 1 });
    return result;
}

/* Decimal digits only, at least one, no overflow. */
static bool ta_uri_number(ta_str text, uint64_t *out);

/* The digits of `text` up to the first '.', as a number; false without one. */
static bool ta_uri_number_part(ta_str *text, uint64_t *out) {
    uint32_t dot = 0;
    while (dot < text->len && text->ptr[dot] != '.') {
        dot += 1;
    }
    if (dot == text->len || !ta_uri_number((ta_str){ .ptr = text->ptr, .len = dot }, out)) {
        return false;
    }
    text->ptr += dot + 1u;
    text->len -= dot + 1u;
    return true;
}

TA_PRIVATE bool ta_session_text_parse(ta_str text, ta_session_text *out) {
    ta_session_text parsed = {0};
    if (!ta_uri_number_part(&text, &parsed.incarnation) || !ta_uri_number_part(&text, &parsed.session) ||
        text.len == 0) {
        return false;
    }
    parsed.family = text;
    *out = parsed;
    return true;
}

TA_PRIVATE void ta_session_text_write(const ta_system *system, ta_text_writer *writer, uint64_t session, ta_str family) {
    ta_text_put_u64(writer, system->incarnation);
    ta_text_put(writer, (ta_str){ .ptr = ".", .len = 1 });
    ta_text_put_u64(writer, session);
    ta_text_put(writer, (ta_str){ .ptr = ".", .len = 1 });
    ta_text_put(writer, family);
}

TA_PRIVATE void ta_session_ref_write(const ta_system *system, ta_text_writer *writer, const ta_actor_ref *ref) {
    ta_actor_id runtime = ref->as.runtime;
    uint64_t session = ta_session_id_of(system, runtime);
    ta_str family = { 0 };
    if (ref->family != 0) {
        family = ta_atom_text(&system->atoms, ref->family);
    } else {
        const ta_actor *actor = ta_actor_lookup(system, runtime);
        if (actor != NULL) {
            family = actor->definition->identity.family.name;
        }
    }
    if (family.len == 0) {
        /* A bare ID a host or a document supplied, naming no live actor. */
        ta_text_put_u64(writer, session);
        return;
    }
    ta_session_text_write(system, writer, session, family);
}

TA_API ta_result ta_session_id_format(const ta_system *system, const ta_session_id_format_params *params) {
    if (system == NULL || params == NULL || params->needed == NULL || params->session == 0 ||
        params->family.len == 0 || params->family.ptr == NULL) {
        return TA_ERR_INVALID;
    }
    ta_text_writer writer = { .buffer = params->buffer, .capacity = params->capacity };
    ta_session_text_write(system, &writer, params->session, params->family);
    return ta_text_finish(&writer, params->needed);
}

static bool ta_uri_number(ta_str text, uint64_t *out) {
    if (text.len == 0) {
        return false;
    }
    uint64_t value = 0;
    for (uint32_t index = 0; index < text.len; index += 1) {
        char digit = text.ptr[index];
        if (digit < '0' || digit > '9' || value > (UINT64_MAX - 9u) / 10u) {
            return false;
        }
        value = value * 10u + (uint64_t)(digit - '0');
    }
    *out = value;
    return true;
}

TA_API ta_result ta_scxml_target_parse(ta_system *system, ta_str text, ta_scxml_target *out) {
    if (system == NULL || out == NULL || (text.ptr == NULL && text.len != 0)) {
        return TA_ERR_INVALID;
    }
    if (text.ptr == NULL) {
        text = (ta_str){ .ptr = "", .len = 0 };
    }
    if (ta_uri_is(text, TA_URI_INTERNAL)) {
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_INTERNAL };
        return TA_OK;
    }
    if (ta_uri_is(text, TA_URI_PARENT)) {
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_PARENT };
        return TA_OK;
    }
    ta_str rest = text;
    if (ta_uri_prefix(&rest, TA_URI_ADDRESS)) {
        /* The target holds a reference to the interned address (ADR 0065). */
        ta_address *named = NULL;
        ta_result result = ta_uri_address_intern(system, rest, &named);
        if (result != TA_OK) {
            return result;
        }
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_ACTOR,
                                  .as.actor = { .kind = TA_ACTOR_REF_NAMED, .as.named = named } };
        return TA_OK;
    }
    rest = text;
    if (ta_uri_prefix(&rest, TA_URI_SESSION)) {
        uint64_t session = 0;
        ta_atom family = 0;
        ta_session_text parsed;
        if (ta_session_text_parse(rest, &parsed)) {
            /* The family is interned as an address's would be (ADR 0065). */
            ta_result result = ta_atom_intern(&system->atoms, parsed.family, &family);
            if (result != TA_OK) {
                return result;
            }
            /* Another system's session is none here; its family still routes it. */
            session = parsed.incarnation == system->incarnation ? parsed.session : 0;
        } else if (!ta_uri_number(rest, &session)) {
            /* A session URI naming no session is inaccessible, not invalid. */
            session = 0;
        }
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_ACTOR,
                                  .as.actor = { .kind = TA_ACTOR_REF_RUNTIME, .family = family,
                                                .as.runtime = session } };
        return TA_OK;
    }
    rest = text;
    if (ta_uri_prefix(&rest, TA_URI_INVOKE) && rest.len != 0) {
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_INVOKE };
        return ta_invoke_id_from_text(system, rest, &out->as.invoke);
    }
    return TA_ERR_INVALID;
}

TA_API void ta_scxml_target_release(ta_scxml_target *target) {
    if (target != NULL && target->kind == TA_SCXML_TARGET_ACTOR) {
        ta_actor_ref_release(&target->as.actor);
    }
}

TA_PRIVATE bool ta_scxml_uri_write(const ta_system *system, ta_text_writer *writer, const ta_scxml_target *target) {
    switch (target->kind) {
        case TA_SCXML_TARGET_INTERNAL:
            ta_text_put(writer, ta_str_from_cstr(TA_URI_INTERNAL));
            return true;
        case TA_SCXML_TARGET_PARENT:
            ta_text_put(writer, ta_str_from_cstr(TA_URI_PARENT));
            return true;
        case TA_SCXML_TARGET_INVOKE:
            ta_text_put(writer, ta_str_from_cstr(TA_URI_INVOKE));
            ta_text_put(writer, ta_invoke_id_text(system, target->as.invoke));
            return true;
        case TA_SCXML_TARGET_ACTOR:
            if (target->as.actor.kind == TA_ACTOR_REF_NAMED) {
                ta_actor_address view = ta_address_view(target->as.actor.as.named);
                ta_actor_uri_write(writer, &view);
                return true;
            }
            /* A runtime reference is written as its session (ADR 0066, ADR 0088). */
            ta_text_put(writer, ta_str_from_cstr(TA_URI_SESSION));
            ta_session_ref_write(system, writer, &target->as.actor);
            return true;
        default:
            /* SELF is the absent target, which has no URI of its own. */
            return false;
    }
}

TA_API ta_result ta_scxml_target_format(const ta_system *system, const ta_scxml_target_format_params *params) {
    if (system == NULL || params == NULL || params->needed == NULL) {
        return TA_ERR_INVALID;
    }
    ta_text_writer writer = { .buffer = params->buffer, .capacity = params->capacity };
    if (!ta_scxml_uri_write(system, &writer, &params->target)) {
        *params->needed = 0;
        return TA_ERR_NOT_FOUND;
    }
    return ta_text_finish(&writer, params->needed);
}

TA_API ta_result ta_actor_uri_format(const ta_actor_uri_format_params *params) {
    if (params == NULL || params->needed == NULL || params->address.family.name.len == 0 ||
        params->address.family.name.ptr == NULL || params->address.name.len == 0 ||
        params->address.name.ptr == NULL) {
        return TA_ERR_INVALID;
    }
    ta_text_writer writer = { .buffer = params->buffer, .capacity = params->capacity };
    ta_actor_uri_write(&writer, &params->address);
    return ta_text_finish(&writer, params->needed);
}


TA_PRIVATE void ta_actor_ref_retain(const ta_actor_ref *ref) {
    if (ref->kind == TA_ACTOR_REF_NAMED) {
        ta_address_retain(ref->as.named);
    }
}

TA_PRIVATE void ta_actor_ref_release(ta_actor_ref *ref) {
    if (ref->kind == TA_ACTOR_REF_NAMED) {
        ta_address_release(ref->as.named);
        ref->as.named = NULL;
    }
}

TA_PRIVATE ta_result ta_actor_ref_localize(ta_system *system, ta_actor_ref *ref) {
    if (ref->kind != TA_ACTOR_REF_NAMED || ref->as.named == NULL || ta_address_in(ref->as.named, &system->addresses)) {
        return TA_OK;
    }
    ta_actor_address view = ta_address_view(ref->as.named);
    return ta_address_table_intern(&system->addresses, &view, &ref->as.named);
}

TA_PRIVATE ta_result ta_actor_ref_detach(ta_arena *arena, ta_actor_ref *ref) {
    if (ref->kind != TA_ACTOR_REF_NAMED || ref->as.named == NULL) {
        return TA_OK;
    }
    return ta_address_detach(arena, ref->as.named, &ref->as.named);
}

TA_PRIVATE ta_actor_ref *ta_event_origin_ref(ta_event *event) {
    return event->origin.kind == TA_ORIGIN_ACTOR ? &event->origin.as.actor : NULL;
}

TA_API void ta_external_delivery_retain(const ta_external_delivery *delivery, ta_external_delivery *out) {
    if (delivery == NULL || out == NULL) {
        return;
    }
    ta_external_delivery copy = *delivery;
    ta_actor_ref_retain(&copy.destination);
    ta_event_retain(&delivery->event, &copy.event);
    *out = copy;
}

TA_API void ta_external_delivery_release(ta_external_delivery *delivery) {
    if (delivery == NULL) {
        return;
    }
    ta_actor_ref_release(&delivery->destination);
    ta_event_release(&delivery->event);
}



TA_PRIVATE bool ta_configuration_contains_atomic(const ta_configuration *config,
                                      ta_state_id atomic_state) {
    if (atomic_state >= config->layout->atomic_state_count) {
        return false;
    }
    return ta_bits_get(config->storage, atomic_state);
}

TA_PRIVATE void ta_configuration_set_atomic(ta_configuration *config,
                                 ta_state_id atomic_state,
                                 bool active) {
    if (atomic_state < config->layout->atomic_state_count) {
        ta_bits_put(config->storage, atomic_state, active);
    }
}

TA_PRIVATE void ta_configuration_clear(ta_configuration *config) {
    ta_mem_zero(config->storage, config->layout->bytes);
}


TA_PRIVATE uint64_t *ta_history_slot_words(const ta_history_state *history, uint32_t slot) {
    return (uint64_t *)((unsigned char *)history->storage + history->layout->slots[slot].offset);
}

/*
 * The implementation of package continuation (see continuation.h for the
 * contract, the lifetime and the layout of the allocation).
 *
 * Lifetime: a continuation is one allocation owned by one actor (or by a
 * session image while paused); ta_continuation_free is the only free and
 * uses the size stored in the header. Nothing here allocates: the
 * allocation is made by the callers, at the safe points the catalog names,
 * after ta_continuation_bytes has answered.
 *
 * Sizes: ta_continuation_bytes is the only place the arrays' offsets and
 * the total are computed. It does it with a ta_layout and finishes it, so
 * an overflow, or a total or offset that does not fit the header's 32 bits,
 * is a TA_ERR_NOMEM result and never a number. The accessors below are
 * field reads with no arithmetic and cannot fail; they are valid for the
 * shape the offsets were computed for.
 *
 * Complexity: ta_continuation_bytes is O(1); the accessors are O(1);
 * retain and release walk the frames, O(frame_count).
 */


/*
 * The sets are the first array and start right after the header, so their
 * offset is not stored: the header is a multiple of the sets' alignment.
 */
_Static_assert(sizeof(ta_continuation) % _Alignof(uint64_t) == 0, "the sets start at sizeof(ta_continuation)");

TA_PRIVATE ta_result ta_continuation_bytes(const ta_continuation *shape, size_t *bytes, ta_continuation_offsets *offsets) {
    ta_layout layout;
    ta_layout_init(&layout, 0);
    ta_layout_add(&layout, &(ta_layout_entry){ sizeof(ta_continuation), _Alignof(ta_continuation), 1u });
    /* Four sets of `words` 64-bit words each, at sizeof(ta_continuation). */
    ta_layout_add(&layout, &(ta_layout_entry){ 4u * sizeof(uint64_t), _Alignof(uint64_t), shape->words });
    size_t decls = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(uint64_t), _Alignof(uint64_t), shape->decl_words });
    size_t history = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(ta_state_id), _Alignof(ta_state_id),
                                                                shape->state_count });
    size_t enabled = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(ta_transition_id), _Alignof(ta_transition_id),
                                                                shape->enabled_count });
    size_t frames = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(ta_exec_frame), _Alignof(ta_exec_frame),
                                                               shape->frame_count });
    size_t end = 0;
    if (ta_layout_finish(&layout, &end, NULL) != TA_OK || end > UINT32_MAX) {
        return TA_ERR_NOMEM;
    }
    *bytes = end;
    if (offsets != NULL) {
        *offsets = (ta_continuation_offsets){ .decls = (uint32_t)decls, .history = (uint32_t)history,
                                              .enabled = (uint32_t)enabled, .frames = (uint32_t)frames };
    }
    return TA_OK;
}

TA_PRIVATE uint64_t *ta_continuation_sets(ta_continuation *continuation) {
    return (uint64_t *)((unsigned char *)continuation + sizeof(ta_continuation));
}

TA_PRIVATE uint64_t *ta_continuation_decls(ta_continuation *continuation) {
    return (uint64_t *)((unsigned char *)continuation + continuation->offsets.decls);
}

TA_PRIVATE ta_state_id *ta_continuation_history(ta_continuation *continuation) {
    return (ta_state_id *)((unsigned char *)continuation + continuation->offsets.history);
}

TA_PRIVATE ta_transition_id *ta_continuation_enabled(ta_continuation *continuation) {
    return (ta_transition_id *)((unsigned char *)continuation + continuation->offsets.enabled);
}

TA_PRIVATE ta_exec_frame *ta_continuation_frames(ta_continuation *continuation) {
    return (ta_exec_frame *)((unsigned char *)continuation + continuation->offsets.frames);
}

TA_PRIVATE void ta_continuation_retain(ta_continuation *continuation) {
    ta_exec_frame *frames = ta_continuation_frames(continuation);
    for (uint32_t index = 0; index < continuation->frame_count; index += 1u) {
        if (frames[index].kind == TA_EXEC_FRAME_FOREACH) {
            ta_value_retain(&frames[index].iteration_snapshot);
        }
    }
}

TA_PRIVATE void ta_continuation_release(ta_continuation *continuation) {
    ta_exec_frame *frames = ta_continuation_frames(continuation);
    for (uint32_t index = 0; index < continuation->frame_count; index += 1u) {
        if (frames[index].kind == TA_EXEC_FRAME_FOREACH) {
            ta_value_release(&frames[index].iteration_snapshot);
        }
    }
}

TA_PRIVATE void ta_continuation_free(ta_allocator allocator, ta_continuation **continuation) {
    ta_continuation *held = *continuation;
    if (held == NULL) {
        return;
    }
    ta_continuation_release(held);
    ta_mem_free(allocator, held, &(ta_allocator_free_params){ .size = held->bytes, .alignment = TA_CONTINUATION_ALIGN });
    *continuation = NULL;
}

TA_PRIVATE bool ta_run_phase_in_microstep(ta_run_phase phase) {
    return phase == TA_PHASE_START || phase == TA_PHASE_EXIT || phase == TA_PHASE_TRANSITIONS ||
           phase == TA_PHASE_ENTER;
}


/* ------------------------------------------------------------------------
 * Keys and the owner heap
 */

typedef struct {
    ta_time due;
    ta_timer_id id;
} ta_timer_key;

static bool ta_timer_key_before(ta_timer_key left, ta_timer_key right) {
    return left.due < right.due || (left.due == right.due && left.id < right.id);
}

static ta_timer_key ta_timer_key_at(const ta_timer_list *list, uint32_t index) {
    return (ta_timer_key){ .due = list->sends[index].due, .id = list->meta[index].id };
}

static bool ta_timer_list_before(const ta_timer_list *left, const ta_timer_list *right) {
    return ta_timer_key_before(ta_timer_key_at(left, 0), ta_timer_key_at(right, 0));
}

/* The heap slot at `index`, which must be below the reserved capacity. */
static ta_timer_list **ta_timer_slot(const ta_timer_heap *heap, uint32_t index) {
    ta_timer_list **const *pages = heap->pages.items;
    return &pages[index / TA_TIMER_PAGE_OWNERS][index % TA_TIMER_PAGE_OWNERS];
}

static ta_timer_list *ta_timer_owner(const ta_timer_heap *heap, uint32_t index) {
    return *ta_timer_slot(heap, index);
}

static void ta_timer_place(ta_timer_heap *heap, uint32_t index, ta_timer_list *list) {
    *ta_timer_slot(heap, index) = list;
    list->heap_index = index;
}

static void ta_timer_sift_up(ta_timer_heap *heap, uint32_t index) {
    ta_timer_list *list = ta_timer_owner(heap, index);
    while (index > 0) {
        uint32_t parent = (index - 1u) / 2u;
        if (!ta_timer_list_before(list, ta_timer_owner(heap, parent))) {
            break;
        }
        ta_timer_place(heap, index, ta_timer_owner(heap, parent));
        index = parent;
    }
    ta_timer_place(heap, index, list);
}

static void ta_timer_sift_down(ta_timer_heap *heap, uint32_t index) {
    ta_timer_list *list = ta_timer_owner(heap, index);
    for (;;) {
        uint32_t smallest = index;
        const ta_timer_list *best = list;
        uint32_t left = index * 2u + 1u;
        uint32_t right = left + 1u;
        if (left < heap->count && ta_timer_list_before(ta_timer_owner(heap, left), best)) {
            smallest = left;
            best = ta_timer_owner(heap, left);
        }
        if (right < heap->count && ta_timer_list_before(ta_timer_owner(heap, right), best)) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }
        ta_timer_place(heap, index, ta_timer_owner(heap, smallest));
        index = smallest;
    }
    ta_timer_place(heap, index, list);
}

static void ta_timer_unindex(ta_timer_heap *heap, ta_timer_list *list) {
    if (!list->indexed) {
        return;
    }
    uint32_t index = list->heap_index;
    list->indexed = false;
    heap->count -= 1u;
    if (index == heap->count) {
        return;
    }
    ta_timer_place(heap, index, ta_timer_owner(heap, heap->count));
    ta_timer_sift_down(heap, index);
    ta_timer_sift_up(heap, index);
}

/*
 * Restores the heap after the list's front may have changed. Inserting a
 * list needs a free heap slot; ta_timer_reserve_heap() provides it.
 */
static void ta_timer_reindex(ta_timer_heap *heap, ta_timer_list *list) {
    if (list->count == 0) {
        ta_timer_unindex(heap, list);
        return;
    }
    if (!list->indexed) {
        list->indexed = true;
        ta_timer_place(heap, heap->count, list);
        heap->count += 1u;
        ta_timer_sift_up(heap, list->heap_index);
        return;
    }
    uint32_t index = list->heap_index;
    ta_timer_sift_down(heap, index);
    ta_timer_sift_up(heap, list->heap_index);
}

static uint32_t ta_timer_heap_capacity(const ta_timer_heap *heap) {
    uint64_t capacity = (uint64_t)heap->pages.count * TA_TIMER_PAGE_OWNERS;
    return capacity > UINT32_MAX ? UINT32_MAX : (uint32_t)capacity;
}

static void ta_timer_free_page(ta_allocator allocator, ta_timer_list **page) {
    ta_mem_free(allocator, page,
                &(ta_allocator_free_params){ .size = (size_t)TA_TIMER_PAGE_OWNERS * sizeof(ta_timer_list *),
                                             .alignment = _Alignof(ta_timer_list *) });
}

static uint32_t ta_timer_grow_capacity(uint32_t current, uint32_t needed) {
    uint32_t capacity = current == 0 ? 4u : current;
    while (capacity < needed) {
        if (capacity > UINT32_MAX / 2u) {
            return needed;
        }
        capacity *= 2u;
    }
    return capacity;
}

/*
 * Makes room for `additional` more owners by adding pages; the heap itself
 * never moves, so no spawn or schedule copies it (as ADR 0063 did for the
 * actor table). TA_ERR_NOMEM changes nothing visible: pages already added
 * stay as spare capacity.
 */
static ta_result ta_timer_reserve_heap(ta_timer_heap *heap, uint32_t additional) {
    if (additional > UINT32_MAX - heap->count) {
        return TA_ERR_NOMEM;
    }
    while (heap->count + additional > ta_timer_heap_capacity(heap)) {
        ta_timer_list **page = ta_mem_alloc(heap->allocator, (size_t)TA_TIMER_PAGE_OWNERS * sizeof(ta_timer_list *),
                                            _Alignof(ta_timer_list *));
        if (page == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_result result = ta_vec_push(&heap->pages, &page);
        if (result != TA_OK) {
            ta_timer_free_page(heap->allocator, page);
            return result;
        }
    }
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * One owner's list
 */

static void ta_timer_free_storage(ta_allocator allocator, ta_timer_list *list) {
    if (list->capacity == 0) {
        return;
    }
    ta_mem_free(allocator, list->sends,
                &(ta_allocator_free_params){ .size = (size_t)list->capacity * sizeof(ta_delayed_send_info),
                                             .alignment = _Alignof(ta_delayed_send_info) });
    ta_mem_free(allocator, list->meta,
                &(ta_allocator_free_params){ .size = (size_t)list->capacity * sizeof(ta_delayed_send_meta),
                                             .alignment = _Alignof(ta_delayed_send_meta) });
    list->sends = NULL;
    list->meta = NULL;
    list->capacity = 0;
}

static ta_result ta_timer_reserve_list(ta_allocator allocator, ta_timer_list *list, uint32_t additional) {
    if (additional > UINT32_MAX - list->count) {
        return TA_ERR_NOMEM;
    }
    if (additional <= list->capacity - list->count) {
        return TA_OK;
    }
    uint32_t capacity = ta_timer_grow_capacity(list->capacity, list->count + additional);
    ta_delayed_send_info *sends = ta_mem_alloc(allocator, (size_t)capacity * sizeof(ta_delayed_send_info),
                                               _Alignof(ta_delayed_send_info));
    ta_delayed_send_meta *meta = ta_mem_alloc(allocator, (size_t)capacity * sizeof(ta_delayed_send_meta),
                                              _Alignof(ta_delayed_send_meta));
    if (sends == NULL || meta == NULL) {
        if (sends != NULL) {
            ta_mem_free(allocator, sends,
                        &(ta_allocator_free_params){ .size = (size_t)capacity * sizeof(ta_delayed_send_info),
                                                     .alignment = _Alignof(ta_delayed_send_info) });
        }
        if (meta != NULL) {
            ta_mem_free(allocator, meta,
                        &(ta_allocator_free_params){ .size = (size_t)capacity * sizeof(ta_delayed_send_meta),
                                                     .alignment = _Alignof(ta_delayed_send_meta) });
        }
        return TA_ERR_NOMEM;
    }
    if (list->count != 0) {
        ta_mem_copy(sends, list->sends, (size_t)list->count * sizeof(ta_delayed_send_info));
        ta_mem_copy(meta, list->meta, (size_t)list->count * sizeof(ta_delayed_send_meta));
    }
    ta_timer_free_storage(allocator, list);
    list->sends = sends;
    list->meta = meta;
    list->capacity = capacity;
    return TA_OK;
}

static void ta_timer_split(const ta_delayed_send *send, ta_delayed_send_info *info, ta_delayed_send_meta *meta) {
    *info = (ta_delayed_send_info){
        .send_id = send->send_id,
        .due = send->due,
        .event = send->event,
        .io_type = send->io_type,
        .destination = send->destination,
        .target = send->target,
        .data = send->data,
        .submitted = send->submitted,
        .has_target_text = send->has_target_text,
        .target_text = send->target_text,
        .has_content = send->has_content,
        .content = send->content,
        .trace = send->trace,
    };
    *meta = (ta_delayed_send_meta){
        .id = send->id,
        .owner = send->owner,
        .origin = send->origin,
        .invoke_id = send->invoke_id,
    };
}

static ta_delayed_send ta_timer_join(const ta_timer_list *list, uint32_t index) {
    const ta_delayed_send_info *info = &list->sends[index];
    const ta_delayed_send_meta *meta = &list->meta[index];
    return (ta_delayed_send){
        .id = meta->id,
        .owner = meta->owner,
        .send_id = info->send_id,
        .due = info->due,
        .event = info->event,
        .io_type = info->io_type,
        .destination = info->destination,
        .target = info->target,
        .data = info->data,
        .submitted = info->submitted,
        .origin = meta->origin,
        .invoke_id = meta->invoke_id,
        .has_content = info->has_content,
        .content = info->content,
        .has_target_text = info->has_target_text,
        .target_text = info->target_text,
        .trace = info->trace,
    };
}

/* The index of the first stored send that sorts after key. */
static uint32_t ta_timer_upper_bound(const ta_timer_list *list, ta_timer_key key) {
    uint32_t low = 0;
    uint32_t high = list->count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (ta_timer_key_before(key, ta_timer_key_at(list, middle))) {
            high = middle;
        } else {
            low = middle + 1u;
        }
    }
    return low;
}

/* Inserts into reserved capacity; the caller reindexes. */
static void ta_timer_insert(ta_timer_list *list, const ta_delayed_send *send) {
    uint32_t at = ta_timer_upper_bound(list, (ta_timer_key){ .due = send->due, .id = send->id });
    for (uint32_t index = list->count; index > at; index -= 1u) {
        list->sends[index] = list->sends[index - 1u];
        list->meta[index] = list->meta[index - 1u];
    }
    ta_timer_split(send, &list->sends[at], &list->meta[at]);
    list->count += 1u;
}

typedef struct {
    ta_send_id send_id;
    bool any_send_id;
    bool include_submitted;
} ta_timer_selection;

static bool ta_timer_selected(const ta_delayed_send_info *send, const ta_timer_selection *selection) {
    return (selection->any_send_id || send->send_id == selection->send_id) &&
           (selection->include_submitted || !send->submitted);
}

/* Releases and removes the selected sends, keeping order; reindexes the list. */
static bool ta_timer_remove_selected(ta_timer_heap *heap, ta_timer_list *list, const ta_timer_selection *selection) {
    if (list == NULL) {
        return false;
    }
    uint32_t kept = 0;
    for (uint32_t index = 0; index < list->count; index += 1u) {
        if (ta_timer_selected(&list->sends[index], selection)) {
            ta_value_release(&list->sends[index].target);
            ta_value_release(&list->sends[index].data);
            /* A stored send owns its destination and origin references (ADR 0065). */
            ta_actor_ref_release(&list->sends[index].destination);
            ta_actor_ref_release(&list->meta[index].origin);
            continue;
        }
        list->sends[kept] = list->sends[index];
        list->meta[kept] = list->meta[index];
        kept += 1u;
    }
    bool found = kept != list->count;
    list->count = kept;
    if (found) {
        ta_timer_reindex(heap, list);
    }
    return found;
}

/* ------------------------------------------------------------------------
 * Public within core
 */

TA_PRIVATE ta_result ta_timer_heap_init(ta_timer_heap *heap, ta_allocator allocator) {
    *heap = (ta_timer_heap){ .allocator = allocator, .next_id = 1 };
    ta_vec_init(&heap->pages, allocator, TA_VEC_LAYOUT(ta_timer_list **));
    return TA_OK;
}

TA_PRIVATE void ta_timer_heap_destroy(ta_timer_heap *heap) {
    /* Lists still indexed belong to actors that outlived the system's teardown. */
    while (heap->count != 0) {
        ta_timer_list *list = ta_timer_owner(heap, 0);
        (void)ta_timer_remove_selected(heap, list,
                                       &(ta_timer_selection){ .any_send_id = true, .include_submitted = true });
    }
    ta_timer_free_storage(heap->allocator, &heap->orphans);
    ta_timer_list ***pages = heap->pages.items;
    for (uint32_t page = 0; page < heap->pages.count; page += 1u) {
        ta_timer_free_page(heap->allocator, pages[page]);
    }
    ta_vec_destroy(&heap->pages);
    *heap = (ta_timer_heap){0};
}

TA_PRIVATE ta_result ta_timer_reserve(ta_timer_heap *heap, ta_timer_list *list, uint32_t additional) {
    if (additional == 0) {
        return TA_OK;
    }
    ta_result result = ta_timer_reserve_heap(heap, list->indexed ? 0u : 1u);
    return result == TA_OK ? ta_timer_reserve_list(heap->allocator, list, additional) : result;
}

TA_PRIVATE ta_result ta_timer_schedule(ta_timer_heap *heap, ta_timer_list *list, const ta_delayed_send *send) {
    ta_result result = ta_timer_reserve(heap, list, 1u);
    if (result != TA_OK) {
        return result;
    }
    ta_delayed_send record = *send;
    if (record.id == 0) {
        record.id = heap->next_id;
        heap->next_id += 1u;
    }
    ta_timer_insert(list, &record);
    ta_timer_reindex(heap, list);
    return TA_OK;
}

TA_PRIVATE ta_result ta_timer_cancel(ta_timer_heap *heap, ta_timer_list *list, ta_send_id send_id) {
    return ta_timer_remove_selected(heap, list, &(ta_timer_selection){ .send_id = send_id }) ? TA_OK
                                                                                            : TA_ERR_NOT_FOUND;
}

TA_PRIVATE ta_result ta_timer_withdraw(ta_timer_heap *heap, ta_timer_list *list, ta_send_id send_id) {
    return ta_timer_remove_selected(heap, list,
                                    &(ta_timer_selection){ .send_id = send_id, .include_submitted = true })
               ? TA_OK
               : TA_ERR_NOT_FOUND;
}

TA_PRIVATE void ta_timer_cancel_unsubmitted(ta_timer_heap *heap, ta_timer_list *list) {
    (void)ta_timer_remove_selected(heap, list, &(ta_timer_selection){ .any_send_id = true });
}

static uint32_t ta_timer_submitted_count(const ta_timer_list *list) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < list->count; index += 1u) {
        count += list->sends[index].submitted ? 1u : 0u;
    }
    return count;
}

TA_PRIVATE ta_result ta_timer_reserve_orphans(ta_timer_heap *heap, const ta_timer_list *list) {
    if (list == NULL || heap->closing) {
        return TA_OK;
    }
    return ta_timer_reserve(heap, &heap->orphans, ta_timer_submitted_count(list));
}

TA_PRIVATE void ta_timer_orphan(ta_timer_heap *heap, ta_timer_list *list) {
    if (list == NULL) {
        return;
    }
    ta_timer_cancel_unsubmitted(heap, list);
    if (heap->closing) {
        (void)ta_timer_remove_selected(heap, list,
                                       &(ta_timer_selection){ .any_send_id = true, .include_submitted = true });
        return;
    }
    if (list->count == 0) {
        return;
    }
    /* Merge the two sorted lists from the back into the reserved room. */
    ta_timer_list *orphans = &heap->orphans;
    uint32_t left = orphans->count;
    uint32_t right = list->count;
    uint32_t out = left + right;
    while (right > 0) {
        out -= 1u;
        if (left > 0 && ta_timer_key_before(ta_timer_key_at(list, right - 1u), ta_timer_key_at(orphans, left - 1u))) {
            left -= 1u;
            orphans->sends[out] = orphans->sends[left];
            orphans->meta[out] = orphans->meta[left];
        } else {
            right -= 1u;
            orphans->sends[out] = list->sends[right];
            orphans->meta[out] = list->meta[right];
        }
    }
    orphans->count += list->count;
    list->count = 0;
    ta_timer_unindex(heap, list);
    ta_timer_reindex(heap, orphans);
}

TA_PRIVATE void ta_timer_list_destroy(ta_timer_heap *heap, ta_timer_list *list) {
    (void)ta_timer_remove_selected(heap, list,
                                   &(ta_timer_selection){ .any_send_id = true, .include_submitted = true });
    ta_timer_unindex(heap, list);
    ta_timer_free_storage(heap->allocator, list);
}

TA_PRIVATE size_t ta_timer_list_bytes(const ta_timer_list *list) {
    return list == NULL ? 0u
                        : (size_t)list->capacity * (sizeof(ta_delayed_send_info) + sizeof(ta_delayed_send_meta));
}

TA_PRIVATE uint32_t ta_timer_copy_unsubmitted(const ta_timer_list *list, ta_delayed_send *out) {
    if (list == NULL) {
        return 0;
    }
    uint32_t count = 0;
    for (uint32_t index = 0; index < list->count; index += 1u) {
        if (list->sends[index].submitted) {
            continue;
        }
        if (out != NULL) {
            out[count] = ta_timer_join(list, index);
        }
        count += 1u;
    }
    return count;
}

TA_PRIVATE bool ta_timer_next_due(const ta_timer_heap *heap, ta_time *out) {
    if (heap->count == 0) {
        return false;
    }
    *out = ta_timer_owner(heap, 0)->sends[0].due;
    return true;
}

TA_PRIVATE ta_result ta_timer_pop(ta_timer_heap *heap, ta_delayed_send *out) {
    if (heap->count == 0) {
        return TA_ERR_NOT_FOUND;
    }
    ta_timer_list *list = ta_timer_owner(heap, 0);
    *out = ta_timer_join(list, 0);
    for (uint32_t index = 1; index < list->count; index += 1u) {
        list->sends[index - 1u] = list->sends[index];
        list->meta[index - 1u] = list->meta[index];
    }
    list->count -= 1u;
    ta_timer_reindex(heap, list);
    return TA_OK;
}


TA_API bool ta_dm_host_in_state(const ta_dm_host *host, ta_state_id state) {
    if (host == NULL || host->impl == NULL || host->impl->actor == NULL) {
        return false;
    }
    return ta_actor_state_active(host->impl->actor, state);
}

TA_API const ta_event *ta_dm_host_event(const ta_dm_host *host) {
    if (host == NULL || host->impl == NULL || host->impl->actor == NULL) {
        return NULL;
    }
    return ta_actor_bound_event(host->impl->actor);
}

TA_API ta_actor_id ta_dm_host_actor(const ta_dm_host *host) {
    if (host == NULL || host->impl == NULL || host->impl->actor == NULL) {
        return 0;
    }
    return host->impl->actor->record.id;
}

static bool ta_dm_host_valid(const ta_dm_host *host) {
    return host != NULL && host->impl != NULL && host->impl->actor != NULL && host->impl->system != NULL;
}

TA_API ta_time ta_dm_host_time(const ta_dm_host *host) {
    if (!ta_dm_host_valid(host)) {
        return 0;
    }
    return ta_interpreter_macrostep_time(host->impl->system, host->impl->actor);
}

/* The render area of the interpreter run that is calling the data model
 * (ADR 0068): rendered text is content-sized (ADR 0092 section 3). */
static ta_allocator ta_dm_arena(const ta_dm_host *host) {
    return ta_arena_allocator(&host->impl->system->render);
}

typedef struct {
    const ta_dm_host *host;
    ta_dm_text_kind kind;
} ta_dm_field;

/* Writes the fields that core renders as text; false when the field has no value. */
static bool ta_dm_write_field(const void *context, ta_text_writer *writer) {
    const ta_dm_field *field = context;
    const ta_system *system = field->host->impl->system;
    const ta_actor *actor = field->host->impl->actor;
    if (field->kind == TA_DM_TEXT_SESSION_ID) {
        ta_session_text_write(system, writer, actor->session_id, actor->definition->identity.family.name);
        return true;
    }
    if (field->kind == TA_DM_TEXT_IO_LOCATION) {
        /* The SCXML processor's location: the address a chart can send to (ADR 0044). */
        ta_scxml_target target = { .kind = TA_SCXML_TARGET_ACTOR, .as.actor = actor->record.self };
        return ta_scxml_uri_write(system, writer, &target);
    }
    const ta_event *event = ta_dm_host_event(field->host);
    if (event == NULL) {
        return false;
    }
    if (field->kind == TA_DM_TEXT_EVENT_SENDID) {
        if (event->send_id == 0) {
            return false;
        }
        ta_send_id_write(system, event->send_id, writer);
        return true;
    }
    ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
    uint32_t count = ta_event_name_segments(&system->event_names, event->name, segments);
    for (uint32_t index = 0; index < count; index += 1) {
        if (index != 0) {
            ta_text_put(writer, (ta_str){ .ptr = ".", .len = 1 });
        }
        ta_text_put(writer, ta_atom_text(&system->atoms, segments[index]));
    }
    return count != 0;
}

static ta_result ta_dm_render_field(const ta_dm_host *host, ta_dm_text_kind kind, ta_str *out) {
    ta_dm_field field = { .host = host, .kind = kind };
    return ta_text_render(ta_dm_arena(host),
                          &(ta_text_render_params){ .write = ta_dm_write_field, .context = &field, .out = out });
}

static ta_address *ta_dm_address(const ta_actor *actor) {
    return actor->record.self.kind == TA_ACTOR_REF_NAMED ? actor->record.self.as.named : NULL;
}

/* An event field that is stored or rendered from the event's origin. */
static ta_result ta_dm_event_text(const ta_dm_host *host, ta_dm_text_kind kind, ta_str *out) {
    const ta_system *system = host->impl->system;
    const ta_actor *actor = host->impl->actor;
    const ta_event *event = ta_dm_host_event(host);
    if (event == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    switch (kind) {
        case TA_DM_TEXT_EVENT_TYPE: {
            static const char *const types[] = { "external", "internal", "platform" };
            if ((uint32_t)event->type >= TA_ARRAY_COUNT(types)) {
                return TA_ERR_NOT_FOUND;
            }
            *out = ta_str_from_cstr(types[event->type]);
            return TA_OK;
        }
        case TA_DM_TEXT_EVENT_ORIGIN:
            return ta_io_origin_uri(system, &(ta_io_origin_render_params){
                                                .origin = &event->origin,
                                                .session_id = actor->session_id,
                                                .address = ta_dm_address(actor),
                                                .arena = ta_dm_arena(host),
                                                .out = out,
                                            });
        case TA_DM_TEXT_EVENT_ORIGINTYPE:
            /*
             * A closed scope keeps its type: a reply then reaches the
             * processor with an empty target, which the processor refuses,
             * so the sender sees error.communication (ADR 0068).
             */
            return ta_io_origin_type(system, &event->origin, out);
        case TA_DM_TEXT_EVENT_INVOKEID:
            if (event->invoke_id == TA_INVOKE_ID_NONE) {
                return TA_ERR_NOT_FOUND;
            }
            /* The SCXML ID text, e.g. the author's ID or `<stateid>.<platformid>`. */
            *out = ta_invoke_id_text(system, event->invoke_id);
            return TA_OK;
        default:
            return ta_dm_render_field(host, kind, out);
    }
}

/* One entry of `_ioprocessors`: its type, or this session's location through it. */
static ta_result ta_dm_io_text(const ta_dm_host *host, const ta_dm_text_params *params) {
    const ta_system *system = host->impl->system;
    const ta_actor *actor = host->impl->actor;
    if (params->io_index == 0) {
        if (params->kind == TA_DM_TEXT_IO_TYPE) {
            *params->out = ta_atom_text(&system->atoms, system->scxml_io_type);
            return TA_OK;
        }
        return ta_dm_render_field(host, TA_DM_TEXT_IO_LOCATION, params->out);
    }
    const ta_io_registration *registration = ta_system_io_at(system, params->io_index);
    if (registration == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (params->kind == TA_DM_TEXT_IO_TYPE) {
        *params->out = ta_atom_text(&system->atoms, registration->type);
        return TA_OK;
    }
    if (registration->io.v->text == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    return registration->io.v->text(registration->io.self, &(ta_io_text_params){
                                                               .kind = TA_IO_TEXT_LOCATION,
                                                               .session_id = actor->session_id,
                                                               .address = ta_dm_address(actor),
                                                               .arena = ta_dm_arena(host),
                                                               .out = params->out,
                                                           });
}

TA_API ta_result ta_dm_host_text(const ta_dm_host *host, const ta_dm_text_params *params) {
    if (!ta_dm_host_valid(host) || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    *params->out = (ta_str){0};
    const ta_system *system = host->impl->system;
    const ta_actor *actor = host->impl->actor;
    ta_result result;
    switch (params->kind) {
        case TA_DM_TEXT_SESSION_ID:
            result = ta_dm_render_field(host, params->kind, params->out);
            break;
        case TA_DM_TEXT_SESSION_NAME:
            if (actor->definition->document_name == TA_ATOM_NONE) {
                return TA_ERR_NOT_FOUND;
            }
            *params->out = ta_atom_text(&system->atoms, actor->definition->document_name);
            return TA_OK;
        case TA_DM_TEXT_IO_TYPE:
        case TA_DM_TEXT_IO_LOCATION:
            result = ta_dm_io_text(host, params);
            break;
        default:
            result = ta_dm_event_text(host, params->kind, params->out);
            break;
    }
    if (result != TA_OK) {
        *params->out = (ta_str){0};
    }
    return result;
}

TA_API ta_result ta_dm_host_state_name(const ta_dm_host *host, ta_state_id state, ta_str *out) {
    if (!ta_dm_host_valid(host) || out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = host->impl->actor->definition;
    if (state >= definition->state_count) {
        return TA_ERR_NOT_FOUND;
    }
    *out = ta_atom_text(&host->impl->system->atoms, definition->states[state].name);
    return TA_OK;
}

TA_API ta_result ta_dm_host_find_state(const ta_dm_host *host, ta_str id, ta_state_id *out) {
    if (!ta_dm_host_valid(host) || out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_system *system = host->impl->system;
    const ta_definition *definition = host->impl->actor->definition;
    ta_atom name = 0;
    if (ta_atom_find(&system->atoms, id, &name) != TA_OK) {
        return TA_ERR_NOT_FOUND;
    }
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (definition->states[state].name == name) {
            *out = state;
            return TA_OK;
        }
    }
    return TA_ERR_NOT_FOUND;
}


/*
 * The Null data model (SCXML B.1, ADR 0057). Its whole expression language
 * is the `In(state)` predicate used in a `cond`. `prepare_definition` refuses
 * a chart with any other expression, naming the expression (ADR 0058), so
 * such a chart fails when it is defined rather than on first use. The
 * remaining operations need data the profile does not have; each fails with
 * a TA_ERR_UNSUPPORTED fault, which the interpreter turns into
 * error.execution.
 *
 * Nothing is stored per actor: instance size 0. A definition with
 * expressions keeps one small record, the expression table core already
 * owns, because an expression reaches the adapter as a ta_expr_id. A
 * definition without expressions keeps nothing.
 */

typedef struct {
    const ta_expression *expressions;
    uint32_t expression_count;
} ta_null_shared;

#define TA_NULL_TEXT(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

/* Every operation the profile does not support answers this way. */
static ta_op_status ta_null_unsupported(ta_fault *fault, ta_str message) {
    if (fault != NULL) {
        *fault = (ta_fault){ .code = TA_ERR_UNSUPPORTED, .message = message };
    }
    return TA_OP_ERROR;
}

static bool ta_null_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static uint32_t ta_null_skip_space(ta_str source, uint32_t at) {
    while (at < source.len && ta_null_space(source.ptr[at])) {
        at += 1u;
    }
    return at;
}

/*
 * The only expression the profile has: `In(id)`, with the ID optionally
 * quoted in ' or ". Returns false when `source` is anything else.
 */
static bool ta_null_parse_in(ta_str source, ta_str *out) {
    uint32_t at = ta_null_skip_space(source, 0);
    if (source.len - at < 4u || source.ptr[at] != 'I' || source.ptr[at + 1u] != 'n') {
        return false;
    }
    at = ta_null_skip_space(source, at + 2u);
    if (at >= source.len || source.ptr[at] != '(') {
        return false;
    }
    at = ta_null_skip_space(source, at + 1u);
    char quote = 0;
    if (at < source.len && (source.ptr[at] == '\'' || source.ptr[at] == '"')) {
        quote = source.ptr[at];
        at += 1u;
    }
    uint32_t start = at;
    while (at < source.len && source.ptr[at] != ')' && source.ptr[at] != quote &&
           (quote != 0 || !ta_null_space(source.ptr[at]))) {
        at += 1u;
    }
    uint32_t end = at;
    if (quote != 0) {
        if (at >= source.len || source.ptr[at] != quote) {
            return false;
        }
        at += 1u;
    }
    at = ta_null_skip_space(source, at);
    if (at >= source.len || source.ptr[at] != ')') {
        return false;
    }
    at = ta_null_skip_space(source, at + 1u);
    if (at != source.len || end == start) {
        return false;
    }
    *out = (ta_str){ .ptr = source.ptr + start, .len = end - start };
    return true;
}

static ta_result ta_null_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    if (params == NULL || params->shared == NULL || params->allocator.v == NULL ||
        (params->expressions == NULL && params->expression_count != 0)) {
        return TA_ERR_INVALID;
    }
    *params->shared = NULL;
    for (uint32_t index = 0; index < params->expression_count; index += 1u) {
        const ta_expression *expression = &params->expressions[index];
        ta_str id = {0};
        if (expression->kind == TA_EXPRESSION_NONE) {
            continue;
        }
        if (expression->kind == TA_EXPRESSION_SOURCE && ta_null_parse_in(expression->as.source, &id)) {
            continue;
        }
        if (params->rejected != NULL) {
            *params->rejected = (ta_expression_rejection){
                .expression = index,
                .offset = 0,
                .length = expression->kind == TA_EXPRESSION_SOURCE ? expression->as.source.len : 0u,
                .message = expression->kind == TA_EXPRESSION_SOURCE
                               ? TA_NULL_TEXT("the null data model has only the In(state) predicate")
                               : TA_NULL_TEXT("the null data model takes expressions as source text"),
            };
        }
        return expression->kind == TA_EXPRESSION_SOURCE ? TA_ERR_INVALID : TA_ERR_UNSUPPORTED;
    }
    if (params->expression_count == 0) {
        return TA_OK;
    }
    ta_null_shared *shared = ta_mem_alloc(params->allocator, sizeof(ta_null_shared), _Alignof(ta_null_shared));
    if (shared == NULL) {
        return TA_ERR_NOMEM;
    }
    *shared = (ta_null_shared){ .expressions = params->expressions, .expression_count = params->expression_count };
    *params->shared = shared;
    return TA_OK;
}

static void ta_null_destroy_definition(void *self, void *shared, ta_allocator allocator) {
    (void)self;
    if (shared == NULL || allocator.v == NULL) {
        return;
    }
    ta_mem_free(allocator, shared,
                &(ta_allocator_free_params){ .size = sizeof(ta_null_shared), .alignment = _Alignof(ta_null_shared) });
}

/* No data: an actor of a null-model chart allocates nothing for it. */
static uint32_t ta_null_instance_size(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 0;
}

static uint32_t ta_null_instance_alignment(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 1;
}

static ta_result ta_null_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self;
    /* A binding is host data, which this profile cannot hold. */
    return params != NULL && params->binding != NULL && params->binding->v != NULL ? TA_ERR_UNSUPPORTED : TA_OK;
}

static void ta_null_destroy_instance(void *self, void *shared, void *instance) {
    (void)self;
    (void)shared;
    (void)instance;
}

static ta_str ta_null_source(const ta_null_shared *shared, ta_expr_id expr) {
    if (shared == NULL || shared->expressions == NULL || expr >= shared->expression_count) {
        return (ta_str){0};
    }
    const ta_expression *expression = &shared->expressions[expr];
    return expression->kind == TA_EXPRESSION_SOURCE ? expression->as.source : (ta_str){0};
}

static ta_op_status ta_null_eval_condition(void *self, const ta_datamodel_eval_condition_params *params) {
    (void)self;
    ta_str id = {0};
    if (!ta_null_parse_in(ta_null_source(params->shared, params->expr), &id)) {
        return ta_null_unsupported(params->fault,
                                   TA_NULL_TEXT("the null data model evaluates only the In(state) predicate"));
    }
    /* SCXML B.1: In() of a state the chart does not have is simply false;
     * core answers TA_ERR_NOT_FOUND for it (ADR 0049). */
    ta_state_id state = 0;
    *params->out = ta_dm_host_find_state(params->host, id, &state) == TA_OK && ta_dm_host_in_state(params->host, state);
    return TA_OP_OK;
}

static ta_op_status ta_null_declare(void *self, const ta_datamodel_declare_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no data bindings"));
}

static ta_op_status ta_null_set_named(void *self, const ta_datamodel_set_named_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no data bindings"));
}

static ta_op_status ta_null_eval_value(void *self, const ta_datamodel_eval_value_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no value expressions"));
}

static ta_op_status ta_null_read_location(void *self, const ta_datamodel_read_location_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no locations"));
}

static ta_op_status ta_null_assign(void *self, const ta_datamodel_assign_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no assignment"));
}

static ta_op_status ta_null_execute_script(void *self, const ta_datamodel_execute_script_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no scripts"));
}

static ta_op_status ta_null_make_params(void *self, const ta_datamodel_make_params_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no values to make params from"));
}

static ta_op_status ta_null_value_text(void *self, const ta_datamodel_value_text_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no values"));
}

static ta_op_status ta_null_text_value(void *self, const ta_datamodel_text_value_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no values"));
}

static ta_op_status ta_null_import_content(void *self, const ta_datamodel_import_content_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model cannot import content"));
}

static ta_op_status ta_null_foreach_snapshot(void *self, const ta_datamodel_foreach_snapshot_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no collections to iterate"));
}

static ta_op_status ta_null_foreach_bind(void *self, const ta_datamodel_foreach_bind_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no collections to iterate"));
}

/* It owns no values, so none has a byte form (ADR 0074, ADR 0090). A
 * null-data-model actor holds no values while it is stable, so it still
 * offloads. */
static ta_op_status ta_null_value_write(void *self, const ta_datamodel_value_write_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no values to write"));
}

static ta_op_status ta_null_value_read(void *self, const ta_datamodel_value_read_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no values to read"));
}

/*
 * `inspect` stays absent: the profile gives the application no data to read,
 * so ta_actor_datamodel_root() reports TA_ERR_UNSUPPORTED for these actors.
 */
static const ta_datamodel_vtable ta_null_datamodel_vtable = {
    .name = { .ptr = "null", .len = 4 },
    .prepare_definition = ta_null_prepare,
    .destroy_definition = ta_null_destroy_definition,
    .instance_size = ta_null_instance_size,
    .instance_alignment = ta_null_instance_alignment,
    .init_instance = ta_null_init_instance,
    .destroy_instance = ta_null_destroy_instance,
    .declare = ta_null_declare,
    .set_named = ta_null_set_named,
    .eval_condition = ta_null_eval_condition,
    .eval_value = ta_null_eval_value,
    .read_location = ta_null_read_location,
    .assign = ta_null_assign,
    .execute_script = ta_null_execute_script,
    .make_params = ta_null_make_params,
    .value_text = ta_null_value_text,
    .text_value = ta_null_text_value,
    .import_content = ta_null_import_content,
    .foreach_snapshot = ta_null_foreach_snapshot,
    .foreach_bind = ta_null_foreach_bind,
    .value_write = ta_null_value_write,
    .value_read = ta_null_value_read,
};

TA_API ta_datamodel ta_null_datamodel(void) {
    return (ta_datamodel){ .self = NULL, .v = &ta_null_datamodel_vtable };
}


typedef struct {
    const ta_system *system;
    ta_scxml_target target;
} ta_io_target_text;

static bool ta_io_write_target(const void *context, ta_text_writer *writer) {
    const ta_io_target_text *text = context;
    return ta_scxml_uri_write(text->system, writer, &text->target);
}

static ta_result ta_io_render_target(const ta_system *system, ta_scxml_target target, ta_allocator arena,
                                     ta_str *out) {
    ta_io_target_text text = { .system = system, .target = target };
    return ta_text_render(arena, &(ta_text_render_params){ .write = ta_io_write_target, .context = &text, .out = out });
}

/* The live scope a SCOPED origin names, or null once it closed. */
static const ta_io_origin_entry *ta_io_scope_record(const ta_system *system, const ta_io_origin *io) {
    const ta_io_origin_entry *record = ta_io_origin_at(&system->io_scopes, io->as.scope.index);
    if (record == NULL || record->generation != io->as.scope.generation || record->processor != io->processor) {
        return NULL;
    }
    return record;
}

TA_PRIVATE ta_result ta_io_origin_uri(const ta_system *system, const ta_io_origin_render_params *params) {
    const ta_origin *origin = params->origin;
    switch (origin->kind) {
        case TA_ORIGIN_ACTOR:
            return ta_io_render_target(system,
                                       (ta_scxml_target){ .kind = TA_SCXML_TARGET_ACTOR, .as.actor = origin->as.actor },
                                       params->arena, params->out);
        case TA_ORIGIN_INVOKE:
            return ta_io_render_target(
                system, (ta_scxml_target){ .kind = TA_SCXML_TARGET_INVOKE, .as.invoke = origin->as.invoke.invoke },
                params->arena, params->out);
        case TA_ORIGIN_IO:
            break;
        default:
            return TA_ERR_NOT_FOUND;
    }
    const ta_io_origin *io = &origin->as.io;
    switch (io->origin_class) {
        case TA_IO_ORIGIN_COMPUTED: {
            const ta_io_registration *registration = ta_system_io_at(system, io->processor);
            if (registration == NULL || registration->io.v->text == NULL) {
                return TA_ERR_NOT_FOUND;
            }
            return registration->io.v->text(registration->io.self, &(ta_io_text_params){
                                                                       .kind = TA_IO_TEXT_ORIGIN,
                                                                       .session_id = params->session_id,
                                                                       .address = params->address,
                                                                       .token = io->as.token,
                                                                       .arena = params->arena,
                                                                       .out = params->out,
                                                                   });
        }
        case TA_IO_ORIGIN_SCOPED: {
            const ta_io_origin_entry *record = ta_io_scope_record(system, io);
            if (record == NULL) {
                return TA_ERR_NOT_FOUND;
            }
            *params->out = ta_io_origin_entry_uri(record);
            return TA_OK;
        }
        case TA_IO_ORIGIN_MESSAGE:
            if (io->as.entry == NULL) {
                return TA_ERR_NOT_FOUND;
            }
            *params->out = ta_io_origin_entry_uri(io->as.entry);
            return TA_OK;
        default:
            return TA_ERR_NOT_FOUND;
    }
}

TA_PRIVATE ta_result ta_io_origin_type(const ta_system *system, const ta_origin *origin, ta_str *out) {
    ta_atom type = TA_ATOM_NONE;
    switch (origin->kind) {
        case TA_ORIGIN_ACTOR:
            type = system->scxml_io_type;
            break;
        case TA_ORIGIN_INVOKE: {
            /*
             * The processor that reaches `#_<invokeid>`: the SCXML one for an
             * SCXML child session, otherwise the invocation's own type, which
             * SCXML 6.4 uses for sends to a non-SCXML service.
             */
            ta_atom invoker = ta_system_invoker_type(system, origin->as.invoke.invoker);
            type = invoker == TA_ATOM_NONE || invoker == system->scxml_invoke_type ? system->scxml_io_type : invoker;
            break;
        }
        case TA_ORIGIN_IO: {
            const ta_io_origin_entry *entry = origin->as.io.origin_class == TA_IO_ORIGIN_MESSAGE ? origin->as.io.as.entry
                                                                                                 : NULL;
            if (ta_io_origin_entry_detached(entry)) {
                *out = ta_io_origin_detached_type(entry);
                return TA_OK;
            }
            const ta_io_registration *registration = ta_system_io_at(system, origin->as.io.processor);
            if (registration == NULL) {
                return TA_ERR_NOT_FOUND;
            }
            type = registration->type;
            break;
        }
        default:
            return TA_ERR_NOT_FOUND;
    }
    *out = ta_atom_text(&system->atoms, type);
    return TA_OK;
}

TA_API ta_result ta_io_origin_make(ta_system *system, const ta_io_origin_make_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    *params->out = (ta_origin){ .kind = TA_ORIGIN_NONE };
    const ta_io_registration *registration = ta_system_io_at(system, params->processor);
    if (registration == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_io_origin io = { .processor = params->processor, .origin_class = params->origin_class };
    switch (params->origin_class) {
        case TA_IO_ORIGIN_COMPUTED:
            if (registration->io.v->text == NULL) {
                return TA_ERR_UNSUPPORTED;
            }
            io.as.token = params->token;
            break;
        case TA_IO_ORIGIN_SCOPED:
            io.as.scope = params->scope;
            if (ta_io_scope_record(system, &io) == NULL) {
                return TA_ERR_NOT_FOUND;
            }
            break;
        case TA_IO_ORIGIN_MESSAGE: {
            if (params->uri.ptr == NULL && params->uri.len != 0) {
                return TA_ERR_INVALID;
            }
            io.as.entry = ta_io_origin_take(&system->io_messages,
                                            &(ta_io_origin_take_params){ .processor = params->processor,
                                                                         .uri = params->uri });
            if (io.as.entry == NULL) {
                return TA_ERR_NOMEM;
            }
            break;
        }
        default:
            return TA_ERR_INVALID;
    }
    *params->out = (ta_origin){ .kind = TA_ORIGIN_IO, .as.io = io };
    return TA_OK;
}

TA_API ta_result ta_io_scope_open(ta_system *system, const ta_io_scope_open_params *params) {
    if (system == NULL || params == NULL || params->out == NULL ||
        (params->uri.ptr == NULL && params->uri.len != 0)) {
        return TA_ERR_INVALID;
    }
    if (ta_system_io_at(system, params->processor) == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    uint32_t index = 0;
    ta_io_origin_entry *record = ta_io_origin_take(
        &system->io_scopes,
        &(ta_io_origin_take_params){ .processor = params->processor, .uri = params->uri, .index = &index });
    if (record == NULL) {
        return TA_ERR_NOMEM;
    }
    *params->out = (ta_io_scope){ .index = index, .generation = record->generation };
    return TA_OK;
}

TA_API void ta_io_scope_close(ta_system *system, ta_io_scope scope) {
    if (system == NULL) {
        return;
    }
    ta_io_origin_entry *record = ta_io_origin_at(&system->io_scopes, scope.index);
    if (record != NULL && record->generation == scope.generation) {
        ta_io_origin_give(record);
    }
}

TA_API ta_result ta_origin_text(const ta_system *system, const ta_origin_text_params *params) {
    if (system == NULL || params == NULL || params->origin == NULL || params->type == NULL ||
        params->uri == NULL) {
        return TA_ERR_INVALID;
    }
    *params->type = (ta_str){0};
    *params->uri = (ta_str){0};
    ta_result result = ta_io_origin_type(system, params->origin, params->type);
    if (result != TA_OK) {
        return result;
    }
    return ta_io_origin_uri(system, &(ta_io_origin_render_params){
                                        .origin = params->origin,
                                        .session_id = params->session_id,
                                        .arena = params->arena,
                                        .out = params->uri,
                                    });
}


/*
 * SCXML <invoke>: starting services at the end of a macrostep, cancelling
 * them when their state exits, <finalize>, autoforwarding, done.invoke, and
 * the host's asynchronous reports.
 *
 * Invoke IDs are SCXML text (the author's `id` or a generated
 * `<stateid>.<platformid>`). The runtime ta_invoke_id is the interned text's
 * atom + 1, so the text is always recoverable and `#_<invokeid>` targets
 * resolve by interning.
 */

/* ------------------------------------------------------------------------
 * IDs
 */

TA_PRIVATE ta_result ta_invoke_id_from_text(ta_system *system, ta_str text, ta_invoke_id *out) {
    ta_atom atom = 0;
    if (text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_atom_intern(&system->atoms, text, &atom);
    if (result == TA_OK) {
        *out = (ta_invoke_id)atom + 1u;
    }
    return result;
}

TA_API ta_result ta_system_intern_invoke_id(ta_system *system, ta_str text, ta_invoke_id *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    return ta_invoke_id_from_text(system, text, out);
}

TA_PRIVATE ta_str ta_invoke_id_text(const ta_system *system, ta_invoke_id invoke) {
    if (invoke == 0 || invoke == TA_INVOKE_ID_NONE || invoke - 1u >= TA_ATOM_NONE) {
        return (ta_str){0};
    }
    return ta_atom_text(&system->atoms, (ta_atom)(invoke - 1u));
}

TA_API ta_result ta_system_invoke_id_text(const ta_system *system, ta_invoke_id invoke, ta_str *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    if (invoke == 0 || invoke == TA_INVOKE_ID_NONE || invoke - 1u >= TA_ATOM_NONE ||
        !ta_atom_valid(&system->atoms, (ta_atom)(invoke - 1u))) {
        return TA_ERR_NOT_FOUND;
    }
    *out = ta_invoke_id_text(system, invoke);
    return TA_OK;
}

/* done.invoke.<id>, one event-name segment per dotted part of the ID text. */
static ta_result ta_invoke_done_name(ta_system *system, ta_invoke_id invoke, ta_event_name_id *out) {
    ta_str text = ta_invoke_id_text(system, invoke);
    ta_event_name_id name = TA_EVENT_NAME_DONE_INVOKE;
    uint32_t start = 0;
    for (uint32_t index = 0; text.ptr != NULL && index <= text.len; index += 1) {
        if (index < text.len && text.ptr[index] != '.') {
            continue;
        }
        if (index > start) {
            ta_atom atom = 0;
            ta_result result = ta_atom_intern(&system->atoms,
                                              (ta_str){ .ptr = text.ptr + start, .len = index - start }, &atom);
            if (result == TA_OK) {
                result = ta_event_name_child(&system->event_names, name, atom, &name);
            }
            if (result != TA_OK) {
                return result;
            }
        }
        start = index + 1u;
    }
    *out = name;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Records
 */

static int64_t ta_invoke_find(const ta_actor *actor, ta_invoke_id invoke) {
    const ta_actor_sparse_state *sparse = actor->sparse;
    for (uint32_t index = 0; sparse != NULL && index < sparse->invoke_count; index += 1) {
        if (sparse->invokes[index].id == invoke) {
            return (int64_t)index;
        }
    }
    return -1;
}

/*
 * The invoke table is a ta_columns (vec.h) of two columns: the public
 * ta_active_invoke records, at offset 0 of the block so that `invokes` and
 * ta_actor_inspection.invokes stay a contiguous array, and the private
 * ta_invoke_meta records parallel to them. Lifetime: session. It is created
 * and grown only where an invoke is added (ta_invoke_start at the end of a
 * macrostep, ta_invoke_restore while loading), never inside a unit of
 * executable content, and freed by ta_invoke_release. Pointers into it are
 * valid only until the next ta_invoke_grow, so a function keeps indices.
 */
static const ta_column ta_invoke_column_shapes[] = {
    { sizeof(ta_active_invoke), _Alignof(ta_active_invoke) },
    { sizeof(ta_invoke_meta), _Alignof(ta_invoke_meta) },
};
static const ta_columns_layout ta_invoke_columns = { ta_invoke_column_shapes, 2u, 4u };

TA_PRIVATE size_t ta_invoke_table_bytes(const ta_columns *table) {
    return ta_columns_bytes(&ta_invoke_columns, table);
}

TA_PRIVATE ta_invoke_decl_id ta_invoke_table_decl(const ta_columns *table, uint32_t index) {
    const ta_invoke_meta *metas = ta_columns_at(&ta_invoke_columns, table, 1);
    return metas[index].decl;
}

/* The private records, parallel to sparse->invokes; NULL while the table is empty. */
static ta_invoke_meta *ta_invoke_metas(const ta_actor_sparse_state *sparse) {
    return ta_columns_at(&ta_invoke_columns, &sparse->invoke_table, 1);
}

/* Makes room for one more invoke; on TA_ERR_NOMEM the table is unchanged. */
static ta_result ta_invoke_grow(ta_system *system, ta_actor_sparse_state *sparse) {
    return ta_columns_grow(system->allocator, &ta_invoke_columns, &sparse->invoke_table);
}

static void ta_invoke_remove_at(ta_actor_sparse_state *sparse, uint32_t index) {
    ta_invoke_meta *metas = ta_invoke_metas(sparse);
    for (uint32_t at = index + 1u; at < sparse->invoke_count; at += 1) {
        sparse->invokes[at - 1u] = sparse->invokes[at];
        metas[at - 1u] = metas[at];
    }
    sparse->invoke_count -= 1u;
}

static ta_invoke_taken ta_invoke_take_index(ta_actor_sparse_state *sparse, uint32_t index) {
    ta_invoke_taken taken = { .active = sparse->invokes[index], .meta = ta_invoke_metas(sparse)[index] };
    /* Remove first: the service may destroy a child that reports back reentrantly. */
    ta_invoke_remove_at(sparse, index);
    return taken;
}

TA_PRIVATE void ta_invoke_cancel_service(const ta_invoke_taken *taken) {
    if (!taken->meta.replayed && taken->meta.invoker.v != NULL && taken->meta.invoker.v->cancel != NULL) {
        ta_fault fault = {0};
        (void)taken->meta.invoker.v->cancel(taken->meta.invoker.self, taken->active.service_handle, &fault);
        ta_value_release(&fault.data);
    }
}

/* Cancels the service and forgets the invocation. */
static void ta_invoke_cancel_at(ta_system *system, ta_actor_sparse_state *sparse, uint32_t index) {
    ta_invoke_taken taken = ta_invoke_take_index(sparse, index);
    if (taken.meta.has_child) {
        ta_actor_exit_invoked(system, taken.meta.child);
    }
    ta_invoke_cancel_service(&taken);
}

static ta_vec *ta_invoke_pending(ta_system *system, ta_actor_sparse_state *sparse) {
    if (sparse->pending_invokes.element_size == 0) {
        ta_vec_init(&sparse->pending_invokes, system->allocator, TA_VEC_LAYOUT(ta_state_id));
    }
    return &sparse->pending_invokes;
}

static const ta_invoker *ta_invoke_find_invoker(const ta_system *system, ta_atom type) {
    for (uint32_t index = 0; index < system->invokers.count; index += 1) {
        const ta_invoker_registration *entry = ta_vec_at(&system->invokers, index);
        if (entry->type == type) {
            return &entry->invoker;
        }
    }
    return NULL;
}

static ta_atom ta_invoke_normalize_type(const ta_system *system, ta_atom type) {
    static const char *const aliases[] = { "scxml", "http://www.w3.org/TR/scxml", "http://www.w3.org/TR/scxml/" };
    ta_str text = ta_atom_text(&system->atoms, type);
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(aliases); index += 1) {
        ta_str alias = { .ptr = aliases[index], .len = 0 };
        while (alias.ptr[alias.len] != '\0') {
            alias.len += 1;
        }
        if (ta_str_equal(text, alias)) {
            return system->scxml_invoke_type;
        }
    }
    return type;
}

/* ------------------------------------------------------------------------
 * Lifecycle hooks
 */

TA_PRIVATE ta_result ta_invoke_state_entered(ta_interpreter *it, ta_state_id state) {
    const ta_definition *definition = it->definition;
    bool has_invokes = false;
    for (uint32_t index = 0; index < definition->invoke_decl_count && !has_invokes; index += 1) {
        has_invokes = definition->invokes[index].owner == state;
    }
    if (!has_invokes) {
        return TA_OK;
    }
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(it->system, it->actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_vec *pending = ta_invoke_pending(it->system, sparse);
    for (uint32_t index = 0; index < pending->count; index += 1) {
        if (*(ta_state_id *)ta_vec_at(pending, index) == state) {
            return TA_OK;
        }
    }
    return ta_vec_push(pending, &state);
}

TA_PRIVATE void ta_invoke_forget_pending(ta_actor *actor, ta_state_id state) {
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL) {
        return;
    }
    ta_vec *pending = &sparse->pending_invokes;
    for (uint32_t index = 0; index < pending->count; index += 1) {
        if (*(ta_state_id *)ta_vec_at(pending, index) == state) {
            *(ta_state_id *)ta_vec_at(pending, index) = *(ta_state_id *)ta_vec_at(pending, pending->count - 1u);
            pending->count -= 1u;
            break;
        }
    }
}

TA_PRIVATE bool ta_invoke_take(ta_actor *actor, ta_state_id state, uint32_t *cursor, ta_invoke_taken *out) {
    ta_actor_sparse_state *sparse = actor->sparse;
    uint32_t index = sparse == NULL ? 0 : *cursor;
    if (sparse != NULL && index > sparse->invoke_count) {
        index = sparse->invoke_count;
    }
    /* Nothing in this loop grows the table, so the column is read once. */
    const ta_invoke_meta *metas = sparse == NULL ? NULL : ta_invoke_metas(sparse);
    for (; index > 0; index -= 1u) {
        if (metas[index - 1u].state == state) {
            *out = ta_invoke_take_index(sparse, index - 1u);
            *cursor = index - 1u;
            return true;
        }
    }
    *cursor = 0;
    return false;
}

TA_PRIVATE ta_result ta_invoke_state_exited(ta_interpreter *it, ta_state_id state) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    if (sparse == NULL) {
        return TA_OK;
    }
    ta_invoke_forget_pending(it->actor, state);
    const ta_invoke_meta *metas = ta_invoke_metas(sparse);
    for (uint32_t index = sparse->invoke_count; index > 0; index -= 1u) {
        if (metas[index - 1u].state == state) {
            ta_interpreter_trace_invoke(it, TA_TRACE_INVOKE_CANCELLED, sparse->invokes[index - 1u].id);
            ta_invoke_cancel_at(it->system, sparse, index - 1u);
            /* Cancelling calls out (a child's exit); re-derive rather than assume the block stayed. */
            metas = ta_invoke_metas(sparse);
        }
    }
    return TA_OK;
}

typedef struct {
    ta_value src;
    ta_value data;
    ta_value params;
    ta_fault fault;
} ta_invoke_values;

static void ta_invoke_values_release(ta_invoke_values *values) {
    ta_value_release(&values->src);
    ta_value_release(&values->data);
    ta_value_release(&values->params);
    ta_value_release(&values->fault.data);
}

static ta_op_status ta_invoke_text_value(ta_interpreter *it, ta_datamodel_text_value_params *params) {
    const ta_definition *definition = it->definition;
    if (definition->datamodel.v->text_value == NULL) {
        return TA_OP_ERROR;
    }
    params->shared = definition->datamodel_shared;
    params->instance = it->actor->datamodel_instance;
    params->host = &it->actor->host;
    return definition->datamodel.v->text_value(definition->datamodel.self, params);
}

static ta_op_status ta_invoke_value_text(ta_interpreter *it, ta_datamodel_value_text_params *params) {
    const ta_definition *definition = it->definition;
    if (definition->datamodel.v->value_text == NULL) {
        return TA_OP_ERROR;
    }
    params->shared = definition->datamodel_shared;
    params->instance = it->actor->datamodel_instance;
    params->host = &it->actor->host;
    return definition->datamodel.v->value_text(definition->datamodel.self, params);
}

/* A step of starting an invocation that failed as an execution error; the
 * fault in its ta_invoke_values says why. */
#define TA_INVOKE_FAILED 1

static ta_result ta_invoke_op(ta_op_status status) {
    return status == TA_OP_OK ? TA_OK : status == TA_OP_NOMEM ? TA_ERR_NOMEM : TA_INVOKE_FAILED;
}

/* Core's own allocation failing is TA_ERR_NOMEM (ADR 0047); anything else
 * core refuses is an execution error. */
static ta_result ta_invoke_core(ta_result result) {
    return result == TA_OK || result == TA_ERR_NOMEM ? result : TA_INVOKE_FAILED;
}

/*
 * The invoke's ID: the author's, or `<stateid>.<platformid>`, also stored
 * through idlocation. A generated ID takes the session's next generated-ID
 * serial, the counter generated send IDs use (ADR 0089 section 5), so a
 * replay of the session numbers its invocations as the original run did.
 * It moves only once the invocation has an outcome (ta_invoke_start), so an
 * invocation that runs out of memory and starts again stores the same ID
 * (ADR 0070).
 */
static ta_result ta_invoke_resolve_id(ta_interpreter *it, const ta_invoke_decl *decl,
                                      ta_invoke_values *values, ta_invoke_id *out) {
    ta_system *system = it->system;
    if (decl->id.kind == TA_ID_SOURCE_LITERAL) {
        *out = (ta_invoke_id)decl->id.as.literal + 1u;
        return TA_OK;
    }
    ta_str state = ta_atom_text(&system->atoms, it->definition->states[decl->owner].name);
    /* Measure, reserve exactly, then fill (D6): the render arena holds the text until its next reset. */
    uint64_t serial = it->actor->next_send_id + 1u;
    uint32_t digit_count = ta_dec_len(serial);
    char *text = NULL;
    uint32_t length = 0;
    if (state.len <= UINT32_MAX - 1u - digit_count) {
        length = state.len + 1u + digit_count;
        text = ta_arena_alloc(&system->render, length, 1);
    }
    if (text == NULL) {
        ta_interpreter_render_fault(&values->fault);
        return TA_INVOKE_FAILED;
    }
    ta_mem_copy(text, state.ptr, state.len);
    text[state.len] = '.';
    ta_dec_write(text + state.len + 1u, serial);
    ta_str id = { .ptr = text, .len = length };
    ta_result result = ta_invoke_core(ta_invoke_id_from_text(system, id, out));
    if (result != TA_OK || decl->id.kind != TA_ID_SOURCE_LOCATION) {
        return result;
    }
    const ta_definition *definition = it->definition;
    ta_value value = {0};
    ta_op_status status = ta_invoke_text_value(it, &(ta_datamodel_text_value_params){
                                                       .text = id, .out = &value, .fault = &values->fault });
    if (status == TA_OP_OK) {
        status = definition->datamodel.v->assign == NULL
                     ? TA_OP_ERROR
                     : definition->datamodel.v->assign(definition->datamodel.self,
                                                       &(ta_datamodel_assign_params){
                                                           .shared = definition->datamodel_shared,
                                                           .instance = it->actor->datamodel_instance,
                                                           .location = decl->id.as.location,
                                                           .value = &value,
                                                           .host = &it->actor->host,
                                                           .fault = &values->fault,
                                                       });
        ta_value_release(&value);
    }
    return ta_invoke_op(status);
}

static ta_result ta_invoke_resolve_type(ta_interpreter *it, const ta_invoke_decl *decl,
                                        ta_invoke_values *values, ta_atom *out) {
    ta_system *system = it->system;
    if (decl->type.kind == TA_TEXT_SOURCE_NONE) {
        *out = system->scxml_invoke_type;
        return TA_OK;
    }
    if (decl->type.kind == TA_TEXT_SOURCE_ATOM) {
        *out = ta_invoke_normalize_type(system, decl->type.as.atom);
        return TA_OK;
    }
    ta_value value = {0};
    ta_str text = {0};
    ta_op_status status = ta_interpreter_eval(it, &(ta_interpreter_value_params){
                                                      .source = { .kind = TA_VALUE_SOURCE_EXPR,
                                                                  .as.expr = decl->type.as.expr },
                                                      .out = &value,
                                                      .fault = &values->fault,
                                                  });
    if (status == TA_OP_OK) {
        status = ta_invoke_value_text(it, &(ta_datamodel_value_text_params){
                                              .value = &value, .out = &text, .fault = &values->fault });
    }
    ta_result result = ta_invoke_op(status);
    if (result == TA_OK) {
        ta_atom atom = 0;
        result = ta_invoke_core(ta_atom_intern(&system->atoms, text, &atom));
        *out = ta_invoke_normalize_type(system, atom);
    }
    ta_value_release(&value);
    return result;
}

/* srcexpr, inline content, and params. A literal src is not imported:
 * ta_invoke_launch passes the text the chart holds (src_text). */
static ta_result ta_invoke_resolve_values(ta_interpreter *it, const ta_invoke_decl *decl,
                                          ta_invoke_values *values) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    ta_op_status status = TA_OP_OK;
    if (decl->src.kind != TA_VALUE_SOURCE_CONTENT) {
        status = ta_interpreter_eval(it, &(ta_interpreter_value_params){
                                             .source = decl->src, .out = &values->src, .fault = &values->fault });
    }
    if (status == TA_OP_OK && decl->data.kind == TA_VALUE_SOURCE_CONTENT) {
        /*
         * Literal content is not imported: core owns no invoker, so it has no
         * semantics to impose, and the invoker receives the bytes and their
         * media type (ADR 0053, SCXML 6.4.2). ta_invoke_start reads them from
         * the definition.
         */
    } else if (status == TA_OP_OK) {
        status = ta_interpreter_eval(it, &(ta_interpreter_value_params){
                                             .source = decl->data, .out = &values->data, .fault = &values->fault });
    }
    /* The content expr and the params leave the session for the invoked
     * service (ADR 0097), so it gets copies of them. */
    if (status == TA_OP_OK) {
        status = ta_interpreter_copy_out(it, &values->data, &values->fault);
    }
    if (status != TA_OP_OK || decl->params.length == 0) {
        return ta_invoke_op(status);
    }
    if (v->make_params == NULL) {
        return TA_INVOKE_FAILED;
    }
    ta_named_value *named = ta_arena_alloc(&it->system->render, decl->params.length * sizeof(ta_named_value),
                                           _Alignof(ta_named_value));
    if (named == NULL) {
        ta_interpreter_render_fault(&values->fault);
        return TA_INVOKE_FAILED;
    }
    uint32_t evaluated = 0;
    for (; evaluated < decl->params.length && status == TA_OP_OK; evaluated += 1) {
        const ta_param_def *param = &definition->params[decl->params.offset + evaluated];
        named[evaluated].name = ta_atom_text(&it->system->atoms, param->name);
        status = ta_interpreter_eval(it, &(ta_interpreter_value_params){
                                             .source = param->value, .out = &named[evaluated].value,
                                             .fault = &values->fault });
    }
    if (status == TA_OP_OK) {
        status = v->make_params(definition->datamodel.self,
                                &(ta_datamodel_make_params_params){
                                    .shared = definition->datamodel_shared,
                                    .instance = it->actor->datamodel_instance,
                                    .values = named,
                                    .count = decl->params.length,
                                    .host = &it->actor->host,
                                    .out = &values->params,
                                    .fault = &values->fault,
                                });
    }
    if (status == TA_OP_OK) {
        status = ta_interpreter_copy_out(it, &values->params, &values->fault);
    }
    for (uint32_t index = 0; index < evaluated; index += 1) {
        ta_value_release(&named[index].value);
    }
    return ta_invoke_op(status);
}

TA_PRIVATE ta_result ta_invoke_restore(ta_system *system, ta_actor *actor, const ta_invoke_restore_params *params) {
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_result result = ta_invoke_grow(system, sparse);
    if (result != TA_OK) {
        return result;
    }
    const ta_invoke_decl *decl = &actor->definition->invokes[params->decl];
    const ta_invoker *invoker = ta_invoke_find_invoker(system, params->type);
    uint32_t index = sparse->invoke_count;
    sparse->invokes[index] = (ta_active_invoke){
        .id = params->id, .type = params->type, .autoforward = decl->autoforward, .active = true,
    };
    ta_invoke_metas(sparse)[index] = (ta_invoke_meta){
        .decl = params->decl, .state = decl->owner, .invoker = invoker != NULL ? *invoker : (ta_invoker){ 0 },
        .replayed = true,
    };
    sparse->invoke_count += 1u;
    return TA_OK;
}

typedef struct {
    ta_invoke_decl_id decl;
    ta_invoke_id id;
    ta_atom type;
    const ta_invoker *invoker;
    ta_invoke_values *values;
} ta_invoke_launch_params;

/* Records an evaluated invocation and asks its invoker to start it; the
 * room for the record and for an error was reserved. */
static ta_result ta_invoke_launch(ta_interpreter *it, const ta_invoke_launch_params *params) {
    ta_system *system = it->system;
    ta_actor_sparse_state *sparse = it->actor->sparse;
    const ta_invoke_decl *decl = &it->definition->invokes[params->decl];
    ta_invoke_values *values = params->values;
    /* Record first, so a child spawned during start() can attach to it. */
    uint32_t index = sparse->invoke_count;
    sparse->invokes[index] = (ta_active_invoke){
        .id = params->id, .type = params->type, .autoforward = decl->autoforward, .active = true,
    };
    ta_invoke_metas(sparse)[index] = (ta_invoke_meta){
        .decl = params->decl, .state = decl->owner, .invoker = *params->invoker, .child = 0,
    };
    sparse->invoke_count += 1u;
    ta_invoke_request request = {
        .system = system,
        .parent = it->actor->record.id,
        .invoke_id = params->id,
        .type = params->type,
        .src = values->src,
        .data = values->data,
        .params = values->params,
        .autoforward = decl->autoforward,
        .trace = ta_interpreter_trace_here(it),
        .definition = it->definition->nested != NULL ? it->definition->nested[params->decl] : 0,
    };
    if (decl->data.kind == TA_VALUE_SOURCE_CONTENT) {
        ta_interpreter_literal_content(it->definition, decl->data.as.content, &request.content_type,
                                       &request.content);
    }
    if (decl->src.kind == TA_VALUE_SOURCE_CONTENT) {
        ta_str media_type = { 0 };
        ta_bytes text = { 0 };
        ta_interpreter_literal_content(it->definition, decl->src.as.content, &media_type, &text);
        request.src_text = (ta_str){ .ptr = (const char *)text.ptr, .len = text.len };
    }
    uint64_t service = 0;
    const ta_invoker *invoker = params->invoker;
    if (ta_actor_replaying(it->actor)) {
        /* A replay crosses no border (ADR 0089 section 3): the invocation is
         * active in core, under the ID the run being replayed gave it, and
         * no invoker hears of it. */
        ta_invoke_metas(sparse)[index].replayed = true;
        return TA_OK;
    }
    ta_op_status status = invoker->v->start(invoker->self, &(ta_invoker_start_params){
                                                               .request = &request, .service_handle = &service,
                                                               .fault = &values->fault });
    int64_t found = ta_invoke_find(it->actor, params->id);
    if (status == TA_OP_OK) {
        if (found >= 0) {
            sparse->invokes[found].service_handle = service;
        }
        return TA_OK;
    }
    if (found >= 0) {
        ta_invoke_remove_at(sparse, (uint32_t)found);
    }
    /* An invoker that ran out of memory started nothing: the invocation
     * starts again when the host resumes the actor (ADR 0070). */
    return status == TA_OP_NOMEM ? TA_ERR_NOMEM : ta_interpreter_execution_error(it, &values->fault);
}

/*
 * Starts one invocation, in three parts (ADR 0070). First the room it needs:
 * the sparse state, a record, and an internal event for its error. Then
 * its ID, type, src, data and params, which may run out of memory interning
 * text or taking scratch. Last the record and the invoker's start(), which
 * cannot run out of memory. An invocation that runs out of memory starts
 * again from the beginning; what it did before is evaluation, and storing
 * the same ID through idlocation.
 */
static ta_result ta_invoke_start(ta_interpreter *it, ta_invoke_decl_id decl_id) {
    ta_system *system = it->system;
    const ta_invoke_decl *decl = &it->definition->invokes[decl_id];
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, it->actor);
    ta_result result = sparse == NULL ? TA_ERR_NOMEM : ta_invoke_grow(system, sparse);
    if (result == TA_OK) {
        result = ta_interpreter_queue_room(system, it->actor);
    }
    if (result != TA_OK) {
        return result;
    }
    uint32_t mark = ta_interpreter_queue_mark(it->actor);
    ta_invoke_values values = {0};
    ta_invoke_id id = 0;
    ta_atom type = 0;
    result = ta_invoke_resolve_id(it, decl, &values, &id);
    if (result == TA_OK) {
        result = ta_invoke_resolve_type(it, decl, &values, &type);
    }
    const ta_invoker *invoker = result == TA_OK ? ta_invoke_find_invoker(system, type) : NULL;
    if (result == TA_OK && (invoker == NULL || invoker->v == NULL || invoker->v->start == NULL)) {
        result = TA_INVOKE_FAILED;
    }
    if (result == TA_OK) {
        result = ta_invoke_resolve_values(it, decl, &values);
    }
    bool launched = false;
    if (result == TA_OK) {
        result = ta_invoke_launch(it, &(ta_invoke_launch_params){
                                          .decl = decl_id, .id = id, .type = type, .invoker = invoker,
                                          .values = &values });
        launched = result == TA_OK && ta_invoke_find(it->actor, id) >= 0;
    } else if (result == TA_INVOKE_FAILED) {
        result = ta_interpreter_execution_error(it, &values.fault);
    }
    ta_invoke_values_release(&values);
    if (result != TA_ERR_NOMEM) {
        /* The invocation has an outcome: INVOKED when it started, then the
         * error it raised when it did not (ADR 0067). */
        if (launched) {
            ta_interpreter_trace_invoke(it, TA_TRACE_INVOKED, id);
        }
        ta_interpreter_trace_raised(it, mark);
        if (decl->id.kind != TA_ID_SOURCE_LITERAL) {
            it->actor->next_send_id += 1u;
        }
    }
    return result;
}

TA_PRIVATE bool ta_invoke_has_pending(const ta_actor *actor) {
    return actor->sparse != NULL && actor->sparse->pending_invokes.count != 0;
}

TA_PRIVATE ta_result ta_invoke_start_pending(ta_interpreter *it, uint32_t *state, uint32_t *decl) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    if (sparse == NULL || sparse->pending_invokes.count == 0) {
        return TA_OK;
    }
    /* A prepared replacement materializes its bootstrap off-route. Starting
     * an invocation is externally visible, so activation releases it. */
    if (!it->actor->published && !ta_actor_replaying(it->actor)) {
        return TA_OK;
    }
    const ta_definition *definition = it->definition;
    ta_vec *pending = &sparse->pending_invokes;
    for (; *state < definition->state_count; *state += 1u, *decl = 0) {
        bool listed = false;
        for (uint32_t index = 0; index < pending->count && !listed; index += 1) {
            listed = *(ta_state_id *)ta_vec_at(pending, index) == *state;
        }
        if (!listed || !ta_actor_state_active(it->actor, *state)) {
            continue;
        }
        for (; *decl < definition->invoke_decl_count; *decl += 1u) {
            if (definition->invokes[*decl].owner != *state) {
                continue;
            }
            ta_result result = ta_invoke_start(it, *decl);
            if (result != TA_OK) {
                return result;
            }
        }
    }
    pending->count = 0;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Communication
 */

typedef struct {
    ta_actor *sender;
    ta_actor_ref destination;
    ta_event event;
    ta_trace_parent trace;
} ta_invoke_delivery;

/*
 * Offers an event to another session through the router. When the router
 * accepts nothing, the event waits in the deadline index, due now, owned by
 * the sender, so ta_system_dispatch_due() offers it again.
 */
static ta_result ta_invoke_submit(ta_system *system, const ta_invoke_delivery *delivery) {
    const ta_actor_sparse_state *sender_sparse = delivery->sender->sparse;
    if (sender_sparse != NULL && sender_sparse->has_parent &&
        delivery->destination.kind == TA_ACTOR_REF_RUNTIME &&
        delivery->destination.as.runtime == sender_sparse->parent.actor &&
        delivery->event.invoke_id == sender_sparse->parent.invoke_id) {
        ta_actor *parent = ta_actor_lookup(system, sender_sparse->parent.actor);
        if (parent == NULL || ta_invoke_find(parent, sender_sparse->parent.invoke_id) < 0) {
            /* SCXML 6.4: once the parent cancels an invocation, events the
             * child emits while running onexit are discarded. */
            return TA_OK;
        }
    }
    ta_external_event_submit_status status = TA_EXTERNAL_EVENT_ERROR;
    ta_fault fault = {0};
    if (system->external_events.v != NULL) {
        ta_external_delivery external = { .destination = delivery->destination, .event = delivery->event,
                                          .trace = delivery->trace };
        status = system->external_events.v->submit(system->external_events.self,
                                                   &(ta_external_event_submit_params){
                                                       .delivery = &external, .fault = &fault });
    }
    ta_value_release(&fault.data);
    if (status != TA_EXTERNAL_EVENT_RETRY) {
        return TA_OK;
    }
    ta_delayed_send record = {
        .owner = delivery->sender->record.id,
        .send_id = delivery->event.send_id,
        .due = ta_clock_now(&system->clock),
        .event = delivery->event.name,
        .io_type = system->scxml_io_type,
        .destination = delivery->destination,
        .submitted = true,
        .origin = delivery->event.origin.kind == TA_ORIGIN_ACTOR ? delivery->event.origin.as.actor
                                                                  : delivery->sender->record.self,
        .invoke_id = delivery->event.invoke_id,
        .trace = delivery->trace,
    };
    record.data = delivery->event.data;
    ta_value_retain(&record.data);
    /* The stored send owns its destination and origin references (ADR 0065). */
    ta_actor_ref_retain(&record.destination);
    ta_actor_ref_retain(&record.origin);
    ta_result result = ta_actor_schedule_send(system, delivery->sender, &record);
    if (result != TA_OK) {
        ta_value_release(&record.data);
        ta_actor_ref_release(&record.destination);
        ta_actor_ref_release(&record.origin);
    }
    return result;
}

TA_PRIVATE bool ta_invoke_is_active(const ta_actor *actor, ta_invoke_id invoke) {
    return ta_invoke_find(actor, invoke) >= 0;
}

TA_PRIVATE bool ta_invoke_destination(const ta_actor *actor, ta_invoke_id invoke, ta_actor_ref *out) {
    int64_t index = ta_invoke_find(actor, invoke);
    if (index < 0 || !ta_invoke_metas(actor->sparse)[index].has_child) {
        return false;
    }
    *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = ta_invoke_metas(actor->sparse)[index].child };
    return true;
}

TA_PRIVATE ta_invoke_id ta_invoke_parent_id(const ta_actor *sender, const ta_actor_ref *destination) {
    if (sender == NULL || sender->sparse == NULL || !sender->sparse->has_parent ||
        destination->kind != TA_ACTOR_REF_RUNTIME || destination->as.runtime != sender->sparse->parent.actor) {
        return TA_INVOKE_ID_NONE;
    }
    return sender->sparse->parent.invoke_id;
}

TA_PRIVATE bool ta_invoke_parent_accepts(const ta_system *system,
                              const ta_actor *sender,
                              const ta_actor_ref *destination) {
    if (sender == NULL || sender->sparse == NULL || !sender->sparse->has_parent ||
        destination->kind != TA_ACTOR_REF_RUNTIME ||
        destination->as.runtime != sender->sparse->parent.actor) {
        return true;
    }
    ta_actor *parent = ta_actor_lookup(system, sender->sparse->parent.actor);
    return parent != NULL && ta_invoke_find(parent, sender->sparse->parent.invoke_id) >= 0;
}

TA_PRIVATE void ta_invoke_attach_child(ta_system *system, const ta_actor_parent *parent, ta_actor_id child) {
    ta_actor *actor = ta_actor_lookup(system, parent->actor);
    int64_t index = actor == NULL ? -1 : ta_invoke_find(actor, parent->invoke_id);
    if (index >= 0) {
        ta_invoke_metas(actor->sparse)[index].child = child;
        ta_invoke_metas(actor->sparse)[index].has_child = true;
    }
}

static ta_result ta_invoke_forward(ta_interpreter *it, uint32_t index, const ta_event *event) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    ta_invoke_meta meta = ta_invoke_metas(sparse)[index];
    if (meta.replayed) {
        return TA_OK; /* nothing runs behind it to forward to */
    }
    if (!meta.has_child && (meta.invoker.v == NULL || meta.invoker.v->forward == NULL)) {
        return TA_OK;
    }
    /*
     * Autoforwarding is an exact envelope copy, including the invoke ID that
     * identifies the child which originally sent the event, and its data
     * leaves the session: the invocation gets a copy of it (ADR 0097). Data
     * that cannot be copied is not forwarded, and is error.execution here.
     */
    ta_event copy = *event;
    ta_value_retain(&copy.data);
    ta_fault fault = {0};
    ta_op_status copied = ta_interpreter_copy_out(it, &copy.data, &fault);
    if (copied != TA_OP_OK) {
        ta_value_release(&copy.data);
        if (copied == TA_OP_NOMEM) {
            ta_value_release(&fault.data);
            return TA_ERR_NOMEM;
        }
        return ta_interpreter_execution_error(it, &fault);
    }
    ta_result result = TA_OK;
    if (meta.has_child) {
        result = ta_invoke_submit(it->system, &(ta_invoke_delivery){
                                                  .sender = it->actor,
                                                  .destination = ta_actor_runtime_ref(it->system, meta.child),
                                                  .event = copy,
                                                  .trace = ta_interpreter_trace_here(it),
                                              });
    } else {
        (void)meta.invoker.v->forward(meta.invoker.self, &(ta_invoker_forward_params){
                                                              .service_handle = sparse->invokes[index].service_handle,
                                                              .event = &copy,
                                                              .fault = &fault,
                                                          });
        ta_value_release(&fault.data);
    }
    if (result == TA_OK) {
        /* Written once it is committed: a submit that fails runs again (ADR 0070). */
        ta_interpreter_trace_forward(it, sparse->invokes[index].id, &copy);
    }
    ta_value_release(&copy.data);
    return result;
}

/* The active invocation an event came from, or -1. */
static int64_t ta_invoke_source(const ta_actor *actor, const ta_event *event) {
    return event->invoke_id == TA_INVOKE_ID_NONE ? -1 : ta_invoke_find(actor, event->invoke_id);
}

TA_PRIVATE bool ta_invoke_before_external(const ta_actor *actor, const ta_event *event) {
    /*
     * An event of an invocation cancelled since it was queued is still
     * processed, without <finalize> or autoforwarding: cancelling destroys
     * the service, so nothing it sends afterwards arrives, and stale host
     * reports are refused by ta_actor_report_invoke().
     */
    if (event->invoke_id != TA_INVOKE_ID_NONE && ta_invoke_source(actor, event) < 0) {
        return false;
    }
    const ta_actor_sparse_state *sparse = actor->sparse;
    ta_span32 finalize = {0};
    if (ta_invoke_finalize_actions(actor, event, &finalize) && finalize.length != 0) {
        return true;
    }
    for (uint32_t index = 0; sparse != NULL && index < sparse->invoke_count; index += 1) {
        if (sparse->invokes[index].autoforward) {
            return true;
        }
    }
    return false;
}

TA_PRIVATE bool ta_invoke_finalize_actions(const ta_actor *actor, const ta_event *event, ta_span32 *out) {
    int64_t index = ta_invoke_source(actor, event);
    if (index < 0) {
        return false;
    }
    *out = actor->definition->invokes[ta_invoke_metas(actor->sparse)[index].decl].finalize_actions;
    return true;
}

TA_PRIVATE ta_result ta_invoke_autoforward(ta_interpreter *it, const ta_event *event, uint32_t *cursor) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    for (; sparse != NULL && *cursor < sparse->invoke_count; *cursor += 1u) {
        if (sparse->invokes[*cursor].autoforward) {
            ta_result result = ta_invoke_forward(it, *cursor, event);
            if (result != TA_OK) {
                return result;
            }
        }
    }
    return TA_OK;
}

/* Sends done.invoke.<id> with the completion data to the invoking parent,
 * under `trace`, the finishing step's, or none when the host publishes it
 * again after a refusal. */
static ta_result ta_invoke_publish(ta_system *system, ta_actor *actor, ta_trace_parent trace) {
    ta_actor_sparse_state *sparse = actor->sparse;
    ta_event_name_id name = 0;
    ta_result result = ta_invoke_done_name(system, sparse->parent.invoke_id, &name);
    if (result != TA_OK) {
        return result;
    }
    ta_event event = {
        .name = name,
        .type = TA_EVENT_EXTERNAL,
        .send_id = TA_SEND_ID_COMPLETION,
        .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = actor->record.self },
        .invoke_id = sparse->parent.invoke_id,
        .data = sparse->completion.has_data ? sparse->completion.data : (ta_value){0},
    };
    return ta_invoke_submit(system, &(ta_invoke_delivery){
                                        .sender = actor,
                                        .destination = ta_actor_runtime_ref(system, sparse->parent.actor),
                                        .event = event,
                                        .trace = trace,
                                    });
}

TA_PRIVATE ta_result ta_invoke_finished(ta_interpreter *it) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    if (sparse == NULL) {
        return TA_OK;
    }
    while (sparse->invoke_count != 0) {
        ta_interpreter_trace_invoke(it, TA_TRACE_INVOKE_CANCELLED, sparse->invokes[sparse->invoke_count - 1u].id);
        ta_invoke_cancel_at(it->system, sparse, sparse->invoke_count - 1u);
    }
    sparse->pending_invokes.count = 0;
    if (!sparse->has_parent) {
        return TA_OK;
    }
    sparse->completion_pending = true;
    return ta_invoke_publish(it->system, it->actor, ta_interpreter_trace_here(it));
}

TA_PRIVATE void ta_invoke_release(ta_system *system, ta_actor *actor) {
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL) {
        return;
    }
    while (sparse->invoke_count != 0) {
        ta_invoke_cancel_at(system, sparse, sparse->invoke_count - 1u);
    }
    ta_columns_release(system->allocator, &ta_invoke_columns, &sparse->invoke_table);
    if (sparse->pending_invokes.element_size != 0) {
        ta_vec_destroy(&sparse->pending_invokes);
    }
}

/* ------------------------------------------------------------------------
 * Host reports
 */

/* error.communication.invoke.<id>.<reason> (ADR 0081) */
static ta_result ta_invoke_error_name(ta_system *system, ta_invoke_id invoke, ta_str reason, ta_event_name_id *out) {
    ta_event_name_id name = 0;
    ta_atom invoke_atom = 0;
    ta_atom reason_atom = 0;
    ta_result result = ta_atom_intern(&system->atoms, (ta_str){ .ptr = "invoke", .len = 6 }, &invoke_atom);
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, reason, &reason_atom);
    }
    if (result == TA_OK) {
        result = ta_event_name_child(&system->event_names, TA_EVENT_NAME_ERROR_COMMUNICATION, invoke_atom, &name);
    }
    ta_str text = ta_invoke_id_text(system, invoke);
    ta_atom id_atom = 0;
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, text, &id_atom);
    }
    if (result == TA_OK) {
        result = ta_event_name_child(&system->event_names, name, id_atom, &name);
    }
    if (result == TA_OK) {
        result = ta_event_name_child(&system->event_names, name, reason_atom, &name);
    }
    *out = name;
    return result;
}

/* One event-name token: not empty, with no '.' and no white space. */
static bool ta_invoke_reason_valid(ta_str reason) {
    if (reason.ptr == NULL || reason.len == 0) {
        return false;
    }
    for (size_t index = 0; index < reason.len; index += 1) {
        char c = reason.ptr[index];
        if (c == '.' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            return false;
        }
    }
    return true;
}

/*
 * Appends error.communication.invoke.<id>.<reason> to the actor's internal
 * queue, with a retained copy of `data` (or none). Queued outside a run, like
 * a failed delayed send's error: counted and recorded here (ADR 0072), under
 * `trace` when there is one. On failure nothing is queued.
 */
static ta_result ta_invoke_raise(ta_system *system, ta_actor *actor, ta_invoke_id invoke, ta_str reason,
                                 const ta_value *data, const ta_trace_parent *trace) {
    ta_event_name_id name = 0;
    ta_result result = ta_invoke_error_name(system, invoke, reason, &name);
    if (result != TA_OK) {
        return result;
    }
    ta_event event = { .name = name, .type = TA_EVENT_PLATFORM, .invoke_id = TA_INVOKE_ID_NONE };
    if (data != NULL) {
        event.data = *data;
        ta_value_retain(&event.data);
    }
    result = ta_interpreter_enqueue_internal(system, actor, &event);
    if (result == TA_OK) {
        ta_interpreter_raised_outside(system, actor, name, trace);
    }
    return result;
}

/*
 * Reconstruction lost the service behind an invocation: it is retired and
 * reported once, with no fault data, because nothing failed — the invocation
 * simply did not survive the image (ADR 0038). The image carries no request
 * to name, so the event has no trace.
 */
TA_PRIVATE ta_result ta_invoke_report_lost(ta_system *system, ta_actor *actor, ta_invoke_id invoke) {
    return ta_invoke_raise(system, actor, invoke, (ta_str){ .ptr = "unavailable", .len = 11 }, NULL, NULL);
}

TA_PRIVATE ta_result ta_invoke_prepare_retire(ta_system *system, ta_actor *actor) {
    ta_actor_sparse_state *sparse = actor->sparse;
    uint32_t count = sparse == NULL ? 0 : sparse->invoke_count;
    if (count == 0) {
        return TA_OK;
    }
    /* Interning is idempotent, so naming each event now leaves the queue
     * work alone as what can fail later. */
    for (uint32_t index = 0; index < count; index += 1u) {
        ta_event_name_id name = 0;
        ta_result result = ta_invoke_error_name(system, sparse->invokes[index].id,
                                                (ta_str){ .ptr = "unavailable", .len = 11 }, &name);
        if (result != TA_OK) {
            return result;
        }
    }
    return ta_interpreter_queue_reserve(system, actor, count);
}

TA_PRIVATE bool ta_invoke_retire_all(ta_system *system, ta_actor *actor) {
    ta_actor_sparse_state *sparse = actor->sparse;
    uint32_t count = sparse == NULL ? 0 : sparse->invoke_count;
    for (uint32_t index = 0; index < count; index += 1u) {
        /* Named and reserved by ta_invoke_prepare_retire(), so this cannot fail. */
        (void)ta_invoke_report_lost(system, actor, sparse->invokes[index].id);
    }
    if (sparse != NULL) {
        sparse->invoke_count = 0;
    }
    return count != 0;
}

/*
 * An ERROR report (ADR 0081). The event joins the internal queue whether
 * the session is idle or inside a macrostep, so it never answers BUSY; an
 * idle session starts a macrostep. With `ends`, the invocation is retired
 * only once the event is queued, so a failure changes nothing.
 */
static ta_result ta_invoke_report_error(ta_system *system, ta_actor *actor, uint32_t index,
                                        const ta_invoke_report_params *params) {
    const ta_invoke_error *error = params->as.error;
    if (error == NULL || !ta_invoke_reason_valid(error->reason)) {
        return TA_ERR_INVALID;
    }
    /* An input reaches a session only between macrosteps (ADR 0089
     * section 4), so the host keeps the report, as it keeps mail that
     * ta_actor_deliver() refused, and a log can say where it fell. */
    if (actor->macrostep || ta_actor_external_event(actor) != NULL) {
        return TA_ERR_BUSY;
    }
    ta_result result = ta_invoke_raise(system, actor, params->invoke_id, error->reason, error->data, &params->trace);
    if (result != TA_OK) {
        return result;
    }
    if (error->ends) {
        ta_invoke_remove_at(actor->sparse, index);
    }
    ta_interpreter_start_macrostep(actor, &params->trace);
    return TA_OK;
}

TA_PRIVATE ta_origin ta_invoke_origin_of(const ta_system *system, const ta_active_invoke *invoke) {
    return (ta_origin){ .kind = TA_ORIGIN_INVOKE,
                        .as.invoke = { .invoke = invoke->id,
                                       .invoker = ta_system_invoker_index(system, invoke->type) } };
}

TA_PRIVATE bool ta_invoke_origin_for(const ta_system *system, const ta_actor *actor, ta_invoke_id invoke, ta_origin *out) {
    int64_t index = ta_invoke_find(actor, invoke);
    if (index < 0) {
        return false;
    }
    *out = ta_invoke_origin_of(system, &actor->sparse->invokes[index]);
    return true;
}

/* An ERROR report; its time becomes the actor's when it is taken (ADR 0094). */
static ta_result ta_invoke_error_report(ta_system *system, ta_actor *actor, const ta_invoke_report_params *params) {
    uint32_t index = (uint32_t)ta_invoke_find(actor, params->invoke_id);
    ta_result result = ta_invoke_report_error(system, actor, index, params);
    if (result == TA_OK) {
        ta_actor_move_clock(actor, params->time); /* the report's time is now the actor's (ADR 0094) */
    }
    return result;
}

TA_API ta_result ta_actor_report_invoke(ta_system *system, const ta_invoke_report_params *params) {
    if (system == NULL || params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    int64_t index = ta_invoke_find(actor, params->invoke_id);
    if (index < 0) {
        return TA_ERR_NOT_FOUND;
    }
    if (!ta_actor_input_time_valid(system, actor, params->time)) {
        return TA_ERR_INVALID;
    }
    ta_event event = { .type = TA_EVENT_EXTERNAL, .invoke_id = params->invoke_id };
    switch (params->kind) {
        case TA_INVOKE_REPORT_EVENT:
            if (params->as.event == NULL) {
                return TA_ERR_INVALID;
            }
            event = *params->as.event;
            event.type = TA_EVENT_EXTERNAL;
            event.invoke_id = params->invoke_id;
            /* Core fills the origin of a service's own event, whatever the host passed (ADR 0068). */
            event.origin = ta_invoke_origin_of(system, &actor->sparse->invokes[index]);
            return ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = params->actor, .event = &event,
                                                                  .trace = params->trace, .time = params->time });
        case TA_INVOKE_REPORT_DONE: {
            ta_result result = ta_invoke_done_name(system, params->invoke_id, &event.name);
            if (result != TA_OK) {
                return result;
            }
            if (params->as.done_data != NULL) {
                event.data = *params->as.done_data;
            }
            event.origin = ta_invoke_origin_of(system, &actor->sparse->invokes[index]);
            return ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = params->actor, .event = &event,
                                                                  .trace = params->trace, .time = params->time });
        }
        case TA_INVOKE_REPORT_ERROR:
            return ta_invoke_error_report(system, actor, params);
        default:
            return TA_ERR_INVALID;
    }
}

TA_API ta_result ta_actor_publish_completion(ta_system *system, ta_actor_id id) {
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_actor_sparse_state *sparse = actor->sparse;
    if (!actor->finished || sparse == NULL || !sparse->has_parent) {
        return TA_ERR_INVALID;
    }
    ta_actor *parent = ta_actor_lookup(system, sparse->parent.actor);
    if (parent == NULL || ta_invoke_find(parent, sparse->parent.invoke_id) < 0) {
        (void)ta_timer_withdraw(&system->timers, ta_actor_timers(actor), TA_SEND_ID_COMPLETION);
        return TA_ERR_NOT_FOUND;
    }
    if (ta_timer_withdraw(&system->timers, ta_actor_timers(actor), TA_SEND_ID_COMPLETION) != TA_OK) {
        return TA_OK; /* already delivered */
    }
    return ta_invoke_publish(system, actor, (ta_trace_parent){0});
}

/* ------------------------------------------------------------------------
 * Nested definitions (ADR 0077)
 */

TA_API ta_result ta_definition_invoke(const ta_system *system, const ta_definition_invoke_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL || params->invoke >= definition->invoke_decl_count) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_invoke_decl *decl = &definition->invokes[params->invoke];
    ta_invoke_info info = {
        .content = decl->data.kind == TA_VALUE_SOURCE_CONTENT ? decl->data.as.content : TA_CONTENT_ID_NONE,
        .src = decl->src.kind == TA_VALUE_SOURCE_CONTENT ? decl->src.as.content : TA_CONTENT_ID_NONE,
        .nested = definition->nested != NULL ? definition->nested[params->invoke] : 0,
    };
    if (decl->type.kind == TA_TEXT_SOURCE_EXPR) {
        info.has_type_expr = true;
    } else {
        ta_atom type = decl->type.kind == TA_TEXT_SOURCE_ATOM ? ta_invoke_normalize_type(system, decl->type.as.atom)
                                                              : system->scxml_invoke_type;
        info.type = ta_atom_text(&system->atoms, type);
    }
    *params->out = info;
    return TA_OK;
}

TA_API ta_result ta_definition_nest(ta_system *system, const ta_definition_nest_params *params) {
    if (system == NULL || params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_definition *parent = ta_system_definition(system, params->parent);
    ta_definition *child = ta_system_definition(system, params->child);
    if (parent == NULL || child == NULL || parent == child || parent->retired || child->retired ||
        params->invoke >= parent->invoke_decl_count ||
        (parent->nested != NULL && parent->nested[params->invoke] != 0)) {
        return TA_ERR_INVALID;
    }
    if (parent->nested == NULL) {
        size_t bytes = (size_t)parent->invoke_decl_count * sizeof(ta_definition_id);
        ta_definition_id *nested = ta_arena_alloc(&parent->arena, bytes, _Alignof(ta_definition_id));
        if (nested == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_zero(nested, bytes);
        parent->nested = nested;
    }
    parent->nested[params->invoke] = child->id;
    child->holders += 1u;
    return TA_OK;
}


TA_API void ta_definition_destroy(ta_definition *definition) {
    if (definition == NULL) {
        return;
    }
    if (!definition->registered && definition->system != NULL) {
        ta_system *system = definition->system;
        if (definition->unregistered_previous != NULL) {
            definition->unregistered_previous->unregistered_next = definition->unregistered_next;
        } else if (system->unregistered_definitions == definition) {
            system->unregistered_definitions = definition->unregistered_next;
        }
        if (definition->unregistered_next != NULL) {
            definition->unregistered_next->unregistered_previous = definition->unregistered_previous;
        }
    }
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (definition->datamodel_prepared) {
        v->destroy_definition(definition->datamodel.self, definition->datamodel_shared,
                              definition->allocator);
    }
    for (uint32_t index = 0; index < definition->expression_count; index += 1) {
        ta_expression *expression = &definition->expressions[index];
        if (expression->kind == TA_EXPRESSION_HOST && expression->as.host.v->release != NULL) {
            expression->as.host.v->release(expression->as.host.self, expression->as.host.value,
                                           definition->allocator);
        }
    }
    if (definition->spans != NULL) {
        ta_mem_free(definition->span_allocator, definition->spans,
                    &(ta_allocator_free_params){ .size = definition->span_bytes,
                                                 .alignment = _Alignof(ta_source_span) });
    }
    ta_allocator allocator = definition->allocator;
    ta_arena_destroy(&definition->arena);
    ta_mem_free(allocator, definition,
                &(ta_allocator_free_params){ .size = sizeof(ta_definition),
                                             .alignment = _Alignof(ta_definition) });
}

TA_PRIVATE const ta_definition *ta_definition_lookup(const ta_system *system, ta_definition_id id) {
    return ta_system_definition(system, id);
}

TA_PRIVATE bool ta_definition_is_atomic(const ta_definition *definition, ta_state_id state) {
    ta_state_kind kind = definition->states[state].kind;
    return kind == TA_STATE_ATOMIC || kind == TA_STATE_FINAL;
}

TA_PRIVATE bool ta_definition_is_descendant(const ta_definition *definition, ta_state_id state, ta_state_id ancestor) {
    if (ancestor == TA_STATE_NONE) {
        return state != TA_STATE_NONE;
    }
    for (ta_state_id at = definition->states[state].parent; at != TA_STATE_NONE;
         at = definition->states[at].parent) {
        if (at == ancestor) {
            return true;
        }
    }
    return false;
}

/*
 * Matches an event against one descriptor (ADR 0060 section 5). A `*`
 * segment matches zero or more event segments, anywhere in the descriptor;
 * a descriptor used up before the event is a prefix match. Two pointers,
 * no recursion: on a mismatch the last `*` passed takes one more event
 * segment and matching resumes after it. O(event x descriptor) time, O(1)
 * space.
 */
typedef struct {
    const ta_atom *event;
    uint32_t event_count;
    const ta_event_pattern_segment *pattern;
    uint32_t pattern_count;
} ta_pattern_match;

static bool ta_pattern_match_run(const ta_pattern_match *match) {
    uint32_t event = 0;
    uint32_t pattern = 0;
    bool starred = false;
    uint32_t star_pattern = 0;
    uint32_t star_event = 0;
    while (pattern < match->pattern_count) {
        const ta_event_pattern_segment *segment = &match->pattern[pattern];
        if (segment->flags & TA_EVENT_PATTERN_SEGMENT_WILDCARD) {
            starred = true;
            star_pattern = pattern;
            star_event = event;
            pattern += 1u;
        } else if (event < match->event_count && match->event[event] == segment->atom) {
            event += 1u;
            pattern += 1u;
        } else if (starred && star_event < match->event_count) {
            star_event += 1u;
            event = star_event;
            pattern = star_pattern + 1u;
        } else {
            return false;
        }
    }
    return true;
}

TA_PRIVATE bool ta_event_pattern_matches(const ta_definition *definition,
                              const ta_event_pattern *pattern,
                              ta_event_name_id event) {
    ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
    uint32_t count = ta_event_name_segments(&definition->system->event_names, event, segments);
    if (count == 0) {
        return false;
    }
    ta_pattern_match match = {
        .event = segments,
        .event_count = count,
        .pattern = definition->event_pattern_segments + pattern->segments.offset,
        .pattern_count = pattern->segments.length,
    };
    return ta_pattern_match_run(&match);
}

TA_API ta_result ta_definition_state(const ta_system *system, const ta_definition_state_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (params->state >= definition->state_count) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_state_def *state = &definition->states[params->state];
    *params->out = (ta_state_info){
        .id = ta_atom_text(&system->atoms, state->name),
        .parent = state->parent,
        .kind = state->kind,
    };
    return TA_OK;
}

TA_API ta_result ta_definition_content(const ta_system *system, const ta_definition_content_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL || params->content >= definition->content_count) {
        return TA_ERR_NOT_FOUND;
    }
    ta_interpreter_literal_content(definition, params->content, &params->out->media_type, &params->out->bytes);
    return TA_OK;
}

TA_API ta_result ta_definition_find_state(const ta_system *system, const ta_definition_find_state_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_atom name = 0;
    if (ta_atom_find(&system->atoms, params->id, &name) != TA_OK) {
        return TA_ERR_NOT_FOUND;
    }
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (definition->states[state].name == name) {
            *params->out = state;
            return TA_OK;
        }
    }
    return TA_ERR_NOT_FOUND;
}

/* Places the actions of one block. */
static void ta_place_block(ta_definition *definition, ta_span32 block, ta_action_place place) {
    for (uint32_t index = 0; index < block.length; index += 1u) {
        place.index = index;
        definition->action_places[block.offset + index] = place;
    }
}

TA_PRIVATE ta_result ta_definition_place_actions(ta_definition *definition) {
    definition->action_places = NULL;
    if (definition->action_count == 0) {
        return TA_OK;
    }
    definition->action_places = ta_arena_alloc(&definition->arena,
                                               (size_t)definition->action_count * sizeof(ta_action_place),
                                               _Alignof(ta_action_place));
    if (definition->action_places == NULL) {
        return TA_ERR_NOMEM;
    }
    /* The top-level blocks: handlers, transitions, <finalize>, the <script>. */
    for (ta_state_id state = 0; state < definition->state_count; state += 1u) {
        const ta_state_def *def = &definition->states[state];
        for (uint32_t handler = 0; handler < def->onentry_handlers.length; handler += 1u) {
            ta_place_block(definition, definition->handlers[def->onentry_handlers.offset + handler],
                           (ta_action_place){ .block = TA_ACTION_IN_ONENTRY, .owner = state, .block_index = handler });
        }
        for (uint32_t handler = 0; handler < def->onexit_handlers.length; handler += 1u) {
            ta_place_block(definition, definition->handlers[def->onexit_handlers.offset + handler],
                           (ta_action_place){ .block = TA_ACTION_IN_ONEXIT, .owner = state, .block_index = handler });
        }
    }
    for (ta_transition_id transition = 0; transition < definition->transition_count; transition += 1u) {
        ta_place_block(definition, definition->transitions[transition].actions,
                       (ta_action_place){ .block = TA_ACTION_IN_TRANSITION, .owner = transition });
    }
    for (uint32_t initial = 0; initial < definition->initial_transition_count; initial += 1u) {
        ta_place_block(definition, definition->initial_transitions[initial].actions,
                       (ta_action_place){ .block = TA_ACTION_IN_TRANSITION,
                                          .owner = definition->transition_count + initial });
    }
    for (uint32_t invoke = 0; invoke < definition->invoke_decl_count; invoke += 1u) {
        /* Which <invoke> of its state: the declarations of a state are in document order. */
        uint32_t nth = 0;
        for (uint32_t earlier = 0; earlier < invoke; earlier += 1u) {
            nth += definition->invokes[earlier].owner == definition->invokes[invoke].owner ? 1u : 0u;
        }
        ta_place_block(definition, definition->invokes[invoke].finalize_actions,
                       (ta_action_place){ .block = TA_ACTION_IN_FINALIZE, .owner = definition->invokes[invoke].owner,
                                          .block_index = nth });
    }
    ta_place_block(definition, definition->global_script_actions,
                   (ta_action_place){ .block = TA_ACTION_IN_SCRIPT, .owner = TA_STATE_NONE });
    /* The blocks nested in <if> and <foreach>, whose owner is an action. */
    for (ta_action_id action = 0; action < definition->action_count; action += 1u) {
        const ta_action_def *def = &definition->action_defs[action];
        if (def->kind == TA_ACTION_IF) {
            ta_place_block(definition, def->as.conditional.then_actions,
                           (ta_action_place){ .block = TA_ACTION_IN_THEN, .owner = action });
            ta_place_block(definition, def->as.conditional.else_actions,
                           (ta_action_place){ .block = TA_ACTION_IN_ELSE, .owner = action });
        } else if (def->kind == TA_ACTION_FOREACH) {
            ta_place_block(definition, def->as.foreach.body_actions,
                           (ta_action_place){ .block = TA_ACTION_IN_FOREACH, .owner = action });
        }
    }
    return TA_OK;
}

TA_API ta_result ta_definition_transition(const ta_system *system, const ta_definition_transition_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_transition_id id = params->transition;
    if (id < definition->transition_count) {
        const ta_transition_def *def = &definition->transitions[id];
        *params->out = (ta_transition_info){
            .origin = TA_TRANSITION_AUTHORED,
            .source = def->source,
            .type = def->type == TA_TRANSITION_INTERNAL ? TA_SCXML_TRANSITION_INTERNAL : TA_SCXML_TRANSITION_EXTERNAL,
            .event = def->event_text == TA_AUTHORED_TEXT_NONE ? (ta_str){ .ptr = "", .len = 0 }
                                                                : definition->authored_texts[def->event_text],
            .targets = def->targets.length == 0 ? NULL : definition->targets + def->targets.offset,
            .target_count = def->targets.length,
            .condition = def->condition,
            .action_count = def->actions.length,
            .document_order = def->document_order,
        };
        return TA_OK;
    }
    /* After the authored transitions come the initial ones (see the header). */
    uint32_t initial = id - definition->transition_count;
    if (initial >= definition->initial_transition_count) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_initial_transition_def *def = &definition->initial_transitions[initial];
    bool bootstrap = initial == definition->root_initial;
    *params->out = (ta_transition_info){
        .origin = bootstrap ? TA_TRANSITION_BOOTSTRAP : TA_TRANSITION_INITIAL,
        .source = bootstrap ? TA_STATE_NONE : def->owner,
        .type = TA_SCXML_TRANSITION_EXTERNAL,
        .event = { .ptr = "", .len = 0 },
        .targets = def->targets.length == 0 ? NULL : definition->targets + def->targets.offset,
        .target_count = def->targets.length,
        .default_targets = !def->authored_targets,
        .condition = TA_EXPR_ID_NONE,
        .action_count = def->actions.length,
    };
    return TA_OK;
}

static ta_scxml_action_kind ta_public_action_kind(ta_action_kind kind) {
    switch (kind) {
        case TA_ACTION_RAISE: return TA_SCXML_ACTION_RAISE;
        case TA_ACTION_SEND: return TA_SCXML_ACTION_SEND;
        case TA_ACTION_CANCEL: return TA_SCXML_ACTION_CANCEL;
        case TA_ACTION_ASSIGN: return TA_SCXML_ACTION_ASSIGN;
        case TA_ACTION_SCRIPT: return TA_SCXML_ACTION_SCRIPT;
        case TA_ACTION_LOG: return TA_SCXML_ACTION_LOG;
        case TA_ACTION_IF: return TA_SCXML_ACTION_IF;
        case TA_ACTION_FOREACH: return TA_SCXML_ACTION_FOREACH;
        case TA_ACTION_INVOKE: return TA_SCXML_ACTION_INVOKE;
    }
    return TA_SCXML_ACTION_NONE;
}

TA_API ta_result ta_definition_action(const ta_system *system, const ta_definition_action_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL || params->action >= definition->action_count) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_action_place *place = &definition->action_places[params->action];
    *params->out = (ta_action_info){
        .kind = ta_public_action_kind(definition->action_defs[params->action].kind),
        .block = place->block,
        .owner = place->owner,
        .block_index = place->block_index,
        .index = place->index,
    };
    return TA_OK;
}

TA_API ta_result ta_definition_expression(const ta_system *system, const ta_definition_expression_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL || params->expression >= definition->expression_count) {
        return TA_ERR_NOT_FOUND;
    }
    *params->out = definition->expressions[params->expression];
    return TA_OK;
}

TA_PRIVATE uint32_t ta_definition_element_count(const ta_definition *definition, ta_element_kind kind) {
    switch (kind) {
        case TA_ELEMENT_STATE: return definition->state_count;
        case TA_ELEMENT_TRANSITION: return definition->transition_count + definition->initial_transition_count;
        case TA_ELEMENT_ACTION: return definition->action_count;
        case TA_ELEMENT_EXPRESSION: return definition->expression_count;
    }
    return 0;
}

TA_PRIVATE uint32_t ta_definition_span_base(const ta_definition *definition, ta_element_kind kind) {
    uint32_t base = 0;
    for (uint32_t before = TA_ELEMENT_STATE; before < (uint32_t)kind; before += 1u) {
        base += ta_definition_element_count(definition, (ta_element_kind)before);
    }
    return base;
}

TA_API ta_result ta_definition_span(const ta_system *system, const ta_definition_span_params *params) {
    if (system == NULL || params == NULL || params->out == NULL || (uint32_t)params->kind > TA_ELEMENT_EXPRESSION) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL || definition->spans == NULL ||
        params->id >= ta_definition_element_count(definition, params->kind)) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_source_span *span = &definition->spans[ta_definition_span_base(definition, params->kind) + params->id];
    if (span->line == 0) {
        return TA_ERR_NOT_FOUND;
    }
    ta_str source = { .ptr = "", .len = 0 };
    if (span->source < definition->source_count) {
        source = definition->sources[span->source].name;
    }
    *params->out = (ta_source_location){ .span = *span, .source = source };
    return TA_OK;
}

/* Whether two definitions' span tables say the same, names included. */
static bool ta_definition_same_spans(const ta_definition *left, const ta_definition *right) {
    if (left->spans == NULL || left->span_count != right->span_count || left->source_count != right->source_count) {
        return false;
    }
    for (uint32_t index = 0; index < left->span_count; index += 1u) {
        const ta_source_span *a = &left->spans[index];
        const ta_source_span *b = &right->spans[index];
        if (a->offset != b->offset || a->length != b->length || a->source != b->source || a->line != b->line ||
            a->column != b->column || a->end_line != b->end_line || a->end_column != b->end_column) {
            return false;
        }
    }
    for (uint32_t index = 0; index < left->source_count; index += 1u) {
        if (!ta_str_equal(left->sources[index].name, right->sources[index].name)) {
            return false;
        }
    }
    return true;
}

TA_PRIVATE void ta_definition_take_spans(ta_definition *live, ta_definition *definition) {
    /* Unchanged spans stay where they are, so what a caller borrowed of
     * them (an export, a source's name) stays valid. */
    if (definition->spans == NULL || ta_definition_same_spans(live, definition)) {
        return;
    }
    ta_source_span *spans = live->spans;
    uint32_t span_count = live->span_count;
    ta_scxml_source *sources = live->sources;
    uint32_t source_count = live->source_count;
    size_t span_bytes = live->span_bytes;
    ta_allocator span_allocator = live->span_allocator;
    live->spans = definition->spans;
    live->span_count = definition->span_count;
    live->sources = definition->sources;
    live->source_count = definition->source_count;
    live->span_bytes = definition->span_bytes;
    live->span_allocator = definition->span_allocator;
    definition->spans = spans;
    definition->span_count = span_count;
    definition->sources = sources;
    definition->source_count = source_count;
    definition->span_bytes = span_bytes;
    definition->span_allocator = span_allocator;
}

TA_PRIVATE ta_result ta_definition_session_bytes(const ta_definition *definition, size_t *bytes) {
    /* The configuration, the late-binding bits (sized like it), and history. */
    size_t total;
    if (!ta_checked_mul((size_t)definition->configuration.bytes, 2u, &total) ||
        !ta_checked_add(total, (size_t)definition->history.storage_bytes, &total)) {
        return TA_ERR_NOMEM;
    }
    *bytes = total;
    return TA_OK;
}

TA_API ta_result ta_definition_inspect(const ta_system *system,
                                ta_definition_id id,
                                ta_definition_inspection *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, id);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_str document_name = { .ptr = "", .len = 0 };
    if (definition->document_name != TA_ATOM_NONE) {
        document_name = ta_atom_text(&system->atoms, definition->document_name);
    }
    *out = (ta_definition_inspection){
        .id = definition->id,
        .identity = definition->identity,
        .document_name = document_name,
        .datamodel = definition->datamodel,
        .binding = definition->binding_mode == TA_BINDING_LATE ? TA_SCXML_BINDING_LATE : TA_SCXML_BINDING_EARLY,
        .state_count = definition->state_count,
        .transition_count = definition->transition_count + definition->initial_transition_count,
        .action_count = definition->action_count,
        .expression_count = definition->expression_count,
        .content_count = definition->content_count,
        .invoke_count = definition->invoke_decl_count,
        .retired = definition->retired,
        .actor_count = definition->actor_count,
        .holds = definition->holds,
        .holders = definition->holders,
        .actor_fixed_bytes = definition->actor_block_bytes,
        .definition_bytes = ta_arena_allocated_bytes(&definition->arena) + definition->span_bytes,
    };
    return TA_OK;
}


/* A state by its interned name, for the name index built after pass one. */
typedef struct {
    ta_atom name;
    ta_state_id state;
} ta_state_key;

/*
 * A known span the spec gives an element (ADR 0085), recorded during the
 * walk and laid out once the counts are final. An initial transition's ID
 * is its index among the initial transitions until then.
 */
typedef struct {
    ta_element_kind kind;
    bool initial;
    uint32_t id;
    /* The spec's span, from 1. */
    uint32_t span;
} ta_span_record;

/*
 * The compiler's explicit stacks (ADR 0060: no recursion in core).
 *
 * A level of pass one: one list of sibling states, the parent they belong
 * to, and the next one to collect. `current` is the state whose children
 * the level above is collecting.
 */
typedef struct {
    const ta_scxml_state_spec *specs;
    ta_state_id parent;
    ta_state_id current;
    uint32_t count;
    uint32_t base;
    uint32_t index;
} ta_collect_frame;

/*
 * A block of executable content being compiled. `action` is the action at
 * `index` while its nested blocks are compiled; `stage` says which one:
 * an <if>'s then block, then its else block, or a <foreach>'s body.
 */
typedef enum {
    TA_BLOCK_NEXT = 0,
    TA_BLOCK_THEN,
    TA_BLOCK_ELSE,
    TA_BLOCK_BODY
} ta_block_stage;

typedef struct {
    const ta_scxml_action_spec *specs;
    ta_spec_member member;
    ta_block_stage stage;
    uint32_t count;
    uint32_t base;
    uint32_t index;
    ta_action_def action;
} ta_block_frame;

/*
 * At or below this many states the name index is scanned; above it, it is
 * binary-searched. The index is sorted either way, so both give the same
 * answer.
 */
#define TA_COMPILER_LINEAR_NAMES 16u

/*
 * Compiles a ta_scxml_spec in two passes. Pass one assigns state IDs in
 * document (pre-)order and records child ranges; pass two resolves targets and
 * compiles transitions, initial transitions, executable content, and data.
 * Every table is built in a growable vector and finally copied into the
 * definition's arena, which owns all compiled storage.
 *
 * Errors (ADR 0058). The compiler keeps the path to the element it is working
 * on: the state it is in (`path_state`, whose own path from the root follows
 * from the state table) and a stack of steps below it. Descending pushes a
 * step and returning pops it; nothing else happens on success. The first
 * fault stops the compile and is recorded once, with the path copied into the
 * caller's error arena.
 */
typedef struct {
    ta_system *system;
    ta_allocator allocator;
    const ta_scxml_spec *spec;
    ta_definition *definition;
    ta_vec states;       /* ta_state_def */
    ta_vec state_specs;  /* const ta_scxml_state_spec * */
    ta_vec children;     /* ta_state_id */
    ta_vec transitions;  /* ta_transition_def */
    ta_vec initials;     /* ta_initial_transition_def */
    ta_vec targets;      /* ta_state_id */
    ta_vec actions;      /* ta_action_def */
    ta_vec handlers;     /* ta_span32: one action block per handler */
    ta_vec params;       /* ta_param_def */
    ta_vec data_decls;   /* ta_data_decl */
    ta_vec initializers; /* ta_initializer_def */
    ta_vec contents;     /* ta_content_def */
    ta_vec patterns;     /* ta_event_pattern */
    ta_vec segments;     /* ta_event_pattern_segment */
    ta_vec expressions;  /* ta_expression */
    ta_vec roles;        /* ta_expr_role, by expression */
    ta_vec history;      /* ta_history_slot */
    ta_vec done_data;    /* ta_done_data_def */
    ta_vec invokes;      /* ta_invoke_decl */
    ta_vec texts;        /* ta_str: authored text kept for export */
    ta_vec path;         /* ta_spec_step: the steps below path_state */
    ta_vec names;        /* ta_state_key: every state, sorted by (name, state) after pass one */
    ta_vec levels;       /* ta_collect_frame: pass one's stack */
    ta_vec blocks;       /* ta_block_frame: the stack of nested blocks being compiled */
    ta_vec spans;        /* ta_span_record: the known spans, in walk order */
    uint32_t action_depth; /* the deepest stack of blocks so far */
    ta_state_id path_state;
    ta_compile_error *error;
    ta_arena *error_arena;
    /* Replay (ADR 0058 section 3): stop at this expression ID. */
    uint32_t locate;
    bool failed;
    uint64_t hash;
} ta_compiler;

typedef struct {
    ta_vec *vec;
    void **out;
    uint32_t *count;
} ta_compiler_seal_params;

#define TA_PHRASE(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

/* ------------------------------------------------------------------------
 * Faults and the path
 */

typedef struct {
    ta_compile_error_code code;
    ta_str subject;
    uint32_t subject_offset;
    uint32_t subject_length;
    ta_str related;
    ta_str message;
    /* The result to return; zero picks the code's usual one. */
    ta_result result;
} ta_compiler_fault;

static ta_state_def *ta_compiler_state(ta_compiler *compiler, ta_state_id state) {
    return ta_vec_at(&compiler->states, state);
}

static const ta_scxml_state_spec *ta_compiler_state_spec(ta_compiler *compiler, ta_state_id state) {
    return *(const ta_scxml_state_spec **)ta_vec_at(&compiler->state_specs, state);
}

/* The state's index in its parent's `states` list: specs are arrays. */
static uint32_t ta_compiler_state_index(ta_compiler *compiler, ta_state_id state) {
    ta_state_id parent = ta_compiler_state(compiler, state)->parent;
    const ta_scxml_state_spec *list =
        parent == TA_STATE_NONE ? compiler->spec->states : ta_compiler_state_spec(compiler, parent)->states;
    return (uint32_t)(ta_compiler_state_spec(compiler, state) - list);
}

static void ta_compiler_copy_path(ta_compiler *compiler) {
    uint32_t depth = 0;
    for (ta_state_id at = compiler->path_state; at != TA_STATE_NONE; at = ta_compiler_state(compiler, at)->parent) {
        depth += 1u;
    }
    uint32_t length = depth + compiler->path.count;
    if (length == 0) {
        return;
    }
    ta_spec_step *steps = ta_arena_alloc(compiler->error_arena, (size_t)length * sizeof(ta_spec_step),
                                         _Alignof(ta_spec_step));
    if (steps == NULL) {
        return; /* the error keeps its code and subject without a path */
    }
    uint32_t index = depth;
    for (ta_state_id at = compiler->path_state; at != TA_STATE_NONE; at = ta_compiler_state(compiler, at)->parent) {
        index -= 1u;
        steps[index] = (ta_spec_step){ .member = TA_SPEC_MEMBER_STATES,
                                       .index = ta_compiler_state_index(compiler, at) };
    }
    for (uint32_t step = 0; step < compiler->path.count; step += 1u) {
        steps[depth + step] = *(const ta_spec_step *)ta_vec_at(&compiler->path, step);
    }
    compiler->error->path = steps;
    compiler->error->path_length = length;
}

static ta_result ta_compiler_record(ta_compiler *compiler, const ta_compiler_fault *fault) {
    ta_result result = fault->result;
    if (result == TA_OK) {
        switch (fault->code) {
            case TA_COMPILE_ERROR_DATAMODEL_MISMATCH:
            case TA_COMPILE_ERROR_UNSUPPORTED_CONSTRUCT:
            case TA_COMPILE_ERROR_LIMIT:
                result = TA_ERR_UNSUPPORTED;
                break;
            default:
                result = TA_ERR_INVALID;
                break;
        }
    }
    if (compiler->failed) {
        return result;
    }
    compiler->failed = true;
    if (compiler->error == NULL) {
        return result;
    }
    *compiler->error = (ta_compile_error){
        .code = fault->code,
        .subject = fault->subject,
        .subject_offset = fault->subject_offset,
        .subject_length = fault->subject_length,
        .related = fault->related,
        .message = fault->message,
    };
    if (compiler->error_arena != NULL) {
        ta_compiler_copy_path(compiler);
    }
    return result;
}

static ta_result ta_compiler_fail(ta_compiler *compiler, ta_compile_error_code code, ta_str subject,
                                  ta_str message) {
    return ta_compiler_record(compiler, &(ta_compiler_fault){ .code = code, .subject = subject, .message = message });
}

static ta_result ta_compiler_enter(ta_compiler *compiler, ta_spec_member member, uint32_t index) {
    ta_spec_step step = { .member = member, .index = index };
    return ta_vec_push(&compiler->path, &step);
}

/* Steps are popped only on success: a fault ends the compile. */
static void ta_compiler_leave(ta_compiler *compiler) {
    compiler->path.count -= 1u;
}

/* Starts the path over at `state` (TA_STATE_NONE: the root). */
static void ta_compiler_at(ta_compiler *compiler, ta_state_id state) {
    compiler->path_state = state;
    compiler->path.count = 0;
}

static ta_result ta_compiler_fail_at(ta_compiler *compiler, ta_spec_member member, uint32_t index,
                                     const ta_compiler_fault *fault) {
    ta_result entered = ta_compiler_enter(compiler, member, index);
    return entered != TA_OK ? entered : ta_compiler_record(compiler, fault);
}

/*
 * Checks the span an element names and records it when it is known
 * (ADR 0085). Nothing about a span is hashed: where an element was written
 * is not what it means, so moving it keeps the revision.
 */
static ta_result ta_compiler_span(ta_compiler *compiler, const ta_span_record *record) {
    if (record->span == 0) {
        return TA_OK;
    }
    const ta_scxml_spec *spec = compiler->spec;
    if (record->span > spec->span_count || spec->spans == NULL) {
        return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_SPAN, TA_SPEC_INDEX_NONE,
                                   &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MALFORMED_SPEC,
                                                         .message = TA_PHRASE("the span is not in the spec's spans") });
    }
    const ta_source_span *span = &spec->spans[record->span - 1u];
    if (span->line == 0) {
        return TA_OK;
    }
    if (span->source >= (spec->source_count == 0 ? 1u : spec->source_count)) {
        return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_SPAN, TA_SPEC_INDEX_NONE,
                                   &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MALFORMED_SPEC,
                                                         .message = TA_PHRASE("the span names no source") });
    }
    return ta_vec_push(&compiler->spans, record);
}

/*
 * Optimal string alignment distance, a transposition counting one edit, so
 * "idel" is one edit from "idle". Error path only. UINT32_MAX when the rows
 * cannot be allocated.
 */
static uint32_t ta_compiler_distance(ta_allocator allocator, ta_str left, ta_str right) {
    uint32_t width = right.len + 1u;
    size_t size = (size_t)width * 3u * sizeof(uint32_t);
    uint32_t *rows = ta_mem_alloc(allocator, size, _Alignof(uint32_t));
    if (rows == NULL) {
        return UINT32_MAX;
    }
    uint32_t *before = rows;
    uint32_t *previous = rows + width;
    uint32_t *current = rows + 2u * width;
    for (uint32_t column = 0; column < width; column += 1u) {
        previous[column] = column;
    }
    for (uint32_t row = 1; row <= left.len; row += 1u) {
        current[0] = row;
        for (uint32_t column = 1; column < width; column += 1u) {
            uint32_t cost = left.ptr[row - 1u] == right.ptr[column - 1u] ? 0u : 1u;
            uint32_t best = previous[column] + 1u;
            if (current[column - 1u] + 1u < best) {
                best = current[column - 1u] + 1u;
            }
            if (previous[column - 1u] + cost < best) {
                best = previous[column - 1u] + cost;
            }
            if (row > 1u && column > 1u && left.ptr[row - 1u] == right.ptr[column - 2u] &&
                left.ptr[row - 2u] == right.ptr[column - 1u] && before[column - 2u] + 1u < best) {
                best = before[column - 2u] + 1u;
            }
            current[column] = best;
        }
        uint32_t *spare = before;
        before = previous;
        previous = current;
        current = spare;
    }
    uint32_t distance = previous[right.len];
    ta_mem_free(allocator, rows, &(ta_allocator_free_params){ .size = size, .alignment = _Alignof(uint32_t) });
    return distance;
}

/* The state ID nearest to `id`, within a third of its length; the first
 * such in document order on a tie. Empty when none is that near. */
static ta_str ta_compiler_nearest(ta_compiler *compiler, ta_str id) {
    ta_str nearest = {0};
    uint32_t best = UINT32_MAX;
    if (id.len > compiler->system->params[TA_PARAM_COMPILE_NEAREST_BYTES]) {
        return nearest; /* nobody misspells a name this long by a few letters */
    }
    for (ta_state_id state = 0; state < compiler->states.count; state += 1u) {
        ta_str candidate = ta_compiler_state_spec(compiler, state)->id;
        uint32_t gap = candidate.len > id.len ? candidate.len - id.len : id.len - candidate.len;
        if (3u * (uint64_t)gap > id.len) {
            continue; /* the distance is at least the gap in length */
        }
        uint32_t distance = ta_compiler_distance(compiler->allocator, id, candidate);
        if (distance != 0 && distance < best && 3u * (uint64_t)distance <= id.len) {
            best = distance;
            nearest = candidate;
        }
    }
    return nearest;
}

/* ------------------------------------------------------------------------
 * Leaves
 */

/* Folds bytes into the running content hash (D5); the visit order is part of the revision. */
static void ta_compiler_hash(ta_compiler *compiler, const void *bytes, size_t size) {
    compiler->hash = ta_fnv1a64(compiler->hash, bytes, size);
}

static void ta_compiler_hash_str(ta_compiler *compiler, ta_str text) {
    ta_compiler_hash(compiler, &text.len, sizeof(text.len));
    if (text.ptr != NULL) {
        ta_compiler_hash(compiler, text.ptr, text.len);
    }
}

static void ta_compiler_hash_u32(ta_compiler *compiler, uint32_t value) {
    ta_compiler_hash(compiler, &value, sizeof(value));
}

static ta_atom_table *ta_compiler_atoms(ta_compiler *compiler) {
    return &compiler->system->atoms;
}

static ta_result ta_compiler_atom(ta_compiler *compiler, ta_str text, ta_atom *out) {
    if (text.ptr == NULL || text.len == 0) {
        return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MISSING_VALUE, text, TA_PHRASE("required text is empty"));
    }
    ta_compiler_hash_str(compiler, text);
    return ta_atom_intern(ta_compiler_atoms(compiler), text, out);
}

static ta_result ta_compiler_atom_at(ta_compiler *compiler, ta_spec_member member, ta_str text, ta_atom *out) {
    ta_result result = ta_compiler_enter(compiler, member, TA_SPEC_INDEX_NONE);
    if (result == TA_OK) {
        result = ta_compiler_atom(compiler, text, out);
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
    }
    return result;
}

TA_PRIVATE ta_result ta_duration_parse(ta_str text, ta_time *out) {
    if (text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    uint32_t index = 0;
    uint64_t whole = 0;
    uint64_t fraction = 0;
    uint64_t scale = 1;
    bool digits = false;
    while (index < text.len && text.ptr[index] >= '0' && text.ptr[index] <= '9') {
        if (whole > (UINT64_MAX - 9u) / 10u) {
            return TA_ERR_INVALID;
        }
        whole = whole * 10u + (uint64_t)(text.ptr[index] - '0');
        digits = true;
        index += 1;
    }
    if (index < text.len && text.ptr[index] == '.') {
        index += 1;
        bool fraction_digits = false;
        while (index < text.len && text.ptr[index] >= '0' && text.ptr[index] <= '9') {
            if (scale < 1000000000ull) {
                fraction = fraction * 10u + (uint64_t)(text.ptr[index] - '0');
                scale *= 10u;
            }
            fraction_digits = true;
            index += 1;
        }
        if (!fraction_digits) {
            return TA_ERR_INVALID;
        }
        digits = true; /* ".5s": SCXML durations are CSS2 times, whose whole part is optional */
    }
    if (!digits) {
        return TA_ERR_INVALID;
    }
    ta_str unit = { .ptr = text.ptr + index, .len = text.len - index };
    uint64_t multiplier = 0;
    if (ta_str_equal(unit, (ta_str){ .ptr = "ms", .len = 2 })) {
        multiplier = 1;
    } else if (ta_str_equal(unit, (ta_str){ .ptr = "s", .len = 1 })) {
        multiplier = 1000;
    } else {
        return TA_ERR_INVALID;
    }
    if (whole > UINT64_MAX / multiplier) {
        return TA_ERR_INVALID;
    }
    /* Sub-millisecond remainders are truncated. */
    *out = whole * multiplier + (fraction * multiplier) / scale;
    return TA_OK;
}

typedef struct {
    const ta_expression *expression;
    ta_expr_role role;
} ta_compiler_expression_params;

static ta_result ta_compiler_expression(ta_compiler *compiler, const ta_compiler_expression_params *params,
                                        ta_expr_id *out) {
    const ta_expression *expression = params->expression;
    if (compiler->locate == compiler->expressions.count &&
        (expression->kind == TA_EXPRESSION_SOURCE || expression->kind == TA_EXPRESSION_HOST)) {
        /* Replay: this is the expression the data model refused. */
        return ta_compiler_fail(compiler, TA_COMPILE_ERROR_EXPRESSION_REJECTED,
                                expression->kind == TA_EXPRESSION_SOURCE ? expression->as.source : (ta_str){0},
                                TA_PHRASE("the data model refused this expression"));
    }
    ta_result result = ta_compiler_span(compiler, &(ta_span_record){ .kind = TA_ELEMENT_EXPRESSION,
                                                                     .id = compiler->expressions.count,
                                                                     .span = expression->span });
    if (result != TA_OK) {
        return result;
    }
    ta_expression copy = { .kind = expression->kind };
    switch (expression->kind) {
        case TA_EXPRESSION_SOURCE:
            ta_compiler_hash_str(compiler, expression->as.source);
            if (expression->as.source.ptr == NULL) {
                return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MISSING_VALUE, (ta_str){0},
                                        TA_PHRASE("an expression has no source text"));
            }
            result = ta_arena_copy_str(&compiler->definition->arena, expression->as.source,
                                       &copy.as.source);
            break;
        case TA_EXPRESSION_HOST:
            /*
             * The fingerprint cannot see into a host expression, only where
             * one is (ADR 0087): a length no source text can have marks it,
             * and registration compares the handles themselves.
             */
            ta_compiler_hash_u32(compiler, UINT32_MAX);
            compiler->definition->host_expressions = true;
            if (expression->as.host.v == NULL || expression->as.host.v->retain == NULL) {
                return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                        TA_PHRASE("a host expression has no retain operation"));
            }
            copy.as.host = expression->as.host;
            result = expression->as.host.v->retain(expression->as.host.self, expression->as.host.value,
                                                   compiler->allocator, &copy.as.host.value);
            if (result != TA_OK && result != TA_ERR_NOMEM) {
                return ta_compiler_record(compiler, &(ta_compiler_fault){
                                                        .code = TA_COMPILE_ERROR_HOST_EXPRESSION,
                                                        .message = TA_PHRASE("a host expression could not be retained"),
                                                        .result = result,
                                                    });
            }
            break;
        case TA_EXPRESSION_NONE:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MISSING_VALUE, (ta_str){0},
                                    TA_PHRASE("an expression is required"));
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown expression kind"));
    }
    if (result != TA_OK) {
        return result;
    }
    *out = compiler->expressions.count;
    /* The role first: a failed push leaves both vectors as they were. */
    result = ta_vec_push(&compiler->roles, &params->role);
    if (result == TA_OK) {
        result = ta_vec_push(&compiler->expressions, &copy);
        if (result != TA_OK) {
            compiler->roles.count -= 1u;
        }
    }
    if (result != TA_OK && copy.kind == TA_EXPRESSION_HOST && copy.as.host.v->release != NULL) {
        copy.as.host.v->release(copy.as.host.self, copy.as.host.value, compiler->allocator);
    }
    return result;
}

typedef struct {
    ta_spec_member member;
    const ta_expression *expression;
    ta_expr_role role;
} ta_compiler_expression_at_params;

static ta_result ta_compiler_expression_at(ta_compiler *compiler, const ta_compiler_expression_at_params *params,
                                           ta_expr_id *out) {
    ta_result result = ta_compiler_enter(compiler, params->member, TA_SPEC_INDEX_NONE);
    if (result == TA_OK) {
        result = ta_compiler_expression(compiler,
                                        &(ta_compiler_expression_params){ params->expression, params->role }, out);
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
    }
    return result;
}

/* A dotted event name, as <raise event> and <send event> write one. */
static ta_result ta_compiler_event_name(ta_compiler *compiler, ta_str text, ta_event_name_id *out) {
    ta_result result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_EVENT, TA_SPEC_INDEX_NONE);
    if (result != TA_OK) {
        return result;
    }
    ta_compiler_hash_str(compiler, text);
    result = ta_event_name_intern_dotted(&compiler->system->event_names, text, out);
    if (result == TA_ERR_INVALID) {
        return text.len == 0
                   ? ta_compiler_fail(compiler, TA_COMPILE_ERROR_MISSING_VALUE, text, TA_PHRASE("an event name is required"))
                   : ta_compiler_fail(compiler, TA_COMPILE_ERROR_BAD_EVENT_DESCRIPTOR, text,
                                      TA_PHRASE("an event name has an empty segment"));
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
    }
    return result;
}

/*
 * Keeps authored text that the compiler normalizes, so ta_definition_export()
 * can rebuild the exact input (ADR 0042). The text is copied into the
 * definition's arena.
 */
static ta_result ta_compiler_keep_text(ta_compiler *compiler, ta_str text, ta_authored_text_id *out) {
    ta_str copy = {0};
    ta_result result = ta_arena_copy_str(&compiler->definition->arena, text, &copy);
    if (result == TA_OK) {
        *out = compiler->texts.count;
        result = ta_vec_push(&compiler->texts, &copy);
    }
    return result;
}

/* `media_type` is empty for literal text (a send target, an invoke src). */
static ta_result ta_compiler_content(ta_compiler *compiler, ta_str media_type, ta_bytes bytes,
                                     ta_content_id *out) {
    ta_content_def content = {0};
    if (media_type.len != 0) {
        ta_result copied = ta_arena_copy_str(&compiler->definition->arena, media_type, &content.media_type);
        if (copied != TA_OK) {
            return copied;
        }
    }
    /* A media type is part of what the chart says, so it is part of its
     * revision (ADR 0053): two charts differing only in one must not share a
     * fingerprint. */
    ta_compiler_hash_str(compiler, media_type);
    if (bytes.len != 0) {
        uint8_t *copy = ta_arena_alloc(&compiler->definition->arena, bytes.len, 1);
        if (copy == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_copy(copy, bytes.ptr, bytes.len);
        content.bytes = (ta_bytes){ .ptr = copy, .len = bytes.len };
    }
    ta_compiler_hash(compiler, bytes.ptr == NULL ? "" : (const void *)bytes.ptr, bytes.len);
    *out = compiler->contents.count;
    return ta_vec_push(&compiler->contents, &content);
}

static ta_result ta_compiler_value(ta_compiler *compiler, const ta_scxml_value_spec *spec,
                                   ta_value_source *out) {
    ta_compiler_hash_u32(compiler, spec->kind);
    switch (spec->kind) {
        case TA_SCXML_VALUE_NONE:
            *out = (ta_value_source){ .kind = TA_VALUE_SOURCE_NONE };
            return TA_OK;
        case TA_SCXML_VALUE_EXPR:
            out->kind = TA_VALUE_SOURCE_EXPR;
            return ta_compiler_expression(compiler, &(ta_compiler_expression_params){ &spec->as.expr, TA_EXPR_ROLE_VALUE },
                                          &out->as.expr);
        case TA_SCXML_VALUE_LOCATION:
            out->kind = TA_VALUE_SOURCE_LOCATION;
            return ta_compiler_expression(compiler,
                                          &(ta_compiler_expression_params){ &spec->as.location, TA_EXPR_ROLE_LOCATION },
                                          &out->as.location);
        case TA_SCXML_VALUE_CONTENT:
            out->kind = TA_VALUE_SOURCE_CONTENT;
            return ta_compiler_content(compiler, spec->as.content.media_type, spec->as.content.bytes,
                                       &out->as.content);
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown value kind"));
    }
}

static ta_result ta_compiler_value_at(ta_compiler *compiler, ta_spec_member member,
                                      const ta_scxml_value_spec *spec, ta_value_source *out) {
    ta_result result = ta_compiler_enter(compiler, member, TA_SPEC_INDEX_NONE);
    if (result == TA_OK) {
        result = ta_compiler_value(compiler, spec, out);
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
    }
    return result;
}

static ta_result ta_compiler_params(ta_compiler *compiler, const ta_scxml_param_spec *specs,
                                    ta_span32 *out) {
    uint32_t count = 0;
    while (specs != NULL && specs[count].name.ptr != NULL) {
        count += 1;
    }
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->params, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        ta_param_def param = {0};
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_PARAMS, index);
        if (result == TA_OK) {
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_NAME, specs[index].name, &param.name);
        }
        if (result == TA_OK) {
            result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &specs[index].value, &param.value);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            *(ta_param_def *)ta_vec_at(&compiler->params, base + index) = param;
        }
    }
    *out = (ta_span32){ .offset = base, .length = count };
    return result;
}

static ta_result ta_compiler_block(ta_compiler *compiler, ta_spec_member member,
                                   const ta_scxml_action_spec *specs, ta_span32 *out);

static ta_result ta_compiler_malformed(ta_compiler *compiler, ta_spec_member member) {
    return ta_compiler_fail_at(compiler, member, TA_SPEC_INDEX_NONE,
                               &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MALFORMED_SPEC,
                                                     .message = TA_PHRASE("unknown kind") });
}

static ta_result ta_compiler_send(ta_compiler *compiler, const ta_scxml_send_spec *spec,
                                  ta_send_action *out) {
    ta_result result = TA_OK;
    switch (spec->id.kind) {
        case TA_SCXML_ID_NONE:
            out->id.kind = TA_ID_SOURCE_GENERATED;
            break;
        case TA_SCXML_ID_LITERAL:
            out->id.kind = TA_ID_SOURCE_LITERAL;
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_ID, spec->id.as.literal, &out->id.as.literal);
            break;
        case TA_SCXML_ID_LOCATION:
            out->id.kind = TA_ID_SOURCE_LOCATION;
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_ID, &spec->id.as.location, TA_EXPR_ROLE_LOCATION },
                                               &out->id.as.location);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_ID);
    }
    if (result != TA_OK) {
        return result;
    }
    switch (spec->type.kind) {
        case TA_SCXML_STRING_NONE:
            out->type.kind = TA_TEXT_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->type.kind = TA_TEXT_SOURCE_ATOM;
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_TYPE, spec->type.as.literal, &out->type.as.atom);
            break;
        case TA_SCXML_STRING_EXPR:
            out->type.kind = TA_TEXT_SOURCE_EXPR;
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_TYPE, &spec->type.as.expr, TA_EXPR_ROLE_VALUE },
                                               &out->type.as.expr);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_TYPE);
    }
    if (result != TA_OK) {
        return result;
    }
    out->delay.text = TA_AUTHORED_TEXT_NONE;
    ta_compiler_hash_u32(compiler, spec->event.kind);
    if (spec->event.kind == TA_SCXML_EVENT_EXPR) {
        out->event.kind = TA_EVENT_SOURCE_EXPR;
        result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_EVENT, &spec->event.as.expr, TA_EXPR_ROLE_VALUE },
                                           &out->event.as.expr);
    } else if (spec->event.kind == TA_SCXML_EVENT_NAME) {
        out->event.kind = TA_EVENT_SOURCE_NAME;
        result = ta_compiler_event_name(compiler, spec->event.as.name, &out->event.as.name);
    } else if (spec->event.kind == TA_SCXML_EVENT_NONE) {
        return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_EVENT, TA_SPEC_INDEX_NONE,
                                   &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MISSING_VALUE,
                                                         .message = TA_PHRASE("a send needs an event") });
    } else {
        return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_EVENT);
    }
    if (result != TA_OK) {
        return result;
    }
    switch (spec->target.kind) {
        case TA_SCXML_STRING_NONE:
            out->target.kind = TA_VALUE_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->target.kind = TA_VALUE_SOURCE_CONTENT;
            result = ta_compiler_content(compiler, (ta_str){0},
                                         (ta_bytes){ .ptr = (const uint8_t *)spec->target.as.literal.ptr,
                                                     .len = spec->target.as.literal.len },
                                         &out->target.as.content);
            break;
        case TA_SCXML_STRING_EXPR:
            out->target.kind = TA_VALUE_SOURCE_EXPR;
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_TARGET, &spec->target.as.expr, TA_EXPR_ROLE_VALUE },
                                               &out->target.as.expr);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_TARGET);
    }
    if (result != TA_OK) {
        return result;
    }
    switch (spec->delay.kind) {
        case TA_SCXML_STRING_NONE:
            out->delay.kind = TA_DELAY_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->delay.kind = TA_DELAY_SOURCE_LITERAL;
            ta_compiler_hash_str(compiler, spec->delay.as.literal);
            result = ta_duration_parse(spec->delay.as.literal, &out->delay.as.literal);
            if (result == TA_ERR_INVALID) {
                return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_DELAY, TA_SPEC_INDEX_NONE,
                                           &(ta_compiler_fault){
                                               .code = TA_COMPILE_ERROR_BAD_DURATION,
                                               .subject = spec->delay.as.literal,
                                               .message = TA_PHRASE("not an SCXML duration such as \"500ms\" or \"2.5s\""),
                                           });
            }
            if (result == TA_OK) {
                result = ta_compiler_keep_text(compiler, spec->delay.as.literal, &out->delay.text);
            }
            break;
        case TA_SCXML_STRING_EXPR:
            out->delay.kind = TA_DELAY_SOURCE_EXPR;
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_DELAY, &spec->delay.as.expr, TA_EXPR_ROLE_VALUE },
                                               &out->delay.as.expr);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_DELAY);
    }
    if (result == TA_OK) {
        result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->data, &out->data);
    }
    if (result == TA_OK) {
        result = ta_compiler_params(compiler, spec->params, &out->params);
    }
    if (result == TA_OK && out->params.length != 0 && out->data.kind != TA_VALUE_SOURCE_NONE) {
        result = ta_compiler_fail(compiler, TA_COMPILE_ERROR_EXCLUSIVE_MEMBERS, (ta_str){0},
                                  TA_PHRASE("a send has both params and content"));
    }
    return result;
}

/*
 * <foreach>: the array is an expression; item and index are variable names
 * whose legality the data model judges when it binds them (SCXML 4.6). The
 * body is compiled by ta_compiler_block after this.
 */
static ta_result ta_compiler_foreach(ta_compiler *compiler, const ta_scxml_action_spec *spec,
                                     ta_foreach_action *out) {
    ta_result result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_EXPR, &spec->as.foreach.array, TA_EXPR_ROLE_VALUE }, &out->array);
    if (result == TA_OK) {
        result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_ITEM, spec->as.foreach.item, &out->item);
    }
    if (result == TA_OK && spec->as.foreach.index.ptr != NULL) {
        out->has_index = true;
        result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_INDEX, spec->as.foreach.index, &out->index);
    }
    return result;
}

/*
 * One action, up to its nested blocks: everything an <if> or <foreach> has
 * before them. ta_compiler_block compiles the nested blocks next, in the
 * order the recursive compiler did.
 */
static ta_result ta_compiler_action(ta_compiler *compiler, const ta_scxml_action_spec *spec,
                                    ta_action_def *out) {
    ta_compiler_hash_u32(compiler, spec->kind);
    ta_result result = TA_OK;
    switch (spec->kind) {
        case TA_SCXML_ACTION_RAISE:
            out->kind = TA_ACTION_RAISE;
            result = ta_compiler_event_name(compiler, spec->as.raise.event, &out->as.raise.event);
            if (result == TA_OK) {
                result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->as.raise.data,
                                              &out->as.raise.data);
            }
            return result;
        case TA_SCXML_ACTION_SEND:
            out->kind = TA_ACTION_SEND;
            return ta_compiler_send(compiler, &spec->as.send, &out->as.send);
        case TA_SCXML_ACTION_CANCEL:
            out->kind = TA_ACTION_CANCEL;
            if (spec->as.cancel.sendid.kind == TA_SCXML_STRING_EXPR) {
                out->as.cancel.sendid.kind = TA_TEXT_SOURCE_EXPR;
                return ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_SENDID, &spec->as.cancel.sendid.as.expr, TA_EXPR_ROLE_VALUE },
                                                 &out->as.cancel.sendid.as.expr);
            }
            if (spec->as.cancel.sendid.kind == TA_SCXML_STRING_NONE) {
                return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_SENDID, TA_SPEC_INDEX_NONE,
                                           &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MISSING_VALUE,
                                                                 .message = TA_PHRASE("a cancel needs a sendid") });
            }
            if (spec->as.cancel.sendid.kind != TA_SCXML_STRING_LITERAL) {
                return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_SENDID);
            }
            out->as.cancel.sendid.kind = TA_TEXT_SOURCE_ATOM;
            return ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_SENDID, spec->as.cancel.sendid.as.literal,
                                       &out->as.cancel.sendid.as.atom);
        case TA_SCXML_ACTION_ASSIGN:
            out->kind = TA_ACTION_ASSIGN;
            if (spec->as.assign.value.kind == TA_SCXML_VALUE_NONE) {
                return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, TA_SPEC_INDEX_NONE,
                                           &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MISSING_VALUE,
                                                                 .message = TA_PHRASE("an assign needs a value") });
            }
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_LOCATION, &spec->as.assign.location, TA_EXPR_ROLE_LOCATION },
                                               &out->as.assign.location);
            if (result == TA_OK) {
                result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->as.assign.value,
                                              &out->as.assign.value);
            }
            return result;
        case TA_SCXML_ACTION_SCRIPT:
            out->kind = TA_ACTION_SCRIPT;
            return ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_EXPR, &spec->as.script.script, TA_EXPR_ROLE_SCRIPT }, &out->as.script);
        case TA_SCXML_ACTION_LOG:
            out->kind = TA_ACTION_LOG;
            if (spec->as.log.label.ptr != NULL) {
                out->as.log.has_label = true;
                result = ta_atom_intern(ta_compiler_atoms(compiler), spec->as.log.label, &out->as.log.label);
            }
            if (result == TA_OK && spec->as.log.expr.kind != TA_EXPRESSION_NONE) {
                out->as.log.has_expr = true;
                result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_EXPR, &spec->as.log.expr, TA_EXPR_ROLE_VALUE },
                                                   &out->as.log.expr);
            }
            return result;
        case TA_SCXML_ACTION_IF:
            out->kind = TA_ACTION_IF;
            return ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_CONDITION, &spec->as.conditional.condition, TA_EXPR_ROLE_CONDITION },
                                             &out->as.conditional.condition);
        case TA_SCXML_ACTION_FOREACH:
            out->kind = TA_ACTION_FOREACH;
            return ta_compiler_foreach(compiler, spec, &out->as.foreach);
        case TA_SCXML_ACTION_INVOKE:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_UNSUPPORTED_CONSTRUCT, (ta_str){0},
                                    TA_PHRASE("<invoke> is not executable content"));
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown action kind"));
    }
}

/* Starts a block: reserves its actions, so nested blocks append behind it. */
static ta_result ta_compiler_block_open(ta_compiler *compiler, ta_spec_member member,
                                        const ta_scxml_action_spec *specs) {
    ta_block_frame frame = { .specs = specs, .member = member };
    while (specs != NULL && specs[frame.count].kind != TA_SCXML_ACTION_NONE) {
        frame.count += 1;
    }
    ta_result result = ta_vec_extend(&compiler->actions, frame.count, &frame.base);
    if (result == TA_OK) {
        result = ta_vec_push(&compiler->blocks, &frame);
    }
    if (result == TA_OK && compiler->blocks.count > compiler->action_depth) {
        compiler->action_depth = compiler->blocks.count;
    }
    return result;
}

static ta_block_frame *ta_compiler_block_top(ta_compiler *compiler) {
    return ta_vec_at(&compiler->blocks, compiler->blocks.count - 1u);
}

/* Stores the finished action of the top block and moves to the next one. */
static void ta_compiler_block_store(ta_compiler *compiler, ta_block_frame *frame) {
    ta_compiler_leave(compiler);
    *(ta_action_def *)ta_vec_at(&compiler->actions, frame->base + frame->index) = frame->action;
    frame->index += 1u;
    frame->stage = TA_BLOCK_NEXT;
}

/*
 * Gives a finished nested block to the action that owns it, in the top
 * frame, and opens the owner's next nested block if it has one.
 */
static ta_result ta_compiler_block_deliver(ta_compiler *compiler, ta_span32 span) {
    ta_block_frame *frame = ta_compiler_block_top(compiler);
    switch (frame->stage) {
        case TA_BLOCK_THEN:
            frame->action.as.conditional.then_actions = span;
            frame->stage = TA_BLOCK_ELSE;
            return ta_compiler_block_open(compiler, TA_SPEC_MEMBER_ELSE,
                                          frame->specs[frame->index].as.conditional.else_actions);
        case TA_BLOCK_ELSE:
            frame->action.as.conditional.else_actions = span;
            ta_compiler_block_store(compiler, frame);
            return TA_OK;
        case TA_BLOCK_BODY:
            frame->action.as.foreach.body_actions = span;
            ta_compiler_block_store(compiler, frame);
            return TA_OK;
        default:
            return TA_ERR_INVALID; /* unreachable: only an owner waits for a block */
    }
}

/*
 * Compiles a block of executable content and every block nested in it,
 * without recursion (ADR 0060). Each open block is a frame; an <if> or
 * <foreach> in a block compiles up to its nested blocks, opens the first,
 * and resumes when it is delivered. Actions, expressions, nested block
 * reservations and path steps come in exactly the order the recursive
 * compiler produced them, so revisions and numbering do not change.
 */
static ta_result ta_compiler_block(ta_compiler *compiler, ta_spec_member member,
                                   const ta_scxml_action_spec *specs, ta_span32 *out) {
    uint32_t floor = compiler->blocks.count;
    ta_result result = ta_compiler_block_open(compiler, member, specs);
    while (result == TA_OK) {
        ta_block_frame *frame = ta_compiler_block_top(compiler);
        if (frame->index == frame->count) {
            ta_span32 span = { .offset = frame->base, .length = frame->count };
            compiler->blocks.count -= 1u;
            if (compiler->blocks.count == floor) {
                *out = span;
                return TA_OK;
            }
            result = ta_compiler_block_deliver(compiler, span);
            continue;
        }
        const ta_scxml_action_spec *spec = &frame->specs[frame->index];
        ta_action_def action = {0};
        result = ta_compiler_enter(compiler, frame->member, frame->index);
        if (result == TA_OK) {
            result = ta_compiler_span(compiler, &(ta_span_record){ .kind = TA_ELEMENT_ACTION,
                                                                   .id = frame->base + frame->index,
                                                                   .span = spec->span });
        }
        if (result == TA_OK) {
            result = ta_compiler_action(compiler, spec, &action);
        }
        if (result != TA_OK) {
            break;
        }
        frame = ta_compiler_block_top(compiler);
        frame->action = action;
        if (action.kind == TA_ACTION_IF) {
            frame->stage = TA_BLOCK_THEN;
            result = ta_compiler_block_open(compiler, TA_SPEC_MEMBER_THEN, spec->as.conditional.then_actions);
        } else if (action.kind == TA_ACTION_FOREACH) {
            frame->stage = TA_BLOCK_BODY;
            result = ta_compiler_block_open(compiler, TA_SPEC_MEMBER_BODY, spec->as.foreach.body);
        } else {
            ta_compiler_block_store(compiler, frame);
        }
    }
    compiler->blocks.count = floor;
    return result;
}

/*
 * Compiles a list of <onentry> or <onexit> handlers. Each handler keeps its
 * own action block, so an error in one handler does not skip the next.
 */
static ta_result ta_compiler_handlers(ta_compiler *compiler, ta_spec_member member,
                                      const ta_scxml_handler_spec *specs, ta_span32 *out) {
    uint32_t count = 0;
    while (specs != NULL && specs[count].actions != NULL) {
        count += 1;
    }
    ta_compiler_hash_u32(compiler, count);
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->handlers, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        ta_span32 block = {0};
        result = ta_compiler_enter(compiler, member, index);
        if (result == TA_OK) {
            result = ta_compiler_block(compiler, TA_SPEC_MEMBER_ACTIONS, specs[index].actions, &block);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            ta_compiler_hash_u32(compiler, block.length);
            *(ta_span32 *)ta_vec_at(&compiler->handlers, base + index) = block;
        }
    }
    *out = (ta_span32){ .offset = base, .length = count };
    return result;
}

static ta_result ta_compiler_kind(ta_compiler *compiler, ta_scxml_state_kind kind, ta_state_kind *out) {
    switch (kind) {
        case TA_SCXML_STATE_ATOMIC: *out = TA_STATE_ATOMIC; return TA_OK;
        case TA_SCXML_STATE_COMPOUND: *out = TA_STATE_COMPOUND; return TA_OK;
        case TA_SCXML_STATE_PARALLEL: *out = TA_STATE_PARALLEL; return TA_OK;
        case TA_SCXML_STATE_FINAL: *out = TA_STATE_FINAL; return TA_OK;
        case TA_SCXML_STATE_HISTORY_SHALLOW: *out = TA_STATE_HISTORY_SHALLOW; return TA_OK;
        case TA_SCXML_STATE_HISTORY_DEEP: *out = TA_STATE_HISTORY_DEEP; return TA_OK;
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown state kind"));
    }
}

static bool ta_state_kind_is_history(ta_state_kind kind) {
    return kind == TA_STATE_HISTORY_SHALLOW || kind == TA_STATE_HISTORY_DEEP;
}

static bool ta_state_key_before(ta_state_key left, ta_state_key right) {
    return left.name < right.name || (left.name == right.name && left.state < right.state);
}

static void ta_state_keys_sift(ta_state_key *keys, uint32_t root, uint32_t count) {
    for (;;) {
        uint32_t largest = root;
        uint32_t left = 2u * root + 1u;
        uint32_t right = left + 1u;
        if (left < count && ta_state_key_before(keys[largest], keys[left])) {
            largest = left;
        }
        if (right < count && ta_state_key_before(keys[largest], keys[right])) {
            largest = right;
        }
        if (largest == root) {
            return;
        }
        ta_state_key swap = keys[root];
        keys[root] = keys[largest];
        keys[largest] = swap;
        root = largest;
    }
}

/*
 * Sorts by (name, state): insertion sort for a few keys, heapsort otherwise,
 * so the sort is O(n log n), in place, and allocates nothing. The order is
 * total, so stability does not matter.
 */
static void ta_state_keys_sort(ta_state_key *keys, uint32_t count) {
    if (count <= TA_COMPILER_LINEAR_NAMES) {
        for (uint32_t index = 1; index < count; index += 1u) {
            ta_state_key key = keys[index];
            uint32_t at = index;
            while (at > 0 && ta_state_key_before(key, keys[at - 1u])) {
                keys[at] = keys[at - 1u];
                at -= 1u;
            }
            keys[at] = key;
        }
        return;
    }
    for (uint32_t root = count / 2u; root > 0; root -= 1u) {
        ta_state_keys_sift(keys, root - 1u, count);
    }
    for (uint32_t end = count - 1u; end > 0; end -= 1u) {
        ta_state_key swap = keys[0];
        keys[0] = keys[end];
        keys[end] = swap;
        ta_state_keys_sift(keys, 0, end);
    }
}

/* The state named `name`. Pass two only: the index is complete and has no
 * duplicates. */
static bool ta_compiler_find(ta_compiler *compiler, ta_atom name, ta_state_id *out) {
    const ta_state_key *keys = compiler->names.items;
    uint32_t count = compiler->names.count;
    if (count <= TA_COMPILER_LINEAR_NAMES) {
        for (uint32_t index = 0; index < count; index += 1u) {
            if (keys[index].name == name) {
                *out = keys[index].state;
                return true;
            }
        }
        return false;
    }
    uint32_t low = 0;
    uint32_t high = count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (keys[middle].name < name) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    if (low < count && keys[low].name == name) {
        *out = keys[low].state;
        return true;
    }
    return false;
}

/*
 * Builds the name index over the states pass one collected, and reports a
 * duplicate ID. Sorted by (name, state), each run of equal names lists its
 * states in document order. So the second key of a run is the duplicate the
 * walk meets first, and the first key is the definition it repeats. Across
 * runs the smallest such second state wins: that is the duplicate a
 * per-state check during pass one would report.
 *
 * `collected` is the result of pass one. A pass-one fault stops collection
 * at a state after every collected one, so a duplicate among the collected
 * states comes earlier in document order and replaces that fault. That keeps
 * the first error in compile order (ADR 0058 section 2).
 */
static ta_result ta_compiler_index_names(ta_compiler *compiler, ta_result collected) {
    uint32_t count = compiler->states.count;
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->names, count, &base);
    if (result != TA_OK) {
        return collected != TA_OK ? collected : result;
    }
    ta_state_key *keys = compiler->names.items;
    for (ta_state_id state = 0; state < count; state += 1u) {
        keys[state] = (ta_state_key){ .name = ta_compiler_state(compiler, state)->name, .state = state };
    }
    ta_state_keys_sort(keys, count);
    ta_state_id duplicate = TA_STATE_NONE;
    ta_state_id first = TA_STATE_NONE;
    for (uint32_t index = 1, run = 0; index < count; index += 1u) {
        if (keys[index].name != keys[index - 1u].name) {
            run = index;
        } else if (index == run + 1u && (duplicate == TA_STATE_NONE || keys[index].state < duplicate)) {
            duplicate = keys[index].state;
            first = keys[run].state;
        }
    }
    if (duplicate == TA_STATE_NONE) {
        return collected;
    }
    if (collected != TA_OK && !compiler->failed) {
        return collected; /* an allocation failure, not a fault in the chart */
    }
    compiler->failed = false;
    ta_compiler_at(compiler, ta_compiler_state(compiler, duplicate)->parent);
    result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_STATES, ta_compiler_state_index(compiler, duplicate));
    if (result != TA_OK) {
        return result;
    }
    return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_ID, TA_SPEC_INDEX_NONE,
                               &(ta_compiler_fault){
                                   .code = TA_COMPILE_ERROR_DUPLICATE_STATE,
                                   .subject = ta_compiler_state_spec(compiler, duplicate)->id,
                                   .related = ta_compiler_state_spec(compiler, first)->id,
                                   .message = TA_PHRASE("duplicate state ID"),
                               });
}

/* Opens a level of pass one: reserves its slots in `children`. */
static ta_result ta_compiler_level_open(ta_compiler *compiler, const ta_scxml_state_spec *specs, ta_state_id parent) {
    ta_collect_frame frame = { .specs = specs, .parent = parent, .current = TA_STATE_NONE };
    while (specs != NULL && specs[frame.count].kind != TA_SCXML_STATE_NONE) {
        frame.count += 1;
    }
    ta_result result = ta_vec_extend(&compiler->children, frame.count, &frame.base);
    return result == TA_OK ? ta_vec_push(&compiler->levels, &frame) : result;
}

/*
 * Pass one: assigns IDs in document pre-order and records child ranges,
 * without recursion (ADR 0060). A state's children are collected right after
 * it, before its next sibling, as the recursive compiler did, so IDs, the
 * layout of `children` and the hash do not change.
 */
static ta_result ta_compiler_collect(ta_compiler *compiler, const ta_scxml_state_spec *specs,
                                     ta_state_id parent, ta_span32 *out) {
    ta_result result = ta_compiler_level_open(compiler, specs, parent);
    while (result == TA_OK) {
        ta_collect_frame *frame = ta_vec_at(&compiler->levels, compiler->levels.count - 1u);
        if (frame->index == frame->count) {
            ta_span32 span = { .offset = frame->base, .length = frame->count };
            compiler->levels.count -= 1u;
            if (compiler->levels.count == 0) {
                *out = span;
                return TA_OK;
            }
            frame = ta_vec_at(&compiler->levels, compiler->levels.count - 1u);
            ta_compiler_state(compiler, frame->current)->children = span;
            frame->index += 1u;
            continue;
        }
        const ta_scxml_state_spec *spec = &frame->specs[frame->index];
        ta_state_def state = {
            .parent = frame->parent,
            .initial = TA_INITIAL_TRANSITION_NONE,
            .done_data = TA_DONE_DATA_NONE,
            .history_slot = UINT32_MAX,
        };
        ta_compiler_at(compiler, frame->parent);
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_STATES, frame->index);
        if (result == TA_OK) {
            ta_compiler_hash_u32(compiler, spec->kind);
            result = ta_compiler_kind(compiler, spec->kind, &state.kind);
        }
        if (result == TA_OK) {
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_ID, spec->id, &state.name);
        }
        if (result == TA_OK) {
            result = ta_compiler_span(compiler, &(ta_span_record){ .kind = TA_ELEMENT_STATE,
                                                                   .id = compiler->states.count,
                                                                   .span = spec->span });
        }
        if (result != TA_OK) {
            break;
        }
        ta_state_id id = compiler->states.count;
        *(ta_state_id *)ta_vec_at(&compiler->children, frame->base + frame->index) = id;
        frame->current = id;
        result = ta_vec_push(&compiler->states, &state);
        if (result == TA_OK) {
            result = ta_vec_push(&compiler->state_specs, &spec);
        }
        if (result == TA_OK) {
            result = ta_compiler_level_open(compiler, spec->states, id);
        }
    }
    compiler->levels.count = 0;
    return result;
}

static ta_result ta_compiler_resolve(ta_compiler *compiler, ta_str id, ta_state_id *out) {
    ta_atom atom = 0;
    ta_result result = ta_compiler_atom(compiler, id, &atom);
    if (result != TA_OK) {
        return result;
    }
    if (ta_compiler_find(compiler, atom, out)) {
        return TA_OK;
    }
    return ta_compiler_record(compiler, &(ta_compiler_fault){
                                            .code = TA_COMPILE_ERROR_UNKNOWN_STATE,
                                            .subject = id,
                                            .related = ta_compiler_nearest(compiler, id),
                                            .message = TA_PHRASE("unknown state"),
                                        });
}

static bool ta_compiler_descends(ta_compiler *compiler, ta_state_id state, ta_state_id ancestor) {
    if (ancestor == TA_STATE_NONE) {
        return true;
    }
    for (ta_state_id at = ta_compiler_state(compiler, state)->parent; at != TA_STATE_NONE;
         at = ta_compiler_state(compiler, at)->parent) {
        if (at == ancestor) {
            return true;
        }
    }
    return false;
}

typedef struct {
    const ta_str *ids;
    /* Targets must be proper descendants of this state (TA_STATE_NONE: any). */
    ta_state_id within;
    ta_span32 *out;
} ta_compiler_targets_params;

static ta_result ta_compiler_targets(ta_compiler *compiler, const ta_compiler_targets_params *params) {
    uint32_t count = 0;
    while (params->ids != NULL && params->ids[count].ptr != NULL) {
        count += 1;
    }
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->targets, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        ta_state_id target = TA_STATE_NONE;
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_TARGETS, index);
        if (result == TA_OK) {
            result = ta_compiler_resolve(compiler, params->ids[index], &target);
        }
        if (result == TA_OK && !ta_compiler_descends(compiler, target, params->within)) {
            result = ta_compiler_record(compiler, &(ta_compiler_fault){
                                                      .code = TA_COMPILE_ERROR_TARGET_OUTSIDE,
                                                      .subject = params->ids[index],
                                                      .related = ta_compiler_state_spec(compiler, params->within)->id,
                                                      .message = TA_PHRASE("target is not inside its state"),
                                                  });
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            *(ta_state_id *)ta_vec_at(&compiler->targets, base + index) = target;
        }
    }
    *params->out = (ta_span32){ .offset = base, .length = count };
    return result;
}

/* Parses space-separated descriptors; a `*` segment is a wildcard. */
static ta_result ta_compiler_descriptors(ta_compiler *compiler, ta_str text, ta_span32 *out) {
    ta_compiler_hash_str(compiler, text);
    uint32_t first = compiler->patterns.count;
    uint32_t index = 0;
    while (text.ptr != NULL && index < text.len) {
        while (index < text.len && (text.ptr[index] == ' ' || text.ptr[index] == '\t' ||
                                    text.ptr[index] == '\n' || text.ptr[index] == '\r')) {
            index += 1;
        }
        uint32_t start = index;
        while (index < text.len && text.ptr[index] != ' ' && text.ptr[index] != '\t' &&
               text.ptr[index] != '\n' && text.ptr[index] != '\r') {
            index += 1;
        }
        if (index == start) {
            break;
        }
        ta_event_pattern pattern = { .segments = { .offset = compiler->segments.count } };
        uint32_t segment_start = start;
        if (index - start == 2 && text.ptr[start] == '.' && text.ptr[start + 1u] == '*') {
            /* ".*" is the empty prefix with the optional ".*" suffix: every event. */
            segment_start = start + 1u;
        }
        for (uint32_t at = segment_start; at <= index; at += 1) {
            if (at < index && text.ptr[at] != '.') {
                continue;
            }
            ta_str segment = { .ptr = text.ptr + segment_start, .len = at - segment_start };
            if (segment.len == 0) {
                return ta_compiler_record(compiler, &(ta_compiler_fault){
                                                        .code = TA_COMPILE_ERROR_BAD_EVENT_DESCRIPTOR,
                                                        .subject = text,
                                                        .subject_offset = start,
                                                        .subject_length = index - start,
                                                        .message = TA_PHRASE("an event descriptor has an empty segment"),
                                                    });
            }
            ta_event_pattern_segment compiled = {0};
            ta_result result = TA_OK;
            if (segment.len == 1 && segment.ptr[0] == '*') {
                compiled.flags = TA_EVENT_PATTERN_SEGMENT_WILDCARD;
                compiled.atom = TA_ATOM_NONE;
            } else {
                result = ta_atom_intern(ta_compiler_atoms(compiler), segment, &compiled.atom);
            }
            /* A trailing `.*` adds nothing to SCXML prefix matching. */
            bool trailing_wildcard = at == index && pattern.segments.length > 0 &&
                                     compiled.flags == TA_EVENT_PATTERN_SEGMENT_WILDCARD;
            if (result == TA_OK && !trailing_wildcard) {
                result = ta_vec_push(&compiler->segments, &compiled);
                pattern.segments.length += 1;
            }
            if (result != TA_OK) {
                return result;
            }
            segment_start = at + 1u;
        }
        ta_result result = ta_vec_push(&compiler->patterns, &pattern);
        if (result != TA_OK) {
            return result;
        }
    }
    *out = (ta_span32){ .offset = first, .length = compiler->patterns.count - first };
    return TA_OK;
}

static ta_result ta_compiler_transition(ta_compiler *compiler, ta_state_id source,
                                        const ta_scxml_transition_spec *spec) {
    ta_transition_def transition = {
        .source = source,
        .condition = UINT32_MAX,
        .document_order = compiler->transitions.count,
        .event_text = TA_AUTHORED_TEXT_NONE,
    };
    ta_compiler_hash_u32(compiler, spec->kind);
    if (spec->kind == TA_SCXML_TRANSITION_EXTERNAL) {
        transition.type = TA_TRANSITION_EXTERNAL;
    } else if (spec->kind == TA_SCXML_TRANSITION_INTERNAL) {
        transition.type = TA_TRANSITION_INTERNAL;
    } else {
        return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                TA_PHRASE("unknown transition kind"));
    }
    /* Transitions are numbered in the order they are stored, after their content. */
    ta_result result = ta_compiler_span(compiler, &(ta_span_record){ .kind = TA_ELEMENT_TRANSITION,
                                                                     .id = compiler->transitions.count,
                                                                     .span = spec->span });
    if (result == TA_OK) {
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_EVENT, TA_SPEC_INDEX_NONE);
    }
    if (result == TA_OK) {
        result = ta_compiler_descriptors(compiler, spec->event, &transition.event_patterns);
    }
    if (result == TA_OK && spec->event.ptr != NULL) {
        result = ta_compiler_keep_text(compiler, spec->event, &transition.event_text);
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
    }
    if (result == TA_OK && spec->condition.kind != TA_EXPRESSION_NONE) {
        result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_CONDITION, &spec->condition, TA_EXPR_ROLE_CONDITION },
                                           &transition.condition);
    }
    if (result == TA_OK) {
        result = ta_compiler_targets(compiler, &(ta_compiler_targets_params){
                                                   .ids = spec->targets,
                                                   .within = TA_STATE_NONE,
                                                   .out = &transition.targets,
                                               });
    }
    if (result == TA_OK) {
        result = ta_compiler_block(compiler, TA_SPEC_MEMBER_ACTIONS, spec->actions, &transition.actions);
    }
    if (result == TA_OK) {
        result = ta_vec_push(&compiler->transitions, &transition);
    }
    return result;
}

typedef struct {
    ta_state_id owner;
    const ta_scxml_initial_spec *spec;
    ta_span32 children;
    ta_initial_transition_id *out;
} ta_compiler_initial_params;

static ta_result ta_compiler_initial(ta_compiler *compiler, const ta_compiler_initial_params *params) {
    ta_initial_transition_def initial = { .owner = params->owner };
    ta_result result = TA_OK;
    if (params->spec != NULL) {
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_INITIAL, TA_SPEC_INDEX_NONE);
        if (result == TA_OK) {
            result = ta_compiler_span(compiler, &(ta_span_record){ .kind = TA_ELEMENT_TRANSITION,
                                                                   .initial = true,
                                                                   .id = compiler->initials.count,
                                                                   .span = params->spec->span });
        }
        if (result != TA_OK) {
            return result;
        }
    }
    if (params->spec != NULL && params->spec->targets != NULL && params->spec->targets[0].ptr != NULL) {
        initial.authored_targets = true;
        result = ta_compiler_targets(compiler, &(ta_compiler_targets_params){
                                                   .ids = params->spec->targets,
                                                   .within = params->owner,
                                                   .out = &initial.targets,
                                               });
    } else {
        /* SCXML default: the first child state in document order. */
        ta_state_id first = TA_STATE_NONE;
        for (uint32_t index = 0; index < params->children.length && first == TA_STATE_NONE; index += 1) {
            ta_state_id child = *(ta_state_id *)ta_vec_at(&compiler->children, params->children.offset + index);
            if (!ta_state_kind_is_history(ta_compiler_state(compiler, child)->kind)) {
                first = child;
            }
        }
        if (first == TA_STATE_NONE) {
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_BAD_STRUCTURE, (ta_str){0},
                                    TA_PHRASE("no child state to enter"));
        }
        initial.targets.offset = compiler->targets.count;
        initial.targets.length = 1;
        result = ta_vec_push(&compiler->targets, &first);
    }
    if (result == TA_OK && params->spec != NULL) {
        result = ta_compiler_block(compiler, TA_SPEC_MEMBER_ACTIONS, params->spec->actions, &initial.actions);
    }
    if (result == TA_OK) {
        if (params->spec != NULL) {
            ta_compiler_leave(compiler);
        }
        *params->out = compiler->initials.count;
        result = ta_vec_push(&compiler->initials, &initial);
    }
    return result;
}

static ta_result ta_compiler_data(ta_compiler *compiler, const ta_scxml_data_spec *specs, ta_state_id owner) {
    for (uint32_t index = 0; specs != NULL && specs[index].name.ptr != NULL; index += 1) {
        ta_data_decl decl = { .owner_state = owner, .initializer = TA_INITIALIZER_NONE };
        ta_result result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_DATA, index);
        if (result == TA_OK) {
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_NAME, specs[index].name, &decl.name);
        }
        if (result == TA_OK && specs[index].initializer.kind != TA_SCXML_VALUE_NONE) {
            ta_initializer_def initializer = {0};
            result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &specs[index].initializer,
                                          &initializer.value);
            if (result == TA_OK) {
                decl.initializer = compiler->initializers.count;
                result = ta_vec_push(&compiler->initializers, &initializer);
            }
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            result = ta_vec_push(&compiler->data_decls, &decl);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

/* <donedata>: exactly one of content or params (SCXML 5.5). */
static ta_result ta_compiler_done_data(ta_compiler *compiler, const ta_scxml_done_data_spec *spec,
                                       ta_done_data_id *out) {
    ta_done_data_def done = {0};
    ta_result result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_DONE_DATA, TA_SPEC_INDEX_NONE);
    if (result == TA_OK) {
        result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->content, &done.content);
    }
    if (result == TA_OK) {
        result = ta_compiler_params(compiler, spec->params, &done.params);
    }
    if (result == TA_OK && done.params.length != 0 && done.content.kind != TA_VALUE_SOURCE_NONE) {
        result = ta_compiler_fail(compiler, TA_COMPILE_ERROR_EXCLUSIVE_MEMBERS, (ta_str){0},
                                  TA_PHRASE("a donedata has both params and content"));
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
        *out = compiler->done_data.count;
        result = ta_vec_push(&compiler->done_data, &done);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Invoke
 */

static ta_result ta_compiler_invoke_text(ta_compiler *compiler, const ta_scxml_string_spec *spec,
                                         ta_value_source *out) {
    ta_compiler_hash_u32(compiler, spec->kind);
    switch (spec->kind) {
        case TA_SCXML_STRING_NONE:
            *out = (ta_value_source){ .kind = TA_VALUE_SOURCE_NONE };
            return TA_OK;
        case TA_SCXML_STRING_LITERAL:
            out->kind = TA_VALUE_SOURCE_CONTENT;
            return ta_compiler_content(compiler, (ta_str){0},
                                       (ta_bytes){ .ptr = (const uint8_t *)spec->as.literal.ptr,
                                                   .len = spec->as.literal.len },
                                       &out->as.content);
        case TA_SCXML_STRING_EXPR:
            out->kind = TA_VALUE_SOURCE_EXPR;
            return ta_compiler_expression(compiler, &(ta_compiler_expression_params){ &spec->as.expr, TA_EXPR_ROLE_VALUE },
                                          &out->as.expr);
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown kind"));
    }
}

static ta_result ta_compiler_invoke(ta_compiler *compiler, const ta_scxml_invoke_spec *spec,
                                    ta_invoke_decl *out) {
    ta_result result = TA_OK;
    switch (spec->id.kind) {
        case TA_SCXML_ID_NONE:
            out->id.kind = TA_ID_SOURCE_GENERATED;
            break;
        case TA_SCXML_ID_LITERAL:
            out->id.kind = TA_ID_SOURCE_LITERAL;
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_ID, spec->id.as.literal, &out->id.as.literal);
            break;
        case TA_SCXML_ID_LOCATION:
            out->id.kind = TA_ID_SOURCE_LOCATION;
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_ID, &spec->id.as.location, TA_EXPR_ROLE_LOCATION },
                                               &out->id.as.location);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_ID);
    }
    if (result != TA_OK) {
        return result;
    }
    switch (spec->type.kind) {
        case TA_SCXML_STRING_NONE:
            out->type.kind = TA_TEXT_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->type.kind = TA_TEXT_SOURCE_ATOM;
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_TYPE, spec->type.as.literal, &out->type.as.atom);
            break;
        case TA_SCXML_STRING_EXPR:
            out->type.kind = TA_TEXT_SOURCE_EXPR;
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_TYPE, &spec->type.as.expr, TA_EXPR_ROLE_VALUE },
                                               &out->type.as.expr);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_TYPE);
    }
    if (result == TA_OK) {
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_SRC, TA_SPEC_INDEX_NONE);
        if (result == TA_OK) {
            result = ta_compiler_invoke_text(compiler, &spec->src, &out->src);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
        }
    }
    if (result == TA_OK) {
        result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->data, &out->data);
    }
    if (result == TA_OK) {
        result = ta_compiler_params(compiler, spec->params, &out->params);
    }
    if (result == TA_OK) {
        result = ta_compiler_block(compiler, TA_SPEC_MEMBER_FINALIZE, spec->finalize, &out->finalize_actions);
    }
    out->autoforward = spec->autoforward;
    ta_compiler_hash_u32(compiler, spec->autoforward ? 1u : 0u);
    /* SCXML: src and inline content are exclusive. */
    if (result == TA_OK && out->src.kind != TA_VALUE_SOURCE_NONE && out->data.kind != TA_VALUE_SOURCE_NONE) {
        result = ta_compiler_fail(compiler, TA_COMPILE_ERROR_EXCLUSIVE_MEMBERS, (ta_str){0},
                                  TA_PHRASE("an invoke has both src and content"));
    }
    return result;
}

static ta_result ta_compiler_invokes(ta_compiler *compiler, const ta_scxml_invoke_spec *specs, ta_state_id owner) {
    for (uint32_t index = 0; specs[index].kind != TA_SCXML_INVOKE_NONE; index += 1) {
        ta_invoke_decl decl = { .owner = owner };
        ta_result result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_INVOKES, index);
        if (result == TA_OK) {
            result = ta_compiler_invoke(compiler, &specs[index], &decl);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            result = ta_vec_push(&compiler->invokes, &decl);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

static ta_result ta_compiler_structure(ta_compiler *compiler, ta_spec_member member, uint32_t index,
                                       ta_str message) {
    ta_compiler_fault fault = { .code = TA_COMPILE_ERROR_BAD_STRUCTURE, .message = message };
    return member == TA_SPEC_MEMBER_NONE ? ta_compiler_record(compiler, &fault)
                                         : ta_compiler_fail_at(compiler, member, index, &fault);
}

/* Pass two for one state. */
static ta_result ta_compiler_compile_state(ta_compiler *compiler, ta_state_id id) {
    const ta_scxml_state_spec *spec = ta_compiler_state_spec(compiler, id);
    ta_state_def state = *ta_compiler_state(compiler, id);
    ta_state_def parent = {0};
    if (state.parent != TA_STATE_NONE) {
        parent = *ta_compiler_state(compiler, state.parent);
    }
    ta_compiler_at(compiler, id);
    uint32_t real_children = 0;
    for (uint32_t index = 0; index < state.children.length; index += 1) {
        ta_state_id child = *(ta_state_id *)ta_vec_at(&compiler->children, state.children.offset + index);
        real_children += ta_state_kind_is_history(ta_compiler_state(compiler, child)->kind) ? 0u : 1u;
    }
    bool history = ta_state_kind_is_history(state.kind);
    if (spec->done_data != NULL && state.kind != TA_STATE_FINAL) {
        return ta_compiler_structure(compiler, TA_SPEC_MEMBER_DONE_DATA, TA_SPEC_INDEX_NONE,
                                     TA_PHRASE("only a final state has <donedata>"));
    }
    switch (state.kind) {
        case TA_STATE_ATOMIC:
        case TA_STATE_FINAL:
            if (state.children.length != 0) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_STATES, 0,
                                             TA_PHRASE("an atomic or final state has no child states"));
            }
            if (spec->initial != NULL) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_INITIAL, TA_SPEC_INDEX_NONE,
                                             TA_PHRASE("an atomic or final state has no <initial>"));
            }
            break;
        case TA_STATE_COMPOUND:
        case TA_STATE_PARALLEL:
            if (real_children == 0) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_NONE, 0,
                                             TA_PHRASE("a compound or parallel state needs a child state"));
            }
            if (state.kind == TA_STATE_PARALLEL && spec->initial != NULL) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_INITIAL, TA_SPEC_INDEX_NONE,
                                             TA_PHRASE("a parallel state has no <initial>"));
            }
            break;
        default:
            if (state.children.length != 0) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_STATES, 0,
                                             TA_PHRASE("a history state has no child states"));
            }
            if (state.parent == TA_STATE_NONE ||
                (parent.kind != TA_STATE_COMPOUND && parent.kind != TA_STATE_PARALLEL)) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_NONE, 0,
                                             TA_PHRASE("a history state must be inside a compound or parallel state"));
            }
            break;
    }

    state.transitions.offset = compiler->transitions.count;
    ta_result result = TA_OK;
    for (uint32_t index = 0; result == TA_OK && spec->transitions != NULL &&
                             spec->transitions[index].kind != TA_SCXML_TRANSITION_NONE;
         index += 1) {
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_TRANSITIONS, index);
        if (result == TA_OK) {
            result = ta_compiler_transition(compiler, id, &spec->transitions[index]);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
        }
    }
    state.transitions.length = compiler->transitions.count - state.transitions.offset;
    if (result != TA_OK) {
        return result;
    }
    if (history) {
        /* A history state has exactly one targetless-event default transition into its parent. */
        ta_compiler_fault fault = { .code = TA_COMPILE_ERROR_BAD_HISTORY_DEFAULT,
                                    .message = TA_PHRASE("a history state needs exactly one default transition") };
        if (state.transitions.length != 1) {
            return state.transitions.length == 0
                       ? ta_compiler_record(compiler, &fault)
                       : ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_TRANSITIONS, 1, &fault);
        }
        const ta_transition_def *fallback = ta_vec_at(&compiler->transitions, state.transitions.offset);
        if (fallback->event_patterns.length != 0 || fallback->condition != UINT32_MAX ||
            fallback->targets.length == 0) {
            fault.message = TA_PHRASE("a history default transition has targets and no event or condition");
            return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_TRANSITIONS, 0, &fault);
        }
        for (uint32_t index = 0; index < fallback->targets.length; index += 1) {
            ta_state_id target = *(ta_state_id *)ta_vec_at(&compiler->targets, fallback->targets.offset + index);
            if (!ta_compiler_descends(compiler, target, state.parent)) {
                result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_TRANSITIONS, 0);
                if (result == TA_OK) {
                    result = ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_TARGETS, index,
                                                 &(ta_compiler_fault){
                                                     .code = TA_COMPILE_ERROR_TARGET_OUTSIDE,
                                                     .subject = spec->transitions[0].targets[index],
                                                     .related = ta_compiler_state_spec(compiler, state.parent)->id,
                                                     .message = TA_PHRASE("target is not inside its state"),
                                                 });
                }
                return result;
            }
        }
        state.history_slot = compiler->history.count;
        ta_history_slot slot = {0};
        result = ta_vec_push(&compiler->history, &slot);
    }
    if (result == TA_OK && state.kind == TA_STATE_COMPOUND) {
        result = ta_compiler_initial(compiler, &(ta_compiler_initial_params){
                                                   .owner = id,
                                                   .spec = spec->initial,
                                                   .children = state.children,
                                                   .out = &state.initial,
                                               });
    }
    if (result == TA_OK) {
        result = ta_compiler_handlers(compiler, TA_SPEC_MEMBER_ONENTRY, spec->onentry, &state.onentry_handlers);
    }
    if (result == TA_OK) {
        result = ta_compiler_handlers(compiler, TA_SPEC_MEMBER_ONEXIT, spec->onexit, &state.onexit_handlers);
    }
    if (result == TA_OK) {
        result = ta_compiler_data(compiler, spec->data, id);
    }
    if (result == TA_OK && spec->done_data != NULL) {
        result = ta_compiler_done_data(compiler, spec->done_data, &state.done_data);
    }
    /* Invoke: compile this state's <invoke> children. */
    if (result == TA_OK && spec->invokes != NULL && spec->invokes[0].kind != TA_SCXML_INVOKE_NONE) {
        result = history ? ta_compiler_structure(compiler, TA_SPEC_MEMBER_INVOKES, 0,
                                                 TA_PHRASE("a history state has no <invoke>"))
                         : ta_compiler_invokes(compiler, spec->invokes, id);
    }
    if (result == TA_OK) {
        ta_atom segment = state.name;
        result = ta_event_name_child(&compiler->system->event_names, TA_EVENT_NAME_DONE_STATE, segment,
                                     &state.done_event);
    }
    if (result == TA_OK) {
        *ta_compiler_state(compiler, id) = state;
    }
    return result;
}

static ta_result ta_compiler_seal(ta_compiler *compiler, const ta_compiler_seal_params *params) {
    *params->out = NULL;
    if (params->count != NULL) {
        *params->count = params->vec->count;
    }
    if (params->vec->count == 0) {
        return TA_OK;
    }
    size_t size = params->vec->count * params->vec->element_size;
    void *copy = ta_arena_alloc(&compiler->definition->arena, size, params->vec->element_alignment);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, params->vec->items, size);
    *params->out = copy;
    return TA_OK;
}

static void ta_compiler_destroy(ta_compiler *compiler) {
    ta_vec *vecs[] = {
        &compiler->states, &compiler->state_specs, &compiler->children, &compiler->transitions,
        &compiler->initials, &compiler->targets, &compiler->actions, &compiler->handlers, &compiler->params,
        &compiler->data_decls, &compiler->initializers, &compiler->contents, &compiler->patterns,
        &compiler->segments, &compiler->expressions, &compiler->roles, &compiler->history, &compiler->done_data,
        &compiler->invokes, &compiler->texts, &compiler->path, &compiler->names, &compiler->levels,
        &compiler->blocks, &compiler->spans,
    };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(vecs); index += 1) {
        ta_vec_destroy(vecs[index]);
    }
}

/*
 * The side table of spans (ADR 0085), once every count is final: one span
 * per element, by kind and ID, and the names of the sources. Nothing is
 * kept when the spec knew no span, so a definition compiled without them
 * costs what it did before.
 */
static ta_result ta_compiler_lay_out_spans(ta_compiler *compiler) {
    ta_definition *definition = compiler->definition;
    if (compiler->spans.count == 0) {
        return TA_OK;
    }
    const ta_scxml_spec *spec = compiler->spec;
    uint32_t count = ta_definition_span_base(definition, TA_ELEMENT_EXPRESSION) +
                     ta_definition_element_count(definition, TA_ELEMENT_EXPRESSION);
    /* One block: the spans, the sources, then the sources' names. */
    size_t sources_at = (size_t)count * sizeof(ta_source_span);
    size_t names_at = sources_at + (size_t)spec->source_count * sizeof(ta_scxml_source);
    size_t bytes = names_at;
    for (uint32_t index = 0; index < spec->source_count; index += 1u) {
        bytes += spec->sources[index].name.len;
    }
    uint8_t *block = ta_mem_alloc(definition->allocator, bytes, _Alignof(ta_source_span));
    if (block == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_source_span *spans = (ta_source_span *)(void *)block;
    ta_mem_zero(spans, sources_at);
    for (uint32_t index = 0; index < compiler->spans.count; index += 1u) {
        const ta_span_record *record = ta_vec_at(&compiler->spans, index);
        uint32_t id = record->initial ? definition->transition_count + record->id : record->id;
        spans[ta_definition_span_base(definition, record->kind) + id] = spec->spans[record->span - 1u];
    }
    ta_scxml_source *sources = spec->source_count != 0 ? (ta_scxml_source *)(void *)(block + sources_at) : NULL;
    char *names = (char *)block + names_at;
    for (uint32_t index = 0; index < spec->source_count; index += 1u) {
        ta_str name = spec->sources[index].name;
        if (name.len != 0) {
            ta_mem_copy(names, name.ptr, name.len);
        }
        sources[index].name = (ta_str){ .ptr = names, .len = name.len };
        names += name.len;
    }
    definition->spans = spans;
    definition->span_count = count;
    definition->sources = sources;
    definition->source_count = spec->source_count;
    definition->span_bytes = bytes;
    definition->span_allocator = definition->allocator;
    return TA_OK;
}

static ta_result ta_compiler_identity(ta_compiler *compiler) {
    ta_definition *definition = compiler->definition;
    const ta_scxml_spec *spec = compiler->spec;
    ta_str family = spec->name;
    ta_bytes revision = {0};
    char fingerprint[16];
    /* An identity with an empty revision names only the family: the
     * revision is still the fingerprint (ADR 0077). */
    bool explicit_revision = spec->identity != NULL && spec->identity->revision.len != 0;
    definition->fingerprinted = !explicit_revision;
    /* Kept whatever the revision, so an explicit revision reused for other
     * content is caught at registration (ADR 0077). */
    definition->content_hash = compiler->hash;
    /* A family-only identity is the default for a chart with no name. */
    if (explicit_revision || (spec->identity != NULL && (family.ptr == NULL || family.len == 0))) {
        family = spec->identity->family.name;
    }
    if (explicit_revision) {
        revision = spec->identity->revision;
    } else {
        ta_hex_write(fingerprint, compiler->hash, 16u);
        revision = (ta_bytes){ .ptr = (const uint8_t *)fingerprint, .len = 16 };
    }
    if (family.ptr == NULL || family.len == 0) {
        return ta_compiler_fail_at(compiler, spec->identity != NULL ? TA_SPEC_MEMBER_IDENTITY : TA_SPEC_MEMBER_NAME,
                                   TA_SPEC_INDEX_NONE,
                                   &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_NO_FAMILY,
                                                         .message = TA_PHRASE("the chart has no name and no identity family") });
    }
    ta_result result = ta_arena_copy_str(&definition->arena, family, &definition->identity.family.name);
    ta_str revision_copy = {0};
    if (result == TA_OK) {
        result = ta_arena_copy_str(&definition->arena,
                                   (ta_str){ .ptr = (const char *)revision.ptr, .len = revision.len },
                                   &revision_copy);
    }
    definition->identity.revision = (ta_bytes){ .ptr = (const uint8_t *)revision_copy.ptr,
                                                .len = revision_copy.len };
    if (result == TA_OK) {
        result = ta_atom_intern(ta_compiler_atoms(compiler), family, &definition->family);
    }
    definition->document_name = TA_ATOM_NONE;
    if (result == TA_OK && spec->name.ptr != NULL && spec->name.len != 0) {
        result = ta_atom_intern(ta_compiler_atoms(compiler), spec->name, &definition->document_name);
    }
    definition->authored_binding = spec->binding;
    if (result == TA_OK && spec->datamodel.ptr != NULL && spec->datamodel.len != 0) {
        result = ta_arena_copy_str(&definition->arena, spec->datamodel, &definition->authored_datamodel);
    }
    return result;
}

static ta_result ta_compiler_finish(ta_compiler *compiler) {
    ta_definition *definition = compiler->definition;
    const ta_compiler_seal_params seals[] = {
        { &compiler->states, (void **)&definition->states, &definition->state_count },
        { &compiler->children, (void **)&definition->children, &definition->child_count },
        { &compiler->transitions, (void **)&definition->transitions, &definition->transition_count },
        { &compiler->initials, (void **)&definition->initial_transitions, &definition->initial_transition_count },
        { &compiler->targets, (void **)&definition->targets, &definition->target_count },
        { &compiler->actions, (void **)&definition->action_defs, &definition->action_count },
        { &compiler->handlers, (void **)&definition->handlers, &definition->handler_count },
        { &compiler->params, (void **)&definition->params, &definition->param_count },
        { &compiler->data_decls, (void **)&definition->data_decls, &definition->data_decl_count },
        { &compiler->initializers, (void **)&definition->initializers, &definition->initializer_count },
        { &compiler->contents, (void **)&definition->contents, &definition->content_count },
        { &compiler->patterns, (void **)&definition->event_patterns, &definition->event_pattern_count },
        { &compiler->segments, (void **)&definition->event_pattern_segments, &definition->event_pattern_segment_count },
        { &compiler->history, (void **)&definition->history.slots, &definition->history.slot_count },
        { &compiler->done_data, (void **)&definition->done_data, &definition->done_data_count },
        { &compiler->invokes, (void **)&definition->invokes, &definition->invoke_decl_count },
        { &compiler->texts, (void **)&definition->authored_texts, &definition->authored_text_count },
    };
    ta_result result = TA_OK;
    for (uint32_t index = 0; result == TA_OK && index < TA_ARRAY_COUNT(seals); index += 1) {
        result = ta_compiler_seal(compiler, &seals[index]);
    }
    if (result != TA_OK) {
        return result;
    }
    result = ta_definition_place_actions(definition);
    if (result != TA_OK) {
        return result;
    }
    definition->max_action_depth = compiler->action_depth;
    if (definition->global_script_actions.length != 0 && definition->max_action_depth == 0) {
        definition->max_action_depth = 1;
    }
    /* Depths in one pass: every parent has a lower ID than its children. */
    if (definition->state_count != 0) {
        size_t size = (size_t)definition->state_count * sizeof(uint32_t);
        uint32_t *depths = ta_mem_alloc(compiler->allocator, size, _Alignof(uint32_t));
        if (depths == NULL) {
            return TA_ERR_NOMEM;
        }
        for (ta_state_id state = 0; state < definition->state_count; state += 1u) {
            ta_state_id parent = definition->states[state].parent;
            depths[state] = parent == TA_STATE_NONE ? 1u : depths[parent] + 1u;
            if (depths[state] > definition->max_state_depth) {
                definition->max_state_depth = depths[state];
            }
        }
        ta_mem_free(compiler->allocator, depths,
                    &(ta_allocator_free_params){ .size = size, .alignment = _Alignof(uint32_t) });
    }
    uint32_t words = ta_bits_words(definition->state_count);
    if (words * 8u > UINT16_MAX) {
        return ta_compiler_fail(compiler, TA_COMPILE_ERROR_LIMIT, (ta_str){0},
                                TA_PHRASE("the chart has more states than a configuration can hold"));
    }
    definition->configuration = (ta_configuration_layout){
        .bytes = (uint16_t)(words * 8u),
        .atomic_state_count = definition->state_count,
    };
    for (uint32_t slot = 0; slot < definition->history.slot_count; slot += 1) {
        definition->history.slots[slot] = (ta_history_slot){ .offset = slot * words * 8u,
                                                             .bytes = (uint16_t)(words * 8u) };
    }
    definition->history.storage_bytes = definition->history.slot_count * words * 8u;
    return ta_compiler_identity(compiler);
}

/*
 * The compile order, which decides which fault a chart with several reports
 * (ADR 0058 section 2): the data-model requirement; pass one over every state
 * in document order (kind, ID, duplicates); the root's <data>; pass two for
 * each state in document order; the root <initial>; the root <script>; the
 * configuration limit and the family; then the data model's preparation.
 */
/* One history state on the walk below, and the next default target to follow. */
typedef struct {
    ta_state_id state;
    uint32_t next;
} ta_history_walk;

/* The default targets of a compiled history state. */
static ta_span32 ta_compiler_history_targets(ta_compiler *compiler, ta_state_id history) {
    const ta_state_def *state = ta_compiler_state(compiler, history);
    const ta_transition_def *fallback = ta_vec_at(&compiler->transitions, state->transitions.offset);
    return fallback->targets;
}

/*
 * A history state's default may target another history state, which is
 * then entered through its own default. That chain must end in real
 * states: SCXML 3.11 requires a target to become a legal configuration
 * once ancestors and defaults are added, and a loop never reaches an
 * atomic state. So a loop, `h` targeting `h` or `h1` and `h2` targeting
 * each other, is BAD_HISTORY_DEFAULT.
 *
 * A depth-first walk over the edges "this history's default targets that
 * history state", with an explicit stack (ADR 0060). Walks start from the
 * history states in document order and each state is finished once, so
 * the loop reported is the same every time. The error is at the target
 * that closes the loop, with the history it returns to as `related`.
 */
static ta_result ta_compiler_history_loops(ta_compiler *compiler) {
    uint32_t count = compiler->states.count;
    uint32_t histories = 0;
    for (ta_state_id state = 0; state < count; state += 1u) {
        histories += ta_state_kind_is_history(ta_compiler_state(compiler, state)->kind) ? 1u : 0u;
    }
    if (histories < 1u) {
        return TA_OK;
    }
    enum { UNSEEN = 0, ON_WALK = 1, FINISHED = 2 };
    ta_vec marks;
    ta_vec walk;
    ta_vec_init(&marks, compiler->allocator, TA_VEC_LAYOUT(uint8_t));
    ta_vec_init(&walk, compiler->allocator, TA_VEC_LAYOUT(ta_history_walk));
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&marks, count, &base);
    for (ta_state_id start = 0; result == TA_OK && start < count; start += 1u) {
        uint8_t *mark = marks.items;
        if (!ta_state_kind_is_history(ta_compiler_state(compiler, start)->kind) || mark[start] != UNSEEN) {
            continue;
        }
        mark[start] = ON_WALK;
        result = ta_vec_push(&walk, &(ta_history_walk){ .state = start });
        while (result == TA_OK && walk.count != 0) {
            ta_history_walk *top = ta_vec_at(&walk, walk.count - 1u);
            ta_span32 targets = ta_compiler_history_targets(compiler, top->state);
            if (top->next == targets.length) {
                ((uint8_t *)marks.items)[top->state] = FINISHED;
                walk.count -= 1u;
                continue;
            }
            uint32_t index = top->next;
            top->next += 1u;
            ta_state_id history = top->state;
            ta_state_id target = *(ta_state_id *)ta_vec_at(&compiler->targets, targets.offset + index);
            if (!ta_state_kind_is_history(ta_compiler_state(compiler, target)->kind)) {
                continue;
            }
            uint8_t seen = ((uint8_t *)marks.items)[target];
            if (seen == ON_WALK) {
                ta_compiler_at(compiler, history);
                result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_TRANSITIONS, 0);
                if (result == TA_OK) {
                    result = ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_TARGETS, index,
                                                 &(ta_compiler_fault){
                                                     .code = TA_COMPILE_ERROR_BAD_HISTORY_DEFAULT,
                                                     .subject = ta_compiler_state_spec(compiler, history)
                                                                    ->transitions[0].targets[index],
                                                     .related = ta_compiler_state_spec(compiler, target)->id,
                                                     .message = TA_PHRASE("history defaults loop and never reach a state"),
                                                 });
                }
            } else if (seen == UNSEEN) {
                ((uint8_t *)marks.items)[target] = ON_WALK;
                result = ta_vec_push(&walk, &(ta_history_walk){ .state = target });
            }
        }
    }
    ta_vec_destroy(&walk);
    ta_vec_destroy(&marks);
    return result;
}

static ta_result ta_compiler_run(ta_compiler *compiler, ta_datamodel datamodel) {
    const ta_scxml_spec *spec = compiler->spec;
    ta_definition *definition = compiler->definition;
    if (spec->datamodel.ptr != NULL && spec->datamodel.len != 0 && !ta_str_equal(spec->datamodel, datamodel.v->name)) {
        return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_DATAMODEL, TA_SPEC_INDEX_NONE,
                                   &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_DATAMODEL_MISMATCH,
                                                         .subject = spec->datamodel,
                                                         .message = TA_PHRASE("the chart requires another data model") });
    }
    ta_compiler_hash_str(compiler, spec->name);
    ta_compiler_hash_u32(compiler, spec->binding);
    ta_span32 roots = {0};
    ta_result result = ta_compiler_collect(compiler, spec->states, TA_STATE_NONE, &roots);
    result = ta_compiler_index_names(compiler, result);
    ta_compiler_at(compiler, TA_STATE_NONE);
    if (result == TA_OK && compiler->states.count == 0) {
        result = ta_compiler_fail(compiler, TA_COMPILE_ERROR_NO_STATES, (ta_str){0},
                                  TA_PHRASE("the chart has no states"));
    }
    /* Data declarations are listed root first, then by state in document order. */
    if (result == TA_OK) {
        result = ta_compiler_data(compiler, spec->data, TA_STATE_NONE);
    }
    for (ta_state_id state = 0; result == TA_OK && state < compiler->states.count; state += 1) {
        result = ta_compiler_compile_state(compiler, state);
    }
    if (result == TA_OK) {
        result = ta_compiler_history_loops(compiler);
    }
    if (result == TA_OK) {
        ta_compiler_at(compiler, TA_STATE_NONE);
        result = ta_compiler_initial(compiler, &(ta_compiler_initial_params){
                                                   .owner = TA_STATE_NONE,
                                                   .spec = spec->initial,
                                                   .children = roots,
                                                   .out = &definition->root_initial,
                                               });
    }
    if (result == TA_OK && spec->script.kind != TA_EXPRESSION_NONE) {
        uint32_t base = 0;
        ta_action_def script = { .kind = TA_ACTION_SCRIPT };
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_SCRIPT, TA_SPEC_INDEX_NONE);
        if (result == TA_OK) {
            result = ta_compiler_span(compiler, &(ta_span_record){ .kind = TA_ELEMENT_ACTION,
                                                                   .id = compiler->actions.count,
                                                                   .span = spec->script_span });
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            result = ta_compiler_expression_at(compiler, &(ta_compiler_expression_at_params){ TA_SPEC_MEMBER_SCRIPT, &spec->script, TA_EXPR_ROLE_SCRIPT }, &script.as.script);
        }
        if (result == TA_OK) {
            result = ta_vec_extend(&compiler->actions, 1, &base);
        }
        if (result == TA_OK) {
            *(ta_action_def *)ta_vec_at(&compiler->actions, base) = script;
            definition->global_script_actions = (ta_span32){ .offset = base, .length = 1 };
        }
    }
    if (result == TA_OK) {
        ta_compiler_at(compiler, TA_STATE_NONE);
        result = ta_compiler_finish(compiler);
    }
    return result;
}

static bool ta_datamodel_complete(const ta_datamodel *datamodel) {
    const ta_datamodel_vtable *v = datamodel->v;
    return v != NULL && v->prepare_definition != NULL && v->destroy_definition != NULL &&
           v->instance_size != NULL && v->instance_alignment != NULL && v->init_instance != NULL &&
           v->destroy_instance != NULL;
}

typedef struct {
    ta_compile_error *error;
    ta_arena *error_arena;
    /* UINT32_MAX for a compile; otherwise a replay that stops at this
     * expression ID and never prepares or keeps a definition. */
    uint32_t locate;
    ta_expression_rejection *rejected;
    ta_definition **out;
} ta_compile_run_params;

static ta_result ta_compile(ta_system *system, const ta_definition_compile_params *params,
                            const ta_compile_run_params *run) {
    const ta_scxml_spec *spec = params->spec;
    ta_definition *definition = ta_mem_alloc(params->allocator, sizeof(ta_definition),
                                             _Alignof(ta_definition));
    if (definition == NULL) {
        return TA_ERR_NOMEM;
    }
    *definition = (ta_definition){
        .allocator = params->allocator,
        .system = system,
        .root_initial = TA_INITIAL_TRANSITION_NONE,
        .binding_mode = spec->binding == TA_SCXML_BINDING_LATE ? TA_BINDING_LATE : TA_BINDING_EARLY,
        .datamodel = params->datamodel,
    };
    ta_result result = ta_arena_init(&definition->arena, params->allocator, 4096);
    if (result != TA_OK) {
        ta_mem_free(params->allocator, definition,
                    &(ta_allocator_free_params){ .size = sizeof(ta_definition),
                                                 .alignment = _Alignof(ta_definition) });
        return result;
    }

    ta_compiler compiler = {
        .system = system,
        .allocator = params->allocator,
        .spec = spec,
        .definition = definition,
        .path_state = TA_STATE_NONE,
        .error = run->error,
        .error_arena = run->error_arena,
        .locate = run->locate,
        .hash = TA_FNV1A64_BASIS,
    };
    const struct { ta_vec *vec; const ta_tree_list_layout *layout; } vecs[] = {
        { &compiler.states, TA_VEC_LAYOUT(ta_state_def) },
        { &compiler.state_specs, TA_VEC_LAYOUT(const ta_scxml_state_spec *) },
        { &compiler.children, TA_VEC_LAYOUT(ta_state_id) },
        { &compiler.transitions, TA_VEC_LAYOUT(ta_transition_def) },
        { &compiler.initials, TA_VEC_LAYOUT(ta_initial_transition_def) },
        { &compiler.targets, TA_VEC_LAYOUT(ta_state_id) },
        { &compiler.actions, TA_VEC_LAYOUT(ta_action_def) },
        { &compiler.handlers, TA_VEC_LAYOUT(ta_span32) },
        { &compiler.params, TA_VEC_LAYOUT(ta_param_def) },
        { &compiler.data_decls, TA_VEC_LAYOUT(ta_data_decl) },
        { &compiler.initializers, TA_VEC_LAYOUT(ta_initializer_def) },
        { &compiler.contents, TA_VEC_LAYOUT(ta_content_def) },
        { &compiler.patterns, TA_VEC_LAYOUT(ta_event_pattern) },
        { &compiler.segments, TA_VEC_LAYOUT(ta_event_pattern_segment) },
        { &compiler.expressions, TA_VEC_LAYOUT(ta_expression) },
        { &compiler.roles, TA_VEC_LAYOUT(ta_expr_role) },
        { &compiler.history, TA_VEC_LAYOUT(ta_history_slot) },
        { &compiler.done_data, TA_VEC_LAYOUT(ta_done_data_def) },
        { &compiler.invokes, TA_VEC_LAYOUT(ta_invoke_decl) },
        { &compiler.texts, TA_VEC_LAYOUT(ta_str) },
        { &compiler.path, TA_VEC_LAYOUT(ta_spec_step) },
        { &compiler.names, TA_VEC_LAYOUT(ta_state_key) },
        { &compiler.levels, TA_VEC_LAYOUT(ta_collect_frame) },
        { &compiler.blocks, TA_VEC_LAYOUT(ta_block_frame) },
        { &compiler.spans, TA_VEC_LAYOUT(ta_span_record) },
    };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(vecs); index += 1) {
        ta_vec_init(vecs[index].vec, params->allocator, vecs[index].layout);
    }

    result = ta_compiler_run(&compiler, params->datamodel);
    if ((result == TA_ERR_INVALID || result == TA_ERR_UNSUPPORTED) && !compiler.failed) {
        /* A refusal from below the compiler's own checks. */
        ta_compiler_at(&compiler, TA_STATE_NONE);
        (void)ta_compiler_fail(&compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                               TA_PHRASE("the spec was refused"));
    }
    /* The expression table moves to the definition even on failure, so it is released once. */
    ta_result sealed = ta_compiler_seal(&compiler, &(ta_compiler_seal_params){
                                                       .vec = &compiler.expressions,
                                                       .out = (void **)&definition->expressions,
                                                       .count = &definition->expression_count,
                                                   });
    if (sealed == TA_OK) {
        sealed = ta_compiler_seal(&compiler, &(ta_compiler_seal_params){
                                                 .vec = &compiler.roles,
                                                 .out = (void **)&definition->expression_roles,
                                             });
    }
    if (sealed != TA_OK) {
        /* Could not move host expressions; release them from the vector. */
        for (uint32_t index = 0; index < compiler.expressions.count; index += 1) {
            ta_expression *expression = ta_vec_at(&compiler.expressions, index);
            if (expression->kind == TA_EXPRESSION_HOST && expression->as.host.v->release != NULL) {
                expression->as.host.v->release(expression->as.host.self, expression->as.host.value,
                                               params->allocator);
            }
        }
        definition->expression_count = 0;
        result = result == TA_OK ? sealed : result;
    }
    if (result == TA_OK && run->locate == UINT32_MAX) {
        result = ta_compiler_lay_out_spans(&compiler);
    }
    ta_compiler_destroy(&compiler);

    if (run->locate != UINT32_MAX) {
        ta_definition_destroy(definition);
        return result;
    }
    if (result == TA_OK) {
        result = params->datamodel.v->prepare_definition(
            params->datamodel.self,
            &(ta_datamodel_prepare_definition_params){
                .expressions = definition->expressions,
                .roles = definition->expression_roles,
                .expression_count = definition->expression_count,
                .allocator = params->allocator,
                .shared = &definition->datamodel_shared,
                .rejected = run->rejected,
            });
        definition->datamodel_prepared = result == TA_OK;
    }
    if (result == TA_OK) {
        definition->datamodel_instance_size =
            params->datamodel.v->instance_size(params->datamodel.self, definition->datamodel_shared);
        definition->datamodel_instance_alignment =
            params->datamodel.v->instance_alignment(params->datamodel.self, definition->datamodel_shared);
        if (definition->datamodel_instance_alignment == 0) {
            definition->datamodel_instance_alignment = 1;
        }
        result = ta_actor_layout(definition);
    }
    if (result != TA_OK) {
        ta_definition_destroy(definition);
        return result;
    }
    definition->unregistered_next = system->unregistered_definitions;
    if (definition->unregistered_next != NULL) {
        definition->unregistered_next->unregistered_previous = definition;
    }
    system->unregistered_definitions = definition;
    *run->out = definition;
    return TA_OK;
}

/*
 * The data model refused the prepared expression table. Name the expression
 * it pointed at by replaying the walk to it: expression IDs are assigned in
 * walk order, so nothing is stored per expression on the success path.
 */
/* The longest prefix of `text` of at most `limit` bytes that does not end
 * inside a UTF-8 sequence. */
static uint32_t ta_utf8_prefix(ta_str text, uint32_t limit) {
    if (text.len <= limit) {
        return text.len;
    }
    uint32_t end = limit;
    while (end > 0 && ((unsigned char)text.ptr[end] & 0xC0u) == 0x80u) {
        end -= 1u;
    }
    return end;
}

static void ta_compile_rejected(ta_system *system, const ta_definition_compile_params *params,
                                const ta_expression_rejection *rejection) {
    ta_compile_error *error = params->error;
    /* Copied first: the message is the adapter's until core calls it again. */
    ta_str message = TA_PHRASE("the data model refused this expression");
    if (params->error_arena != NULL && rejection->message.ptr != NULL && rejection->message.len != 0) {
        ta_str copy = {0};
        ta_str kept = { .ptr = rejection->message.ptr,
                        .len = ta_utf8_prefix(rejection->message, system->params[TA_PARAM_COMPILE_MESSAGE_BYTES]) };
        if (ta_arena_copy_str(params->error_arena, kept, &copy) == TA_OK) {
            message = copy;
        }
    }
    if (rejection->expression != TA_EXPR_ID_NONE) {
        ta_result replayed = ta_compile(system, params, &(ta_compile_run_params){
                                                            .error = error,
                                                            .error_arena = params->error_arena,
                                                            .locate = rejection->expression,
                                                        });
        if (replayed == TA_OK) {
            *error = (ta_compile_error){0}; /* no such expression: about none */
        }
    }
    error->code = TA_COMPILE_ERROR_EXPRESSION_REJECTED;
    if (rejection->expression != TA_EXPR_ID_NONE && rejection->offset <= error->subject.len &&
        rejection->length <= error->subject.len - rejection->offset) {
        error->subject_offset = rejection->offset;
        error->subject_length = rejection->length;
    }
    error->message = message;
}

/*
 * A compiled definition whose interpreter needs more scratch than the
 * system's interpreterBytes (ADR 0092 section 3) is refused here, before it
 * can be registered, with both figures in the error.
 */
static ta_result ta_compile_check_scratch(ta_system *system, const ta_definition_compile_params *params) {
    ta_definition *definition = *params->out;
    size_t need;
    if (ta_interpreter_scratch_need(definition, &need) != TA_OK) {
        need = SIZE_MAX; /* more than any system has: the refusal below */
    }
    uint32_t have = system->params[TA_PARAM_INTERPRETER_BYTES];
    if (need <= have) {
        return TA_OK;
    }
    ta_definition_destroy(definition);
    *params->out = NULL;
    if (params->error == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_str message = TA_PHRASE("the chart needs more interpreter scratch than this system has (interpreterBytes)");
    char text[128];
    ta_text_writer writer = { .buffer = text, .capacity = sizeof text };
    ta_text_put(&writer, TA_PHRASE("the chart needs "));
    ta_text_put_u64(&writer, (uint64_t)need);
    ta_text_put(&writer, TA_PHRASE(" bytes of interpreter scratch; this system has "));
    ta_text_put_u64(&writer, have);
    ta_text_put(&writer, TA_PHRASE(" (interpreterBytes)"));
    uint32_t needed = 0;
    ta_str copy = { 0 };
    if (params->error_arena != NULL && ta_text_finish(&writer, &needed) == TA_OK &&
        ta_arena_copy_str(params->error_arena, (ta_str){ .ptr = text, .len = writer.length }, &copy) == TA_OK) {
        message = copy;
    }
    *params->error = (ta_compile_error){ .code = TA_COMPILE_ERROR_LIMIT, .message = message };
    return TA_ERR_UNSUPPORTED;
}

TA_API ta_result ta_definition_compile_spec(ta_system *system, const ta_definition_compile_params *params) {
    if (params != NULL && params->error != NULL) {
        *params->error = (ta_compile_error){0};
    }
    if (system == NULL || params == NULL || params->spec == NULL || params->out == NULL ||
        params->allocator.v == NULL || !ta_datamodel_complete(&params->datamodel)) {
        return TA_ERR_INVALID;
    }
    ta_expression_rejection rejection = { .expression = TA_EXPR_ID_NONE };
    ta_compile_error error = {0};
    ta_result result = ta_compile(system, params, &(ta_compile_run_params){
                                                      .error = params->error != NULL ? &error : NULL,
                                                      .error_arena = params->error_arena,
                                                      .locate = UINT32_MAX,
                                                      .rejected = &rejection,
                                                      .out = params->out,
                                                  });
    if (result == TA_OK) {
        return ta_compile_check_scratch(system, params);
    }
    if (params->error == NULL || (result != TA_ERR_INVALID && result != TA_ERR_UNSUPPORTED)) {
        return result;
    }
    if (error.code == TA_COMPILE_ERROR_NONE) {
        /* The compiler passed the spec and the data model refused it. */
        *params->error = (ta_compile_error){0};
        ta_compile_rejected(system, params, &rejection);
        return result;
    }
    *params->error = error;
    return result;
}

/* ------------------------------------------------------------------------
 * Formatting
 */

/*
 * The formatter's output: one buffer of compileTextBytes, allocated once.
 * Appending past the end is cut off, which the field limits below keep from
 * happening; it is the backstop, not the rule.
 */
typedef struct {
    char *bytes;
    uint32_t length;
    uint32_t capacity;
} ta_text_builder;

static void ta_text_append(ta_text_builder *text, ta_str piece) {
    uint32_t room = text->capacity - text->length;
    uint32_t count = ta_utf8_prefix(piece, room);
    if (count != 0) {
        ta_mem_copy(text->bytes + text->length, piece.ptr, count);
        text->length += count;
    }
}

static void ta_text_append_u32(ta_text_builder *text, uint32_t value) {
    char digits[TA_DEC_U64_DIGITS];
    uint32_t count = ta_dec_write(digits, value);
    ta_text_append(text, (ta_str){ .ptr = digits, .len = count });
}

/*
 * Output limits for one field, and how much of a deep path is shown, from
 * the line's size (compileTextBytes, ADR 0092): IDs, the subject and related
 * text get a sixteenth, the message three thirty-seconds, and the path what
 * is left once those and its elision marker have their worst case, a
 * quarter of its steps at the head. At 1024 bytes that is 64, 96, and 2 and
 * 6 steps, the limits this formatter always had.
 */
typedef struct {
    uint32_t id;
    uint32_t text;
    uint32_t head;
    uint32_t tail;
} ta_format_limits;

static ta_format_limits ta_format_limits_for(uint32_t size) {
    ta_format_limits limits = { .id = size / 16u, .text = size * 3u / 32u };
    /* ": " message ' "' subject '"' "; it must be inside \"" related '"' */
    uint32_t fixed = 2u + limits.text + 2u + limits.id + 1u + 22u + limits.id + 1u;
    uint32_t elision = 19u; /* ".[4294967295 steps]" */
    /* "." member "[" index "]" "(" id ")": the longest member name is 11 bytes */
    uint32_t step = 1u + 11u + 12u + 2u + limits.id;
    uint32_t steps = size > fixed + elision ? (size - fixed - elision) / step : 0u;
    if (steps < 2u) {
        steps = 2u;
    }
    limits.head = steps / 4u != 0 ? steps / 4u : 1u;
    limits.tail = steps - limits.head;
    return limits;
}

/*
 * The length in bytes of the unit of `text` starting at `at`: a well-formed
 * UTF-8 sequence, or one byte that is not part of one.
 */
static uint32_t ta_utf8_unit(ta_str text, uint32_t at) {
    unsigned char lead = (unsigned char)text.ptr[at];
    uint32_t length = lead < 0xC0u ? 1u : lead < 0xE0u ? 2u : lead < 0xF0u ? 3u : lead < 0xF8u ? 4u : 1u;
    if (length > text.len - at) {
        return 1u;
    }
    for (uint32_t index = 1; index < length; index += 1u) {
        if (((unsigned char)text.ptr[at + index] & 0xC0u) != 0x80u) {
            return 1u;
        }
    }
    return length;
}

/* How a unit is written: escaped when it would break the line, or, in a
 * quoted field, the quotes. */
static uint32_t ta_text_escape(ta_str text, uint32_t at, uint32_t unit, bool quoted, char out[4]) {
    unsigned char byte = (unsigned char)text.ptr[at];
    if (unit == 1u && (byte < 0x20u || byte == 0x7Fu || byte >= 0x80u)) {
        if (byte == '\n' || byte == '\t' || byte == '\r') {
            out[0] = '\\';
            out[1] = byte == '\n' ? 'n' : byte == '\t' ? 't' : 'r';
            return 2u;
        }
        out[0] = '\\';
        out[1] = 'x';
        out[2] = ta_hex_digit(byte >> 4, false);
        out[3] = ta_hex_digit(byte, false);
        return 4u;
    }
    if (quoted && unit == 1u && (byte == '"' || byte == '\\')) {
        out[0] = '\\';
        out[1] = (char)byte;
        return 2u;
    }
    ta_mem_copy(out, text.ptr + at, unit);
    return unit;
}

/*
 * Appends `value` escaped, in at most `limit` bytes of output. When it does
 * not fit, it is cut at a unit boundary and ends in "...". A `quoted` field
 * also escapes quotes and backslashes.
 */
static void ta_text_append_field(ta_text_builder *text, ta_str value, uint32_t limit, bool quoted) {
    uint32_t total = 0;
    char unit_text[4];
    for (uint32_t at = 0; at < value.len && total <= limit;) {
        uint32_t unit = ta_utf8_unit(value, at);
        total += ta_text_escape(value, at, unit, quoted, unit_text);
        at += unit;
    }
    uint32_t budget = total <= limit ? limit : limit - 3u;
    uint32_t written = 0;
    for (uint32_t at = 0; at < value.len;) {
        uint32_t unit = ta_utf8_unit(value, at);
        uint32_t size = ta_text_escape(value, at, unit, quoted, unit_text);
        if (written + size > budget) {
            break;
        }
        ta_text_append(text, (ta_str){ .ptr = unit_text, .len = size });
        written += size;
        at += unit;
    }
    if (total > limit) {
        ta_text_append(text, TA_PHRASE("..."));
    }
}

static ta_str ta_spec_member_name(ta_spec_member member) {
    switch (member) {
        case TA_SPEC_MEMBER_STATES: return TA_PHRASE("states");
        case TA_SPEC_MEMBER_TRANSITIONS: return TA_PHRASE("transitions");
        case TA_SPEC_MEMBER_TARGETS: return TA_PHRASE("targets");
        case TA_SPEC_MEMBER_ONENTRY: return TA_PHRASE("onentry");
        case TA_SPEC_MEMBER_ONEXIT: return TA_PHRASE("onexit");
        case TA_SPEC_MEMBER_ACTIONS: return TA_PHRASE("actions");
        case TA_SPEC_MEMBER_THEN: return TA_PHRASE("then");
        case TA_SPEC_MEMBER_ELSE: return TA_PHRASE("else");
        case TA_SPEC_MEMBER_BODY: return TA_PHRASE("body");
        case TA_SPEC_MEMBER_FINALIZE: return TA_PHRASE("finalize");
        case TA_SPEC_MEMBER_INVOKES: return TA_PHRASE("invokes");
        case TA_SPEC_MEMBER_DATA: return TA_PHRASE("data");
        case TA_SPEC_MEMBER_PARAMS: return TA_PHRASE("params");
        case TA_SPEC_MEMBER_INITIAL: return TA_PHRASE("initial");
        case TA_SPEC_MEMBER_DONE_DATA: return TA_PHRASE("done_data");
        case TA_SPEC_MEMBER_SCRIPT: return TA_PHRASE("script");
        case TA_SPEC_MEMBER_ID: return TA_PHRASE("id");
        case TA_SPEC_MEMBER_NAME: return TA_PHRASE("name");
        case TA_SPEC_MEMBER_EVENT: return TA_PHRASE("event");
        case TA_SPEC_MEMBER_CONDITION: return TA_PHRASE("condition");
        case TA_SPEC_MEMBER_TYPE: return TA_PHRASE("type");
        case TA_SPEC_MEMBER_SRC: return TA_PHRASE("src");
        case TA_SPEC_MEMBER_TARGET: return TA_PHRASE("target");
        case TA_SPEC_MEMBER_DELAY: return TA_PHRASE("delay");
        case TA_SPEC_MEMBER_DATA_VALUE: return TA_PHRASE("value");
        case TA_SPEC_MEMBER_LOCATION: return TA_PHRASE("location");
        case TA_SPEC_MEMBER_EXPR: return TA_PHRASE("expr");
        case TA_SPEC_MEMBER_ITEM: return TA_PHRASE("item");
        case TA_SPEC_MEMBER_INDEX: return TA_PHRASE("index");
        case TA_SPEC_MEMBER_SENDID: return TA_PHRASE("sendid");
        case TA_SPEC_MEMBER_DATAMODEL: return TA_PHRASE("datamodel");
        case TA_SPEC_MEMBER_IDENTITY: return TA_PHRASE("identity");
        case TA_SPEC_MEMBER_SPAN: return TA_PHRASE("span");
        default: return TA_PHRASE("?");
    }
}

TA_API ta_result ta_compile_error_format(const ta_compile_error_format_params *params) {
    if (params == NULL || params->error == NULL || params->arena == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_compile_error *error = params->error;
    uint32_t size = params->system != NULL ? params->system->params[TA_PARAM_COMPILE_TEXT_BYTES]
                                           : (uint32_t)TA_PARAM_DEFAULT_COMPILE_TEXT_BYTES;
    ta_format_limits limits = ta_format_limits_for(size);
    ta_text_builder text = { .bytes = ta_arena_alloc(params->arena, size, 1), .capacity = size };
    if (text.bytes == NULL) {
        return TA_ERR_NOMEM;
    }
    /*
     * A deep path shows its first and last steps. States are resolved while
     * the path goes through states only, so the walk follows every step,
     * shown or not.
     */
    const ta_scxml_state_spec *list = params->spec != NULL ? params->spec->states : NULL;
    uint32_t length = error->path_length;
    bool elide = length > limits.head + limits.tail;
    for (uint32_t index = 0; index < length; index += 1u) {
        ta_spec_step step = error->path[index];
        const ta_scxml_state_spec *state = NULL;
        if (step.member == TA_SPEC_MEMBER_STATES && list != NULL) {
            state = &list[step.index];
            list = state->states;
        } else {
            list = NULL;
        }
        bool shown = !elide || index < limits.head || index >= length - limits.tail;
        if (elide && index == limits.head) {
            ta_text_append(&text, TA_PHRASE(".["));
            ta_text_append_u32(&text, length - limits.head - limits.tail);
            ta_text_append(&text, TA_PHRASE(" steps]"));
        }
        if (!shown) {
            continue;
        }
        if (index != 0) {
            ta_text_append(&text, TA_PHRASE("."));
        }
        ta_text_append(&text, ta_spec_member_name(step.member));
        if (step.index != TA_SPEC_INDEX_NONE) {
            ta_text_append(&text, TA_PHRASE("["));
            ta_text_append_u32(&text, step.index);
            ta_text_append(&text, TA_PHRASE("]"));
        }
        if (state != NULL) {
            ta_text_append(&text, TA_PHRASE("("));
            ta_text_append_field(&text, state->id, limits.id, false);
            ta_text_append(&text, TA_PHRASE(")"));
        }
    }
    if (length != 0) {
        ta_text_append(&text, TA_PHRASE(": "));
    }
    ta_text_append_field(&text, error->message.len != 0 ? error->message : TA_PHRASE("the spec was refused"),
                         limits.text, false);
    if (error->subject.len != 0) {
        ta_text_append(&text, TA_PHRASE(" \""));
        ta_text_append_field(&text, error->subject, limits.id, true);
        ta_text_append(&text, TA_PHRASE("\""));
    }
    if (error->related.len != 0) {
        switch (error->code) {
            case TA_COMPILE_ERROR_UNKNOWN_STATE:
                ta_text_append(&text, TA_PHRASE("; nearest is \""));
                break;
            case TA_COMPILE_ERROR_TARGET_OUTSIDE:
                ta_text_append(&text, TA_PHRASE("; it must be inside \""));
                break;
            default:
                ta_text_append(&text, TA_PHRASE("; see also \""));
                break;
        }
        ta_text_append_field(&text, error->related, limits.id, true);
        ta_text_append(&text, TA_PHRASE("\""));
    }
    *params->out = (ta_str){ .ptr = text.bytes, .len = text.length };
    return TA_OK;
}


/*
 * Rebuilds the author-level ta_scxml_spec of a compiled definition. The
 * walk mirrors the compiler (builder.c) so that recompiling the result hashes
 * the same input in the same order: every list is rebuilt in the order the
 * compiler recorded it, and input the compiler normalizes comes from the
 * authored text the definition keeps (delay literals, transition event
 * descriptors, authored initial targets, the <scxml> binding and datamodel).
 */
typedef struct {
    const ta_system *system;
    const ta_definition *definition;
    ta_arena *arena;
    /* The first failure; later allocations are skipped. */
    ta_result result;
    /* Action blocks still to export, each with the member that receives it
     * (ADR 0060: no recursion). At most one per block, so the capacity is
     * bounded by the definition. Lifetime: one export call; arena memory,
     * allocated at the first ta_export_actions and never grown or freed on
     * its own. Deliberately not a ta_vec (vec.h). */
    struct ta_export_block *pending;
    uint32_t pending_count;
    uint32_t pending_capacity;
    /* Whether every exported element indexes the spans table, known span or
     * not, as an encode asks (ADR 0085). Otherwise only the elements whose
     * span the definition knows do. */
    bool numbered;
} ta_exporter;

struct ta_export_block {
    ta_span32 block;
    const ta_scxml_action_spec **out;
};

/*
 * The `span` of an exported element: its place in the spans table, which is
 * laid out as the definition's, plus 1; 0 when it has no known span and the
 * caller did not ask for every element.
 */
static uint32_t ta_export_span(const ta_exporter *exporter, ta_element_kind kind, uint32_t id) {
    const ta_definition *definition = exporter->definition;
    uint32_t place = ta_definition_span_base(definition, kind) + id;
    if (!exporter->numbered && (definition->spans == NULL || definition->spans[place].line == 0)) {
        return 0;
    }
    return place + 1u;
}

/* Non-null text for an empty literal, which the RPC codecs keep apart from absent. */
static const char ta_export_empty[] = "";

/*
 * Allocates `count` zeroed list elements plus the zero terminator. An empty
 * list is null.
 */
static void *ta_export_list(ta_exporter *exporter, const ta_tree_list_layout *layout, uint32_t count) {
    if (count == 0 || exporter->result != TA_OK) {
        return NULL;
    }
    size_t size = 0;
    void *items = NULL;
    if (ta_checked_mul(layout->element_size, (size_t)count + 1u, &size)) {
        items = ta_arena_alloc(exporter->arena, size, layout->element_alignment);
    }
    if (items == NULL) {
        exporter->result = TA_ERR_NOMEM;
        return NULL;
    }
    ta_mem_zero(items, size);
    return items;
}

static ta_str ta_export_atom(const ta_exporter *exporter, ta_atom atom) {
    ta_str text = ta_atom_text(&exporter->system->atoms, atom);
    return text.ptr == NULL ? (ta_str){ .ptr = ta_export_empty, .len = 0 } : text;
}

static ta_str ta_export_state_id(const ta_exporter *exporter, ta_state_id state) {
    return ta_export_atom(exporter, exporter->definition->states[state].name);
}

static ta_str ta_export_authored(const ta_exporter *exporter, ta_authored_text_id text) {
    if (text == TA_AUTHORED_TEXT_NONE) {
        return (ta_str){0};
    }
    return exporter->definition->authored_texts[text];
}

/* A dotted event name. The compiler rejects empty segments, so joining them restores the text. */
static ta_str ta_export_event_name(ta_exporter *exporter, ta_event_name_id name) {
    ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
    uint32_t count = ta_event_name_segments(&exporter->system->event_names, name, segments);
    if (count == 1) {
        return ta_export_atom(exporter, segments[0]);
    }
    size_t length = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        length += ta_atom_text(&exporter->system->atoms, segments[index]).len + (index == 0 ? 0u : 1u);
    }
    char *text = exporter->result == TA_OK ? ta_arena_alloc(exporter->arena, length + 1u, 1) : NULL;
    if (text == NULL) {
        exporter->result = exporter->result == TA_OK ? TA_ERR_NOMEM : exporter->result;
        return (ta_str){ .ptr = ta_export_empty, .len = 0 };
    }
    size_t at = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        ta_str segment = ta_atom_text(&exporter->system->atoms, segments[index]);
        if (index != 0) {
            text[at++] = '.';
        }
        if (segment.len != 0) {
            ta_mem_copy(text + at, segment.ptr, segment.len);
        }
        at += segment.len;
    }
    text[at] = '\0';
    return (ta_str){ .ptr = text, .len = (uint32_t)length };
}

static ta_expression ta_export_expression(const ta_exporter *exporter, ta_expr_id expr) {
    ta_expression expression = exporter->definition->expressions[expr];
    expression.span = ta_export_span(exporter, TA_ELEMENT_EXPRESSION, expr);
    return expression;
}

static ta_bytes ta_export_content(const ta_exporter *exporter, ta_content_id content) {
    ta_bytes bytes = exporter->definition->contents[content].bytes;
    return bytes.ptr == NULL ? (ta_bytes){ .ptr = (const uint8_t *)ta_export_empty, .len = 0 } : bytes;
}

static ta_scxml_value_spec ta_export_value(const ta_exporter *exporter, const ta_value_source *source) {
    ta_scxml_value_spec out = {0};
    switch (source->kind) {
        case TA_VALUE_SOURCE_EXPR:
            out.kind = TA_SCXML_VALUE_EXPR;
            out.as.expr = ta_export_expression(exporter, source->as.expr);
            break;
        case TA_VALUE_SOURCE_LOCATION:
            out.kind = TA_SCXML_VALUE_LOCATION;
            out.as.location = ta_export_expression(exporter, source->as.location);
            break;
        case TA_VALUE_SOURCE_CONTENT:
            out.kind = TA_SCXML_VALUE_CONTENT;
            /* The media type round-trips, so a decoded chart recompiles to
             * the same revision (ADR 0053). */
            out.as.content.media_type = exporter->definition->contents[source->as.content].media_type;
            out.as.content.bytes = ta_export_content(exporter, source->as.content);
            break;
        default:
            break;
    }
    return out;
}

/* A `type` / `typeexpr` or `sendid` / `sendidexpr` pair. */
static ta_scxml_string_spec ta_export_text_source(const ta_exporter *exporter, const ta_text_source *source) {
    ta_scxml_string_spec out = {0};
    if (source->kind == TA_TEXT_SOURCE_ATOM) {
        out.kind = TA_SCXML_STRING_LITERAL;
        out.as.literal = ta_export_atom(exporter, source->as.atom);
    } else if (source->kind == TA_TEXT_SOURCE_EXPR) {
        out.kind = TA_SCXML_STRING_EXPR;
        out.as.expr = ta_export_expression(exporter, source->as.expr);
    }
    return out;
}

/* A send `target` or invoke `src`: the compiler stores literal text as content. */
static ta_scxml_string_spec ta_export_literal_source(const ta_exporter *exporter, const ta_value_source *source) {
    ta_scxml_string_spec out = {0};
    if (source->kind == TA_VALUE_SOURCE_CONTENT) {
        ta_bytes bytes = ta_export_content(exporter, source->as.content);
        out.kind = TA_SCXML_STRING_LITERAL;
        out.as.literal = (ta_str){ .ptr = (const char *)bytes.ptr, .len = bytes.len };
    } else if (source->kind == TA_VALUE_SOURCE_EXPR) {
        out.kind = TA_SCXML_STRING_EXPR;
        out.as.expr = ta_export_expression(exporter, source->as.expr);
    }
    return out;
}

static ta_scxml_id_spec ta_export_id(const ta_exporter *exporter, const ta_id_source *source) {
    ta_scxml_id_spec out = {0};
    if (source->kind == TA_ID_SOURCE_LITERAL) {
        out.kind = TA_SCXML_ID_LITERAL;
        out.as.literal = ta_export_atom(exporter, source->as.literal);
    } else if (source->kind == TA_ID_SOURCE_LOCATION) {
        out.kind = TA_SCXML_ID_LOCATION;
        out.as.location = ta_export_expression(exporter, source->as.location);
    }
    return out;
}

static const ta_scxml_param_spec *ta_export_params(ta_exporter *exporter, ta_span32 span) {
    ta_scxml_param_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_param_spec), span.length);
    for (uint32_t index = 0; items != NULL && index < span.length; index += 1) {
        const ta_param_def *param = &exporter->definition->params[span.offset + index];
        items[index] = (ta_scxml_param_spec){
            .name = ta_export_atom(exporter, param->name),
            .value = ta_export_value(exporter, &param->value),
        };
    }
    return items;
}

/* Queues a nested block for ta_export_actions to fill into `*out`. */
static void ta_export_later(ta_exporter *exporter, ta_span32 block, const ta_scxml_action_spec **out) {
    *out = NULL;
    if (block.length == 0 || exporter->result != TA_OK) {
        return;
    }
    if (exporter->pending_count == exporter->pending_capacity) {
        exporter->result = TA_ERR_NOMEM; /* unreachable: the capacity counts every block */
        return;
    }
    exporter->pending[exporter->pending_count++] = (struct ta_export_block){ .block = block, .out = out };
}

static void ta_export_send(ta_exporter *exporter, const ta_send_action *send, ta_scxml_send_spec *out) {
    out->id = ta_export_id(exporter, &send->id);
    out->type = ta_export_text_source(exporter, &send->type);
    if (send->event.kind == TA_EVENT_SOURCE_EXPR) {
        out->event.kind = TA_SCXML_EVENT_EXPR;
        out->event.as.expr = ta_export_expression(exporter, send->event.as.expr);
    } else {
        out->event.kind = TA_SCXML_EVENT_NAME;
        out->event.as.name = ta_export_event_name(exporter, send->event.as.name);
    }
    out->target = ta_export_literal_source(exporter, &send->target);
    if (send->delay.kind == TA_DELAY_SOURCE_LITERAL) {
        out->delay.kind = TA_SCXML_STRING_LITERAL;
        out->delay.as.literal = ta_export_authored(exporter, send->delay.text);
    } else if (send->delay.kind == TA_DELAY_SOURCE_EXPR) {
        out->delay.kind = TA_SCXML_STRING_EXPR;
        out->delay.as.expr = ta_export_expression(exporter, send->delay.as.expr);
    }
    out->data = ta_export_value(exporter, &send->data);
    out->params = ta_export_params(exporter, send->params);
}

static void ta_export_action(ta_exporter *exporter, ta_action_id id, ta_scxml_action_spec *out) {
    const ta_action_def *action = &exporter->definition->action_defs[id];
    out->span = ta_export_span(exporter, TA_ELEMENT_ACTION, id);
    switch (action->kind) {
        case TA_ACTION_RAISE:
            out->kind = TA_SCXML_ACTION_RAISE;
            out->as.raise.event = ta_export_event_name(exporter, action->as.raise.event);
            out->as.raise.data = ta_export_value(exporter, &action->as.raise.data);
            break;
        case TA_ACTION_SEND:
            out->kind = TA_SCXML_ACTION_SEND;
            ta_export_send(exporter, &action->as.send, &out->as.send);
            break;
        case TA_ACTION_CANCEL:
            out->kind = TA_SCXML_ACTION_CANCEL;
            out->as.cancel.sendid = ta_export_text_source(exporter, &action->as.cancel.sendid);
            break;
        case TA_ACTION_ASSIGN:
            out->kind = TA_SCXML_ACTION_ASSIGN;
            out->as.assign.location = ta_export_expression(exporter, action->as.assign.location);
            out->as.assign.value = ta_export_value(exporter, &action->as.assign.value);
            break;
        case TA_ACTION_SCRIPT:
            out->kind = TA_SCXML_ACTION_SCRIPT;
            out->as.script.script = ta_export_expression(exporter, action->as.script);
            break;
        case TA_ACTION_LOG:
            out->kind = TA_SCXML_ACTION_LOG;
            if (action->as.log.has_label) {
                out->as.log.label = ta_export_atom(exporter, action->as.log.label);
            }
            if (action->as.log.has_expr) {
                out->as.log.expr = ta_export_expression(exporter, action->as.log.expr);
            }
            break;
        case TA_ACTION_IF:
            out->kind = TA_SCXML_ACTION_IF;
            out->as.conditional.condition = ta_export_expression(exporter, action->as.conditional.condition);
            ta_export_later(exporter, action->as.conditional.then_actions, &out->as.conditional.then_actions);
            ta_export_later(exporter, action->as.conditional.else_actions, &out->as.conditional.else_actions);
            break;
        case TA_ACTION_FOREACH:
            out->kind = TA_SCXML_ACTION_FOREACH;
            out->as.foreach.array = ta_export_expression(exporter, action->as.foreach.array);
            out->as.foreach.item = ta_export_atom(exporter, action->as.foreach.item);
            if (action->as.foreach.has_index) {
                out->as.foreach.index = ta_export_atom(exporter, action->as.foreach.index);
            }
            ta_export_later(exporter, action->as.foreach.body_actions, &out->as.foreach.body);
            break;
        default:
            /* The compiler accepts neither <invoke> nor extensions as executable content. */
            exporter->result = exporter->result == TA_OK ? TA_ERR_UNSUPPORTED : exporter->result;
            break;
    }
}

/*
 * Exports a block and every block nested in it. A nested block is queued by
 * the action that owns it and filled in from the queue, so nothing recurses;
 * each block's output is written through the member that receives it, so the
 * order the queue is drained in does not matter.
 */
static const ta_scxml_action_spec *ta_export_actions(ta_exporter *exporter, ta_span32 block) {
    const ta_scxml_action_spec *result = NULL;
    if (exporter->pending == NULL && exporter->result == TA_OK && exporter->definition->action_count != 0) {
        /* Every block but a root block is nested in one action, and an
         * action owns at most two: capacity for all of them at once. The
         * queue is allocated once per export call, from the call's arena
         * (freed with it, never individually), and never grows; it is not a
         * ta_vec, which frees with an allocator. Sizes are checked, so a
         * count that cannot be queued fails the export instead of wrapping. */
        size_t bytes = 0;
        bool fits = exporter->definition->action_count <= (UINT32_MAX - 1u) / 2u;
        exporter->pending_capacity = fits ? 2u * exporter->definition->action_count + 1u : 0;
        exporter->pending = NULL;
        if (fits && ta_checked_mul(exporter->pending_capacity, sizeof(struct ta_export_block), &bytes)) {
            exporter->pending = ta_arena_alloc(exporter->arena, bytes, _Alignof(struct ta_export_block));
        }
        if (exporter->pending == NULL) {
            exporter->pending_capacity = 0;
            exporter->result = TA_ERR_NOMEM;
        }
    }
    ta_export_later(exporter, block, &result);
    while (exporter->pending_count != 0 && exporter->result == TA_OK) {
        struct ta_export_block next = exporter->pending[--exporter->pending_count];
        ta_scxml_action_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_action_spec), next.block.length);
        *next.out = items;
        for (uint32_t index = 0; items != NULL && index < next.block.length; index += 1) {
            ta_export_action(exporter, next.block.offset + index, &items[index]);
        }
    }
    exporter->pending_count = 0;
    return result;
}

/* One <onentry>/<onexit> handler per element; an empty handler keeps a non-null, empty action list. */
static const ta_scxml_handler_spec *ta_export_handlers(ta_exporter *exporter, ta_span32 span) {
    ta_scxml_handler_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_handler_spec), span.length);
    for (uint32_t index = 0; items != NULL && index < span.length; index += 1) {
        ta_span32 block = exporter->definition->handlers[span.offset + index];
        const ta_scxml_action_spec *actions = ta_export_actions(exporter, block);
        if (actions == NULL && exporter->result == TA_OK) {
            actions = ta_arena_alloc(exporter->arena, sizeof(ta_scxml_action_spec), _Alignof(ta_scxml_action_spec));
            if (actions == NULL) {
                exporter->result = TA_ERR_NOMEM;
            } else {
                ta_mem_zero((void *)actions, sizeof(ta_scxml_action_spec));
            }
        }
        items[index].actions = actions;
    }
    return items;
}

static const ta_str *ta_export_targets(ta_exporter *exporter, ta_span32 span) {
    ta_str *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_str), span.length);
    for (uint32_t index = 0; items != NULL && index < span.length; index += 1) {
        items[index] = ta_export_state_id(exporter, exporter->definition->targets[span.offset + index]);
    }
    return items;
}

/* Null when the author wrote neither targets nor actions: the compiler then takes the first child. */
static const ta_scxml_initial_spec *ta_export_initial(ta_exporter *exporter, ta_initial_transition_id id) {
    if (id == TA_INITIAL_TRANSITION_NONE || exporter->result != TA_OK) {
        return NULL;
    }
    const ta_initial_transition_def *initial = &exporter->definition->initial_transitions[id];
    if (!initial->authored_targets && initial->actions.length == 0) {
        return NULL;
    }
    ta_scxml_initial_spec *out = ta_arena_alloc(exporter->arena, sizeof(*out), _Alignof(ta_scxml_initial_spec));
    if (out == NULL) {
        exporter->result = TA_ERR_NOMEM;
        return NULL;
    }
    *out = (ta_scxml_initial_spec){
        .targets = initial->authored_targets ? ta_export_targets(exporter, initial->targets) : NULL,
        .actions = ta_export_actions(exporter, initial->actions),
        /* Initial transitions are numbered after the <transition> elements. */
        .span = ta_export_span(exporter, TA_ELEMENT_TRANSITION, exporter->definition->transition_count + id),
    };
    return out;
}

static const ta_scxml_transition_spec *ta_export_transitions(ta_exporter *exporter, ta_span32 span) {
    ta_scxml_transition_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_transition_spec), span.length);
    for (uint32_t index = 0; items != NULL && index < span.length; index += 1) {
        const ta_transition_def *transition = &exporter->definition->transitions[span.offset + index];
        items[index] = (ta_scxml_transition_spec){
            .kind = transition->type == TA_TRANSITION_INTERNAL ? TA_SCXML_TRANSITION_INTERNAL
                                                                : TA_SCXML_TRANSITION_EXTERNAL,
            .span = ta_export_span(exporter, TA_ELEMENT_TRANSITION, span.offset + index),
            .event = ta_export_authored(exporter, transition->event_text),
            .targets = ta_export_targets(exporter, transition->targets),
            .actions = ta_export_actions(exporter, transition->actions),
        };
        if (transition->condition != UINT32_MAX) {
            items[index].condition = ta_export_expression(exporter, transition->condition);
        }
    }
    return items;
}

/* The <data> children of `owner` (TA_STATE_NONE: the <scxml> element), in document order. */
static const ta_scxml_data_spec *ta_export_data(ta_exporter *exporter, ta_state_id owner) {
    const ta_definition *definition = exporter->definition;
    uint32_t count = 0;
    for (uint32_t index = 0; index < definition->data_decl_count; index += 1) {
        count += definition->data_decls[index].owner_state == owner ? 1u : 0u;
    }
    ta_scxml_data_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_data_spec), count);
    uint32_t at = 0;
    for (uint32_t index = 0; items != NULL && index < definition->data_decl_count; index += 1) {
        const ta_data_decl *decl = &definition->data_decls[index];
        if (decl->owner_state != owner) {
            continue;
        }
        items[at].name = ta_export_atom(exporter, decl->name);
        if (decl->initializer != TA_INITIALIZER_NONE) {
            items[at].initializer = ta_export_value(exporter, &definition->initializers[decl->initializer].value);
        }
        at += 1;
    }
    return items;
}

static const ta_scxml_invoke_spec *ta_export_invokes(ta_exporter *exporter, ta_state_id owner) {
    const ta_definition *definition = exporter->definition;
    uint32_t count = 0;
    for (uint32_t index = 0; index < definition->invoke_decl_count; index += 1) {
        count += definition->invokes[index].owner == owner ? 1u : 0u;
    }
    ta_scxml_invoke_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_invoke_spec), count);
    uint32_t at = 0;
    for (uint32_t index = 0; items != NULL && index < definition->invoke_decl_count; index += 1) {
        const ta_invoke_decl *invoke = &definition->invokes[index];
        if (invoke->owner != owner) {
            continue;
        }
        items[at] = (ta_scxml_invoke_spec){
            .kind = TA_SCXML_INVOKE,
            .id = ta_export_id(exporter, &invoke->id),
            .type = ta_export_text_source(exporter, &invoke->type),
            .src = ta_export_literal_source(exporter, &invoke->src),
            .data = ta_export_value(exporter, &invoke->data),
            .autoforward = invoke->autoforward,
            .params = ta_export_params(exporter, invoke->params),
            .finalize = ta_export_actions(exporter, invoke->finalize_actions),
        };
        at += 1;
    }
    return items;
}

static const ta_scxml_done_data_spec *ta_export_done_data(ta_exporter *exporter, ta_done_data_id id) {
    if (id == TA_DONE_DATA_NONE || exporter->result != TA_OK) {
        return NULL;
    }
    const ta_done_data_def *done = &exporter->definition->done_data[id];
    ta_scxml_done_data_spec *out = ta_arena_alloc(exporter->arena, sizeof(*out), _Alignof(ta_scxml_done_data_spec));
    if (out == NULL) {
        exporter->result = TA_ERR_NOMEM;
        return NULL;
    }
    *out = (ta_scxml_done_data_spec){
        .content = ta_export_value(exporter, &done->content),
        .params = ta_export_params(exporter, done->params),
    };
    return out;
}

static ta_scxml_state_kind ta_export_state_kind(ta_state_kind kind) {
    switch (kind) {
        case TA_STATE_ATOMIC: return TA_SCXML_STATE_ATOMIC;
        case TA_STATE_COMPOUND: return TA_SCXML_STATE_COMPOUND;
        case TA_STATE_PARALLEL: return TA_SCXML_STATE_PARALLEL;
        case TA_STATE_FINAL: return TA_SCXML_STATE_FINAL;
        case TA_STATE_HISTORY_SHALLOW: return TA_SCXML_STATE_HISTORY_SHALLOW;
        case TA_STATE_HISTORY_DEEP: return TA_SCXML_STATE_HISTORY_DEEP;
        default: return TA_SCXML_STATE_NONE;
    }
}

/* Every member of a state's spec except its child states. */
static void ta_export_state(ta_exporter *exporter, ta_state_id id, ta_scxml_state_spec *out) {
    const ta_definition *definition = exporter->definition;
    const ta_state_def *state = &definition->states[id];
    *out = (ta_scxml_state_spec){
        .kind = ta_export_state_kind(state->kind),
        .span = ta_export_span(exporter, TA_ELEMENT_STATE, id),
        .id = ta_export_state_id(exporter, id),
        .initial = state->kind == TA_STATE_COMPOUND ? ta_export_initial(exporter, state->initial) : NULL,
        .transitions = ta_export_transitions(exporter, state->transitions),
        .onentry = ta_export_handlers(exporter, state->onentry_handlers),
        .onexit = ta_export_handlers(exporter, state->onexit_handlers),
        .data = ta_export_data(exporter, id),
        .invokes = ta_export_invokes(exporter, id),
        .done_data = ta_export_done_data(exporter, state->done_data),
    };
}

/*
 * The spans table the exported elements index (ADR 0085): the definition's
 * own, borrowed, or all zero when it keeps none and the caller numbered the
 * elements anyway, as ta_definition_encode() does to learn where each one
 * lands.
 */
static void ta_export_spans(ta_exporter *exporter, ta_scxml_spec *spec) {
    const ta_definition *definition = exporter->definition;
    uint32_t count = ta_definition_span_base(definition, TA_ELEMENT_EXPRESSION) +
                     ta_definition_element_count(definition, TA_ELEMENT_EXPRESSION);
    spec->span_count = count;
    spec->spans = definition->spans;
    spec->sources = definition->sources;
    spec->source_count = definition->source_count;
    if (definition->spans != NULL || count == 0 || exporter->result != TA_OK) {
        return;
    }
    ta_source_span *zeros = ta_arena_alloc(exporter->arena, (size_t)count * sizeof(ta_source_span),
                                           _Alignof(ta_source_span));
    if (zeros == NULL) {
        exporter->result = TA_ERR_NOMEM;
        return;
    }
    ta_mem_zero(zeros, (size_t)count * sizeof(ta_source_span));
    spec->spans = zeros;
}

/*
 * The state tree in one pass, without recursion (ADR 0060). The compiler
 * numbers states in document pre-order and records the roots first in
 * `children`, so every state is visited after its parent: the parent
 * allocates its list of child states and hands each child its slot, and the
 * child fills it when its turn comes.
 */
static const ta_scxml_state_spec *ta_export_roots(ta_exporter *exporter) {
    const ta_definition *definition = exporter->definition;
    uint32_t count = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        count += definition->states[state].parent == TA_STATE_NONE ? 1u : 0u;
    }
    ta_scxml_state_spec *roots = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_state_spec), count);
    if (roots == NULL) {
        return NULL;
    }
    ta_scxml_state_spec **slots = ta_arena_alloc(exporter->arena,
                                                 (size_t)definition->state_count * sizeof(ta_scxml_state_spec *),
                                                 _Alignof(ta_scxml_state_spec *));
    if (slots == NULL) {
        exporter->result = TA_ERR_NOMEM;
        return NULL;
    }
    for (uint32_t index = 0; index < count; index += 1) {
        slots[definition->children[index]] = &roots[index];
    }
    for (ta_state_id id = 0; id < definition->state_count && exporter->result == TA_OK; id += 1) {
        const ta_state_def *state = &definition->states[id];
        ta_scxml_state_spec *out = slots[id];
        ta_export_state(exporter, id, out);
        ta_scxml_state_spec *children =
            ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_state_spec), state->children.length);
        out->states = children;
        for (uint32_t index = 0; children != NULL && index < state->children.length; index += 1) {
            slots[definition->children[state->children.offset + index]] = &children[index];
        }
    }
    return roots;
}

TA_PRIVATE ta_result ta_export_spec(const ta_system *system, const ta_export_request *request) {
    const ta_definition *definition = ta_definition_lookup(system, request->definition);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_exporter exporter = { .system = system,
                             .definition = definition,
                             .arena = request->arena,
                             .numbered = request->numbered };
    ta_scxml_spec *spec = ta_arena_alloc(request->arena, sizeof(*spec), _Alignof(ta_scxml_spec));
    if (spec == NULL) {
        return TA_ERR_NOMEM;
    }
    *spec = (ta_scxml_spec){
        .name = definition->document_name == TA_ATOM_NONE ? (ta_str){0}
                                                          : ta_export_atom(&exporter, definition->document_name),
        .datamodel = definition->authored_datamodel,
        .identity = definition->fingerprinted ? NULL : &definition->identity,
        .binding = definition->authored_binding,
        .states = ta_export_roots(&exporter),
        .initial = ta_export_initial(&exporter, definition->root_initial),
        .data = ta_export_data(&exporter, TA_STATE_NONE),
    };
    if (definition->global_script_actions.length == 1) {
        const ta_action_def *script = &definition->action_defs[definition->global_script_actions.offset];
        spec->script = ta_export_expression(&exporter, script->as.script);
        spec->script_span = ta_export_span(&exporter, TA_ELEMENT_ACTION, definition->global_script_actions.offset);
    }
    if (exporter.numbered || definition->spans != NULL) {
        ta_export_spans(&exporter, spec);
    }
    if (exporter.result != TA_OK) {
        return exporter.result;
    }
    *request->out = spec;
    return TA_OK;
}

TA_API ta_result ta_definition_export(const ta_system *system, const ta_definition_export_params *params) {
    if (system == NULL || params == NULL || params->arena == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    return ta_export_spec(system, &(ta_export_request){
                                      .definition = params->definition,
                                      .arena = params->arena,
                                      .out = params->out,
                                  });
}


/*
 * Import: decode, compile, register. The decode's arena holds the spec,
 * which may point into the input; compile copies what it keeps, so neither
 * outlives this call as far as the definition is concerned.
 */

static void ta_import_fail(const ta_definition_import_params *params, const ta_import_error *error) {
    if (params->error != NULL) {
        *params->error = *error;
    }
}

/* ------------------------------------------------------------------------
 * Locating a compile error in the input (ADR 0058 section 5)
 */

static bool ta_import_within(ta_bytes input, const char *ptr, uint32_t len) {
    const char *begin = (const char *)input.ptr;
    return ptr != NULL && input.ptr != NULL && ptr >= begin && (size_t)(ptr - begin) <= input.len &&
           len <= input.len - (size_t)(ptr - begin);
}

/* Lines and columns count from 1; a column counts bytes, as txml's do. */
static ta_chart_position ta_import_position(ta_bytes input, uint64_t offset) {
    ta_chart_position at = { .offset = offset, .line = 1, .column = 1 };
    for (uint64_t index = 0; index < offset && index < input.len; index += 1u) {
        if (input.ptr[index] == '\n') {
            at.line += 1u;
            at.column = 1;
        } else {
            at.column += 1u;
        }
    }
    return at;
}

typedef enum {
    TA_NODE_NONE = 0,
    TA_NODE_SPEC,
    TA_NODE_STATE,
    TA_NODE_TRANSITION,
    TA_NODE_HANDLER,
    TA_NODE_INITIAL,
    TA_NODE_ACTION,
    TA_NODE_DATA,
    TA_NODE_PARAM,
    TA_NODE_INVOKE,
    TA_NODE_DONE_DATA,
    TA_NODE_TEXT
} ta_node_kind;

/* One element of a spec reached by a path; TA_NODE_TEXT is a text member. */
typedef struct {
    ta_node_kind kind;
    const void *ptr;
    ta_str text;
} ta_node;

static ta_str ta_expression_text(const ta_expression *expression) {
    return expression->kind == TA_EXPRESSION_SOURCE ? expression->as.source : (ta_str){0};
}

static ta_str ta_value_text(const ta_scxml_value_spec *value) {
    switch (value->kind) {
        case TA_SCXML_VALUE_EXPR: return ta_expression_text(&value->as.expr);
        case TA_SCXML_VALUE_LOCATION: return ta_expression_text(&value->as.location);
        case TA_SCXML_VALUE_CONTENT:
            return (ta_str){ .ptr = (const char *)value->as.content.bytes.ptr, .len = value->as.content.bytes.len };
        default: return (ta_str){0};
    }
}

static ta_str ta_string_text(const ta_scxml_string_spec *string) {
    switch (string->kind) {
        case TA_SCXML_STRING_LITERAL: return string->as.literal;
        case TA_SCXML_STRING_EXPR: return ta_expression_text(&string->as.expr);
        default: return (ta_str){0};
    }
}

static ta_str ta_id_text(const ta_scxml_id_spec *id) {
    switch (id->kind) {
        case TA_SCXML_ID_LITERAL: return id->as.literal;
        case TA_SCXML_ID_LOCATION: return ta_expression_text(&id->as.location);
        default: return (ta_str){0};
    }
}

static ta_node ta_text_node(ta_str text) {
    return (ta_node){ .kind = TA_NODE_TEXT, .text = text };
}

static ta_node ta_list_node(ta_node_kind kind, const void *element) {
    return (ta_node){ .kind = kind, .ptr = element };
}

static ta_node ta_action_step(const ta_scxml_action_spec *action, ta_spec_step step) {
    uint32_t i = step.index;
    switch (action->kind) {
        case TA_SCXML_ACTION_RAISE:
            if (step.member == TA_SPEC_MEMBER_EVENT) return ta_text_node(action->as.raise.event);
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&action->as.raise.data));
            return (ta_node){0};
        case TA_SCXML_ACTION_SEND: {
            const ta_scxml_send_spec *send = &action->as.send;
            switch (step.member) {
                case TA_SPEC_MEMBER_PARAMS: return ta_list_node(TA_NODE_PARAM, &send->params[i]);
                case TA_SPEC_MEMBER_ID: return ta_text_node(ta_id_text(&send->id));
                case TA_SPEC_MEMBER_TYPE: return ta_text_node(ta_string_text(&send->type));
                case TA_SPEC_MEMBER_EVENT:
                    if (send->event.kind == TA_SCXML_EVENT_NAME) return ta_text_node(send->event.as.name);
                    if (send->event.kind == TA_SCXML_EVENT_EXPR) return ta_text_node(ta_expression_text(&send->event.as.expr));
                    return (ta_node){0};
                case TA_SPEC_MEMBER_TARGET: return ta_text_node(ta_string_text(&send->target));
                case TA_SPEC_MEMBER_DELAY: return ta_text_node(ta_string_text(&send->delay));
                case TA_SPEC_MEMBER_DATA_VALUE: return ta_text_node(ta_value_text(&send->data));
                default: return (ta_node){0};
            }
        }
        case TA_SCXML_ACTION_CANCEL:
            if (step.member == TA_SPEC_MEMBER_SENDID) return ta_text_node(ta_string_text(&action->as.cancel.sendid));
            return (ta_node){0};
        case TA_SCXML_ACTION_ASSIGN:
            if (step.member == TA_SPEC_MEMBER_LOCATION) return ta_text_node(ta_expression_text(&action->as.assign.location));
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&action->as.assign.value));
            return (ta_node){0};
        case TA_SCXML_ACTION_SCRIPT:
            if (step.member == TA_SPEC_MEMBER_EXPR) return ta_text_node(ta_expression_text(&action->as.script.script));
            return (ta_node){0};
        case TA_SCXML_ACTION_LOG:
            if (step.member == TA_SPEC_MEMBER_EXPR) return ta_text_node(ta_expression_text(&action->as.log.expr));
            return (ta_node){0};
        case TA_SCXML_ACTION_IF:
            switch (step.member) {
                case TA_SPEC_MEMBER_CONDITION: return ta_text_node(ta_expression_text(&action->as.conditional.condition));
                case TA_SPEC_MEMBER_THEN: return ta_list_node(TA_NODE_ACTION, &action->as.conditional.then_actions[i]);
                case TA_SPEC_MEMBER_ELSE: return ta_list_node(TA_NODE_ACTION, &action->as.conditional.else_actions[i]);
                default: return (ta_node){0};
            }
        case TA_SCXML_ACTION_FOREACH:
            switch (step.member) {
                case TA_SPEC_MEMBER_EXPR: return ta_text_node(ta_expression_text(&action->as.foreach.array));
                case TA_SPEC_MEMBER_ITEM: return ta_text_node(action->as.foreach.item);
                case TA_SPEC_MEMBER_INDEX: return ta_text_node(action->as.foreach.index);
                case TA_SPEC_MEMBER_BODY: return ta_list_node(TA_NODE_ACTION, &action->as.foreach.body[i]);
                default: return (ta_node){0};
            }
        default:
            return (ta_node){0};
    }
}

/* Follows one step of a compile error's path. */
static ta_node ta_node_step(ta_node node, ta_spec_step step) {
    uint32_t i = step.index;
    switch (node.kind) {
        case TA_NODE_SPEC: {
            const ta_scxml_spec *spec = node.ptr;
            switch (step.member) {
                case TA_SPEC_MEMBER_STATES: return ta_list_node(TA_NODE_STATE, &spec->states[i]);
                case TA_SPEC_MEMBER_DATA: return ta_list_node(TA_NODE_DATA, &spec->data[i]);
                case TA_SPEC_MEMBER_INITIAL: return ta_list_node(TA_NODE_INITIAL, spec->initial);
                case TA_SPEC_MEMBER_SCRIPT: return ta_text_node(ta_expression_text(&spec->script));
                case TA_SPEC_MEMBER_NAME: return ta_text_node(spec->name);
                case TA_SPEC_MEMBER_DATAMODEL: return ta_text_node(spec->datamodel);
                default: return (ta_node){0};
            }
        }
        case TA_NODE_STATE: {
            const ta_scxml_state_spec *state = node.ptr;
            switch (step.member) {
                case TA_SPEC_MEMBER_STATES: return ta_list_node(TA_NODE_STATE, &state->states[i]);
                case TA_SPEC_MEMBER_TRANSITIONS: return ta_list_node(TA_NODE_TRANSITION, &state->transitions[i]);
                case TA_SPEC_MEMBER_ONENTRY: return ta_list_node(TA_NODE_HANDLER, &state->onentry[i]);
                case TA_SPEC_MEMBER_ONEXIT: return ta_list_node(TA_NODE_HANDLER, &state->onexit[i]);
                case TA_SPEC_MEMBER_DATA: return ta_list_node(TA_NODE_DATA, &state->data[i]);
                case TA_SPEC_MEMBER_INVOKES: return ta_list_node(TA_NODE_INVOKE, &state->invokes[i]);
                case TA_SPEC_MEMBER_INITIAL: return ta_list_node(TA_NODE_INITIAL, state->initial);
                case TA_SPEC_MEMBER_DONE_DATA: return ta_list_node(TA_NODE_DONE_DATA, state->done_data);
                case TA_SPEC_MEMBER_ID: return ta_text_node(state->id);
                default: return (ta_node){0};
            }
        }
        case TA_NODE_TRANSITION: {
            const ta_scxml_transition_spec *transition = node.ptr;
            switch (step.member) {
                case TA_SPEC_MEMBER_TARGETS: return ta_text_node(transition->targets[i]);
                case TA_SPEC_MEMBER_ACTIONS: return ta_list_node(TA_NODE_ACTION, &transition->actions[i]);
                case TA_SPEC_MEMBER_EVENT: return ta_text_node(transition->event);
                case TA_SPEC_MEMBER_CONDITION: return ta_text_node(ta_expression_text(&transition->condition));
                default: return (ta_node){0};
            }
        }
        case TA_NODE_HANDLER: {
            const ta_scxml_handler_spec *handler = node.ptr;
            if (step.member == TA_SPEC_MEMBER_ACTIONS) return ta_list_node(TA_NODE_ACTION, &handler->actions[i]);
            return (ta_node){0};
        }
        case TA_NODE_INITIAL: {
            const ta_scxml_initial_spec *initial = node.ptr;
            if (step.member == TA_SPEC_MEMBER_TARGETS) return ta_text_node(initial->targets[i]);
            if (step.member == TA_SPEC_MEMBER_ACTIONS) return ta_list_node(TA_NODE_ACTION, &initial->actions[i]);
            return (ta_node){0};
        }
        case TA_NODE_DATA: {
            const ta_scxml_data_spec *data = node.ptr;
            if (step.member == TA_SPEC_MEMBER_NAME) return ta_text_node(data->name);
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&data->initializer));
            return (ta_node){0};
        }
        case TA_NODE_PARAM: {
            const ta_scxml_param_spec *param = node.ptr;
            if (step.member == TA_SPEC_MEMBER_NAME) return ta_text_node(param->name);
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&param->value));
            return (ta_node){0};
        }
        case TA_NODE_DONE_DATA: {
            const ta_scxml_done_data_spec *done = node.ptr;
            if (step.member == TA_SPEC_MEMBER_PARAMS) return ta_list_node(TA_NODE_PARAM, &done->params[i]);
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&done->content));
            return (ta_node){0};
        }
        case TA_NODE_INVOKE: {
            const ta_scxml_invoke_spec *invoke = node.ptr;
            switch (step.member) {
                case TA_SPEC_MEMBER_PARAMS: return ta_list_node(TA_NODE_PARAM, &invoke->params[i]);
                case TA_SPEC_MEMBER_FINALIZE: return ta_list_node(TA_NODE_ACTION, &invoke->finalize[i]);
                case TA_SPEC_MEMBER_ID: return ta_text_node(ta_id_text(&invoke->id));
                case TA_SPEC_MEMBER_TYPE: return ta_text_node(ta_string_text(&invoke->type));
                case TA_SPEC_MEMBER_SRC: return ta_text_node(ta_string_text(&invoke->src));
                case TA_SPEC_MEMBER_DATA_VALUE: return ta_text_node(ta_value_text(&invoke->data));
                default: return (ta_node){0};
            }
        }
        case TA_NODE_ACTION:
            return ta_action_step(node.ptr, step);
        default:
            return (ta_node){0};
    }
}

/* The text that stands for a node, if any of it lies within `input`. */
static ta_str ta_node_anchor(ta_node node, ta_bytes input) {
    ta_str candidates[3] = { {0}, {0}, {0} };
    switch (node.kind) {
        case TA_NODE_TEXT: candidates[0] = node.text; break;
        case TA_NODE_SPEC: candidates[0] = ((const ta_scxml_spec *)node.ptr)->name; break;
        case TA_NODE_STATE: candidates[0] = ((const ta_scxml_state_spec *)node.ptr)->id; break;
        case TA_NODE_TRANSITION: {
            const ta_scxml_transition_spec *transition = node.ptr;
            candidates[0] = transition->event;
            candidates[1] = ta_expression_text(&transition->condition);
            candidates[2] = transition->targets != NULL ? transition->targets[0] : (ta_str){0};
            break;
        }
        case TA_NODE_INITIAL: {
            const ta_scxml_initial_spec *initial = node.ptr;
            candidates[0] = initial->targets != NULL ? initial->targets[0] : (ta_str){0};
            break;
        }
        case TA_NODE_DATA: candidates[0] = ((const ta_scxml_data_spec *)node.ptr)->name; break;
        case TA_NODE_PARAM: candidates[0] = ((const ta_scxml_param_spec *)node.ptr)->name; break;
        case TA_NODE_INVOKE: {
            const ta_scxml_invoke_spec *invoke = node.ptr;
            candidates[0] = ta_id_text(&invoke->id);
            candidates[1] = ta_string_text(&invoke->type);
            candidates[2] = ta_string_text(&invoke->src);
            break;
        }
        case TA_NODE_ACTION: {
            /* The first text member the action has. */
            static const ta_spec_member members[] = {
                TA_SPEC_MEMBER_EVENT, TA_SPEC_MEMBER_LOCATION, TA_SPEC_MEMBER_EXPR, TA_SPEC_MEMBER_CONDITION,
                TA_SPEC_MEMBER_SENDID, TA_SPEC_MEMBER_TARGET, TA_SPEC_MEMBER_TYPE, TA_SPEC_MEMBER_ID,
            };
            for (uint32_t index = 0; index < sizeof(members) / sizeof(members[0]); index += 1u) {
                ta_node text = ta_action_step(node.ptr, (ta_spec_step){ .member = members[index],
                                                                        .index = TA_SPEC_INDEX_NONE });
                if (text.kind == TA_NODE_TEXT && ta_import_within(input, text.text.ptr, text.text.len)) {
                    return text.text;
                }
            }
            break;
        }
        default: break;
    }
    for (uint32_t index = 0; index < 3u; index += 1u) {
        if (ta_import_within(input, candidates[index].ptr, candidates[index].len)) {
            return candidates[index];
        }
    }
    return (ta_str){0};
}

/*
 * Where in `input` a compile error is: its subject, if the subject is a
 * slice of the input; otherwise the deepest element on its path whose text
 * is. False when neither. Error path only.
 */
static bool ta_import_locate(const ta_scxml_spec *spec, const ta_compile_error *error, ta_bytes input,
                             uint64_t *offset) {
    const char *begin = (const char *)input.ptr;
    if (error->subject.len != 0 && ta_import_within(input, error->subject.ptr, error->subject.len)) {
        *offset = (uint64_t)(error->subject.ptr - begin) + error->subject_offset;
        return true;
    }
    for (uint32_t depth = error->path_length + 1u; depth > 0; depth -= 1u) {
        ta_node node = { .kind = TA_NODE_SPEC, .ptr = spec };
        for (uint32_t step = 0; step + 1u < depth && node.kind != TA_NODE_NONE; step += 1u) {
            node = ta_node_step(node, error->path[step]);
            if (node.kind != TA_NODE_TEXT && node.ptr == NULL) {
                node.kind = TA_NODE_NONE;
            }
        }
        if (node.kind == TA_NODE_NONE) {
            continue;
        }
        ta_str anchor = ta_node_anchor(node, input);
        if (anchor.ptr != NULL) {
            *offset = (uint64_t)(anchor.ptr - begin);
            return true;
        }
    }
    return false;
}

/*
 * The data model the document names, or the first when it names none. A name
 * no entry has also gets the first, whose compile then refuses the chart
 * with a mismatch at the `datamodel` attribute: one error path, located in
 * the document (ADR 0058, ADR 0076).
 */
static ta_datamodel ta_import_datamodel(const ta_definition_import_params *params, ta_str name) {
    if (name.ptr != NULL && name.len != 0) {
        for (uint32_t index = 0; index < params->datamodel_count; index += 1u) {
            const ta_datamodel *candidate = &params->datamodels[index];
            if (candidate->v != NULL && ta_str_equal(candidate->v->name, name)) {
                return *candidate;
            }
        }
    }
    return params->datamodels[0];
}

TA_API ta_result ta_definition_import(ta_system *system, const ta_definition_import_params *params) {
    if (system == NULL || params == NULL || params->codec.v == NULL || params->arena == NULL ||
        params->out == NULL || (params->input.ptr == NULL && params->input.len != 0) ||
        params->datamodels == NULL || params->datamodel_count == 0) {
        return TA_ERR_INVALID;
    }
    if (params->error != NULL) {
        *params->error = (ta_import_error){0};
    }
    if (params->codec.v->decode == NULL) {
        return TA_ERR_UNSUPPORTED;
    }

    const ta_scxml_spec *decoded = NULL;
    ta_chart_error decode_error = {0};
    ta_result result = params->codec.v->decode(params->codec.self,
                                               &(ta_chart_decode_params){
                                                   .input = params->input,
                                                   .name = params->name,
                                                   .arena = params->arena,
                                                   .resolver = params->resolver,
                                                   .lowering = params->lowering,
                                                   .identity = params->identity,
                                                   .out = &decoded,
                                                   .error = &decode_error,
                                               });
    if (result == TA_ERR_INVALID) {
        ta_import_fail(params, &(ta_import_error){ .stage = TA_IMPORT_STAGE_DECODE,
                                                   .as.decode = decode_error,
                                                   .located = true,
                                                   .at = decode_error.at });
        return result;
    }
    if (result != TA_OK) {
        return result;
    }
    if (decoded == NULL) {
        return TA_ERR_INVALID; /* a codec that reports success must produce a spec */
    }

    /* No chart syntax carries an identity, so the caller's is the chart's,
     * whether or not the codec copied it across. */
    ta_scxml_spec *spec = ta_arena_alloc(params->arena, sizeof(ta_scxml_spec), _Alignof(ta_scxml_spec));
    if (spec == NULL) {
        return TA_ERR_NOMEM;
    }
    *spec = *decoded;
    spec->identity = params->identity;

    ta_definition *definition = NULL;
    ta_compile_error compile_error = {0};
    result = ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                    .spec = spec,
                                                    .datamodel = ta_import_datamodel(params, spec->datamodel),
                                                    .allocator = params->allocator,
                                                    .out = &definition,
                                                    .error = params->error != NULL ? &compile_error : NULL,
                                                    .error_arena = params->arena,
                                                });
    if ((result == TA_ERR_INVALID || result == TA_ERR_UNSUPPORTED) && params->error != NULL) {
        ta_import_error error = { .stage = TA_IMPORT_STAGE_COMPILE, .as.compile = compile_error };
        uint64_t offset = 0;
        if (ta_import_locate(spec, &compile_error, params->input, &offset)) {
            error.located = true;
            error.at = ta_import_position(params->input, offset);
        }
        ta_import_fail(params, &error);
        return result;
    }
    if (result == TA_ERR_INVALID || result == TA_ERR_UNSUPPORTED) {
        return result;
    }
    if (result != TA_OK) {
        return result;
    }
    result = ta_system_register_definition(system, definition, params->out);
    if (result != TA_OK) {
        /* Registration takes ownership only on success. */
        ta_definition_destroy(definition);
    }
    return result;
}

/*
 * Encode: export, then the codec's encode. The identity no syntax carries is
 * handed back beside the bytes. Asked where elements landed, the export
 * numbers every element by kind and ID (ADR 0085), and the codec's reports
 * land in one table laid out the same way.
 */

typedef struct {
    ta_source_span *table;
    uint32_t count;
} ta_encode_spans;

static void ta_encode_span_written(void *self, uint32_t span, const ta_source_span *where) {
    ta_encode_spans *spans = self;
    if (span != 0 && span <= spans->count && where != NULL) {
        spans->table[span - 1u] = *where;
    }
}

static const ta_chart_span_sink_vtable ta_encode_span_sink = { .written = ta_encode_span_written };

/* The part of the table for one kind of element; null when it has none. */
static const ta_source_span *ta_encode_spans_of(const ta_encode_spans *spans, const ta_definition *definition,
                                                ta_element_kind kind) {
    if (ta_definition_element_count(definition, kind) == 0) {
        return NULL;
    }
    return spans->table + ta_definition_span_base(definition, kind);
}

TA_API ta_result ta_definition_encode(const ta_system *system, const ta_definition_encode_params *params) {
    if (system == NULL || params == NULL || params->codec.v == NULL || params->arena == NULL ||
        params->out == NULL) {
        return TA_ERR_INVALID;
    }
    if (params->error != NULL) {
        *params->error = (ta_chart_error){0};
    }
    if (params->codec.v->encode == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    const ta_scxml_spec *spec = NULL;
    ta_result result = ta_export_spec(system, &(ta_export_request){
                                                  .definition = params->definition,
                                                  .arena = params->arena,
                                                  .numbered = params->spans != NULL,
                                                  .out = &spec,
                                              });
    if (result != TA_OK) {
        return result;
    }
    ta_encode_spans spans = { .count = params->spans != NULL ? spec->span_count : 0 };
    if (spans.count != 0) {
        spans.table = ta_arena_alloc(params->arena, (size_t)spans.count * sizeof(ta_source_span),
                                     _Alignof(ta_source_span));
        if (spans.table == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_zero(spans.table, (size_t)spans.count * sizeof(ta_source_span));
    }
    ta_chart_error encode_error = {0};
    result = params->codec.v->encode(params->codec.self, &(ta_chart_encode_params){
                                                             .spec = spec,
                                                             .arena = params->arena,
                                                             .expressions = params->expressions,
                                                             .spans = { .self = &spans,
                                                                        .v = params->spans != NULL ? &ta_encode_span_sink
                                                                                                   : NULL },
                                                             .out = params->out,
                                                             .error = &encode_error,
                                                         });
    if (result == TA_ERR_INVALID && params->error != NULL) {
        *params->error = encode_error;
    }
    if (result == TA_OK && params->spans != NULL) {
        const ta_definition *definition = ta_definition_lookup(system, params->definition);
        *params->spans = (ta_source_map){
            .states = ta_encode_spans_of(&spans, definition, TA_ELEMENT_STATE),
            .state_count = ta_definition_element_count(definition, TA_ELEMENT_STATE),
            .transitions = ta_encode_spans_of(&spans, definition, TA_ELEMENT_TRANSITION),
            .transition_count = ta_definition_element_count(definition, TA_ELEMENT_TRANSITION),
            .actions = ta_encode_spans_of(&spans, definition, TA_ELEMENT_ACTION),
            .action_count = ta_definition_element_count(definition, TA_ELEMENT_ACTION),
            .expressions = ta_encode_spans_of(&spans, definition, TA_ELEMENT_EXPRESSION),
            .expression_count = ta_definition_element_count(definition, TA_ELEMENT_EXPRESSION),
        };
    }
    if (result == TA_OK && params->identity != NULL) {
        *params->identity = spec->identity;
    }
    return result;
}


TA_PRIVATE ta_actor *ta_actor_lookup(const ta_system *system, ta_actor_id id) {
    if (system == NULL) {
        return NULL;
    }
    uint32_t slot = ta_actor_id_slot(id);
    uint32_t generation = ta_actor_id_generation(id);
    const ta_actor_slot *entry = ta_actor_slot_at(system, slot);
    return entry != NULL && entry->generation == generation ? entry->actor : NULL;
}

TA_PRIVATE ta_actor_slot *ta_actor_slot_at(const ta_system *system, uint32_t slot) {
    if (slot >= system->actors.count) {
        return NULL;
    }
    ta_actor_slot *const *pages = system->actors.pages.items;
    return &pages[slot / TA_ACTOR_PAGE_SLOTS][slot % TA_ACTOR_PAGE_SLOTS];
}

TA_PRIVATE void ta_actor_table_destroy(ta_actor_table *table, ta_allocator allocator) {
    ta_actor_slot **pages = table->pages.items;
    for (uint32_t page = 0; page < table->pages.count; page += 1u) {
        ta_mem_free(allocator, pages[page],
                    &(ta_allocator_free_params){ .size = TA_ACTOR_PAGE_SLOTS * sizeof(ta_actor_slot),
                                                 .alignment = _Alignof(ta_actor_slot) });
    }
    ta_vec_destroy(&table->pages);
    table->count = 0;
}

TA_PRIVATE ta_result ta_actor_layout(ta_definition *definition) {
    ta_layout layout;
    size_t session_bytes = 0;
    if (ta_definition_session_bytes(definition, &session_bytes) != TA_OK) {
        return TA_ERR_NOMEM; /* the definition keeps its old (zero) layout */
    }
    ta_layout_init(&layout, 0);
    ta_layout_add(&layout, &(ta_layout_entry){ .size = sizeof(ta_actor), .alignment = _Alignof(ta_actor), .count = 1u });
    size_t session = ta_layout_add(&layout, &(ta_layout_entry){ .size = session_bytes,
                                                                .alignment = _Alignof(uint64_t),
                                                                .count = 1u });
    size_t instance = ta_layout_add(&layout, &(ta_layout_entry){ .size = definition->datamodel_instance_size,
                                                                 .alignment = definition->datamodel_instance_alignment,
                                                                 .count = 1u });
    size_t bytes;
    size_t alignment;
    ta_result result = ta_layout_finish(&layout, &bytes, &alignment);
    if (result != TA_OK) {
        return result; /* the definition keeps its old (zero) layout */
    }
    definition->actor_session_offset = session;
    definition->actor_session_bytes = session_bytes;
    definition->actor_instance_offset = instance;
    definition->actor_block_bytes = bytes;
    definition->actor_block_alignment = alignment;
    return TA_OK;
}

/*
 * Issues the next slot, allocating a page when the last one is full. Pages
 * never move, so this never copies the table (ADR 0063).
 */
static ta_result ta_actor_table_grow(ta_system *system, uint32_t *out) {
    ta_actor_table *table = &system->actors;
    if (table->count == UINT32_MAX) {
        return TA_ERR_NOMEM; /* UINT32_MAX is no slot: it ends the free list and a walk */
    }
    if ((uint64_t)table->count == (uint64_t)table->pages.count * TA_ACTOR_PAGE_SLOTS) {
        ta_actor_slot *page = ta_mem_alloc(system->allocator, TA_ACTOR_PAGE_SLOTS * sizeof(ta_actor_slot),
                                           _Alignof(ta_actor_slot));
        if (page == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_result result = ta_vec_push(&table->pages, &page);
        if (result != TA_OK) {
            ta_mem_free(system->allocator, page,
                        &(ta_allocator_free_params){ .size = TA_ACTOR_PAGE_SLOTS * sizeof(ta_actor_slot),
                                                     .alignment = _Alignof(ta_actor_slot) });
            return result;
        }
    }
    *out = table->count;
    table->count += 1u;
    *ta_actor_slot_at(system, *out) = (ta_actor_slot){ .generation = 1, .next_free = UINT32_MAX };
    return TA_OK;
}

TA_API uint32_t ta_actor_id_slot(ta_actor_id id) {
    return (uint32_t)(id & 0xffffffffu);
}

TA_API uint32_t ta_actor_id_generation(ta_actor_id id) {
    return (uint32_t)(id >> 32);
}

TA_API ta_actor_id ta_actor_id_make(uint32_t slot, uint32_t generation) {
    return ((ta_actor_id)generation << 32) | slot;
}

TA_PRIVATE ta_timer_list *ta_actor_timers(const ta_actor *actor) {
    return actor->sparse == NULL ? NULL : &actor->sparse->timers;
}

TA_PRIVATE ta_result ta_actor_schedule_send(ta_system *system, ta_actor *actor, const ta_delayed_send *send) {
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    return ta_timer_schedule(&system->timers, &sparse->timers, send);
}

TA_PRIVATE ta_result ta_actor_reserve_send(ta_system *system, ta_actor *actor) {
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    return ta_timer_reserve(&system->timers, &sparse->timers, 1u);
}

TA_PRIVATE ta_result ta_actor_timer_home(ta_system *system, ta_actor_id owner, ta_timer_list **out) {
    ta_actor *actor = ta_actor_lookup(system, owner);
    if (actor == NULL) {
        *out = &system->timers.orphans;
        return TA_OK;
    }
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    *out = &sparse->timers;
    return TA_OK;
}

static void ta_activation_effect_release(ta_activation_effect *effect) {
    if (effect->kind == TA_ACTIVATION_SEND) {
        ta_value_release(&effect->as.send.send.target);
        ta_value_release(&effect->as.send.send.data);
        ta_actor_ref_release(&effect->as.send.send.destination);
        ta_actor_ref_release(&effect->as.send.send.origin);
    } else if (effect->kind == TA_ACTIVATION_LOG) {
        ta_value_release(&effect->as.log.value);
    }
}

static void ta_actor_discard_activation_effects(ta_actor *actor) {
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL || sparse->activation_effects == NULL) {
        return;
    }
    ta_activation_effects *effects = sparse->activation_effects;
    for (uint32_t index = 0; index < effects->entries.count; index += 1u) {
        ta_activation_effect_release(ta_vec_at(&effects->entries, index));
    }
    ta_allocator allocator = effects->entries.allocator;
    ta_vec_destroy(&effects->entries);
    ta_mem_free(allocator, effects,
                &(ta_allocator_free_params){ .size = sizeof(ta_activation_effects),
                                             .alignment = _Alignof(ta_activation_effects) });
    sparse->activation_effects = NULL;
}

static ta_activation_effects *ta_actor_activation(ta_system *system, ta_actor *actor);

static ta_result ta_actor_hold_effect(ta_system *system, ta_actor *actor, const ta_activation_effect *effect) {
    ta_activation_effects *effects = ta_actor_activation(system, actor);
    if (effects == NULL) {
        return TA_ERR_NOMEM;
    }
    return ta_vec_push(&effects->entries, effect);
}

TA_PRIVATE ta_result ta_actor_hold_send(ta_system *system, ta_actor *actor, const ta_activation_send *send) {
    return ta_actor_hold_effect(system, actor,
                                &(ta_activation_effect){ .kind = TA_ACTIVATION_SEND, .as.send = *send });
}

TA_PRIVATE ta_result ta_actor_hold_log(ta_system *system, ta_actor *actor, const ta_log_record *record) {
    ta_activation_effect effect = { .kind = TA_ACTIVATION_LOG, .as.log = *record };
    ta_value_retain(&effect.as.log.value);
    ta_result result = ta_actor_hold_effect(system, actor, &effect);
    if (result != TA_OK) {
        ta_value_release(&effect.as.log.value);
    }
    return result;
}

TA_PRIVATE bool ta_actor_cancel_held_send(ta_actor *actor, ta_send_id send_id) {
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL || sparse->activation_effects == NULL) {
        return false;
    }
    ta_vec *entries = &sparse->activation_effects->entries;
    bool found = false;
    for (uint32_t index = entries->count; index > 0; index -= 1u) {
        ta_activation_effect *effect = ta_vec_at(entries, index - 1u);
        if (effect->kind != TA_ACTIVATION_SEND || effect->as.send.send.send_id != send_id) {
            continue;
        }
        ta_activation_effect_release(effect);
        for (uint32_t at = index; at < entries->count; at += 1u) {
            *(ta_activation_effect *)ta_vec_at(entries, at - 1u) = *(ta_activation_effect *)ta_vec_at(entries, at);
        }
        entries->count -= 1u;
        found = true;
    }
    return found;
}

TA_PRIVATE bool ta_actor_state_active(const ta_actor *actor, ta_state_id state) {
    const ta_definition *definition = actor->definition;
    if (state >= definition->state_count) {
        return false;
    }
    if (actor->live_active != NULL) {
        return ta_bits_get(actor->live_active, state);
    }
    if (ta_definition_is_atomic(definition, state)) {
        return ta_configuration_contains_atomic(&actor->configuration, state);
    }
    for (ta_state_id leaf = 0; leaf < definition->state_count; leaf += 1) {
        if (ta_configuration_contains_atomic(&actor->configuration, leaf) &&
            ta_definition_is_descendant(definition, leaf, state)) {
            return true;
        }
    }
    return false;
}

TA_PRIVATE void ta_actor_exit_invoked(ta_system *system, ta_actor_id id) {
    ta_interpreter_exit_invoked(system, id);
}

/*
 * Takes the head of the free list, or grows the table when it is empty.
 * Scanning the table for a hole instead would make every spawn cost a walk
 * over the live population, which is what invariant 17 forbids: an ephemeral
 * child of a four-million-actor system must not pay for its siblings.
 */
static ta_result ta_actor_claim_slot(ta_system *system, ta_actor *actor, ta_actor_id *out) {
    uint32_t slot = system->free_actor_slot;
    if (slot == UINT32_MAX) {
        ta_result result = ta_actor_table_grow(system, &slot);
        if (result != TA_OK) {
            return result;
        }
    } else {
        system->free_actor_slot = ta_actor_slot_at(system, slot)->next_free;
    }
    ta_actor_slot *entry = ta_actor_slot_at(system, slot);
    entry->actor = actor;
    entry->next_free = UINT32_MAX;
    *out = ta_actor_id_make(slot, entry->generation);
    return TA_OK;
}

/*
 * Tears the actor down completely, then uncounts it. The last actor of a
 * definition nothing holds frees it (ADR 0077), so nothing below may touch it.
 */
TA_PRIVATE void ta_actor_free(ta_system *system, ta_actor *actor) {
    const ta_definition *definition = actor->definition;
    ta_actor_discard_activation_effects(actor);
    if (actor->datamodel_initialized) {
        definition->datamodel.v->destroy_instance(definition->datamodel.self, definition->datamodel_shared,
                                                  actor->datamodel_instance);
    }
    /* A named actor holds a reference to its address; a runtime ref holds nothing. */
    ta_actor_ref_release(&actor->record.self);
    ta_interpreter_release_actor_state(system, actor);
    if (actor->session_node != NULL) {
        ta_session_remove(&system->sessions, actor->session_node);
        ta_session_node_free(&system->sessions, actor->session_node);
    }
    /* The record, the session storage and the instance are one block (ADR 0063). */
    ta_mem_free(system->allocator, actor,
                &(ta_allocator_free_params){ .size = definition->actor_block_bytes,
                                             .alignment = definition->actor_block_alignment });
    ta_definition *owner = ta_system_definition(system, definition->id);
    owner->actor_count -= 1u;
    system->live_actors -= 1u;
    ta_system_settle_definition(system, owner);
}

/*
 * Takes the address for the actor: copies its text into actor-owned storage
 * and claims it in the host directory. claim is a compare-and-swap against
 * "no entry", so an address another actor holds — resident or unloaded —
 * answers TA_ERR_STALE and this spawn leaves nothing behind.
 */
static ta_result ta_actor_claim_address(ta_system *system,
                                        ta_actor *actor,
                                        const ta_actor_address *address) {
    if (system->directory.v == NULL || system->directory.v->claim == NULL) {
        /* A logical address is only meaningful against a host directory. */
        return TA_ERR_UNSUPPORTED;
    }
    if (address->family.name.ptr == NULL || address->family.name.len == 0 ||
        address->name.ptr == NULL || address->name.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_actor_ref self = { .kind = TA_ACTOR_REF_NAMED };
    ta_result result = ta_address_intern(system, address, &self.as.named);
    if (result != TA_OK) {
        return result;
    }
    result = system->directory.v->claim(
        system->directory.self,
        &(ta_actor_directory_claim_params){
            .address = self.as.named,
            .entry = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id },
        });
    if (result != TA_OK) {
        ta_actor_ref_release(&self);
        return result;
    }
    /* From here the actor answers to its address: every origin core writes
     * for it is the address, so a reply outlives this runtime ID. */
    actor->record.self = self;
    return TA_OK;
}

/*
 * Frees the slot an actor was given, so a failed spawn reuses it. The slot's
 * next occupant gets a new generation, so this ID goes stale. Generation 0
 * is never issued (ADR 0059): a wrap from UINT32_MAX goes to 1.
 */
TA_PRIVATE void ta_actor_release_slot(ta_system *system, ta_actor_id id) {
    uint32_t slot = ta_actor_id_slot(id);
    ta_actor_slot *entry = ta_actor_slot_at(system, slot);
    entry->actor = NULL;
    entry->generation = entry->generation == UINT32_MAX ? 1u : entry->generation + 1u;
    entry->next_free = system->free_actor_slot;
    system->free_actor_slot = slot;
}

/*
 * Gives up the route of a named actor that is being destroyed. remove
 * compares against this actor's own entry, so an incumbent whose address has
 * already been taken over by an activated replacement removes nothing: the
 * successor keeps the route. A failed comparison is that case, not an error.
 */
static void ta_actor_release_address(ta_system *system, const ta_actor *actor) {
    if (actor->record.self.kind != TA_ACTOR_REF_NAMED || !actor->published) {
        return;
    }
    if (system->directory.v == NULL || system->directory.v->remove == NULL) {
        return;
    }
    if (system->directory.v->resolve != NULL) {
        ta_actor_directory_entry current = {0};
        ta_result result = system->directory.v->resolve(
            system->directory.self,
            &(ta_actor_directory_resolve_params){ .address = actor->record.self.as.named, .out = &current });
        if (result == TA_OK &&
            (current.kind != TA_ACTOR_DIRECTORY_RESIDENT || current.actor != actor->record.id)) {
            return;
        }
    }
    (void)system->directory.v->remove(
        system->directory.self,
        &(ta_actor_directory_remove_params){
            .address = actor->record.self.as.named,
            .expected = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id },
        });
}

/*
 * The host directory's compare-and-swap. Every residency change is one of
 * these: the caller states the entry it believes is there, and a directory
 * that disagrees answers TA_ERR_STALE and changes nothing. That is what
 * keeps a residency change retryable — a refused swap has done nothing, so
 * the caller may simply try again (ADR 0014).
 */
TA_PRIVATE ta_result ta_actor_directory_swap(ta_system *system, const ta_actor_directory_replace_params *params) {
    if (system->directory.v == NULL || system->directory.v->replace == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    return system->directory.v->replace(system->directory.self, params);
}

/* The same comparison, for an entry that should end up gone. */
TA_PRIVATE ta_result ta_actor_directory_drop(ta_system *system, const ta_actor_directory_remove_params *params) {
    if (system->directory.v == NULL || system->directory.v->remove == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    return system->directory.v->remove(system->directory.self, params);
}

/*
 * Releases the name of an actor that is not resident. The comparison is
 * against UNLOADED, so this cannot remove a name some live actor answers to:
 * a resident entry, or none at all, is TA_ERR_STALE. Core owns nothing of
 * the actor at this point — the host deletes its checkpoint afterwards.
 */
TA_API ta_result ta_actor_remove_unloaded(ta_system *system, const ta_actor_remove_unloaded_params *params) {
    if (system == NULL || params == NULL || params->address == NULL) {
        return TA_ERR_INVALID;
    }
    ta_address *address = NULL;
    ta_result result = ta_address_intern(system, params->address, &address);
    if (result != TA_OK) {
        return result;
    }
    result = ta_actor_directory_drop(
        system,
        &(ta_actor_directory_remove_params){
            .address = address,
            .expected = { .kind = TA_ACTOR_DIRECTORY_UNLOADED },
        });
    /* The unloaded session ends with its name (ADR 0066). */
    if (result == TA_OK && address->unloaded != NULL) {
        ta_session_node *parked = address->unloaded;
        ta_session_remove(&system->sessions, parked);
        ta_session_node_free(&system->sessions, parked);
    }
    ta_address_release(address);
    return result;
}

/*
 * Resolves a logical address through the host directory. An unloaded actor
 * exists but has no runtime ID, which is TA_ERR_UNLOADED and leaves *out
 * alone; only a resident entry writes one.
 */
TA_API ta_result ta_actor_find(ta_system *system, const ta_actor_find_params *params) {
    if (system == NULL || params == NULL || params->address == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    if (system->directory.v == NULL || system->directory.v->resolve == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_address *address = NULL;
    ta_result result = ta_address_intern(system, params->address, &address);
    if (result != TA_OK) {
        return result;
    }
    ta_actor_directory_entry entry = { .kind = TA_ACTOR_DIRECTORY_NONE };
    result = system->directory.v->resolve(
        system->directory.self,
        &(ta_actor_directory_resolve_params){ .address = address, .out = &entry });
    ta_address_release(address);
    if (result != TA_OK) {
        return result;
    }
    if (entry.kind == TA_ACTOR_DIRECTORY_RESIDENT) {
        *params->out = entry.actor;
        return TA_OK;
    }
    return entry.kind == TA_ACTOR_DIRECTORY_UNLOADED ? TA_ERR_UNLOADED : TA_ERR_NOT_FOUND;
}

/*
 * A blank actor of `definition`: session storage zeroed, a data-model
 * instance allocated but not initialized, a slot claimed, and a record that
 * answers to its runtime ID. Spawn bootstraps it; reconstruction fills it
 * from an image instead, so both start here.
 */
TA_PRIVATE ta_result ta_actor_new(ta_system *system, ta_definition *definition, ta_actor **out) {
    /* maxActors is answered as an allocator that ran out (ADR 0092 section 4). */
    if (!ta_system_quota_actor(system)) {
        return TA_ERR_NOMEM;
    }
    /* One allocation: the record, the session storage, the instance (ADR 0063). */
    ta_actor *actor = ta_mem_alloc(system->allocator, definition->actor_block_bytes, definition->actor_block_alignment);
    if (actor == NULL) {
        return TA_ERR_NOMEM;
    }
    unsigned char *block = (unsigned char *)actor;
    *actor = (ta_actor){ .definition = definition, .published = true, .macrostep = true };
    /* Counted from here so every ta_actor_free() path can uncount it. */
    definition->actor_count += 1u;
    system->live_actors += 1u;
    actor->host_impl = (ta_dm_host_impl){ .system = system, .actor = actor };
    actor->host = (ta_dm_host){ .impl = &actor->host_impl };

    size_t config_bytes = definition->configuration.bytes;
    actor->session_bytes = definition->actor_session_bytes;
    actor->session_storage = block + definition->actor_session_offset;
    ta_mem_zero(actor->session_storage, actor->session_bytes);
    unsigned char *storage = actor->session_storage;
    actor->configuration = (ta_configuration){ .storage = storage, .layout = &definition->configuration };
    actor->late_initialized = (uint64_t *)(storage + config_bytes);
    actor->history = (ta_history_state){ .storage = storage + config_bytes * 2u, .layout = &definition->history };

    /* An instance of size 0 (the null data model) points at the block's end. */
    actor->datamodel_instance = block + definition->actor_instance_offset;
    ta_mem_zero(actor->datamodel_instance, definition->datamodel_instance_size);

    ta_actor_id id = 0;
    ta_result result = ta_actor_claim_slot(system, actor, &id);
    if (result != TA_OK) {
        ta_actor_free(system, actor);
        return result;
    }
    actor->record = (ta_actor_record){
        .id = id,
        .definition = definition->id,
        .self = { .kind = TA_ACTOR_REF_RUNTIME, .family = definition->family, .as.runtime = id },
        .residency = TA_ACTOR_RUNNING,
    };
    /* A new session is named by the runtime ID it starts with (ADR 0066). */
    actor->session_id = id;
    *out = actor;
    return TA_OK;
}

/*
 * Re-arms the delayed sends a host captured from an earlier session of this
 * actor (ADR 0074 section 8). Everything is checked and the timer list
 * reserved first, so scheduling cannot fail halfway; a failure leaves the
 * actor to be freed by the caller, which frees whatever was armed.
 */
static ta_result ta_actor_rearm_sends(ta_system *system, ta_actor *actor, const ta_actor_spawn_params *params) {
    uint32_t count = params->pending_send_count;
    if (count == 0) {
        return TA_OK;
    }
    if (params->pending_sends == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = actor->definition;
    for (uint32_t index = 0; index < count; index += 1) {
        const ta_delayed_send_info *info = &params->pending_sends[index];
        if (!ta_event_name_valid(&system->event_names, info->event) ||
            (info->has_content && info->content >= definition->content_count) ||
            (info->has_target_text && info->target_text >= definition->content_count) ||
            (info->destination.kind == TA_ACTOR_REF_RUNTIME &&
             !ta_atom_valid(&system->atoms, info->destination.family))) {
            return TA_ERR_INVALID;
        }
    }
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_result result = ta_timer_reserve(&system->timers, &sparse->timers, count);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        const ta_delayed_send_info *info = &params->pending_sends[index];
        ta_delayed_send send = {
            .owner = actor->record.id,
            .send_id = info->send_id,
            .due = info->due,
            .event = info->event,
            .io_type = info->io_type,
            .destination = info->destination,
            .target = info->target,
            .data = info->data,
            .has_content = info->has_content,
            .content = info->content,
            .has_target_text = info->has_target_text,
            .target_text = info->target_text,
            .invoke_id = TA_INVOKE_ID_NONE,
            .trace = info->trace,
        };
        /* Runtime ID 0 names no actor, so as an SCXML send's destination it
         * means the session being spawned: a send to itself, stored by a
         * host that cannot know the new session's ID in advance. */
        if (send.destination.kind == TA_ACTOR_REF_RUNTIME && send.destination.as.runtime == 0 &&
            ta_system_is_scxml_io(system, send.io_type)) {
            send.destination.as.runtime = actor->record.id;
            send.destination.family = definition->family;
        }
        ta_actor_ref_retain(&send.destination);
        ta_value_retain(&send.target);
        ta_value_retain(&send.data);
        result = ta_timer_schedule(&system->timers, &sparse->timers, &send);
        if (result != TA_OK) {
            ta_actor_ref_release(&send.destination);
            ta_value_release(&send.target);
            ta_value_release(&send.data);
            break;
        }
        /* A later <send> never reuses a re-armed generated ID. */
        if ((send.send_id & TA_GENERATED_SEND_ID_BIT) != 0) {
            uint64_t counter = send.send_id & ~TA_GENERATED_SEND_ID_BIT;
            if (counter > actor->next_send_id) {
                actor->next_send_id = counter;
            }
        }
    }
    return result;
}

/* How many invoking parents stand above an actor: 0 for one nobody invoked. */
static uint32_t ta_actor_invoke_depth(ta_system *system, ta_actor_id id) {
    uint32_t depth = 0;
    const ta_actor *actor = ta_actor_lookup(system, id);
    while (actor != NULL && actor->sparse != NULL && actor->sparse->has_parent && depth < UINT32_MAX - 1u) {
        depth += 1u;
        actor = ta_actor_lookup(system, actor->sparse->parent.actor);
    }
    return depth;
}

TA_API ta_result ta_actor_spawn(ta_system *system, const ta_actor_spawn_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_definition *definition = ta_system_definition(system, params->definition);
    if (definition == NULL || definition->retired) {
        return TA_ERR_NOT_FOUND;
    }
    if (params->parent != NULL && ta_actor_invoke_depth(system, params->parent->actor) + 1u >=
                                      system->params[TA_PARAM_INTERPRETER_NESTING]) {
        /* Exiting it would need an interpreter region past interpreterNesting
         * (ADR 0092 section 3). */
        return TA_ERR_UNSUPPORTED;
    }
    ta_actor *actor = NULL;
    ta_result result = ta_actor_new(system, definition, &actor);
    if (result != TA_OK) {
        return result;
    }
    ta_actor_id id = actor->record.id;

    /* A traced spawn's parent waits in an event block for the bootstrap
     * macrostep (ADR 0067); an untraced one needs no block. */
    if (!ta_trace_parent_empty(&params->trace)) {
        result = ta_interpreter_hold_block(system, actor);
        if (result != TA_OK) {
            ta_actor_release_slot(system, id);
            ta_actor_free(system, actor);
            return result;
        }
        actor->event_block->slots.trace = params->trace;
    }

    if (params->parent != NULL) {
        actor->sparse = ta_mem_alloc(system->allocator, sizeof(ta_actor_sparse_state),
                                     _Alignof(ta_actor_sparse_state));
        if (actor->sparse == NULL) {
            ta_actor_release_slot(system, id);
            ta_actor_free(system, actor);
            return TA_ERR_NOMEM;
        }
        *actor->sparse = (ta_actor_sparse_state){ .parent = *params->parent, .has_parent = true };
    }

    result = definition->datamodel.v->init_instance(
        definition->datamodel.self,
        &(ta_datamodel_init_instance_params){
            .shared = definition->datamodel_shared,
            .instance = actor->datamodel_instance,
            .binding = params->datamodel_binding,
            .host = &actor->host,
        });
    if (result != TA_OK) {
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor);
        return result;
    }
    actor->datamodel_initialized = true;
    result = ta_actor_rearm_sends(system, actor, params);
    if (result != TA_OK) {
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor); /* frees what was re-armed, too */
        return result;
    }
    /* The claim is the last fallible step: bootstrap has not run yet, so a
     * refused address leaves no trace of this spawn anywhere. */
    if (params->address != NULL) {
        result = ta_actor_claim_address(system, actor, params->address);
        if (result != TA_OK) {
            ta_actor_release_slot(system, id);
            ta_actor_free(system, actor);
            return result;
        }
    }
    if (params->parent != NULL) {
        /* Invoke: the invoking parent routes #_<invokeid> and autoforwarding to this child. */
        ta_invoke_attach_child(system, params->parent, id);
    }
    ta_trace_lifecycle(system, actor, TA_TRACE_SPAWNED, &params->trace);
    *params->out = id;
    return TA_OK;
}

TA_PRIVATE bool ta_actor_replaying(const ta_actor *actor) {
    return !actor->published && actor->sparse != NULL && actor->sparse->activation_effects != NULL &&
           actor->sparse->activation_effects->replaying;
}

TA_PRIVATE ta_time ta_actor_now(const ta_system *system, const ta_actor *actor) {
    return actor != NULL && ta_actor_replaying(actor) ? actor->sparse->activation_effects->clock
                                                      : ta_clock_now(&system->clock);
}

/* The held-effects record a prepared actor keeps; a replaying one needs it
 * from the start, for its clock. */
static ta_activation_effects *ta_actor_activation(ta_system *system, ta_actor *actor) {
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return NULL;
    }
    if (sparse->activation_effects == NULL) {
        sparse->activation_effects = ta_mem_alloc(system->allocator, sizeof(ta_activation_effects),
                                                  _Alignof(ta_activation_effects));
        if (sparse->activation_effects == NULL) {
            return NULL;
        }
        *sparse->activation_effects = (ta_activation_effects){0};
        ta_vec_init(&sparse->activation_effects->entries, system->allocator, TA_VEC_LAYOUT(ta_activation_effect));
    }
    return sparse->activation_effects;
}

/* The named self of a prepared actor: the incumbent's, or `address` interned. */
static ta_result ta_actor_prepared_self(ta_system *system,
                                        const ta_actor_prepare_params *params,
                                        const ta_definition *definition,
                                        ta_actor_ref *out) {
    if (params->incumbent != 0) {
        ta_actor *incumbent = ta_actor_lookup(system, params->incumbent);
        if (incumbent == NULL) {
            return TA_ERR_NOT_FOUND;
        }
        if (!incumbent->published || incumbent->record.self.kind != TA_ACTOR_REF_NAMED ||
            !ta_str_equal(ta_address_view(incumbent->record.self.as.named).family.name,
                          definition->identity.family.name)) {
            return TA_ERR_INVALID;
        }
        *out = incumbent->record.self;
        ta_actor_ref_retain(out);
        return TA_OK;
    }
    const ta_actor_address *address = params->address;
    if (address == NULL || address->name.ptr == NULL || address->name.len == 0 ||
        !ta_str_equal(address->family.name, definition->identity.family.name)) {
        return TA_ERR_INVALID;
    }
    if (system->directory.v == NULL || system->directory.v->claim == NULL || system->directory.v->replace == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_NAMED };
    return ta_address_intern(system, address, &out->as.named);
}

/* ------------------------------------------------------------------------
 * An actor's state, exported and restored (ADR 0094 section 3)
 */


/* Everything a state names must exist in `definition`, before anything is
 * created. */
static ta_result ta_actor_state_check(const ta_system *system, const ta_definition *definition,
                                      const ta_actor_state *state) {
    const ta_definition_identity *have = &definition->identity;
    const ta_definition_identity *want = &state->definition;
    ta_str have_revision = { .ptr = (const char *)have->revision.ptr, .len = have->revision.len };
    ta_str want_revision = { .ptr = (const char *)want->revision.ptr, .len = want->revision.len };
    if (!ta_str_equal(have->family.name, want->family.name) || !ta_str_equal(have_revision, want_revision)) {
        return TA_ERR_DEFINITION_MISMATCH;
    }
    if ((state->active_count != 0 && state->active == NULL) || (state->late_count != 0 && state->late == NULL) ||
        (state->history_count != 0 && state->history == NULL) || (state->send_count != 0 && state->sends == NULL) ||
        (state->invoke_count != 0 && state->invokes == NULL)) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < state->active_count; index += 1u) {
        if (state->active[index] >= definition->state_count) {
            return TA_ERR_NOT_FOUND;
        }
    }
    for (uint32_t index = 0; index < state->late_count; index += 1u) {
        if (state->late[index] >= definition->state_count) {
            return TA_ERR_NOT_FOUND;
        }
    }
    for (uint32_t index = 0; index < state->history_count; index += 1u) {
        const ta_history_entry *entry = &state->history[index];
        if (entry->history >= definition->state_count || entry->state >= definition->state_count ||
            definition->states[entry->history].history_slot == UINT32_MAX) {
            return TA_ERR_NOT_FOUND;
        }
    }
    for (uint32_t index = 0; index < state->invoke_count; index += 1u) {
        if (state->invokes[index].decl >= definition->invoke_decl_count) {
            return TA_ERR_NOT_FOUND;
        }
    }
    for (uint32_t index = 0; index < state->send_count; index += 1u) {
        const ta_delayed_send_info *info = &state->sends[index];
        if (info->submitted) {
            continue;
        }
        if (!ta_event_name_valid(&system->event_names, info->event) || !ta_atom_valid(&system->atoms, info->io_type) ||
            (info->has_content && info->content >= definition->content_count) ||
            (info->has_target_text && info->target_text >= definition->content_count)) {
            return TA_ERR_INVALID;
        }
    }
    return TA_OK;
}

/* Gives a replaying actor, prepared and not bootstrapped, a checked state. */
static ta_result ta_actor_state_restore(ta_system *system, ta_actor *actor, const ta_actor_state *state) {
    const ta_definition *definition = actor->definition;
    ta_configuration_clear(&actor->configuration);
    for (uint32_t index = 0; index < state->active_count; index += 1u) {
        ta_configuration_set_atomic(&actor->configuration, state->active[index], true);
    }
    for (uint32_t index = 0; index < state->late_count && actor->late_initialized != NULL; index += 1u) {
        ta_bits_put(actor->late_initialized, state->late[index], true);
    }
    for (uint32_t index = 0; index < state->history_count; index += 1u) {
        const ta_history_entry *entry = &state->history[index];
        uint32_t slot = definition->states[entry->history].history_slot;
        ta_bits_put(ta_history_slot_words(&actor->history, slot), entry->state, true);
    }
    /* It starts idle: the state it restores is past its bootstrap. */
    actor->bootstrapped = true;
    actor->macrostep = false;
    actor->record.residency = TA_ACTOR_IDLE;
    actor->next_send_id = state->generated_ids;
    actor->trace_macrostep = state->macrostep;
    actor->trace_microstep = state->microstep;
    actor->trace_lifecycle = state->lifecycle;
    ta_result result = TA_OK;
    for (uint32_t index = 0; result == TA_OK && index < state->send_count; index += 1u) {
        const ta_delayed_send_info *info = &state->sends[index];
        if (info->submitted) {
            continue; /* it left the actor before the state was taken */
        }
        ta_delayed_send send = { .owner = actor->record.id,
                                 .send_id = info->send_id,
                                 .due = info->due,
                                 .event = info->event,
                                 .io_type = info->io_type,
                                 .destination = info->destination,
                                 .target = info->target,
                                 .data = info->data,
                                 .has_content = info->has_content,
                                 .content = info->content,
                                 .has_target_text = info->has_target_text,
                                 .target_text = info->target_text,
                                 .invoke_id = TA_INVOKE_ID_NONE,
                                 .trace = info->trace };
        /* Runtime ID 0, as for ta_actor_spawn()'s pending sends, is the
         * actor itself: a host cannot know its new session in advance. */
        if (send.destination.kind == TA_ACTOR_REF_RUNTIME && send.destination.as.runtime == 0 &&
            ta_system_is_scxml_io(system, send.io_type)) {
            send.destination.as.runtime = actor->record.id;
            send.destination.family = definition->family;
        }
        ta_actor_ref_retain(&send.destination);
        ta_value_retain(&send.target);
        ta_value_retain(&send.data);
        result = ta_actor_hold_send(system, actor, &(ta_activation_send){ .send = send, .immediate = false });
        if (result != TA_OK) {
            ta_actor_ref_release(&send.destination);
            ta_value_release(&send.target);
            ta_value_release(&send.data);
        }
    }
    for (uint32_t index = 0; result == TA_OK && index < state->invoke_count; index += 1u) {
        const ta_invoke_state *invoke = &state->invokes[index];
        result = ta_invoke_restore(system, actor, &(ta_invoke_restore_params){ .id = invoke->id,
                                                                              .type = invoke->type,
                                                                              .decl = invoke->decl });
    }
    return result;
}

TA_API ta_result ta_actor_export_state(ta_system *system, const ta_actor_export_params *params) {
    if (system == NULL || params == NULL || params->state_count == NULL || params->history_count == NULL ||
        params->invoke_count == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    *params->state_count = 0;
    *params->history_count = 0;
    *params->invoke_count = 0;
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (!actor->published) {
        return TA_ERR_INVALID;
    }
    if (actor->finished) {
        return TA_ERR_DONE;
    }
    if (actor->macrostep || ta_actor_external_event(actor) != NULL || ta_interpreter_internal_pending(actor)) {
        return TA_ERR_BUSY;
    }
    const ta_definition *definition = actor->definition;
    const ta_actor_sparse_state *sparse = actor->sparse;
    uint32_t active = 0;
    uint32_t late = 0;
    uint32_t history = 0;
    uint32_t invokes = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1u) {
        active += ta_configuration_contains_atomic(&actor->configuration, state) ? 1u : 0u;
        late += actor->late_initialized != NULL && ta_bits_get(actor->late_initialized, state) ? 1u : 0u;
        uint32_t slot = definition->states[state].history_slot;
        for (ta_state_id recorded = 0; slot != UINT32_MAX && recorded < definition->state_count; recorded += 1u) {
            history += ta_bits_get(ta_history_slot_words(&actor->history, slot), recorded) ? 1u : 0u;
        }
    }
    for (uint32_t index = 0; sparse != NULL && index < sparse->invoke_count; index += 1u) {
        invokes += sparse->invokes[index].active ? 1u : 0u;
    }
    *params->state_count = active + late;
    *params->history_count = history;
    *params->invoke_count = invokes;
    if (active + late > params->state_capacity || history > params->history_capacity ||
        invokes > params->invoke_capacity || (active + late != 0 && params->states == NULL) ||
        (history != 0 && params->history == NULL) || (invokes != 0 && params->invokes == NULL)) {
        return TA_ERR_NOMEM;
    }
    uint32_t next_active = 0;
    uint32_t next_late = active;
    uint32_t next_history = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1u) {
        if (ta_configuration_contains_atomic(&actor->configuration, state)) {
            params->states[next_active++] = state;
        }
        if (actor->late_initialized != NULL && ta_bits_get(actor->late_initialized, state)) {
            params->states[next_late++] = state;
        }
        uint32_t slot = definition->states[state].history_slot;
        for (ta_state_id recorded = 0; slot != UINT32_MAX && recorded < definition->state_count; recorded += 1u) {
            if (ta_bits_get(ta_history_slot_words(&actor->history, slot), recorded)) {
                params->history[next_history++] = (ta_history_entry){ .history = state, .state = recorded };
            }
        }
    }
    uint32_t next_invoke = 0;
    for (uint32_t index = 0; sparse != NULL && index < sparse->invoke_count; index += 1u) {
        if (sparse->invokes[index].active) {
            params->invokes[next_invoke++] = (ta_invoke_state){ .id = sparse->invokes[index].id,
                                                                .type = sparse->invokes[index].type,
                                                                .decl = ta_invoke_table_decl(&sparse->invoke_table, index) };
        }
    }
    ta_value root = { 0 };
    if (definition->datamodel.v->inspect != NULL) {
        if (definition->datamodel.v->inspect(definition->datamodel.self,
                                             &(ta_datamodel_inspect_params){ .shared = definition->datamodel_shared,
                                                                             .instance = actor->datamodel_instance,
                                                                             .host = &actor->host,
                                                                             .out = &root }) != TA_OK) {
            root = (ta_value){ 0 };
        }
    }
    *params->out = (ta_actor_state){
        .definition = definition->identity,
        .generated_ids = actor->next_send_id,
        .clock = ta_actor_now(system, actor),
        .macrostep = actor->trace_macrostep,
        .microstep = actor->trace_microstep,
        .lifecycle = actor->trace_lifecycle,
        .active = active != 0 ? params->states : NULL,
        .active_count = active,
        .late = late != 0 ? params->states + active : NULL,
        .late_count = late,
        .history = history != 0 ? params->history : NULL,
        .history_count = history,
        .sends = sparse != NULL && sparse->timers.count != 0 ? sparse->timers.sends : NULL,
        .send_count = sparse != NULL ? sparse->timers.count : 0,
        .invokes = invokes != 0 ? params->invokes : NULL,
        .invoke_count = invokes,
        .root = root,
    };
    return TA_OK;
}

TA_API ta_result ta_actor_prepare(ta_system *system, const ta_actor_prepare_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_definition *definition = ta_system_definition(system, params->definition);
    if (definition == NULL || definition->retired) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_time *at = params->at;
    const ta_value *binding = params->datamodel_binding;
    ta_time spawn_at = 0;
    /* A state gives the clock, the binding and everything else (ADR 0094). */
    const ta_actor_state *state = params->state;
    if (state != NULL) {
        if (params->at != NULL) {
            return TA_ERR_INVALID;
        }
        ta_result checked = ta_actor_state_check(system, definition, state);
        if (checked != TA_OK) {
            return checked;
        }
        spawn_at = state->clock;
        at = &spawn_at;
        if (binding == NULL && state->root.v != NULL) {
            binding = &state->root;
        }
    }
    if (at != NULL && *at > ta_clock_now(&system->clock)) {
        return TA_ERR_INVALID;
    }
    ta_actor_ref self = {0};
    ta_result result = ta_actor_prepared_self(system, params, definition, &self);
    if (result != TA_OK) {
        return result;
    }
    ta_actor *actor = NULL;
    result = ta_actor_new(system, definition, &actor);
    if (result != TA_OK) {
        ta_actor_ref_release(&self);
        return result;
    }
    ta_actor_id id = actor->record.id;
    actor->published = false;
    actor->replacement_of = params->incumbent;
    actor->record.self = self;
    if (at != NULL) {
        ta_activation_effects *effects = ta_actor_activation(system, actor);
        if (effects == NULL) {
            result = TA_ERR_NOMEM;
        } else {
            effects->replaying = true;
            effects->clock = *at;
        }
    }
    /* A traced bootstrap's parent waits in an event block (ADR 0067). */
    if (result == TA_OK && !ta_trace_parent_empty(&params->trace)) {
        result = ta_interpreter_hold_block(system, actor);
        if (result == TA_OK) {
            actor->event_block->slots.trace = params->trace;
        }
    }
    if (result == TA_OK) {
        result = definition->datamodel.v->init_instance(
            definition->datamodel.self,
            &(ta_datamodel_init_instance_params){
                .shared = definition->datamodel_shared,
                .instance = actor->datamodel_instance,
                .binding = binding,
                .host = &actor->host,
            });
        actor->datamodel_initialized = result == TA_OK;
    }
    if (result == TA_OK && state != NULL) {
        result = ta_actor_state_restore(system, actor, state);
    }
    if (result != TA_OK) {
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor);
        return result;
    }
    *params->out = id;
    return TA_OK;
}

/* Held sends that publishing will schedule: all of them, or, with a clock
 * move to `*at`, those due after it. */
static uint32_t ta_actor_activation_send_count(const ta_actor *actor, const ta_time *at) {
    const ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL || sparse->activation_effects == NULL) {
        return 0;
    }
    uint32_t count = 0;
    const ta_vec *entries = &sparse->activation_effects->entries;
    for (uint32_t index = 0; index < entries->count; index += 1u) {
        const ta_activation_effect *effect = ta_vec_at(entries, index);
        count += effect->kind == TA_ACTIVATION_SEND && (at == NULL || effect->as.send.send.due > *at) ? 1u : 0u;
    }
    return count;
}

static void ta_actor_free_activation(ta_actor_sparse_state *sparse) {
    ta_activation_effects *effects = sparse->activation_effects;
    ta_allocator allocator = effects->entries.allocator;
    ta_vec_destroy(&effects->entries);
    ta_mem_free(allocator, effects,
                &(ta_allocator_free_params){ .size = sizeof(ta_activation_effects),
                                             .alignment = _Alignof(ta_activation_effects) });
    sparse->activation_effects = NULL;
}

/*
 * Releases what an actor prepared without `at` held: logs to the sink,
 * immediate sends dispatched, delayed sends scheduled. A replaying actor
 * holds only delayed sends, and they are scheduled: it dropped everything
 * else as it ran (ADR 0089 section 3).
 */
static void ta_actor_release_activation_effects(ta_system *system, ta_actor *actor) {
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL || sparse->activation_effects == NULL) {
        return;
    }
    ta_activation_effects *effects = sparse->activation_effects;
    for (uint32_t index = 0; index < effects->entries.count; index += 1u) {
        ta_activation_effect *effect = ta_vec_at(&effects->entries, index);
        if (effect->kind == TA_ACTIVATION_LOG) {
            if (system->log_sink.v != NULL) {
                system->log_sink.v->write(system->log_sink.self, &effect->as.log);
            }
            ta_value_release(&effect->as.log.value);
            continue;
        }
        ta_delayed_send *send = &effect->as.send.send;
        if (effect->as.send.immediate) {
            ta_external_event_submit_status status = ta_interpreter_dispatch_delayed(system, send, false);
            if (status != TA_EXTERNAL_EVENT_RETRY) {
                continue;
            }
            /* Dispatch marked it submitted and took its origin and invoke ID. */
        }
        /* Reserved by ta_actor_publish(), so this cannot fail. */
        (void)ta_timer_schedule(&system->timers, &sparse->timers, send);
    }
    ta_actor_free_activation(sparse);
}

/*
 * Puts a prepared actor on the route, as one directory call. With no
 * incumbent, an address with no entry is claimed and an UNLOADED one is
 * replaced, and the unloaded session ends with it (ADR 0066); a resident
 * one is BUSY.
 */
static ta_result ta_actor_publish_route(ta_system *system, ta_actor *actor, ta_actor_id incumbent) {
    ta_address *address = actor->record.self.as.named;
    ta_actor_directory_entry resident = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id };
    if (incumbent != 0) {
        return ta_actor_directory_swap(
            system, &(ta_actor_directory_replace_params){
                        .address = address,
                        .expected = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = incumbent },
                        .replacement = resident,
                    });
    }
    ta_result result = system->directory.v->claim(
        system->directory.self, &(ta_actor_directory_claim_params){ .address = address, .entry = resident });
    if (result != TA_ERR_STALE) {
        return result;
    }
    result = ta_actor_directory_swap(system, &(ta_actor_directory_replace_params){
                                                 .address = address,
                                                 .expected = { .kind = TA_ACTOR_DIRECTORY_UNLOADED },
                                                 .replacement = resident,
                                             });
    if (result == TA_ERR_STALE) {
        return TA_ERR_BUSY;
    }
    if (result == TA_OK && address->unloaded != NULL) {
        ta_session_node *parked = address->unloaded;
        ta_session_remove(&system->sessions, parked);
        ta_session_node_free(&system->sessions, parked);
    }
    return result;
}

TA_API ta_result ta_actor_publish(ta_system *system, const ta_actor_publish_params *params) {
    if (system == NULL || params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    ta_actor *incumbent = params->incumbent == 0 ? NULL : ta_actor_lookup(system, params->incumbent);
    if (actor == NULL || (params->incumbent != 0 && incumbent == NULL)) {
        return TA_ERR_NOT_FOUND;
    }
    if (actor->published || actor->replacement_of != params->incumbent ||
        actor->record.self.kind != TA_ACTOR_REF_NAMED ||
        (incumbent != NULL && (!incumbent->published || incumbent->record.self.kind != TA_ACTOR_REF_NAMED))) {
        return TA_ERR_INVALID;
    }
    if (actor->finished) {
        return TA_ERR_DONE;
    }
    if (actor->macrostep || actor->record.residency != TA_ACTOR_IDLE) {
        return TA_ERR_BUSY;
    }
    bool replaying = ta_actor_replaying(actor);
    /* A last clock move is only for a replaying actor (ADR 0094 section 1). */
    const ta_time *at = params->at;
    if (at != NULL && (!replaying || !ta_actor_input_time_valid(system, actor, *at))) {
        return TA_ERR_INVALID;
    }
    /* Everything that can fail comes first, so a refusal changes nothing. */
    uint32_t sends = ta_actor_activation_send_count(actor, at);
    ta_result result = sends == 0 ? TA_OK : ta_timer_reserve(&system->timers, &actor->sparse->timers, sends);
    if (result == TA_OK && replaying) {
        result = ta_invoke_prepare_retire(system, actor);
    }
    if (result == TA_OK) {
        result = ta_actor_publish_route(system, actor, params->incumbent);
    }
    if (result != TA_OK) {
        return result;
    }
    if (at != NULL) {
        ta_actor_move_clock(actor, *at);
    }
    actor->published = true;
    actor->replacement_of = 0;
    ta_actor_touch(actor);
    ta_actor_release_activation_effects(system, actor);
    if (replaying) {
        /* Its services never started, and the world moved on while it was
         * away: each active invocation is reported lost, as after a load. */
        if (ta_invoke_retire_all(system, actor)) {
            ta_interpreter_start_macrostep(actor, NULL);
        }
    } else if (actor->sparse != NULL && actor->sparse->pending_invokes.count != 0) {
        actor->macrostep = true;
        actor->record.residency = TA_ACTOR_RUNNING;
    }
    return TA_OK;
}

/*
 * Whether `time` may be an input's time for `actor` (ADR 0094 section 1): a
 * replaying actor's is no earlier than its clock and no later than the
 * system's; every other actor's is the system's own.
 */
TA_PRIVATE bool ta_actor_input_time_valid(const ta_system *system, const ta_actor *actor, ta_time time) {
    ta_time now = ta_clock_now(&system->clock);
    if (!ta_actor_replaying(actor)) {
        return time == now;
    }
    return time >= actor->sparse->activation_effects->clock && time <= now;
}

/*
 * Moves a replaying actor's clock to `time`, dropping every held delayed
 * send due by then: time reached it in the run being replayed, so it fired
 * then (ADR 0089 section 2). Nothing to do for any other actor. The caller
 * has checked the time.
 */
TA_PRIVATE void ta_actor_move_clock(ta_actor *actor, ta_time time) {
    if (!ta_actor_replaying(actor)) {
        return;
    }
    ta_activation_effects *effects = actor->sparse->activation_effects;
    ta_vec *entries = &effects->entries;
    uint32_t kept = 0;
    for (uint32_t index = 0; index < entries->count; index += 1u) {
        ta_activation_effect *effect = ta_vec_at(entries, index);
        if (effect->kind == TA_ACTIVATION_SEND && effect->as.send.send.due <= time) {
            ta_activation_effect_release(effect);
            continue;
        }
        *(ta_activation_effect *)ta_vec_at(entries, kept) = *effect;
        kept += 1u;
    }
    if (kept != entries->count) {
        entries->count = kept;
        ta_actor_touch(actor);
    }
    effects->clock = time;
}

/*
 * Checks an I/O origin against the registry, and gives an event from one of
 * the actor's invocations an invoke origin when it has none (ADR 0068).
 * That covers hosts that post invoker events through their own mailbox.
 */
static ta_result ta_actor_accept_origin(const ta_system *system, const ta_actor *actor, ta_event *event) {
    if (event->origin.kind == TA_ORIGIN_IO) {
        const ta_io_origin *io = &event->origin.as.io;
        if (ta_system_io_at(system, io->processor) == NULL ||
            (io->origin_class == TA_IO_ORIGIN_MESSAGE && io->as.entry == NULL) ||
            (uint32_t)io->origin_class > (uint32_t)TA_IO_ORIGIN_MESSAGE) {
            return TA_ERR_INVALID;
        }
        return TA_OK;
    }
    if (event->origin.kind != TA_ORIGIN_NONE || event->invoke_id == TA_INVOKE_ID_NONE) {
        return TA_OK;
    }
    (void)ta_invoke_origin_for(system, actor, event->invoke_id, &event->origin);
    return TA_OK;
}

TA_PRIVATE ta_result ta_operator_jump_bytes(uint32_t count, uint32_t *bytes) {
    ta_layout layout;
    size_t total = 0;
    ta_layout_init(&layout, offsetof(ta_operator_jump, targets));
    ta_layout_add(&layout, &(ta_layout_entry){ .size = sizeof(ta_state_id), .alignment = _Alignof(ta_state_id),
                                               .count = count });
    if (ta_layout_finish(&layout, &total, NULL) != TA_OK || total > UINT32_MAX) {
        return TA_ERR_NOMEM;
    }
    *bytes = (uint32_t)total;
    return TA_OK;
}

TA_API ta_result ta_actor_deliver(ta_system *system, const ta_actor_deliver_params *params) {
    if (params == NULL || params->event == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_event *event = params->event;
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_operator_transition *transition = params->transition;
    if (!ta_event_name_valid(&system->event_names, event->name) || event->name == TA_EVENT_NAME_ROOT ||
        (transition != NULL && !ta_interpreter_jump_valid(actor->definition, transition)) ||
        !ta_actor_input_time_valid(system, actor, params->time)) {
        return TA_ERR_INVALID;
    }
    /* Before busy: a finished actor never becomes free again (ADR 0055). */
    if (actor->finished) {
        return TA_ERR_DONE;
    }
    if (actor->macrostep || ta_actor_external_event(actor) != NULL) {
        return TA_ERR_BUSY;
    }
    /* An idle actor's configuration is the one the operator transition runs
     * from: it is the first step its macrostep takes (ADR 0082). */
    if (transition != NULL && transition->source != TA_STATE_NONE &&
        !ta_actor_state_active(actor, transition->source)) {
        return TA_ERR_INVALID;
    }
    ta_event accepted = *event;
    ta_result result = ta_actor_accept_origin(system, actor, &accepted);
    if (result != TA_OK) {
        return result;
    }
    /* The operator transition, and the event block the event lives in until
     * its macrostep ends (ADR 0069), are the steps here that can fail, and
     * they change nothing when they do. */
    ta_operator_jump *jump = NULL;
    if (transition != NULL) {
        uint32_t jump_bytes = 0;
        if (ta_operator_jump_bytes(transition->target_count, &jump_bytes) != TA_OK) {
            return TA_ERR_NOMEM; /* the count does not fit; nothing was allocated */
        }
        jump = ta_mem_alloc(system->allocator, jump_bytes, _Alignof(ta_operator_jump));
        if (jump == NULL) {
            return TA_ERR_NOMEM;
        }
        jump->bytes = jump_bytes;
        jump->source = transition->source;
        jump->target_count = transition->target_count;
        ta_mem_copy(jump->targets, transition->targets, (size_t)transition->target_count * sizeof(ta_state_id));
    }
    result = ta_interpreter_hold_block(system, actor);
    if (result != TA_OK) {
        if (jump != NULL) {
            ta_mem_free(system->allocator, jump,
                        &(ta_allocator_free_params){ .size = jump->bytes,
                                                     .alignment = _Alignof(ta_operator_jump) });
        }
        return result;
    }
    /* Nothing below can fail: the input's time is now the actor's. */
    ta_actor_move_clock(actor, params->time);
    ta_event_slots *slots = &actor->event_block->slots;
    ta_event_retain(&accepted, &slots->external_event);
    slots->external = TA_EXTERNAL_WAITING;
    slots->jump = jump;
    /* The macrostep that takes it runs under the delivery's parent. */
    slots->trace = params->trace;
    actor->macrostep = true;
    ta_actor_touch(actor);
    actor->record.residency = TA_ACTOR_RUNNING;
    return TA_OK;
}

TA_API ta_result ta_actor_resume(ta_system *system, const ta_actor_resume_params *params) {
    if (params == NULL || params->status == NULL || params->budget == 0) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    return ta_interpreter_resume(system, actor, params);
}

TA_API ta_result ta_actor_abandon(ta_system *system, ta_actor_id id) {
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    return ta_interpreter_abandon(system, actor);
}

/*
 * An operator's withdrawal (ADR 0084): what a <cancel> of `send` in the
 * actor does to its pending sends, from outside any macrostep. The actor
 * changes, so an image captured before no longer describes it, and the
 * record is lifecycle-like: its key is the session's next lifecycle key.
 */
static ta_result ta_actor_withdraw(ta_system *system, ta_actor *actor, ta_send_id send);

TA_API ta_result ta_actor_withdraw_send(ta_system *system, ta_actor_id id, ta_send_id send) {
    if (send == 0) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL || ta_actor_timers(actor) == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    return ta_actor_withdraw(system, actor, send);
}

static ta_result ta_actor_withdraw(ta_system *system, ta_actor *actor, ta_send_id send) {
    ta_timer_list *list = ta_actor_timers(actor);
    /* A replaying actor's delayed sends are held, not pending (ADR 0089). */
    bool held = ta_actor_replaying(actor) && ta_actor_cancel_held_send(actor, send);
    if (ta_timer_cancel(&system->timers, list, send) != TA_OK && !held) {
        for (uint32_t index = 0; index < list->count; index += 1u) {
            if (list->sends[index].send_id == send) {
                return TA_ERR_DONE; /* only submitted ones are left */
            }
        }
        return TA_ERR_NOT_FOUND;
    }
    ta_actor_touch(actor);
    TA_COUNT(system, actor->definition, sends_cancelled);
    ta_step_key key = ta_trace_lifecycle_key(actor);
    ta_trace_context none = {0};
    if (ta_trace_lifecycle_wanted(&system->trace, TA_TRACE_WITHDRAWN, &none)) {
        ta_trace_record record = ta_trace_record_blank(TA_TRACE_WITHDRAWN);
        record.key = key;
        record.actor = actor->record.id;
        record.definition = actor->definition->id;
        record.now = ta_actor_now(system, actor);
        record.send = send;
        ta_trace_write(&system->trace, &record);
    }
    return TA_OK;
}

/* The family fills the padding after `kind` (ADR 0088). */
_Static_assert(sizeof(ta_actor_ref) == 16, "ta_actor_ref must stay 16 bytes");
_Static_assert(sizeof(ta_step_key) == 24, "ta_step_key must stay 24 bytes: its family fills padding");

TA_PRIVATE ta_atom ta_actor_family_of(const ta_system *system, ta_actor_id id) {
    const ta_actor *actor = ta_actor_lookup(system, id);
    return actor != NULL ? actor->definition->family : 0;
}

TA_PRIVATE ta_actor_ref ta_actor_runtime_ref(const ta_system *system, ta_actor_id id) {
    return (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .family = ta_actor_family_of(system, id), .as.runtime = id };
}

TA_API ta_result ta_actor_destroy(ta_system *system, ta_actor_id id) {
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse != NULL && sparse->completion_pending && sparse->has_parent) {
        ta_actor_ref parent = ta_actor_runtime_ref(system, sparse->parent.actor);
        if (ta_invoke_parent_accepts(system, actor, &parent)) {
            return TA_ERR_BUSY;
        }
    }
    /* Sends already submitted stay queued for their receivers (ADR 0054). */
    ta_result result = ta_timer_reserve_orphans(&system->timers, ta_actor_timers(actor));
    if (result != TA_OK) {
        return result;
    }
    ta_timer_orphan(&system->timers, ta_actor_timers(actor));
    ta_trace_lifecycle(system, actor, TA_TRACE_DESTROYED, NULL);
    ta_actor_release_address(system, actor);
    ta_actor_release_slot(system, id);
    ta_actor_free(system, actor);
    return TA_OK;
}

static const ta_event *ta_actor_paused_event(const ta_actor *actor) {
    if (!actor->macrostep) {
        return NULL;
    }
    return ta_actor_external_event(actor);
}

TA_PRIVATE const ta_event *ta_actor_external_event(const ta_actor *actor) {
    const ta_event_block *block = actor->event_block;
    if (block == NULL || block->slots.external == TA_EXTERNAL_NONE) {
        return NULL;
    }
    return &block->slots.external_event;
}

TA_PRIVATE const ta_event *ta_actor_bound_event(const ta_actor *actor) {
    const ta_event_block *block = actor->event_block;
    if (block == NULL) {
        return NULL;
    }
    switch ((ta_event_binding)block->slots.bound) {
    case TA_BOUND_EXTERNAL:
        return &block->slots.external_event;
    case TA_BOUND_INTERNAL:
        return &block->events[0].event;
    case TA_BOUND_KEPT:
        return &block->slots.kept_event;
    case TA_BOUND_NONE:
    default:
        return NULL;
    }
}

TA_PRIVATE uint32_t ta_actor_waiting_internal(const ta_actor *actor) {
    const ta_event_block *block = actor->event_block;
    if (block == NULL) {
        return 0;
    }
    return block->count - (block->slots.bound == TA_BOUND_INTERNAL ? 1u : 0u);
}

/* Everything the core allocates for the actor now; see ta_actor_inspection. */
static size_t ta_actor_allocated_bytes(const ta_actor *actor) {
    size_t total = actor->definition->actor_block_bytes;
    if (actor->event_block != NULL) {
        total += ta_event_block_bytes(actor->event_block->capacity);
    }
    const ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse != NULL) {
        total += sizeof(ta_actor_sparse_state);
        total += ta_vec_bytes(&sparse->pending_invokes);
        total += ta_invoke_table_bytes(&sparse->invoke_table);
        if (sparse->activation_effects != NULL) {
            total += sizeof(ta_activation_effects);
            total += ta_vec_bytes(&sparse->activation_effects->entries);
        }
        total += ta_timer_list_bytes(&sparse->timers);
        if (sparse->paused != NULL) {
            total += sparse->paused->bytes;
        }
    }
    return total;
}

TA_API ta_result ta_actor_inspect(ta_system *system, ta_actor_id id, ta_actor_inspection *out) {
    if (out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_definition *definition = actor->definition;
    uint32_t active = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        active += ta_actor_state_active(actor, state) ? 1u : 0u;
    }
    *out = (ta_actor_inspection){
        .actor = id,
        .session_id = actor->session_id,
        .self = actor->record.self,
        .definition_id = definition->id,
        .definition = definition->identity,
        .residency = actor->record.residency,
        .published = actor->published,
        .active_state_count = active,
        .macrostep_in_progress = actor->macrostep,
        .microstep_in_progress = actor->sparse != NULL && actor->sparse->paused != NULL &&
                                 ta_run_phase_in_microstep(actor->sparse->paused->at.phase),
        .step = ta_actor_step(actor),
        .position = ta_actor_position(actor),
        .current_event = ta_actor_paused_event(actor),
        .allocated_bytes = ta_actor_allocated_bytes(actor),
        .final_state = TA_STATE_NONE,
        .trace = { .context = ta_actor_macrostep_parent(actor).context,
                   .cause = ta_actor_step(actor) },
    };
    /* A taken head is `_event`, not work that waits. */
    uint32_t waiting = ta_actor_waiting_internal(actor);
    if (waiting != 0) {
        out->internal_events = actor->event_block->events + (actor->event_block->count - waiting);
        out->internal_count = waiting;
    }
    if (actor->sparse != NULL) {
        out->invokes = actor->sparse->invokes;
        out->invoke_count = actor->sparse->invoke_count;
        out->delayed_sends = actor->sparse->timers.count == 0 ? NULL : actor->sparse->timers.sends;
        out->delayed_send_count = actor->sparse->timers.count;
        out->has_completion = actor->finished;
        out->completion = actor->finished ? &actor->sparse->completion : NULL;
        out->final_state = actor->finished ? actor->sparse->final_state : TA_STATE_NONE;
        out->has_parent = actor->sparse->has_parent;
        out->parent = actor->sparse->parent;
    }
    return TA_OK;
}

TA_API ta_result ta_actor_datamodel_root(ta_system *system, ta_actor_id id, ta_value *out) {
    if (out == NULL) {
        return TA_ERR_INVALID;
    }
    *out = (ta_value){0};
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_definition *definition = actor->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->inspect == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_result result = v->inspect(definition->datamodel.self,
                                  &(ta_datamodel_inspect_params){
                                      .shared = definition->datamodel_shared,
                                      .instance = actor->datamodel_instance,
                                      .host = &actor->host,
                                      .out = out,
                                  });
    if (result != TA_OK) {
        *out = (ta_value){0};
    }
    return result;
}

TA_API ta_result ta_system_visit_actors(ta_system *system, const ta_actor_visit_params *params) {
    if (params == NULL) {
        return TA_ERR_INVALID;
    }
    /* A walk of everything may leave `next` null. */
    uint32_t unused = 0;
    uint32_t *next = params->next != NULL ? params->next : &unused;
    *next = params->from;
    if (system == NULL || params->inspector.visit == NULL) {
        return TA_ERR_INVALID;
    }
    uint32_t visited = 0;
    uint32_t slot = params->from;
    for (; slot < system->actors.count; slot += 1u) {
        if (params->limit != 0 && visited == params->limit) {
            break;
        }
        /* Read the entry each time: a visit may spawn and grow the table. */
        const ta_actor_slot *entry = ta_actor_slot_at(system, slot);
        if (entry->actor == NULL) {
            continue;
        }
        ta_actor_inspection inspection;
        ta_result result = ta_actor_inspect(system, entry->actor->record.id, &inspection);
        if (result == TA_OK) {
            result = params->inspector.visit(params->inspector.self, &inspection);
        }
        if (result != TA_OK) {
            *next = slot; /* not taken: the next call offers it again */
            return result;
        }
        visited += 1u;
    }
    *next = slot >= system->actors.count ? TA_ACTOR_VISIT_END : slot;
    return TA_OK;
}

TA_API ta_result ta_actor_active_states(ta_system *system, const ta_actor_active_states_params *params) {
    if (params == NULL || params->count == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_definition *definition = actor->definition;
    uint32_t count = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        count += ta_actor_state_active(actor, state) ? 1u : 0u;
    }
    *params->count = count;
    if (count > params->capacity || (count != 0 && params->states == NULL)) {
        return TA_ERR_NOMEM;
    }
    uint32_t at = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (ta_actor_state_active(actor, state)) {
            params->states[at++] = state;
        }
    }
    return TA_OK;
}

TA_API ta_result ta_actor_history(ta_system *system, const ta_actor_history_params *params) {
    if (params == NULL || params->count == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_definition *definition = actor->definition;
    if (params->history >= definition->state_count ||
        definition->states[params->history].history_slot == UINT32_MAX) {
        return TA_ERR_INVALID;
    }
    const uint64_t *words = ta_history_slot_words(&actor->history, definition->states[params->history].history_slot);
    uint32_t count = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        count += ta_bits_get(words, state) ? 1u : 0u;
    }
    *params->count = count;
    if (count > params->capacity || (count != 0 && params->states == NULL)) {
        return TA_ERR_NOMEM;
    }
    uint32_t at = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (ta_bits_get(words, state)) {
            params->states[at++] = state;
        }
    }
    return TA_OK;
}

TA_API ta_result ta_actor_get_completion(ta_system *system, ta_actor_id id, ta_actor_completion *out) {
    if (out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (!actor->finished) {
        return TA_ERR_BUSY;
    }
    *out = (ta_actor_completion){ .has_data = false };
    if (actor->sparse == NULL || !actor->sparse->completion.has_data) {
        return TA_OK;
    }
    out->data = actor->sparse->completion.data;
    ta_value_retain(&out->data);
    out->has_data = true;
    return TA_OK;
}


/*
 * A session image owns one arena, and every core-owned byte it keeps lives
 * there: the session storage, the queued events, the invocation records, the
 * delayed sends, and the text of the definition identity and of a named
 * address. That text would otherwise dangle — the identity's family name and
 * revision live in the *definition's* arena, and a named actor's address text
 * dies with the actor — and an image is meant to outlive both.
 *
 * Host values are not copied. They are retained, because a ta_value is a
 * handle and retain/release is its whole contract (ADR 0050). That is also
 * what makes capture all-or-nothing: every arena allocation happens first,
 * and only once all of them have succeeded does the image retain anything.
 * There is no half-retained image to unwind.
 */
struct ta_session_image {
    ta_allocator allocator;
    ta_arena arena;

    /* Local freshness: which actor this came from, and at which epoch. */
    ta_actor_id source_actor;
    /* The session this image continues (ADR 0066). */
    uint64_t session_id;
    uint64_t source_epoch;

    ta_actor_ref self;
    ta_definition_identity definition;
    ta_actor_residency residency;
    bool published;

    void *session_storage;
    size_t session_bytes;

    /* Execution state; the actor holds all of it between microsteps. */
    bool bootstrapped;
    bool macrostep;
    bool finished;
    /* The step counters, and whether the macrostep has begun (ADR 0067);
     * the macrostep's trace parent is in `slots`. */
    bool trace_begun;
    uint32_t trace_microstep;
    uint32_t trace_lifecycle;
    uint64_t trace_macrostep;
    /* The external and kept events and what `_event` names (ADR 0069). A
     * bound queue head is internal_events[0]. An operator transition waiting
     * with the external event is copied into the arena (ADR 0082). */
    ta_event_slots slots;
    uint64_t next_send_id;

    ta_queued_event *internal_events;
    uint32_t internal_count;
    /* Logical records only: a service handle is runtime-only and is dropped. */
    ta_active_invoke *invokes;
    uint32_t invoke_count;
    ta_delayed_send *delayed_sends;
    uint32_t delayed_send_count;
    ta_state_id *pending_invokes;
    uint32_t pending_invoke_count;

    bool has_parent;
    ta_actor_parent parent;
    bool completion_pending;
    ta_actor_completion completion;
    /* The final state that ended the session, with `finished` (ADR 0056). */
    ta_state_id final_state;
    /* A microstep paused inside executable content (ADR 0061), or null. */
    ta_continuation *paused;
};

/* ------------------------------------------------------------------------
 * Arena-backed copies of the text an image must not borrow
 */

static ta_result ta_image_copy_str(ta_arena *arena, ta_str *text) {
    if (text->ptr == NULL) {
        return TA_OK;
    }
    char *copy = ta_arena_alloc(arena, (size_t)text->len + 1u, 1);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, text->ptr, text->len);
    copy[text->len] = '\0';
    text->ptr = copy;
    return TA_OK;
}

/*
 * An image carries its addresses as detached copies in its own arena (ADR
 * 0065): the strings, not the capturing system's handles, so the image does
 * not depend on that system (ADR 0028). A runtime ref owns nothing.
 */
static ta_result ta_image_copy_address(ta_arena *arena, ta_actor_ref *ref) {
    return ta_actor_ref_detach(arena, ref);
}

/*
 * An I/O origin goes into the image as text (ADR 0068): the processor type
 * and the URI, whatever its class. A closed scope, or a COMPUTED origin its
 * processor cannot render, is stored as no origin. Nothing is retained.
 */
static ta_result ta_image_copy_io_origin(const ta_system *system, ta_arena *arena, ta_origin *origin) {
    ta_str type = {0};
    ta_str uri = {0};
    ta_result result = ta_io_origin_type(system, origin, &type);
    if (result == TA_OK) {
        result = ta_io_origin_uri(system, &(ta_io_origin_render_params){
                                              .origin = origin,
                                              .arena = ta_arena_allocator(arena),
                                              .out = &uri,
                                          });
    }
    if (result == TA_ERR_NOT_FOUND) {
        *origin = (ta_origin){ .kind = TA_ORIGIN_NONE };
        return TA_OK;
    }
    ta_io_origin_entry *detached = NULL;
    if (result == TA_OK) {
        result = ta_io_origin_detach(arena, &(ta_io_origin_detach_params){ .type = type, .uri = uri, .out = &detached });
    }
    if (result != TA_OK) {
        return result;
    }
    *origin = (ta_origin){ .kind = TA_ORIGIN_IO,
                           .as.io = { .origin_class = TA_IO_ORIGIN_MESSAGE, .as.entry = detached } };
    return TA_OK;
}

static ta_result ta_image_copy_origin(const ta_system *system, ta_arena *arena, ta_event *event) {
    if (event->origin.kind == TA_ORIGIN_IO) {
        return ta_image_copy_io_origin(system, arena, &event->origin);
    }
    ta_actor_ref *origin = ta_event_origin_ref(event);
    return origin == NULL ? TA_OK : ta_actor_ref_detach(arena, origin);
}

/*
 * A detached I/O origin becomes a MESSAGE origin of this system holding its
 * URI. An unknown processor type leaves no origin: the event is delivered,
 * and only a reply is lost.
 */
static ta_result ta_image_localize_io_origin(ta_system *system, ta_origin *origin) {
    const ta_io_origin_entry *detached = origin->as.io.as.entry;
    if (origin->as.io.origin_class != TA_IO_ORIGIN_MESSAGE || !ta_io_origin_entry_detached(detached)) {
        return TA_OK;
    }
    ta_atom type = TA_ATOM_NONE;
    uint32_t processor = 0;
    if (ta_atom_find(&system->atoms, ta_io_origin_detached_type(detached), &type) != TA_OK ||
        ta_system_find_io(system, type, &processor) != TA_OK || processor == 0) {
        *origin = (ta_origin){ .kind = TA_ORIGIN_NONE };
        return TA_OK;
    }
    return ta_io_origin_make(system, &(ta_io_origin_make_params){
                                         .processor = processor,
                                         .origin_class = TA_IO_ORIGIN_MESSAGE,
                                         .uri = ta_io_origin_entry_uri(detached),
                                         .out = origin,
                                     });
}

/* Makes an event restored from an image hold this system's origin handle. */
static ta_result ta_image_localize_origin(ta_system *system, ta_event *event) {
    if (event->origin.kind == TA_ORIGIN_IO) {
        return ta_image_localize_io_origin(system, &event->origin);
    }
    ta_actor_ref *origin = ta_event_origin_ref(event);
    return origin == NULL ? TA_OK : ta_actor_ref_localize(system, origin);
}

static ta_result ta_image_copy_identity(ta_arena *arena, ta_definition_identity *identity) {
    ta_result result = ta_image_copy_str(arena, &identity->family.name);
    if (result != TA_OK) {
        return result;
    }
    ta_str revision = { .ptr = (const char *)identity->revision.ptr, .len = identity->revision.len };
    result = ta_image_copy_str(arena, &revision);
    identity->revision = (ta_bytes){ .ptr = (const uint8_t *)revision.ptr, .len = revision.len };
    return result;
}

/* ------------------------------------------------------------------------
 * Capture
 */

/* What the arena has to hold, so one block usually serves the whole image. */
static size_t ta_image_measure(const ta_actor *actor) {
    const ta_actor_sparse_state *sparse = actor->sparse;
    size_t total = actor->session_bytes + 64u;
    total += (size_t)actor->definition->identity.family.name.len + 1u;
    total += (size_t)actor->definition->identity.revision.len + 1u;
    if (actor->record.self.kind == TA_ACTOR_REF_NAMED) {
        ta_actor_address view = ta_address_view(actor->record.self.as.named);
        total += sizeof(ta_address) + (size_t)view.family.name.len + (size_t)view.name.len + 2u;
    }
    if (actor->event_block != NULL) {
        total += (size_t)actor->event_block->count * sizeof(ta_queued_event);
        if (actor->event_block->slots.jump != NULL) {
            /* An estimate only: the jump stored the size it was allocated with. */
            total += actor->event_block->slots.jump->bytes + _Alignof(ta_operator_jump);
        }
    }
    if (sparse != NULL) {
        total += (size_t)sparse->invoke_count * sizeof(ta_active_invoke);
        total += (size_t)sparse->pending_invokes.count * sizeof(ta_state_id);
    }
    total += (size_t)ta_timer_copy_unsubmitted(ta_actor_timers(actor), NULL) * sizeof(ta_delayed_send);
    if (sparse != NULL && sparse->paused != NULL) {
        total += sparse->paused->bytes + TA_CONTINUATION_ALIGN;
    }
    return total;
}

/* Every arena allocation of a capture; nothing here retains anything. */
static ta_result ta_image_fill(ta_session_image *image, const ta_actor *actor) {
    ta_arena *arena = &image->arena;
    const ta_system *system = actor->host_impl.system;
    const ta_actor_sparse_state *sparse = actor->sparse;

    image->session_storage = ta_arena_alloc(arena, actor->session_bytes, _Alignof(uint64_t));
    if (image->session_storage == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(image->session_storage, actor->session_storage, actor->session_bytes);
    image->session_bytes = actor->session_bytes;

    ta_result result = ta_image_copy_identity(arena, &image->definition);
    if (result == TA_OK) {
        result = ta_image_copy_address(arena, &image->self);
    }
    if (result == TA_OK) {
        result = ta_image_copy_origin(system, arena, &image->slots.external_event);
    }
    if (result == TA_OK) {
        result = ta_image_copy_origin(system, arena, &image->slots.kept_event);
    }
    if (result != TA_OK) {
        return result;
    }

    /* An operator transition waiting with the external event (ADR 0082). */
    if (actor->event_block != NULL && actor->event_block->slots.jump != NULL) {
        size_t bytes = actor->event_block->slots.jump->bytes;
        image->slots.jump = ta_arena_alloc(arena, bytes, _Alignof(ta_operator_jump));
        if (image->slots.jump == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_copy(image->slots.jump, actor->event_block->slots.jump, bytes);
    }

    if (actor->event_block != NULL && actor->event_block->count != 0) {
        uint32_t count = actor->event_block->count;
        image->internal_events = ta_arena_alloc(arena, (size_t)count * sizeof(ta_queued_event),
                                                _Alignof(ta_queued_event));
        if (image->internal_events == NULL) {
            return TA_ERR_NOMEM;
        }
        for (uint32_t index = 0; index < count; index += 1) {
            image->internal_events[index] = actor->event_block->events[index];
            result = ta_image_copy_origin(system, arena, &image->internal_events[index].event);
            if (result != TA_OK) {
                return result;
            }
        }
        image->internal_count = count;
    }

    if (sparse != NULL && sparse->invoke_count != 0) {
        image->invokes = ta_arena_alloc(arena, (size_t)sparse->invoke_count * sizeof(ta_active_invoke),
                                        _Alignof(ta_active_invoke));
        if (image->invokes == NULL) {
            return TA_ERR_NOMEM;
        }
        for (uint32_t index = 0; index < sparse->invoke_count; index += 1) {
            image->invokes[index] = sparse->invokes[index];
            /* Runtime-only: the service is gone by the time this is restored. */
            image->invokes[index].service_handle = 0;
        }
        image->invoke_count = sparse->invoke_count;
    }

    if (sparse != NULL && sparse->pending_invokes.count != 0) {
        uint32_t count = sparse->pending_invokes.count;
        image->pending_invokes = ta_arena_alloc(arena, (size_t)count * sizeof(ta_state_id),
                                                _Alignof(ta_state_id));
        if (image->pending_invokes == NULL) {
            return TA_ERR_NOMEM;
        }
        for (uint32_t index = 0; index < count; index += 1) {
            image->pending_invokes[index] = *(const ta_state_id *)ta_vec_at(&sparse->pending_invokes, index);
        }
        image->pending_invoke_count = count;
    }

    if (sparse != NULL && sparse->paused != NULL) {
        image->paused = ta_arena_alloc(arena, sparse->paused->bytes, TA_CONTINUATION_ALIGN);
        if (image->paused == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_copy(image->paused, sparse->paused, sparse->paused->bytes);
    }

    uint32_t sends = ta_timer_copy_unsubmitted(ta_actor_timers(actor), NULL);
    if (sends != 0) {
        image->delayed_sends = ta_arena_alloc(arena, (size_t)sends * sizeof(ta_delayed_send),
                                              _Alignof(ta_delayed_send));
        if (image->delayed_sends == NULL) {
            return TA_ERR_NOMEM;
        }
        (void)ta_timer_copy_unsubmitted(ta_actor_timers(actor), image->delayed_sends);
        image->delayed_send_count = sends;
        for (uint32_t index = 0; index < sends; index += 1) {
            ta_delayed_send *send = &image->delayed_sends[index];
            result = ta_image_copy_address(arena, &send->destination);
            if (result == TA_OK) {
                result = ta_image_copy_address(arena, &send->origin);
            }
            if (result != TA_OK) {
                return result;
            }
        }
    }
    return TA_OK;
}

/* The second half of a capture: infallible, so it cannot leave a half-image. */
static void ta_image_retain(ta_session_image *image) {
    ta_value_retain(&image->slots.external_event.data);
    ta_value_retain(&image->slots.kept_event.data);
    ta_value_retain(&image->completion.data);
    for (uint32_t index = 0; index < image->internal_count; index += 1) {
        ta_value_retain(&image->internal_events[index].event.data);
    }
    for (uint32_t index = 0; index < image->delayed_send_count; index += 1) {
        ta_value_retain(&image->delayed_sends[index].target);
        ta_value_retain(&image->delayed_sends[index].data);
    }
    if (image->paused != NULL) {
        ta_continuation_retain(image->paused);
    }
}

/* The actor's event slots, with only the events it holds (see below). */
static ta_event_slots ta_image_slots(const ta_actor *actor) {
    if (actor->event_block == NULL) {
        return (ta_event_slots){0};
    }
    const ta_event_slots *slots = &actor->event_block->slots;
    return (ta_event_slots){
        .external = slots->external,
        .bound = slots->bound,
        .bound_before = slots->bound_before,
        .has_kept = slots->has_kept,
        .external_event = slots->external != TA_EXTERNAL_NONE ? slots->external_event : (ta_event){0},
        .kept_event = slots->has_kept ? slots->kept_event : (ta_event){0},
        .trace = actor->macrostep ? slots->trace : (ta_trace_parent){0},
    };
}

TA_API ta_result ta_actor_capture(ta_system *system, const ta_actor_capture_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    *params->out = NULL;
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    /* An unpublished prepared replacement is not a routable session yet. */
    if (!actor->published) {
        return TA_ERR_BUSY;
    }

    ta_session_image *image = ta_mem_alloc(system->allocator, sizeof(ta_session_image),
                                           _Alignof(ta_session_image));
    if (image == NULL) {
        return TA_ERR_NOMEM;
    }
    const ta_actor_sparse_state *sparse = actor->sparse;
    *image = (ta_session_image){
        .allocator = system->allocator,
        .source_actor = actor->record.id,
        .session_id = actor->session_id,
        .source_epoch = actor->session_epoch,
        .self = actor->record.self,
        .definition = actor->definition->identity,
        .residency = actor->record.residency,
        .published = actor->published,
        .bootstrapped = actor->bootstrapped,
        .macrostep = actor->macrostep,
        .finished = actor->finished,
        .trace_begun = actor->trace_begun,
        .trace_microstep = actor->trace_microstep,
        .trace_lifecycle = actor->trace_lifecycle,
        .trace_macrostep = actor->trace_macrostep,
        /*
         * An event is the actor's only while its flag says so. Releasing an
         * event leaves its bytes, origin included, in place, so the image
         * takes, and later retains, detaches and releases, only what the
         * flags say the actor holds.
         */
        .slots = ta_image_slots(actor),
        .next_send_id = actor->next_send_id,
        .has_parent = sparse != NULL && sparse->has_parent,
        .parent = sparse != NULL ? sparse->parent : (ta_actor_parent){0},
        .completion_pending = sparse != NULL && sparse->completion_pending,
        .completion = sparse != NULL ? sparse->completion : (ta_actor_completion){0},
        .final_state = sparse != NULL && actor->finished ? sparse->final_state : TA_STATE_NONE,
    };
    ta_result result = ta_arena_init(&image->arena, system->allocator, ta_image_measure(actor));
    if (result == TA_OK) {
        result = ta_image_fill(image, actor);
    }
    if (result != TA_OK) {
        /* Nothing was retained yet, so the arena is the whole of it. */
        ta_arena_destroy(&image->arena);
        ta_mem_free(system->allocator, image,
                    &(ta_allocator_free_params){ .size = sizeof(ta_session_image),
                                                 .alignment = _Alignof(ta_session_image) });
        return result;
    }
    ta_image_retain(image);
    *params->out = image;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Reading and releasing an image
 */

TA_API ta_result ta_session_image_next_deadline(const ta_session_image *image, ta_time *out) {
    if (image == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    if (image->delayed_send_count == 0) {
        return TA_ERR_NOT_FOUND;
    }
    ta_time earliest = image->delayed_sends[0].due;
    for (uint32_t index = 1; index < image->delayed_send_count; index += 1) {
        if (image->delayed_sends[index].due < earliest) {
            earliest = image->delayed_sends[index].due;
        }
    }
    *out = earliest;
    return TA_OK;
}

TA_API void ta_session_image_destroy(ta_session_image *image) {
    if (image == NULL) {
        return;
    }
    /* The mirror of ta_image_retain(), in the same order. */
    ta_value_release(&image->slots.external_event.data);
    ta_value_release(&image->slots.kept_event.data);
    ta_value_release(&image->completion.data);
    for (uint32_t index = 0; index < image->internal_count; index += 1) {
        ta_value_release(&image->internal_events[index].event.data);
    }
    for (uint32_t index = 0; index < image->delayed_send_count; index += 1) {
        ta_value_release(&image->delayed_sends[index].target);
        ta_value_release(&image->delayed_sends[index].data);
    }
    if (image->paused != NULL) {
        ta_continuation_release(image->paused);
    }
    ta_allocator allocator = image->allocator;
    ta_arena_destroy(&image->arena);
    ta_mem_free(allocator, image,
                &(ta_allocator_free_params){ .size = sizeof(ta_session_image),
                                             .alignment = _Alignof(ta_session_image) });
}

/* ------------------------------------------------------------------------
 * Reconstruction
 */

/*
 * The definition the image names, which must be the exact revision. A
 * different revision of the same family would reinterpret every state ID, so
 * it is a mismatch rather than a near miss; no definition of the family at
 * all is simply not found.
 */
TA_API ta_result ta_session_image_definition(const ta_system *system, const ta_session_image *image, ta_definition_id *out) {
    if (system == NULL || image == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_system_find_definition(system, &image->definition, out);
    if (result != TA_ERR_NOT_FOUND) {
        return result;
    }
    for (uint32_t slot = 0; slot < system->definitions.count; slot += 1) {
        const ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        const ta_definition *definition = entry->definition;
        if (definition != NULL && !definition->retired &&
            ta_str_equal(definition->identity.family.name, image->definition.family.name)) {
            return TA_ERR_DEFINITION_MISMATCH;
        }
    }
    return TA_ERR_NOT_FOUND;
}

/*
 * A runtime reference to the actor that was captured means "itself", and the
 * reconstructed session has a new runtime ID. Left alone, a self-addressed
 * delayed send would fire into the id of a session that no longer exists.
 */
static void ta_image_rebind_self(ta_actor_ref *ref, ta_actor_id was, ta_actor_id now) {
    if (ref->kind == TA_ACTOR_REF_RUNTIME && ref->as.runtime == was) {
        ref->as.runtime = now;
    }
}

/* Restores the queued events, invocation losses, and delayed sends. */
static ta_result ta_image_restore_session(ta_session_image *image, ta_actor *actor, ta_system *system) {
    for (uint32_t index = 0; index < image->internal_count; index += 1) {
        ta_event event;
        ta_event_retain(&image->internal_events[index].event, &event);
        ta_result result = ta_image_localize_origin(system, &event);
        if (result != TA_OK) {
            ta_event_release(&event);
            return result;
        }
        result = ta_interpreter_enqueue_internal(system, actor, &event);
        if (result != TA_OK) {
            return result;
        }
    }
    if (actor->event_block != NULL) {
        actor->event_block->slots.bound = image->slots.bound;
        actor->event_block->slots.bound_before = image->slots.bound_before;
    }
    /*
     * Invoker service handles do not survive an image, so every invocation
     * that was active is retired and reported once, in the order it was
     * captured, after the events the image already carried.
     */
    for (uint32_t index = 0; index < image->invoke_count; index += 1) {
        if (!image->invokes[index].active) {
            continue;
        }
        ta_result result = ta_invoke_report_lost(system, actor, image->invokes[index].id);
        if (result != TA_OK) {
            return result;
        }
    }
    for (uint32_t index = 0; index < image->delayed_send_count; index += 1) {
        ta_delayed_send send = image->delayed_sends[index];
        /* A fresh timer ID; the SCXML send ID that <cancel> names is kept. */
        send.id = 0;
        send.owner = actor->record.id;
        /* A destination naming this session is its session ID and survives a
         * reload as it is; only an unnamed actor's runtime origin moves. */
        ta_image_rebind_self(&send.origin, image->source_actor, actor->record.id);
        ta_value_retain(&send.target);
        ta_value_retain(&send.data);
        /* The image's detached addresses become this system's handles. */
        ta_result result = ta_actor_ref_localize(system, &send.destination);
        if (result == TA_OK) {
            result = ta_actor_ref_localize(system, &send.origin);
        }
        if (result == TA_OK) {
            result = ta_actor_schedule_send(system, actor, &send);
        }
        if (result != TA_OK) {
            ta_value_release(&send.target);
            ta_value_release(&send.data);
            ta_actor_ref_release(&send.destination);
            ta_actor_ref_release(&send.origin);
            return result;
        }
    }
    /*
     * Reporting the lost invocations put work in the queue of an actor the
     * image recorded as idle, so it has a macrostep to run again.
     */
    if (ta_interpreter_internal_pending(actor)) {
        ta_interpreter_start_macrostep(actor, NULL);
    }
    return TA_OK;
}

/*
 * Gives the actor its own copy of a microstep the image captured paused
 * inside executable content (ADR 0061 section 4); its snapshots are
 * retained again, so the image keeps its own.
 */
static ta_result ta_image_restore_pause(const ta_session_image *image, ta_actor *actor, ta_system *system) {
    /* The offsets are not trusted from the image: they are computed for the
     * actor's definition, and the image's counts and size must agree with
     * that shape before anything is allocated. */
    ta_continuation shape;
    size_t needed = 0;
    ta_result result = ta_continuation_capacity(actor->definition, &shape, &needed);
    if (result != TA_OK) {
        return result;
    }
    const ta_continuation *saved = image->paused;
    if (saved->bytes < needed || saved->words != shape.words || saved->decl_words != shape.decl_words ||
        saved->state_count != shape.state_count || saved->enabled_count > shape.enabled_count ||
        saved->frame_count > shape.frame_count) {
        return TA_ERR_INVALID;
    }
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_continuation *copy = ta_mem_alloc(system->allocator, saved->bytes, TA_CONTINUATION_ALIGN);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, saved, saved->bytes);
    copy->offsets = shape.offsets;
    ta_continuation_retain(copy);
    sparse->paused = copy;
    return TA_OK;
}

/* Reconstruction without its LOADED record, which a load writes only once
 * the directory publishes the session. */
static ta_result ta_image_reconstruct(ta_system *system, const ta_actor_reconstruct_params *params) {
    if (system == NULL || params == NULL || params->image == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_session_image *image = (ta_session_image *)params->image;
    ta_definition_id definition_id = 0;
    ta_result result = ta_session_image_definition(system, image, &definition_id);
    if (result != TA_OK) {
        return result;
    }
    ta_definition *definition = ta_system_definition(system, definition_id);
    ta_actor *actor = NULL;
    result = ta_actor_new(system, definition, &actor);
    if (result != TA_OK) {
        return result;
    }
    ta_actor_id id = actor->record.id;
    /*
     * The image's session continues here unless a resident session holds
     * its ID (ADR 0066): two live sessions never share one. Its ID is not
     * this actor's runtime ID, so the session table finds it.
     */
    if (image->session_id != 0 && !ta_session_resident(system, image->session_id)) {
        ta_session_node *node = NULL;
        result = ta_session_node_new(&system->sessions, &node);
        if (result != TA_OK) {
            ta_actor_release_slot(system, id);
            ta_actor_free(system, actor);
            return result;
        }
        node->id = image->session_id;
        node->actor = actor;
        ta_session_insert(&system->sessions, node);
        actor->session_id = image->session_id;
        actor->session_node = node;
    }

    /* The interpreter state the image describes, restored before anything
     * fallible runs against it. */
    ta_mem_copy(actor->session_storage, image->session_storage, image->session_bytes);
    /*
     * A named actor's logical self is its address and survives verbatim; an
     * anonymous one's self is its runtime ID, and this is a new session, so
     * it answers to the new one. Restoring the captured ID would leave the
     * actor addressing a session that no longer exists — every self-send,
     * every reply address, every delayed timer.
     */
    actor->record.self = image->self;
    if (image->self.kind == TA_ACTOR_REF_RUNTIME) {
        actor->record.self.as.runtime = id;
    }
    actor->record.residency = image->residency;
    actor->published = image->published;
    actor->bootstrapped = image->bootstrapped;
    actor->macrostep = image->macrostep;
    actor->finished = image->finished;
    actor->trace_begun = image->trace_begun;
    actor->trace_microstep = image->trace_microstep;
    /* Skip the ordinal an UNLOADED record takes after this image was
     * captured (ta_actor_unload validates the image first, then writes it),
     * so the LOADED that follows never reuses its key. After a reconstruct
     * the ordinal is simply unused. */
    actor->trace_lifecycle = image->trace_lifecycle + 1u;
    actor->trace_macrostep = image->trace_macrostep;
    actor->next_send_id = image->next_send_id;
    actor->session_epoch = image->source_epoch;
    /* The events live in an event block (ADR 0069). What `_event` names is
     * restored with the queue it may point into. */
    ta_event_slots *slots = NULL;
    if (image->slots.external != TA_EXTERNAL_NONE || image->slots.has_kept ||
        !ta_trace_parent_empty(&image->slots.trace)) {
        result = ta_interpreter_hold_block(system, actor);
        slots = result == TA_OK ? &actor->event_block->slots : NULL;
    }
    if (slots != NULL) {
        slots->trace = image->slots.trace;
    }
    if (slots != NULL && image->slots.external != TA_EXTERNAL_NONE) {
        ta_event_retain(&image->slots.external_event, &slots->external_event);
        slots->external = image->slots.external;
    }
    if (slots != NULL && image->slots.has_kept) {
        ta_event_retain(&image->slots.kept_event, &slots->kept_event);
        slots->has_kept = true;
    }
    /*
     * The image carries detached addresses; the actor holds this system's
     * handles (ADR 0065). A failure leaves a detached one behind, which
     * releasing ignores.
     */
    if (result == TA_OK) {
        result = ta_actor_ref_localize(system, &actor->record.self);
    }
    if (result == TA_OK && slots != NULL && slots->external != TA_EXTERNAL_NONE) {
        result = ta_image_localize_origin(system, &slots->external_event);
    }
    if (result == TA_OK && slots != NULL && slots->has_kept) {
        result = ta_image_localize_origin(system, &slots->kept_event);
    }
    if (result == TA_OK && slots != NULL && image->slots.jump != NULL) {
        /* The size is computed here from the count, not trusted from the image's
         * copy of the header. A size that does not fit is a refused allocation:
         * nothing is allocated, and the failure path after this block releases
         * what the restore took. */
        uint32_t bytes = 0;
        if (ta_operator_jump_bytes(image->slots.jump->target_count, &bytes) != TA_OK) {
            result = TA_ERR_NOMEM;
        } else {
            slots->jump = ta_mem_alloc(system->allocator, bytes, _Alignof(ta_operator_jump));
            if (slots->jump == NULL) {
                result = TA_ERR_NOMEM;
            } else {
                ta_mem_copy(slots->jump, image->slots.jump, bytes);
                slots->jump->bytes = bytes;
            }
        }
    }
    if (result == TA_OK && image->paused != NULL) {
        result = ta_image_restore_pause(image, actor, system);
    }

    if (result == TA_OK && (image->finished || image->has_parent || image->completion.has_data ||
                            image->internal_count != 0 || image->invoke_count != 0 ||
                            image->pending_invoke_count != 0)) {
        ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
        if (sparse == NULL) {
            result = TA_ERR_NOMEM;
        } else {
            sparse->has_parent = image->has_parent;
            sparse->parent = image->parent;
            sparse->completion_pending = image->completion_pending;
            sparse->completion = image->completion;
            ta_value_retain(&sparse->completion.data);
            sparse->final_state = image->final_state;
            if (image->pending_invoke_count != 0 && sparse->pending_invokes.element_size == 0) {
                ta_vec_init(&sparse->pending_invokes, system->allocator, TA_VEC_LAYOUT(ta_state_id));
            }
            for (uint32_t index = 0; result == TA_OK && index < image->pending_invoke_count; index += 1) {
                result = ta_vec_push(&sparse->pending_invokes, &image->pending_invokes[index]);
            }
        }
    }
    if (result == TA_OK) {
        result = definition->datamodel.v->init_instance(
            definition->datamodel.self,
            &(ta_datamodel_init_instance_params){
                .shared = definition->datamodel_shared,
                .instance = actor->datamodel_instance,
                .binding = params->datamodel_binding,
                .host = &actor->host,
            });
        actor->datamodel_initialized = result == TA_OK;
    }
    if (result == TA_OK) {
        result = ta_image_restore_session(image, actor, system);
    }
    if (result != TA_OK) {
        /* Nothing of this actor is published, so the image is untouched and
         * the caller may try again. */
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor); /* frees its delayed sends too */
        return result;
    }
    *params->out = id;
    return TA_OK;
}

TA_API ta_result ta_actor_reconstruct(ta_system *system, const ta_actor_reconstruct_params *params) {
    ta_result result = ta_image_reconstruct(system, params);
    if (result == TA_OK) {
        ta_trace_lifecycle(system, ta_actor_lookup(system, *params->out), TA_TRACE_LOADED, NULL);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Residency: unload and load
 *
 * A residency change is not a lifecycle change. The actor stays logically
 * alive throughout, its name stays reserved, and the only thing that moves
 * is where its session lives. The two operations are mirror images, and
 * each orders its directory swap against its fallible work so that a
 * failure anywhere leaves the actor exactly where it started (ADR 0014):
 *
 *   unload  swaps first, then discards  — nothing is thrown away until the
 *                                         route is gone, so a refused swap
 *                                         leaves a resident actor intact
 *   load    builds first, then swaps    — nothing is routable until the
 *                                         session is whole, so a refused
 *                                         swap leaves an unloaded actor
 */

TA_API ta_result ta_actor_unload(ta_system *system, const ta_actor_unload_params *params) {
    if (system == NULL || params == NULL || params->image == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    /* Only a named actor can be unloaded: an anonymous session has no name
     * to hold its place, so evicting it would simply lose it. */
    if (actor->record.self.kind != TA_ACTOR_REF_NAMED) {
        return TA_ERR_INVALID;
    }
    const ta_session_image *image = params->image;
    /*
     * The image must describe this actor as it is now. An image captured
     * before the actor processed another event would check-point a session
     * the actor has already moved past, so the host is sent back to capture
     * again rather than silently persisting the older one.
     */
    if (image->source_actor != actor->record.id || image->source_epoch != actor->session_epoch) {
        return TA_ERR_STALE;
    }
    /* Room for the submitted sends to outlive the session, before anything changes. */
    ta_result result = ta_timer_reserve_orphans(&system->timers, ta_actor_timers(actor));
    if (result != TA_OK) {
        return result;
    }
    /* And the entry that keeps the session findable while it is out (ADR 0066). */
    ta_session_node *parked = NULL;
    result = ta_session_node_new(&system->sessions, &parked);
    if (result != TA_OK) {
        return result;
    }

    result = ta_actor_directory_swap(
        system,
        &(ta_actor_directory_replace_params){
            .address = actor->record.self.as.named,
            .expected = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id },
            .replacement = { .kind = TA_ACTOR_DIRECTORY_UNLOADED },
        });
    if (result != TA_OK) {
        /* Still resident, still routable, and the same image will do. */
        ta_session_node_free(&system->sessions, parked);
        return result;
    }

    /*
     * The route is gone, so the session may go. Its not-yet-submitted sends
     * live in the image now and must not also sit in the resident heap,
     * where they would fire for an actor that is not there; a send already
     * handed to the router is an envelope of its own and stays, exactly as
     * it does when a session ends. One sent to this session names it by
     * session ID, and the parked entry routes its retry by address to
     * whatever session loads here next (ADR 0066).
     */
    ta_address *address = actor->record.self.as.named;
    parked->id = actor->session_id;
    parked->address = address;
    ta_address_retain(address);
    ta_timer_orphan(&system->timers, ta_actor_timers(actor));
    ta_trace_lifecycle(system, actor, TA_TRACE_UNLOADED, NULL);
    ta_actor_release_slot(system, actor->record.id);
    ta_actor_free(system, actor);
    if (address->unloaded != NULL) {
        ta_session_remove(&system->sessions, address->unloaded);
        ta_session_node_free(&system->sessions, address->unloaded);
    }
    address->unloaded = parked;
    ta_session_insert(&system->sessions, parked);
    return TA_OK;
}

/* A load from an in-memory image, with the binding to use. */
typedef struct {
    const ta_session_image *image;
    const ta_value *binding;
    const ta_trace_parent *trace;
    ta_actor_id *out;
} ta_image_load_params;

static ta_result ta_image_load(ta_system *system, const ta_image_load_params *params) {
    const ta_session_image *image = params->image;
    if (image->self.kind != TA_ACTOR_REF_NAMED) {
        return TA_ERR_INVALID;
    }

    /*
     * Built off-route: the directory still reads UNLOADED, so nothing can
     * reach this session until it is whole. Everything that can fail —
     * finding the definition, allocating, binding the data model, restoring
     * the queue and the timers — happens here.
     */
    ta_actor_id id = 0;
    ta_result result = ta_image_reconstruct(
        system,
        &(ta_actor_reconstruct_params){ .image = image,
                                        .datamodel_binding = params->binding,
                                        .out = &id });
    if (result != TA_OK) {
        return result;
    }

    result = ta_actor_directory_swap(
        system,
        &(ta_actor_directory_replace_params){
            .address = ta_actor_lookup(system, id)->record.self.as.named,
            .expected = { .kind = TA_ACTOR_DIRECTORY_UNLOADED },
            .replacement = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = id },
        });
    if (result != TA_OK) {
        /*
         * Nothing ever routed to this session, so taking it back down leaves
         * no trace: the actor is still unloaded, the image is untouched, and
         * the host may load it again.
         */
        ta_actor *actor = ta_actor_lookup(system, id);
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor); /* frees its delayed sends too */
        return result;
    }
    /* The session is resident again: its parked entry goes (ADR 0066). */
    ta_actor *loaded = ta_actor_lookup(system, id);
    ta_address *address = loaded->record.self.as.named;
    if (address->unloaded != NULL) {
        ta_session_node *parked = address->unloaded;
        ta_session_remove(&system->sessions, parked);
        ta_session_node_free(&system->sessions, parked);
    }
    ta_trace_lifecycle(system, loaded, TA_TRACE_LOADED, params->trace);
    *params->out = id;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Offloading (ADR 0074)
 *
 * An offloaded actor is its image as bytes, in this build's private layout,
 * read only by the system that wrote them. Offloading is capture, encode,
 * unload; loading is decode, then the load above. The image is the one
 * place the actor's state is gathered already, so going through it keeps
 * one definition of "what an unloaded actor is".
 *
 * The layout: a header; the session storage; the definition identity's
 * text; the self address; the invocation records and pending invokes, raw;
 * the delayed sends, raw, each followed by its destination and origin; and
 * the values, each through the actor's data model (value_write): the root,
 * the completion data, then each send's target and data. Raw records are
 * safe because nothing but this build reads them; their pointers and values
 * are rewritten on the way back in.
 */

#define TA_OFFLOAD_MAGIC 0x464f4154u /* "TAOF" */

typedef struct {
    uint32_t magic;
    uint32_t header_bytes;
    uint64_t system;
    uint64_t token;
    ta_actor_id source_actor;
    uint64_t session_id;
    uint64_t source_epoch;
    uint64_t trace_macrostep;
    uint64_t next_send_id;
    uint32_t trace_microstep;
    uint32_t trace_lifecycle;
    uint32_t residency;
    ta_state_id final_state;
    uint32_t session_bytes;
    uint32_t family_len;
    uint32_t revision_len;
    uint32_t invoke_count;
    uint32_t pending_invoke_count;
    uint32_t delayed_send_count;
    uint8_t published;
    uint8_t bootstrapped;
    uint8_t finished;
    uint8_t trace_begun;
} ta_offload_header;

/* Value `index` of an image, in the order the bytes carry them: the root,
 * the completion data, then each send's target and data. */
static ta_value *ta_offload_slot(ta_session_image *image, ta_value *root, uint32_t index) {
    if (index == 0) {
        return root;
    }
    if (index == 1) {
        return &image->completion.data;
    }
    ta_delayed_send *send = &image->delayed_sends[(index - 2u) / 2u];
    return (index - 2u) % 2u == 0 ? &send->target : &send->data;
}

static size_t ta_offload_address_bytes(const ta_address *address) {
    return sizeof(ta_address) + (size_t)address->family_len + (size_t)address->name_len + 2u;
}

/* A ref: the raw record, then a named one's address record. */
static size_t ta_offload_ref_bytes(const ta_actor_ref *ref) {
    size_t bytes = sizeof(ta_actor_ref);
    if (ref->kind == TA_ACTOR_REF_NAMED && ref->as.named != NULL) {
        bytes += sizeof(uint32_t) + ta_offload_address_bytes(ref->as.named);
    }
    return bytes;
}

static uint8_t *ta_offload_put(uint8_t *at, const void *source, size_t bytes) {
    if (bytes != 0) {
        ta_mem_copy(at, source, bytes);
    }
    return at + bytes;
}

static uint8_t *ta_offload_put_ref(uint8_t *at, const ta_actor_ref *ref) {
    at = ta_offload_put(at, ref, sizeof(ta_actor_ref));
    if (ref->kind == TA_ACTOR_REF_NAMED && ref->as.named != NULL) {
        uint32_t bytes = (uint32_t)ta_offload_address_bytes(ref->as.named);
        at = ta_offload_put(at, &bytes, sizeof bytes);
        at = ta_offload_put(at, ref->as.named, bytes);
    }
    return at;
}

/*
 * An arena sink (ADR 0090): one unit's pieces gathered into one run of the
 * arena, which doubles as the unit grows. The arena keeps the runs it
 * outgrew until it is reset, which is the offload's own lifetime.
 */
typedef struct {
    ta_arena *arena;
    uint8_t *ptr;
    size_t len;
    size_t cap;
} ta_arena_sink;

static ta_op_status ta_arena_sink_begin(void *self) {
    ((ta_arena_sink *)self)->len = 0;
    return TA_OP_OK;
}

static ta_op_status ta_arena_sink_write(void *self, ta_bytes bytes) {
    ta_arena_sink *sink = self;
    if (bytes.len == 0) {
        return TA_OP_OK;
    }
    if ((size_t)bytes.len > sink->cap - sink->len) {
        size_t need = sink->len + (size_t)bytes.len;
        size_t cap = sink->cap < 64u ? 64u : sink->cap;
        while (cap < need) {
            cap *= 2u;
        }
        uint8_t *grown = ta_arena_alloc(sink->arena, cap, _Alignof(uint64_t));
        if (grown == NULL) {
            return TA_OP_NOMEM;
        }
        if (sink->len != 0) {
            ta_mem_copy(grown, sink->ptr, sink->len);
        }
        sink->ptr = grown;
        sink->cap = cap;
    }
    ta_mem_copy(sink->ptr + sink->len, bytes.ptr, bytes.len);
    sink->len += bytes.len;
    return TA_OP_OK;
}

static void ta_arena_sink_end(void *self, bool keep) {
    if (!keep) {
        ((ta_arena_sink *)self)->len = 0;
    }
}

static const ta_byte_sink_vtable ta_arena_sink_vtable = {
    .begin = ta_arena_sink_begin,
    .write = ta_arena_sink_write,
    .end = ta_arena_sink_end,
};

/* A memory source: the bytes of one value, handed out in the reader's pieces. */
typedef struct {
    ta_bytes bytes;
    uint32_t at;
} ta_memory_source;

static ta_op_status ta_memory_source_read(void *self, const ta_byte_read_params *params) {
    ta_memory_source *source = self;
    uint32_t left = source->bytes.len - source->at;
    uint32_t count = left < params->capacity ? left : params->capacity;
    if (count != 0) {
        ta_mem_copy(params->buffer, source->bytes.ptr + source->at, count);
    }
    source->at += count;
    *params->got = count;
    return TA_OP_OK;
}

static const ta_byte_source_vtable ta_memory_source_vtable = { .read = ta_memory_source_read };

/* One value through the data model; an absent value is stored as absent. */
typedef struct {
    const ta_definition *definition;
    const ta_value *value;
    ta_arena *arena;
    ta_bytes *out;
} ta_offload_value_params;

static ta_result ta_offload_encode_value(const ta_offload_value_params *params) {
    *params->out = (ta_bytes){0};
    if (params->value == NULL || params->value->v == NULL) {
        return TA_OK;
    }
    const ta_datamodel *datamodel = &params->definition->datamodel;
    if (datamodel->v->value_write == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_arena_sink state = { .arena = params->arena };
    ta_byte_sink sink = { .self = &state, .v = &ta_arena_sink_vtable };
    ta_fault fault = {0};
    (void)sink.v->begin(sink.self);
    ta_op_status status = datamodel->v->value_write(
        datamodel->self, &(ta_datamodel_value_write_params){ .shared = params->definition->datamodel_shared,
                                                             .value = params->value,
                                                             .sink = sink,
                                                             .durable = false,
                                                             .fault = &fault });
    sink.v->end(sink.self, status == TA_OP_OK);
    if (status == TA_OP_ERROR) {
        ta_value_release(&fault.data);
    }
    if (status == TA_OP_NOMEM) {
        return TA_ERR_NOMEM;
    }
    if (status != TA_OP_OK || state.len > UINT32_MAX - 1u) {
        return TA_ERR_UNSUPPORTED;
    }
    *params->out = (ta_bytes){ .ptr = state.ptr, .len = (uint32_t)state.len };
    return TA_OK;
}

/* A stable actor only (ADR 0074 section 6): nothing runs, waits or is owed. */
static bool ta_offload_stable(const ta_session_image *image) {
    return !image->macrostep && image->paused == NULL && image->internal_count == 0 &&
           !image->completion_pending && !image->has_parent && image->slots.external == TA_EXTERNAL_NONE &&
           !image->slots.has_kept;
}

typedef struct {
    ta_session_image *image;
    const ta_definition *definition;
    const ta_value *root;
    ta_system *system;
    ta_arena *arena;
    ta_bytes *out;
} ta_offload_encode_params;

static size_t ta_offload_records_bytes(const ta_session_image *image) {
    size_t total = sizeof(ta_offload_header) + image->session_bytes + image->definition.family.name.len +
                   image->definition.revision.len + ta_offload_ref_bytes(&image->self) +
                   (size_t)image->invoke_count * sizeof(ta_active_invoke) +
                   (size_t)image->pending_invoke_count * sizeof(ta_state_id) +
                   (size_t)image->delayed_send_count * sizeof(ta_delayed_send);
    for (uint32_t index = 0; index < image->delayed_send_count; index += 1) {
        total += ta_offload_ref_bytes(&image->delayed_sends[index].destination);
        total += ta_offload_ref_bytes(&image->delayed_sends[index].origin);
    }
    return total;
}

static uint8_t *ta_offload_put_records(uint8_t *at, const ta_session_image *image, const ta_offload_header *header) {
    at = ta_offload_put(at, header, sizeof *header);
    at = ta_offload_put(at, image->session_storage, image->session_bytes);
    at = ta_offload_put(at, image->definition.family.name.ptr, image->definition.family.name.len);
    at = ta_offload_put(at, image->definition.revision.ptr, image->definition.revision.len);
    at = ta_offload_put_ref(at, &image->self);
    at = ta_offload_put(at, image->invokes, (size_t)image->invoke_count * sizeof(ta_active_invoke));
    at = ta_offload_put(at, image->pending_invokes, (size_t)image->pending_invoke_count * sizeof(ta_state_id));
    for (uint32_t index = 0; index < image->delayed_send_count; index += 1) {
        const ta_delayed_send *send = &image->delayed_sends[index];
        at = ta_offload_put(at, send, sizeof *send);
        at = ta_offload_put_ref(at, &send->destination);
        at = ta_offload_put_ref(at, &send->origin);
    }
    return at;
}

static ta_offload_header ta_offload_header_of(const ta_session_image *image, const ta_system *system) {
    return (ta_offload_header){
        .magic = TA_OFFLOAD_MAGIC,
        .header_bytes = sizeof(ta_offload_header),
        .system = (uint64_t)(uintptr_t)system,
        .token = system->offload_token,
        .source_actor = image->source_actor,
        .session_id = image->session_id,
        .source_epoch = image->source_epoch,
        .trace_macrostep = image->trace_macrostep,
        .next_send_id = image->next_send_id,
        .trace_microstep = image->trace_microstep,
        .trace_lifecycle = image->trace_lifecycle,
        .residency = (uint32_t)image->residency,
        .final_state = image->final_state,
        .session_bytes = (uint32_t)image->session_bytes,
        .family_len = image->definition.family.name.len,
        .revision_len = (uint32_t)image->definition.revision.len,
        .invoke_count = image->invoke_count,
        .pending_invoke_count = image->pending_invoke_count,
        .delayed_send_count = image->delayed_send_count,
        .published = image->published,
        .bootstrapped = image->bootstrapped,
        .finished = image->finished,
        .trace_begun = image->trace_begun,
    };
}

static ta_result ta_offload_encode(const ta_offload_encode_params *params) {
    ta_session_image *image = params->image;
    uint32_t value_count = 2u + 2u * image->delayed_send_count;
    ta_bytes *values = ta_arena_alloc(params->arena, (size_t)value_count * sizeof(ta_bytes), _Alignof(ta_bytes));
    if (values == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_value root = params->root != NULL ? *params->root : (ta_value){0};
    size_t total = ta_offload_records_bytes(image);
    for (uint32_t index = 0; index < value_count; index += 1) {
        ta_result result = ta_offload_encode_value(&(ta_offload_value_params){
            .definition = params->definition,
            .value = ta_offload_slot(image, &root, index),
            .arena = params->arena,
            .out = &values[index],
        });
        if (result != TA_OK) {
            return result;
        }
        total += sizeof(uint32_t) + values[index].len;
    }
    uint8_t *buffer = ta_arena_alloc(params->arena, total, _Alignof(uint64_t));
    if (buffer == NULL) {
        return TA_ERR_NOMEM;
    }
    if (params->system->offload_token == 0) {
        params->system->offload_token = ta_system_new_span_id(params->system);
    }
    ta_offload_header header = ta_offload_header_of(image, params->system);
    uint8_t *at = ta_offload_put_records(buffer, image, &header);
    for (uint32_t index = 0; index < value_count; index += 1) {
        const ta_value *value = ta_offload_slot(image, &root, index);
        /* A present value's length carries +1, so zero bytes still say "present". */
        uint32_t length = value->v == NULL ? 0u : (uint32_t)values[index].len + 1u;
        at = ta_offload_put(at, &length, sizeof length);
        at = ta_offload_put(at, values[index].ptr, values[index].len);
    }
    *params->out = (ta_bytes){ .ptr = buffer, .len = (size_t)(at - buffer) };
    return TA_OK;
}

TA_API ta_result ta_actor_offload(ta_system *system, const ta_actor_offload_params *params) {
    if (system == NULL || params == NULL || params->arena == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    *params->out = (ta_bytes){0};
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (actor->record.self.kind != TA_ACTOR_REF_NAMED) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = actor->definition;
    ta_session_image *image = NULL;
    ta_result result = ta_actor_capture(system, &(ta_actor_capture_params){ .actor = params->actor, .out = &image });
    if (result != TA_OK) {
        return result;
    }
    ta_bytes bytes = {0};
    result = ta_offload_stable(image) ? TA_OK : TA_ERR_BUSY;
    if (result == TA_OK) {
        result = ta_offload_encode(&(ta_offload_encode_params){ .image = image,
                                                                .definition = definition,
                                                                .root = params->root,
                                                                .system = system,
                                                                .arena = params->arena,
                                                                .out = &bytes });
    }
    if (result == TA_OK) {
        result = ta_actor_unload(system, &(ta_actor_unload_params){ .actor = params->actor, .image = image });
    }
    ta_session_image_destroy(image);
    if (result == TA_OK) {
        *params->out = bytes;
    }
    return result;
}

typedef struct {
    const uint8_t *at;
    const uint8_t *end;
} ta_offload_reader;

static bool ta_offload_take(ta_offload_reader *reader, void *out, size_t bytes) {
    if ((size_t)(reader->end - reader->at) < bytes) {
        return false;
    }
    if (bytes != 0) {
        ta_mem_copy(out, reader->at, bytes);
    }
    reader->at += bytes;
    return true;
}

/* A ref as ta_offload_put_ref() wrote it; a named one's address goes into `arena`, detached. */
static ta_result ta_offload_take_ref(ta_offload_reader *reader, ta_arena *arena, ta_actor_ref *ref) {
    if (!ta_offload_take(reader, ref, sizeof *ref)) {
        *ref = (ta_actor_ref){0};
        return TA_ERR_INVALID;
    }
    if (ref->kind != TA_ACTOR_REF_NAMED || ref->as.named == NULL) {
        return TA_OK;
    }
    ref->as.named = NULL;
    uint32_t bytes = 0;
    if (!ta_offload_take(reader, &bytes, sizeof bytes) || bytes < sizeof(ta_address) ||
        (size_t)(reader->end - reader->at) < bytes) {
        *ref = (ta_actor_ref){0};
        return TA_ERR_INVALID;
    }
    ta_address *address = ta_arena_alloc(arena, bytes, _Alignof(ta_address));
    if (address == NULL) {
        *ref = (ta_actor_ref){0};
        return TA_ERR_NOMEM;
    }
    (void)ta_offload_take(reader, address, bytes);
    if (ta_offload_address_bytes(address) != bytes) {
        *ref = (ta_actor_ref){0};
        return TA_ERR_INVALID;
    }
    address->table = NULL;
    address->next = NULL;
    address->unloaded = NULL;
    address->references = 0;
    ref->as.named = address;
    return TA_OK;
}

/* The text of the image's identity, copied into its arena. */
static ta_result ta_offload_take_identity(ta_offload_reader *reader, const ta_offload_header *header,
                                          ta_session_image *image) {
    char *family = ta_arena_alloc(&image->arena, (size_t)header->family_len + 1u, 1);
    uint8_t *revision = ta_arena_alloc(&image->arena, (size_t)header->revision_len + 1u, 1);
    if (family == NULL || revision == NULL) {
        return TA_ERR_NOMEM;
    }
    if (!ta_offload_take(reader, family, header->family_len) ||
        !ta_offload_take(reader, revision, header->revision_len)) {
        return TA_ERR_INVALID;
    }
    family[header->family_len] = '\0';
    image->definition = (ta_definition_identity){
        .family = { .name = { .ptr = family, .len = header->family_len } },
        .revision = { .ptr = revision, .len = header->revision_len },
    };
    return TA_OK;
}

/* An array of raw records, copied into the image's arena. */
typedef struct {
    ta_offload_reader *reader;
    ta_arena *arena;
    uint32_t count;
    size_t size;
    size_t alignment;
    void **out;
} ta_offload_array_params;

static ta_result ta_offload_take_array(const ta_offload_array_params *params) {
    *params->out = NULL;
    if (params->count == 0) {
        return TA_OK;
    }
    size_t bytes = (size_t)params->count * params->size;
    void *copy = ta_arena_alloc(params->arena, bytes, params->alignment);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    if (!ta_offload_take(params->reader, copy, bytes)) {
        return TA_ERR_INVALID;
    }
    *params->out = copy;
    return TA_OK;
}

static ta_result ta_offload_take_sends(ta_offload_reader *reader, uint32_t count, ta_session_image *image) {
    if (count == 0) {
        return TA_OK;
    }
    image->delayed_sends = ta_arena_alloc(&image->arena, (size_t)count * sizeof(ta_delayed_send),
                                          _Alignof(ta_delayed_send));
    if (image->delayed_sends == NULL) {
        return TA_ERR_NOMEM;
    }
    for (uint32_t index = 0; index < count; index += 1) {
        ta_delayed_send *send = &image->delayed_sends[index];
        if (!ta_offload_take(reader, send, sizeof *send)) {
            return TA_ERR_INVALID;
        }
        /* From here on the values and refs are this image's own. */
        send->target = (ta_value){0};
        send->data = (ta_value){0};
        send->origin = (ta_actor_ref){0};
        image->delayed_send_count = index + 1u;
        ta_result result = ta_offload_take_ref(reader, &image->arena, &send->destination);
        if (result == TA_OK) {
            result = ta_offload_take_ref(reader, &image->arena, &send->origin);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

/* Everything but the values: arena copies of the records, refs detached. */
static ta_result ta_offload_take_records(ta_offload_reader *reader, const ta_offload_header *header,
                                         ta_session_image *image) {
    ta_arena *arena = &image->arena;
    ta_result result = ta_offload_take_array(&(ta_offload_array_params){
        .reader = reader, .arena = arena, .count = header->session_bytes, .size = 1,
        .alignment = _Alignof(uint64_t), .out = &image->session_storage });
    image->session_bytes = header->session_bytes;
    if (result == TA_OK) {
        result = ta_offload_take_identity(reader, header, image);
    }
    if (result == TA_OK) {
        result = ta_offload_take_ref(reader, arena, &image->self);
    }
    if (result == TA_OK) {
        result = ta_offload_take_array(&(ta_offload_array_params){
            .reader = reader, .arena = arena, .count = header->invoke_count, .size = sizeof(ta_active_invoke),
            .alignment = _Alignof(ta_active_invoke), .out = (void **)&image->invokes });
        image->invoke_count = image->invokes != NULL ? header->invoke_count : 0;
    }
    if (result == TA_OK) {
        result = ta_offload_take_array(&(ta_offload_array_params){
            .reader = reader, .arena = arena, .count = header->pending_invoke_count, .size = sizeof(ta_state_id),
            .alignment = _Alignof(ta_state_id), .out = (void **)&image->pending_invokes });
        image->pending_invoke_count = image->pending_invokes != NULL ? header->pending_invoke_count : 0;
    }
    if (result == TA_OK) {
        result = ta_offload_take_sends(reader, header->delayed_send_count, image);
    }
    return result;
}

/* The values, through the data model of the definition the image names. */
static ta_result ta_offload_take_values(ta_offload_reader *reader, ta_system *system, ta_session_image *image,
                                        ta_value *root) {
    ta_definition_id definition_id = 0;
    ta_result result = ta_session_image_definition(system, image, &definition_id);
    if (result != TA_OK) {
        return result;
    }
    const ta_definition *definition = ta_system_definition(system, definition_id);
    const ta_datamodel *datamodel = &definition->datamodel;
    uint32_t value_count = 2u + 2u * image->delayed_send_count;
    for (uint32_t index = 0; index < value_count; index += 1) {
        uint32_t length = 0;
        if (!ta_offload_take(reader, &length, sizeof length) ||
            (length != 0 && (size_t)(reader->end - reader->at) < (size_t)length - 1u)) {
            return TA_ERR_INVALID;
        }
        if (length == 0) {
            continue;
        }
        ta_bytes bytes = { .ptr = reader->at, .len = (size_t)length - 1u };
        reader->at += length - 1u;
        if (datamodel->v->value_read == NULL) {
            return TA_ERR_UNSUPPORTED;
        }
        ta_value value = {0};
        ta_memory_source memory = { .bytes = bytes };
        ta_fault fault = {0};
        ta_op_status status = datamodel->v->value_read(
            datamodel->self,
            &(ta_datamodel_value_read_params){ .shared = definition->datamodel_shared,
                                               .source = { .self = &memory, .v = &ta_memory_source_vtable },
                                               .durable = false,
                                               .out = &value,
                                               .fault = &fault });
        if (status == TA_OP_ERROR) {
            ta_value_release(&fault.data);
        }
        if (status == TA_OP_NOMEM) {
            return TA_ERR_NOMEM;
        }
        if (status != TA_OP_OK) {
            return TA_ERR_INVALID;
        }
        *ta_offload_slot(image, root, index) = value;
    }
    image->completion.has_data = image->completion.data.v != NULL;
    return reader->at == reader->end ? TA_OK : TA_ERR_INVALID;
}

static void ta_offload_free_image(ta_system *system, ta_session_image *image) {
    ta_mem_free(system->allocator, image,
                &(ta_allocator_free_params){ .size = sizeof(ta_session_image),
                                             .alignment = _Alignof(ta_session_image) });
}

/* A fresh image for bytes whose header says they are this system's. */
static ta_result ta_offload_image(ta_system *system, const ta_offload_header *header, size_t bytes,
                                  ta_session_image **out) {
    ta_session_image *image = ta_mem_alloc(system->allocator, sizeof(ta_session_image), _Alignof(ta_session_image));
    if (image == NULL) {
        return TA_ERR_NOMEM;
    }
    *image = (ta_session_image){
        .allocator = system->allocator,
        .source_actor = header->source_actor,
        .session_id = header->session_id,
        .source_epoch = header->source_epoch,
        .residency = (ta_actor_residency)header->residency,
        .published = header->published != 0,
        .bootstrapped = header->bootstrapped != 0,
        .finished = header->finished != 0,
        .trace_begun = header->trace_begun != 0,
        .trace_microstep = header->trace_microstep,
        .trace_lifecycle = header->trace_lifecycle,
        .trace_macrostep = header->trace_macrostep,
        .next_send_id = header->next_send_id,
        .final_state = header->final_state,
    };
    ta_result result = ta_arena_init(&image->arena, system->allocator, bytes + 256u);
    if (result != TA_OK) {
        ta_offload_free_image(system, image);
        return result;
    }
    *out = image;
    return TA_OK;
}

/* Bytes this system offloaded, back as an image; nothing is kept on failure. */
static ta_result ta_offload_decode(ta_system *system, ta_bytes bytes, ta_session_image **out, ta_value *root) {
    *root = (ta_value){0};
    ta_offload_reader reader = { .at = bytes.ptr, .end = bytes.ptr + bytes.len };
    ta_offload_header header;
    if (bytes.ptr == NULL || !ta_offload_take(&reader, &header, sizeof header) || header.magic != TA_OFFLOAD_MAGIC ||
        header.header_bytes != sizeof(ta_offload_header)) {
        return TA_ERR_INVALID;
    }
    if (header.system != (uint64_t)(uintptr_t)system || system->offload_token == 0 ||
        header.token != system->offload_token) {
        return TA_ERR_STALE;
    }
    ta_session_image *image = NULL;
    ta_result result = ta_offload_image(system, &header, bytes.len, &image);
    if (result == TA_OK) {
        result = ta_offload_take_records(&reader, &header, image);
        if (result == TA_OK) {
            result = ta_offload_take_values(&reader, system, image, root);
        }
        if (result != TA_OK) {
            ta_value_release(root);
            *root = (ta_value){0};
            ta_session_image_destroy(image);
            return result;
        }
        *out = image;
    }
    return result;
}

TA_API ta_result ta_actor_load(ta_system *system, const ta_actor_load_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    if (params->root != NULL) {
        *params->root = (ta_value){0};
    }
    if (params->image != NULL) {
        return ta_image_load(system, &(ta_image_load_params){ .image = params->image,
                                                              .binding = params->datamodel_binding,
                                                              .trace = &params->trace,
                                                              .out = params->out });
    }
    ta_session_image *image = NULL;
    ta_value root = {0};
    ta_result result = ta_offload_decode(system, params->bytes, &image, &root);
    if (result != TA_OK) {
        return result;
    }
    const ta_value *binding = params->datamodel_binding;
    if (binding == NULL && root.v != NULL) {
        binding = &root;
    }
    result = ta_image_load(system, &(ta_image_load_params){ .image = image,
                                                            .binding = binding,
                                                            .trace = &params->trace,
                                                            .out = params->out });
    ta_session_image_destroy(image);
    if (result == TA_OK && params->root != NULL) {
        *params->root = root;
    } else {
        ta_value_release(&root);
    }
    return result;
}



#define TA_SCXML_IO_URI "http://www.w3.org/TR/scxml/#SCXMLEventProcessor"

/* ------------------------------------------------------------------------
 * System parameters (ADR 0092)
 */

typedef struct {
    const char *name;
    const char *unit;
    uint32_t default_value;
    uint32_t min;
    uint32_t max;
} ta_param_row;

/* One row per ta_param, in its order. The bounds are the only ones: no
 * other code checks a parameter again. */
static const ta_param_row ta_param_rows[TA_PARAM_COUNT] = {
    [TA_PARAM_MICROSTEP_LIMIT] = { "microstepLimit", "microsteps", TA_PARAM_DEFAULT_MICROSTEP_LIMIT, 0, 2147483647u },
    [TA_PARAM_EVENT_BLOCK_CACHE] = { "eventBlockCache", "blocks", TA_PARAM_DEFAULT_EVENT_BLOCK_CACHE, 0, 65536u },
    [TA_PARAM_EVENT_NAME_DEPTH] = { "eventNameDepth", "segments", TA_PARAM_DEFAULT_EVENT_NAME_DEPTH, 8,
                                    TA_EVENT_NAME_MAX_DEPTH },
    [TA_PARAM_COMPILE_MESSAGE_BYTES] = { "compileMessageBytes", "bytes", TA_PARAM_DEFAULT_COMPILE_MESSAGE_BYTES, 16,
                                         65536u },
    [TA_PARAM_COMPILE_TEXT_BYTES] = { "compileTextBytes", "bytes", TA_PARAM_DEFAULT_COMPILE_TEXT_BYTES, 256, 65536u },
    [TA_PARAM_COMPILE_NEAREST_BYTES] = { "compileNearestBytes", "bytes", TA_PARAM_DEFAULT_COMPILE_NEAREST_BYTES, 0,
                                         65536u },
    [TA_PARAM_INTERPRETER_BYTES] = { "interpreterBytes", "bytes", TA_PARAM_DEFAULT_INTERPRETER_BYTES, 1024,
                                     1073741824u },
    [TA_PARAM_RENDER_BYTES] = { "renderBytes", "bytes", TA_PARAM_DEFAULT_RENDER_BYTES, 256, 1073741824u },
    [TA_PARAM_INTERPRETER_NESTING] = { "interpreterNesting", "interpreters", TA_PARAM_DEFAULT_INTERPRETER_NESTING, 1,
                                       64 },
    [TA_PARAM_MAX_ACTORS] = { "maxActors", "actors", TA_PARAM_DEFAULT_MAX_ACTORS, 1, 2147483647u },
    [TA_PARAM_MAX_PENDING_SENDS] = { "maxPendingSends", "sends", TA_PARAM_DEFAULT_MAX_PENDING_SENDS, 1, 2147483647u },
    [TA_PARAM_MAX_DEFINITIONS] = { "maxDefinitions", "definitions", TA_PARAM_DEFAULT_MAX_DEFINITIONS, 1, 2147483647u },
};

TA_API ta_result ta_param_info_of(ta_param param, ta_param_info *out) {
    if (out == NULL || (uint32_t)param >= (uint32_t)TA_PARAM_COUNT) {
        return TA_ERR_INVALID;
    }
    const ta_param_row *row = &ta_param_rows[param];
    *out = (ta_param_info){ .name = ta_str_from_cstr(row->name),
                            .default_value = row->default_value,
                            .min = row->min,
                            .max = row->max,
                            .unit = ta_str_from_cstr(row->unit) };
    return TA_OK;
}

TA_API ta_result ta_system_param(const ta_system *system, ta_param param, uint64_t *out) {
    if (system == NULL || out == NULL || (uint32_t)param >= (uint32_t)TA_PARAM_COUNT) {
        return TA_ERR_INVALID;
    }
    *out = system->params[param];
    return TA_OK;
}

/* Every parameter's value: the defaults, then the config's, each checked
 * against its bounds once. TA_ERR_INVALID names the parameter at fault. */
static ta_result ta_params_resolve(const ta_system_config *config, uint32_t values[TA_PARAM_COUNT]) {
    bool given[TA_PARAM_COUNT] = { false };
    for (uint32_t index = 0; index < (uint32_t)TA_PARAM_COUNT; index += 1u) {
        values[index] = ta_param_rows[index].default_value;
    }
    if (config->param_count != 0 && config->params == NULL) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < config->param_count; index += 1u) {
        const ta_param_value *value = &config->params[index];
        uint32_t param = (uint32_t)value->param;
        if (param >= (uint32_t)TA_PARAM_COUNT) {
            return TA_ERR_INVALID;
        }
        const ta_param_row *row = &ta_param_rows[param];
        if (given[param] || value->value < row->min || value->value > row->max) {
            if (config->rejected != NULL) {
                *config->rejected = value->param;
            }
            return TA_ERR_INVALID;
        }
        given[param] = true;
        values[param] = (uint32_t)value->value;
    }
    return TA_OK;
}

/*
 * Where the parts of a system's one arena allocation go (ADR 0092 section 3):
 * an interpreter region (the structural arena and, after it, the render
 * arena), then the table of nested regions. System lifetime: the figures
 * depend on parameters that are fixed when the system is created, so the
 * allocation that ta_system_arena_open makes and the free that
 * ta_system_arena_close does with `arena_bytes` always agree. The nested
 * regions are allocated one by one, later, each of `bytes`.
 */
typedef struct {
    size_t render;       /* offset of the render arena; the structural arena is [0, render) */
    size_t bytes;        /* one interpreter region: structural arena and render arena */
    size_t nested_table; /* offset of the table of nested regions, aligned for it */
    size_t total;        /* the whole arena allocation: region 0 and the table */
} ta_region_layout;

/*
 * Lays out the system arena. Each arena starts at the alignment of an arena
 * block, so the render arena starts at the structural arena's size rounded
 * up. It never allocates, and answers TA_ERR_NOMEM with `*out` untouched
 * when a size does not fit in a size_t (on a 32-bit host, a large
 * interpreterBytes with a deep interpreterNesting), so ta_system_create
 * fails before anything is allocated for the arena.
 */
static ta_result ta_interpreter_region_layout(const uint32_t params[TA_PARAM_COUNT], ta_region_layout *out) {
    ta_layout layout;
    ta_region_layout region;
    ta_layout_init(&layout, 0);
    ta_arena_fixed_add(&layout, params[TA_PARAM_INTERPRETER_BYTES]);
    region.render = ta_arena_fixed_add(&layout, params[TA_PARAM_RENDER_BYTES]);
    /* One interpreter region is the two arenas; the layout is a value, so a
     * copy finished here gives that size and the original goes on to the table. */
    ta_layout arenas = layout;
    if (ta_layout_finish(&arenas, &region.bytes, NULL) != TA_OK) {
        return TA_ERR_NOMEM;
    }
    /* The table has one entry per nested interpreter; the first has region 0. */
    region.nested_table = ta_layout_add(&layout, &(ta_layout_entry){ .size = sizeof(ta_nested_region),
                                                                     .alignment = _Alignof(ta_nested_region),
                                                                     .count = params[TA_PARAM_INTERPRETER_NESTING] - 1u });
    ta_result result = ta_layout_finish(&layout, &region.total, NULL);
    if (result == TA_OK) {
        *out = region;
    }
    return result;
}

/* Lays a region's structural arena over the start of its memory. */
static void ta_interpreter_region_open_scratch(const ta_region_layout *layout, void *memory, ta_arena *scratch) {
    ta_arena_init_fixed(scratch, memory, layout->render);
}

/* Lays a region's render arena over the rest of its memory. */
static void ta_interpreter_region_open_render(const ta_region_layout *layout, void *memory, ta_arena *render) {
    ta_arena_init_fixed(render, (unsigned char *)memory + layout->render, layout->bytes - layout->render);
}

TA_PRIVATE ta_result ta_system_nested_region(ta_system *system, uint32_t depth, ta_arena *scratch, ta_arena *render) {
    if (depth == 0 || depth >= system->params[TA_PARAM_INTERPRETER_NESTING]) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_region_layout layout;
    ta_result result = ta_interpreter_region_layout(system->params, &layout);
    if (result != TA_OK) {
        return result;
    }
    ta_nested_region *region = &system->nested[depth - 1u];
    if (region->memory == NULL) {
        region->memory = ta_mem_alloc(system->allocator, layout.bytes, _Alignof(ta_arena_block));
        if (region->memory == NULL) {
            return TA_ERR_NOMEM;
        }
        region->bytes = layout.bytes;
    }
    ta_interpreter_region_open_scratch(&layout, region->memory, scratch);
    ta_interpreter_region_open_render(&layout, region->memory, render);
    return TA_OK;
}

/*
 * The memory every run needs, allocated once (ADR 0092 section 3):
 * interpreter region 0 with its render area, then the table of nested
 * regions; and the event-block cache, filled to its parameter.
 */
static ta_result ta_system_arena_open(ta_system *system) {
    ta_region_layout layout;
    ta_result result = ta_interpreter_region_layout(system->params, &layout);
    if (result != TA_OK) {
        return result;
    }
    system->arena_memory = ta_mem_alloc(system->allocator, layout.total, _Alignof(ta_arena_block));
    if (system->arena_memory == NULL) {
        return TA_ERR_NOMEM;
    }
    system->arena_bytes = layout.total;
    ta_interpreter_region_open_scratch(&layout, system->arena_memory, &system->scratch);
    ta_interpreter_region_open_render(&layout, system->arena_memory, &system->render);
    system->nested = (ta_nested_region *)((unsigned char *)system->arena_memory + layout.nested_table);
    for (uint32_t index = 0; index + 1u < system->params[TA_PARAM_INTERPRETER_NESTING]; index += 1u) {
        system->nested[index] = (ta_nested_region){ 0 };
    }
    return TA_OK;
}

static void ta_system_arena_close(ta_system *system) {
    if (system->arena_memory == NULL) {
        return;
    }
    for (uint32_t index = 0; index + 1u < system->params[TA_PARAM_INTERPRETER_NESTING]; index += 1u) {
        ta_nested_region *region = &system->nested[index];
        if (region->memory != NULL) {
            ta_mem_free(system->allocator, region->memory,
                        &(ta_allocator_free_params){ .size = region->bytes, .alignment = _Alignof(ta_arena_block) });
        }
    }
    ta_mem_free(system->allocator, system->arena_memory,
                &(ta_allocator_free_params){ .size = system->arena_bytes, .alignment = _Alignof(ta_arena_block) });
    system->arena_memory = NULL;
    system->scratch = (ta_arena){ 0 };
    system->render = (ta_arena){ 0 };
}

TA_PRIVATE bool ta_system_quota_actor(ta_system *system) {
    if (system->live_actors >= system->params[TA_PARAM_MAX_ACTORS]) {
        system->counters.quota_refusals += 1u;
        return false;
    }
    return true;
}

TA_API ta_result ta_system_create(const ta_system_config *config, ta_system **out) {
    if (config == NULL || out == NULL || config->allocator.v == NULL ||
        config->allocator.v->alloc == NULL || config->allocator.v->free == NULL) {
        return TA_ERR_INVALID;
    }
    uint32_t params[TA_PARAM_COUNT];
    ta_result resolved = ta_params_resolve(config, params);
    if (resolved != TA_OK) {
        return resolved;
    }
    ta_system *system = ta_mem_alloc(config->allocator, sizeof(ta_system), _Alignof(ta_system));
    if (system == NULL) {
        return TA_ERR_NOMEM;
    }
    *system = (ta_system){ .allocator = config->allocator,
                           .incarnation = config->incarnation,
                           .block_cache_limit = params[TA_PARAM_EVENT_BLOCK_CACHE],
                           .trace = ta_null_trace_sink(),
                           .definition_sink = ta_null_definition_sink() };
    ta_mem_copy(system->params, params, sizeof params);
    ta_system_seed_trace_ids(system, 0);
    ta_clock_set(&system->clock, config->initial_time);
    ta_vec_init(&system->actors.pages, config->allocator, TA_VEC_LAYOUT(ta_actor_slot *));
    system->free_actor_slot = UINT32_MAX;
    ta_vec_init(&system->definitions, config->allocator, TA_VEC_LAYOUT(ta_definition_slot));
    system->free_definition_slot = UINT32_MAX;
    ta_vec_init(&system->io_processors, config->allocator, TA_VEC_LAYOUT(ta_io_registration));
    ta_vec_init(&system->invokers, config->allocator, TA_VEC_LAYOUT(ta_invoker_registration));
    ta_address_table_init(&system->addresses, config->allocator);
    ta_io_origin_table_init(&system->io_scopes, config->allocator, true);
    ta_io_origin_table_init(&system->io_messages, config->allocator, false);
    ta_result result = ta_session_table_init(&system->sessions, config->allocator);
    if (result == TA_OK) {
        result = ta_atom_table_init(&system->atoms, config->allocator);
    }
    if (result != TA_OK) {
        ta_session_table_destroy(&system->sessions);
        ta_mem_free(config->allocator, system,
                    &(ta_allocator_free_params){ .size = sizeof(ta_system), .alignment = _Alignof(ta_system) });
        return result;
    }
    result = ta_event_name_table_init(&system->event_names, config->allocator, &system->atoms);
    if (result == TA_OK) {
        system->event_names.max_depth = params[TA_PARAM_EVENT_NAME_DEPTH];
        result = ta_timer_heap_init(&system->timers, config->allocator);
    }
    if (result == TA_OK) {
        result = ta_system_arena_open(system);
    }
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, ta_str_from_cstr(TA_SCXML_IO_URI), &system->scxml_io_type);
    }
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, ta_str_from_cstr("scxml"), &system->scxml_io_alias);
    }
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, ta_str_from_cstr("http://www.w3.org/TR/scxml/"),
                                &system->scxml_invoke_type);
    }
    if (result != TA_OK) {
        ta_system_destroy(system);
        return result;
    }
    *out = system;
    return TA_OK;
}

TA_API void ta_system_destroy(ta_system *system) {
    if (system == NULL) {
        return;
    }
    /* Teardown releases submitted sends instead of keeping them as orphans. */
    system->timers.closing = true;
    for (uint32_t slot = 0; slot < system->actors.count; slot += 1) {
        ta_actor_slot *entry = ta_actor_slot_at(system, slot);
        if (entry->actor != NULL) {
            (void)ta_actor_destroy(system, ta_actor_id_make(slot, entry->generation));
        }
    }
    while (system->unregistered_definitions != NULL) {
        ta_definition_destroy(system->unregistered_definitions);
    }
    /* Every actor is gone, so every definition goes, retired or not, and
     * the host hears of each while it is still intact (ADR 0079). */
    for (uint32_t slot = 0; slot < system->definitions.count; slot += 1) {
        ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        if (entry->definition != NULL) {
            ta_system_tell_definition(system, entry->definition, TA_DEFINITION_EVENT_FREED);
            ta_definition_destroy(entry->definition);
            entry->definition = NULL;
        }
    }
    ta_vec_destroy(&system->definitions);
    ta_timer_heap_destroy(&system->timers);
    ta_actor_table_destroy(&system->actors, system->allocator);
    /* After the actors: their held events released their MESSAGE origins. */
    ta_io_origin_table_destroy(&system->io_scopes);
    ta_io_origin_table_destroy(&system->io_messages);
    ta_vec_destroy(&system->io_processors);
    ta_vec_destroy(&system->invokers);
    system->block_cache_limit = 0;
    ta_interpreter_block_cache_trim(system);
    ta_system_arena_close(system);
    ta_interpreter_release_spares(system);
    /* Unloaded sessions hold addresses; give them back before the table goes. */
    ta_session_table_destroy(&system->sessions);
    /* Last: everything above gave its addresses back. */
    ta_address_table_destroy(&system->addresses);
    ta_event_name_table_destroy(&system->event_names);
    ta_atom_table_destroy(&system->atoms);
    ta_allocator allocator = system->allocator;
    ta_mem_free(allocator, system,
                &(ta_allocator_free_params){ .size = sizeof(ta_system), .alignment = _Alignof(ta_system) });
}

TA_API ta_result ta_system_set_actor_directory(ta_system *system, ta_actor_directory directory) {
    if (system == NULL) {
        return TA_ERR_INVALID;
    }
    system->directory = directory;
    return TA_OK;
}

TA_API ta_result ta_system_set_log_sink(ta_system *system, ta_log_sink sink) {
    if (system == NULL || (sink.v != NULL && sink.v->write == NULL)) {
        return TA_ERR_INVALID;
    }
    system->log_sink = sink;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * The definition sink (ADR 0079)
 */

static void ta_null_definition_notify(void *self, const ta_definition_event *event) {
    (void)self;
    (void)event;
}

static const ta_definition_sink_vtable ta_null_definition_vtable = { .notify = ta_null_definition_notify };

TA_API ta_definition_sink ta_null_definition_sink(void) {
    return (ta_definition_sink){ .self = NULL, .v = &ta_null_definition_vtable };
}

TA_API ta_result ta_system_set_definition_sink(ta_system *system, ta_definition_sink sink) {
    if (system == NULL || sink.v == NULL || sink.v->notify == NULL) {
        return TA_ERR_INVALID;
    }
    system->definition_sink = sink;
    return TA_OK;
}

/*
 * One definition event, told twice (ADR 0080): first as a lifecycle record
 * to the trace sink, when its mask asks for the kind, then to the definition
 * sink (ADR 0079). The record goes first, so a trace sink reading the
 * definition finds it as intact as the definition sink does, and before the
 * host's cleanup of it.
 */
TA_PRIVATE void ta_system_tell_definition(ta_system *system, const ta_definition *definition, ta_definition_event_kind kind) {
    static const ta_trace_kind trace_kinds[] = {
        [TA_DEFINITION_EVENT_REGISTERED] = TA_TRACE_DEFINITION_REGISTERED,
        [TA_DEFINITION_EVENT_RETIRED] = TA_TRACE_DEFINITION_RETIRED,
        [TA_DEFINITION_EVENT_FREED] = TA_TRACE_DEFINITION_FREED,
    };
    ta_trace_kind trace_kind = trace_kinds[kind];
    ta_trace_context none = { 0 };
    if (ta_trace_lifecycle_wanted(&system->trace, trace_kind, &none)) {
        ta_trace_record record = ta_trace_record_blank(trace_kind);
        /* One key per definition and event: a definition ID is never issued
         * twice (ADR 0043), and each event happens at most once to it. */
        record.key = (ta_step_key){ .session = TA_TRACE_DEFINITIONS_SESSION,
                                    .macrostep = definition->id,
                                    .microstep = (uint32_t)kind + 1u };
        record.definition = definition->id;
        record.identity = definition->identity;
        record.now = ta_clock_now(&system->clock);
        ta_trace_write(&system->trace, &record);
    }
    ta_definition_event event = { .kind = kind, .definition = definition->id, .identity = definition->identity };
    system->definition_sink.v->notify(system->definition_sink.self, &event);
}

TA_API ta_result ta_system_set_external_event_router(ta_system *system, ta_external_event_router router) {
    if (system == NULL || (router.v != NULL && router.v->submit == NULL)) {
        return TA_ERR_INVALID;
    }
    system->external_events = router;
    return TA_OK;
}

static bool ta_bytes_same(ta_bytes left, ta_bytes right);

/* The newest registered, non-retired definition with this identity, or NULL. */
static ta_definition *ta_system_live_definition(const ta_system *system, const ta_definition_identity *identity) {
    /* Slots are reused, so registration order decides: the newest match wins. */
    ta_definition *best = NULL;
    for (uint32_t slot = 0; slot < system->definitions.count; slot += 1) {
        const ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        ta_definition *definition = entry->definition;
        if (definition == NULL || definition->retired ||
            (best != NULL && best->registration_serial > definition->registration_serial)) {
            continue;
        }
        if (ta_str_equal(definition->identity.family.name, identity->family.name) &&
            ta_bytes_same(definition->identity.revision, identity->revision)) {
            best = definition;
        }
    }
    return best;
}

/*
 * Whether two definitions of the same content hold the same handles
 * (ADR 0087): every host expression's (v, self, value), the value being
 * what `retain` returned, is equal as pointers. Equal content hashes mean
 * the host expressions sit in the same places, so the tables line up.
 */
static bool ta_definition_same_handles(const ta_definition *left, const ta_definition *right) {
    if (left->expression_count != right->expression_count) {
        return false;
    }
    for (uint32_t index = 0; index < left->expression_count; index += 1u) {
        const ta_expression *a = &left->expressions[index];
        const ta_expression *b = &right->expressions[index];
        if (a->kind != b->kind) {
            return false;
        }
        if (a->kind == TA_EXPRESSION_HOST &&
            (a->as.host.v != b->as.host.v || a->as.host.self != b->as.host.self || a->as.host.value != b->as.host.value)) {
            return false;
        }
    }
    return true;
}

/*
 * The live definition that answers `definition`'s registration, or NULL to
 * register it beside the others (ADR 0077, ADR 0087). Every live
 * definition with its identity is looked at: other content under it is
 * TA_ERR_DEFINITION_MISMATCH, whatever the data-model adapter; a match has
 * the same adapter and the same handles, and the newest match answers.
 */
static ta_result ta_system_match_definition(const ta_system *system, const ta_definition *definition,
                                            ta_definition **out) {
    ta_definition *best = NULL;
    for (uint32_t slot = 0; slot < system->definitions.count; slot += 1u) {
        const ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        ta_definition *live = entry->definition;
        if (live == NULL || live->retired || !ta_str_equal(live->identity.family.name, definition->identity.family.name) ||
            !ta_bytes_same(live->identity.revision, definition->identity.revision)) {
            continue;
        }
        if (live->content_hash != definition->content_hash) {
            return TA_ERR_DEFINITION_MISMATCH;
        }
        /* The same chart compiled against another data-model adapter is
         * another definition: it runs differently, so it registers beside. */
        if (live->datamodel.self != definition->datamodel.self || live->datamodel.v != definition->datamodel.v) {
            continue;
        }
        if (live->host_expressions && !ta_definition_same_handles(live, definition)) {
            continue;
        }
        if (best == NULL || live->registration_serial > best->registration_serial) {
            best = live;
        }
    }
    *out = best;
    return TA_OK;
}

TA_API ta_result ta_system_register_definition(ta_system *system, ta_definition *definition, ta_definition_id *out) {
    if (system == NULL || definition == NULL || out == NULL || definition->system != system ||
        definition->registered) {
        return TA_ERR_INVALID;
    }
    /*
     * Idempotent by identity (ADR 0077): a live definition with this
     * identity, the same content and the same handles is the one registered,
     * and it gains a hold. The same identity for other content is refused
     * rather than silently answered with the old chart.
     */
    ta_definition *live = NULL;
    ta_result result = ta_system_match_definition(system, definition, &live);
    if (result != TA_OK) {
        return result;
    }
    if (live != NULL) {
        /* Where the chart is written now (ADR 0085): the spans of the
         * registration that asked, when it brought any. */
        ta_definition_take_spans(live, definition);
        live->holds += 1u;
        *out = live->id;
        ta_definition_destroy(definition);
        return TA_OK;
    }
    if (system->live_definitions >= system->params[TA_PARAM_MAX_DEFINITIONS]) {
        /* A quota, answered as an allocator that ran out (ADR 0092 section 4). */
        system->counters.quota_refusals += 1u;
        return TA_ERR_NOMEM;
    }
    uint32_t slot = system->free_definition_slot;
    if (slot == UINT32_MAX) {
        ta_definition_slot fresh = { .generation = 1, .next_free = UINT32_MAX };
        result = ta_vec_push(&system->definitions, &fresh);
        if (result != TA_OK) {
            return result;
        }
        slot = system->definitions.count - 1u;
    }
    ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
    if (slot == system->free_definition_slot) {
        system->free_definition_slot = entry->next_free;
    }
    if (definition->occupancy == NULL && definition->state_count != 0) {
        definition->occupancy = ta_arena_alloc(&definition->arena, (size_t)definition->state_count * sizeof(uint32_t),
                                               _Alignof(uint32_t));
        if (definition->occupancy == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_zero(definition->occupancy, (size_t)definition->state_count * sizeof(uint32_t));
    }
    entry->definition = definition;
    entry->next_free = UINT32_MAX;
    system->live_definitions += 1u;
    if (definition->unregistered_previous != NULL) {
        definition->unregistered_previous->unregistered_next = definition->unregistered_next;
    } else {
        system->unregistered_definitions = definition->unregistered_next;
    }
    if (definition->unregistered_next != NULL) {
        definition->unregistered_next->unregistered_previous = definition->unregistered_previous;
    }
    definition->unregistered_previous = NULL;
    definition->unregistered_next = NULL;
    definition->id = ((ta_definition_id)entry->generation << 32) | slot;
    definition->registered = true;
    definition->holds = 1u;
    definition->registration_serial = system->next_definition_serial++;
    *out = definition->id;
    ta_system_tell_definition(system, definition, TA_DEFINITION_EVENT_REGISTERED);
    return TA_OK;
}

TA_PRIVATE ta_definition *ta_system_definition(const ta_system *system, ta_definition_id id) {
    if (system == NULL) {
        return NULL;
    }
    uint32_t slot = (uint32_t)(id & 0xffffffffu);
    uint32_t generation = (uint32_t)(id >> 32);
    if (slot >= system->definitions.count) {
        return NULL;
    }
    const ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
    return entry->generation == generation ? entry->definition : NULL;
}

static bool ta_definition_unused(const ta_definition *definition) {
    return definition->holds == 0 && definition->holders == 0 && definition->actor_count == 0;
}

/*
 * Frees `definition` if nothing uses it any more (ADR 0077): no hold, no
 * definition nesting it, and no resident actor. Freeing a definition drops
 * its hold on each definition it nests, which may free those in turn; the
 * cascade runs as a worklist, not by recursion (ADR 0060). It runs in two
 * passes (ADR 0079): the first collects the cascade, the second tells the
 * host and frees in reverse order of discovery. A nested definition is
 * discovered after the one nesting it, so leaves go first, and each is
 * intact when the host hears of it.
 */
TA_PRIVATE void ta_system_settle_definition(ta_system *system, ta_definition *definition) {
    ta_definition *pending = ta_definition_unused(definition) ? definition : NULL;
    if (pending != NULL) {
        pending->free_next = NULL;
    }
    ta_definition *doomed = NULL;
    while (pending != NULL) {
        ta_definition *freeing = pending;
        pending = freeing->free_next;
        for (uint32_t index = 0; freeing->nested != NULL && index < freeing->invoke_decl_count; index += 1u) {
            ta_definition *child = ta_system_definition(system, freeing->nested[index]);
            if (child == NULL) {
                continue;
            }
            child->holders -= 1u;
            if (ta_definition_unused(child)) {
                child->free_next = pending;
                pending = child;
            }
        }
        /* Out of the worklist, so its link can thread the second pass. */
        freeing->free_next = doomed;
        doomed = freeing;
    }
    while (doomed != NULL) {
        ta_definition *freeing = doomed;
        doomed = freeing->free_next;
        ta_system_tell_definition(system, freeing, TA_DEFINITION_EVENT_FREED);
        uint32_t slot = (uint32_t)(freeing->id & 0xffffffffu);
        ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        /* The slot's next occupant gets a new generation, so this ID goes stale. */
        entry->definition = NULL;
        system->live_definitions -= 1u;
        entry->generation += 1u;
        entry->next_free = system->free_definition_slot;
        system->free_definition_slot = slot;
        ta_definition_destroy(freeing);
    }
}

TA_API ta_result ta_system_release_definition(ta_system *system, ta_definition_id id) {
    if (system == NULL) {
        return TA_ERR_INVALID;
    }
    ta_definition *definition = ta_system_definition(system, id);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (definition->holds == 0) {
        return TA_ERR_INVALID;
    }
    definition->holds -= 1u;
    ta_system_settle_definition(system, definition);
    return TA_OK;
}

TA_API ta_result ta_system_retire_definition(ta_system *system, ta_definition_id id) {
    if (system == NULL) {
        return TA_ERR_INVALID;
    }
    ta_definition *definition = ta_system_definition(system, id);
    if (definition == NULL) {
        /*
         * A slot's generation advances only when its definition is freed, so
         * an older generation of an existing slot was retired and freed.
         */
        uint32_t slot = (uint32_t)(id & 0xffffffffu);
        uint32_t generation = (uint32_t)(id >> 32);
        bool freed = slot < system->definitions.count && generation != 0 &&
                     generation < ((const ta_definition_slot *)ta_vec_at(&system->definitions, slot))->generation;
        return freed ? TA_OK : TA_ERR_NOT_FOUND;
    }
    if (!definition->retired) {
        definition->retired = true;
        ta_system_tell_definition(system, definition, TA_DEFINITION_EVENT_RETIRED);
    }
    definition->holds = 0;
    ta_system_settle_definition(system, definition);
    return TA_OK;
}

static bool ta_bytes_same(ta_bytes left, ta_bytes right) {
    if (left.len != right.len) {
        return false;
    }
    for (uint32_t index = 0; index < left.len; index += 1) {
        if (left.ptr[index] != right.ptr[index]) {
            return false;
        }
    }
    return true;
}

TA_API ta_result ta_system_find_definition(const ta_system *system,
                                    const ta_definition_identity *identity,
                                    ta_definition_id *out) {
    if (system == NULL || identity == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *live = ta_system_live_definition(system, identity);
    if (live == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    *out = live->id;
    return TA_OK;
}

TA_API ta_result ta_system_definitions(const ta_system *system, const ta_system_definitions_params *params) {
    if (system == NULL || params == NULL || params->count == NULL) {
        if (params != NULL && params->count != NULL) {
            *params->count = 0;
        }
        return TA_ERR_INVALID;
    }
    /* Slots in increasing order: a definition held throughout a walk keeps
     * its slot, so it is listed exactly once (ADR 0078). */
    uint32_t count = 0;
    for (uint32_t slot = params->from; slot < system->definitions.count; slot += 1u) {
        if (params->limit != 0 && count == params->limit) {
            break;
        }
        const ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        if (entry->definition == NULL) {
            continue;
        }
        if (params->definitions != NULL && count < params->capacity) {
            params->definitions[count] = entry->definition->id;
        }
        count += 1u;
    }
    *params->count = count;
    return count > params->capacity ? TA_ERR_NOMEM : TA_OK;
}

TA_PRIVATE bool ta_system_is_scxml_io(const ta_system *system, ta_atom type) {
    return type == system->scxml_io_type || type == system->scxml_io_alias;
}

TA_PRIVATE const ta_io *ta_system_io_of(const ta_system *system, ta_atom type) {
    for (uint32_t index = 0; index < system->io_processors.count; index += 1) {
        const ta_io_registration *entry = ta_vec_at(&system->io_processors, index);
        if (entry->type == type) {
            return &entry->io;
        }
    }
    return NULL;
}

TA_PRIVATE const ta_io_registration *ta_system_io_at(const ta_system *system, uint32_t index) {
    if (index == 0 || index > system->io_processors.count) {
        return NULL;
    }
    return ta_vec_at(&system->io_processors, index - 1u);
}

TA_API ta_result ta_system_find_io(const ta_system *system, ta_atom type, uint32_t *index) {
    if (system == NULL || index == NULL) {
        return TA_ERR_INVALID;
    }
    if (ta_system_is_scxml_io(system, type)) {
        *index = 0;
        return TA_OK;
    }
    for (uint32_t at = 0; at < system->io_processors.count; at += 1) {
        const ta_io_registration *entry = ta_vec_at(&system->io_processors, at);
        if (entry->type == type) {
            *index = at + 1u;
            return TA_OK;
        }
    }
    return TA_ERR_NOT_FOUND;
}

/* Registering a type again replaces its adapter at the same index (ADR 0068). */
TA_API ta_result ta_system_register_io(ta_system *system, const ta_io_registration_params *params) {
    if (system == NULL || params == NULL || params->io.v == NULL || params->io.v->send == NULL ||
        !ta_atom_valid(&system->atoms, params->type) || ta_system_is_scxml_io(system, params->type)) {
        return TA_ERR_INVALID;
    }
    uint32_t index = 0;
    if (ta_system_find_io(system, params->type, &index) == TA_OK) {
        ta_io_registration *entry = ta_vec_at(&system->io_processors, index - 1u);
        entry->io = params->io;
    } else {
        ta_io_registration entry = { .type = params->type, .io = params->io };
        ta_result result = ta_vec_push(&system->io_processors, &entry);
        if (result != TA_OK) {
            return result;
        }
        index = system->io_processors.count;
    }
    if (params->index != NULL) {
        *params->index = index;
    }
    return TA_OK;
}

TA_PRIVATE uint32_t ta_system_invoker_index(const ta_system *system, ta_atom type) {
    for (uint32_t index = 0; index < system->invokers.count; index += 1) {
        const ta_invoker_registration *entry = ta_vec_at(&system->invokers, index);
        if (entry->type == type) {
            return index;
        }
    }
    return UINT32_MAX;
}

TA_PRIVATE ta_atom ta_system_invoker_type(const ta_system *system, uint32_t index) {
    if (index >= system->invokers.count) {
        return TA_ATOM_NONE;
    }
    const ta_invoker_registration *entry = ta_vec_at(&system->invokers, index);
    return entry->type;
}

TA_API ta_result ta_system_register_invoker(ta_system *system, ta_atom type, ta_invoker invoker) {
    if (system == NULL || invoker.v == NULL || !ta_atom_valid(&system->atoms, type)) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < system->invokers.count; index += 1) {
        ta_invoker_registration *entry = ta_vec_at(&system->invokers, index);
        if (entry->type == type) {
            entry->invoker = invoker;
            return TA_OK;
        }
    }
    ta_invoker_registration entry = { .type = type, .invoker = invoker };
    return ta_vec_push(&system->invokers, &entry);
}

TA_API ta_result ta_system_intern_atom(ta_system *system, ta_str text, ta_atom *out) {
    if (system == NULL || out == NULL || text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    return ta_atom_intern(&system->atoms, text, out);
}

TA_API ta_result ta_system_atom_text(const ta_system *system, ta_atom atom, ta_str *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    if (!ta_atom_valid(&system->atoms, atom)) {
        return TA_ERR_NOT_FOUND;
    }
    *out = ta_atom_text(&system->atoms, atom);
    return TA_OK;
}

TA_API ta_result ta_event_name_intern(ta_system *system, const ta_atom_list *segments, ta_event_name_id *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    return ta_event_name_intern_atoms_internal(&system->event_names, segments, out);
}

TA_API bool ta_event_name_matches(const ta_system *system, ta_event_name_id event, ta_event_name_id prefix) {
    return system != NULL && ta_event_name_matches_internal(&system->event_names, event, prefix);
}

TA_API ta_result ta_event_name_format(const ta_system *system, const ta_event_name_format_params *params) {
    if (system == NULL || params == NULL || params->needed == NULL ||
        !ta_event_name_valid(&system->event_names, params->name)) {
        return TA_ERR_INVALID;
    }
    ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
    uint32_t count = ta_event_name_segments(&system->event_names, params->name, segments);
    uint32_t length = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        length += ta_atom_text(&system->atoms, segments[index]).len + (index == 0 ? 0u : 1u);
    }
    *params->needed = length + 1u;
    if (params->buffer == NULL || params->capacity < length + 1u) {
        return TA_ERR_NOMEM;
    }
    uint32_t at = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        if (index != 0) {
            params->buffer[at++] = '.';
        }
        ta_str text = ta_atom_text(&system->atoms, segments[index]);
        ta_mem_copy(params->buffer + at, text.ptr, text.len);
        at += text.len;
    }
    params->buffer[at] = '\0';
    return TA_OK;
}

TA_API ta_result ta_system_set_time(ta_system *system, ta_time now) {
    if (system == NULL || now < ta_clock_now(&system->clock)) {
        return TA_ERR_INVALID;
    }
    ta_clock_set(&system->clock, now);
    return TA_OK;
}

TA_API ta_time ta_system_time(const ta_system *system) {
    return system == NULL ? 0 : ta_clock_now(&system->clock);
}

TA_API uint64_t ta_system_incarnation(const ta_system *system) {
    return system == NULL ? 0 : system->incarnation;
}

TA_API ta_atom ta_system_scxml_io_type(const ta_system *system) {
    return system == NULL ? TA_ATOM_NONE : system->scxml_io_type;
}

TA_API ta_result ta_system_next_deadline(const ta_system *system, ta_time *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    return ta_timer_next_due(&system->timers, out) ? TA_OK : TA_ERR_NOT_FOUND;
}

TA_API ta_result ta_system_dispatch_due(ta_system *system, const ta_due_dispatch_params *params) {
    if (system == NULL || params == NULL || params->budget == 0 || params->dispatched == NULL ||
        params->more_due == NULL) {
        return TA_ERR_INVALID;
    }
    uint32_t dispatched = 0;
    bool retry = false;
    ta_time now = ta_clock_now(&system->clock);
    while (dispatched < params->budget) {
        ta_time due = 0;
        if (!ta_timer_next_due(&system->timers, &due) || due > now) {
            break;
        }
        ta_delayed_send send;
        (void)ta_timer_pop(&system->timers, &send);
        ta_actor *owner = ta_actor_lookup(system, send.owner);
        /*
         * A pending send leaves its owner's list as it fires, and an image
         * captures that list, so an image taken before no longer describes
         * the owner (ADR 0014). A submitted send is in no image.
         */
        if (owner != NULL && !send.submitted) {
            ta_actor_touch(owner);
        }
        const ta_definition *owner_definition = owner != NULL ? owner->definition : NULL;
        ta_external_event_submit_status fired = ta_interpreter_dispatch_delayed(system, &send, false);
        if (fired != TA_EXTERNAL_EVENT_RETRY) {
            if (owner_definition != NULL) {
                TA_COUNT(system, owner_definition, delayed_fired);
            } else {
                system->counters.delayed_fired += 1u;
            }
        }
        if (fired == TA_EXTERNAL_EVENT_RETRY) {
            /*
             * Rescheduling keeps its ID, and so its place in deadline order.
             * The owner may be gone by now; its submitted sends are orphans.
             */
            ta_timer_list *home = NULL;
            ta_result result = ta_actor_timer_home(system, send.owner, &home);
            if (result == TA_OK) {
                result = ta_timer_schedule(&system->timers, home, &send);
            }
            if (result != TA_OK) {
                ta_value_release(&send.target);
                ta_value_release(&send.data);
                return result;
            }
            retry = true;
            break;
        }
        dispatched += 1;
    }
    ta_time next = 0;
    *params->dispatched = dispatched;
    *params->more_due = retry || (ta_timer_next_due(&system->timers, &next) && next <= now);
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Counters and occupancy (ADR 0072)
 */

TA_API ta_result ta_definition_counters(const ta_system *system, ta_definition_id definition, ta_counters *out) {
    const ta_definition *found = ta_system_definition(system, definition);
    if (found == NULL || out == NULL) {
        return out == NULL ? TA_ERR_INVALID : TA_ERR_NOT_FOUND;
    }
    *out = found->counters;
    return TA_OK;
}

TA_API ta_result ta_definition_set_microstep_limit(ta_system *system, ta_definition_id definition, uint32_t limit) {
    ta_definition *found = system == NULL ? NULL : ta_system_definition(system, definition);
    if (found == NULL) {
        return system == NULL ? TA_ERR_INVALID : TA_ERR_NOT_FOUND;
    }
    found->microstep_limit = limit;
    return TA_OK;
}

TA_API void ta_system_counters(const ta_system *system, ta_counters *out) {
    if (system != NULL && out != NULL) {
        *out = system->counters;
    }
}

TA_API ta_result ta_definition_occupancy(const ta_system *system, const ta_definition_occupancy_params *params) {
    if (params == NULL || params->count == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_system_definition(system, params->definition);
    if (definition == NULL) {
        *params->count = 0;
        return TA_ERR_NOT_FOUND;
    }
    *params->count = definition->state_count;
    if (params->states == NULL || params->capacity < definition->state_count) {
        return TA_ERR_NOMEM;
    }
    if (definition->state_count != 0) {
        ta_mem_copy(params->states, definition->occupancy, (size_t)definition->state_count * sizeof(uint32_t));
    }
    return TA_OK;
}


/*
 * SCXML execution following the W3C algorithm (Appendix D). The actor's
 * configuration stores active atomic states; a microstep expands it into the
 * full active set, applies exits and entries to that set, and writes the
 * atomic states back.
 */

/* Executable content failed; error.execution is queued and the block stops (SCXML 4.9). */
#define TA_EXEC_FAILED 1

/* A block stopped at a pause point with its frames left open (ADR 0061). */
#define TA_EXEC_PAUSED 2

/* In `enabled`: the document's initial transition, which bootstrap takes. It
 * is not a transition ID (ADR 0071 numbers initial transitions after the
 * authored ones); it only marks the bootstrap microstep. */
#define TA_ENABLED_BOOTSTRAP (UINT32_MAX - 1u)

/* In `enabled`: an operator transition (ADR 0082), which `it->jump` holds
 * while the microstep begins. It has no executable content, so nothing
 * after that reads it, and a continuation carries this mark alone. */
#define TA_ENABLED_OPERATOR (UINT32_MAX - 2u)

/* Whether an entry of `enabled` is one of the definition's transitions. */
static bool ta_enabled_authored(ta_transition_id transition) {
    return transition != TA_ENABLED_BOOTSTRAP && transition != TA_ENABLED_OPERATOR;
}

/* A list of states: a transition's targets, in the definition or in an
 * operator transition. */
typedef struct {
    const ta_state_id *ids;
    uint32_t count;
} ta_state_list;

typedef struct {
    ta_state_id source;
    ta_state_list targets;
    ta_transition_type type;
    ta_span32 actions;
} ta_transition_view;

static ta_state_list ta_targets(const ta_interpreter *it, ta_span32 targets) {
    return (ta_state_list){ .ids = targets.length == 0 ? NULL : it->definition->targets + targets.offset,
                            .count = targets.length };
}

/* The executable content of an entry of `enabled`; an operator transition
 * has none. */
static ta_span32 ta_enabled_actions(const ta_definition *definition, ta_transition_id transition) {
    if (transition == TA_ENABLED_BOOTSTRAP) {
        return definition->initial_transitions[definition->root_initial].actions;
    }
    if (transition == TA_ENABLED_OPERATOR) {
        return (ta_span32){0};
    }
    return definition->transitions[transition].actions;
}

static ta_transition_view ta_view(const ta_interpreter *it, ta_transition_id transition) {
    const ta_definition *definition = it->definition;
    if (transition == TA_ENABLED_BOOTSTRAP) {
        const ta_initial_transition_def *initial = &definition->initial_transitions[definition->root_initial];
        return (ta_transition_view){
            .source = TA_STATE_NONE,
            .targets = ta_targets(it, initial->targets),
            .type = TA_TRANSITION_EXTERNAL,
            .actions = ta_enabled_actions(definition, transition),
        };
    }
    if (transition == TA_ENABLED_OPERATOR) {
        const ta_operator_jump *jump = it->jump;
        return (ta_transition_view){
            .source = jump == NULL ? TA_STATE_NONE : jump->source,
            .targets = { .ids = jump == NULL ? NULL : jump->targets, .count = jump == NULL ? 0 : jump->target_count },
            .type = TA_TRANSITION_INTERNAL,
        };
    }
    const ta_transition_def *def = &definition->transitions[transition];
    return (ta_transition_view){
        .source = def->source,
        .targets = ta_targets(it, def->targets),
        .type = def->type,
        .actions = ta_enabled_actions(definition, transition),
    };
}

static const ta_state_def *ta_state(const ta_interpreter *it, ta_state_id state) {
    return &it->definition->states[state];
}

static bool ta_is_history(const ta_interpreter *it, ta_state_id state) {
    ta_state_kind kind = ta_state(it, state)->kind;
    return kind == TA_STATE_HISTORY_SHALLOW || kind == TA_STATE_HISTORY_DEEP;
}

static bool ta_descends(const ta_interpreter *it, ta_state_id state, ta_state_id ancestor) {
    return ta_definition_is_descendant(it->definition, state, ancestor);
}

static ta_state_id ta_child_at(const ta_interpreter *it, ta_state_id state, uint32_t index) {
    return it->definition->children[ta_state(it, state)->children.offset + index];
}

static void ta_bits_clear(const ta_interpreter *it, uint64_t *words) {
    ta_mem_zero(words, it->words * sizeof(uint64_t));
}

static bool ta_bits_any(const ta_interpreter *it, const uint64_t *words) {
    for (uint32_t index = 0; index < it->words; index += 1) {
        if (words[index] != 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------
 * Tracing (ADR 0067)
 *
 * Every record is written once, when the step that produced it commits: a
 * step that runs out of memory and runs again (ADR 0070) writes its records
 * on the run that commits, and on no other. Events a step queued are written
 * as RAISED records from a queue mark, after the step's own record. The
 * guards of a selection wait in `conditions` until the selection completes.
 */

/* One guard the selection in progress evaluated. */
struct ta_trace_condition {
    ta_transition_id transition;
    ta_expr_id expr;
    bool result;
    bool failed;
    ta_event_name_id detail;
    int32_t code;
    /* The queue index of the error.execution it raised, or UINT32_MAX. */
    uint32_t raised;
};

static bool ta_tracing(const ta_interpreter *it, ta_trace_kind kind) {
    return (it->trace_kinds & TA_TRACE_BIT(kind)) != 0;
}

TA_PRIVATE ta_trace_parent ta_actor_macrostep_parent(const ta_actor *actor) {
    return actor->event_block == NULL ? (ta_trace_parent){0} : actor->event_block->slots.trace;
}

TA_PRIVATE ta_step_key ta_actor_step(const ta_actor *actor) {
    if (!actor->trace_begun) {
        return (ta_step_key){ .session = actor->session_id, .family = actor->definition->family };
    }
    return (ta_step_key){ .session = actor->session_id, .macrostep = actor->trace_macrostep,
                          .microstep = actor->trace_microstep, .family = actor->definition->family };
}

/* The key of the step running now; outside a macrostep, the session alone. */
static ta_step_key ta_trace_key(const ta_interpreter *it) {
    return ta_actor_step(it->actor);
}

TA_PRIVATE ta_trace_parent ta_interpreter_trace_here(const ta_interpreter *it) {
    return (ta_trace_parent){ .context = it->trace_context, .cause = ta_trace_key(it) };
}

/* The iteration, from 0, whose body the innermost <foreach> among the
 * first `count` frames runs. */
static uint32_t ta_frames_iteration(const ta_exec_frame *frames, uint32_t count) {
    for (uint32_t index = count; index > 0; index -= 1u) {
        const ta_exec_frame *frame = &frames[index - 1u];
        if (frame->kind == TA_EXEC_FRAME_FOREACH) {
            return frame->iteration == 0 ? 0 : frame->iteration - 1u;
        }
    }
    return 0;
}

static ta_trace_position ta_trace_nowhere(void) {
    return (ta_trace_position){ .at = TA_TRACE_AT_NONE, .state = TA_STATE_NONE, .transition = TA_TRANSITION_NONE,
                                .action = TA_ACTION_NONE };
}

/* What a run keeps of where it stands: in scratch while it runs, in the
 * actor's continuation once it stopped. The sets and the default-history
 * map are read only at a microstep's start (ADR 0086). */
typedef struct {
    ta_micro_position at;
    const ta_transition_id *enabled;
    uint32_t enabled_count;
    const ta_exec_frame *frames;
    uint32_t frame_count;
    const uint64_t *to_exit;
    const uint64_t *to_enter;
    const uint64_t *default_entry;
    const ta_state_id *default_history;
} ta_run_view;

/* The phase of a position, with its state and transition; no action. */
static ta_trace_position ta_phase_position(const ta_definition *definition, const ta_run_view *view) {
    ta_trace_position position = ta_trace_nowhere();
    switch (view->at.phase) {
        case TA_PHASE_FINISH:
            if (view->at.stage != TA_FINISH_EXIT) {
                break;
            }
            /* fall through: exiting every state, highest first */
        case TA_PHASE_EXIT:
            position.at = TA_TRACE_AT_EXIT;
            position.state = view->at.state == 0 ? TA_STATE_NONE : view->at.state - 1u;
            break;
        case TA_PHASE_TRANSITIONS:
            position.at = TA_TRACE_AT_TRANSITION;
            if (view->at.state < view->enabled_count && ta_enabled_authored(view->enabled[view->at.state])) {
                position.transition = view->enabled[view->at.state];
                position.state = definition->transitions[position.transition].source;
            }
            break;
        case TA_PHASE_ENTER:
            position.at = TA_TRACE_AT_ENTER;
            position.state = view->at.state;
            break;
        case TA_PHASE_DATA:
        case TA_PHASE_SCRIPT:
            position.at = TA_TRACE_AT_BOOTSTRAP;
            break;
        case TA_PHASE_FINALIZE:
            position.at = TA_TRACE_AT_FINALIZE;
            break;
        case TA_PHASE_SELECT:
            position.at = TA_TRACE_AT_SELECT;
            break;
        case TA_PHASE_INVOKES:
            position.at = TA_TRACE_AT_INVOKE;
            position.state = view->at.state;
            break;
        case TA_PHASE_NONE:
        default:
            break;
    }
    return position;
}

/* The first block of a state's handlers that holds a unit, or an empty one. */
static ta_span32 ta_first_block(const ta_definition *definition, ta_span32 handlers) {
    for (uint32_t index = 0; index < handlers.length; index += 1u) {
        ta_span32 block = definition->handlers[handlers.offset + index];
        if (block.length != 0) {
            return block;
        }
    }
    return (ta_span32){0};
}

/* The position of the first action of `block`, run where `at` stands. */
static ta_trace_position ta_unit_at(const ta_definition *definition, const ta_run_view *view, ta_micro_position at,
                                    ta_span32 block) {
    ta_run_view there = *view;
    there.at = at;
    ta_trace_position position = ta_phase_position(definition, &there);
    position.action = block.length == 0 ? TA_ACTION_NONE : block.offset;
    return position;
}

/*
 * Where a microstep stands at its start (ADR 0086): the first unit it runs,
 * found from its sets without running anything, in the order the microstep
 * runs units: the <onexit> handlers of the exit set, highest first; the
 * enabled transitions' content; then, for each state of the entry set,
 * lowest first, its <onentry> handlers, its initial transition's content
 * and its history default's content. A microstep with no unit stands at
 * its first transition, with no action.
 */
static ta_trace_position ta_start_position(const ta_definition *definition, const ta_run_view *view) {
    for (ta_state_id state = definition->state_count; state > 0; state -= 1u) {
        ta_span32 block = ta_bits_get(view->to_exit, state - 1u)
                              ? ta_first_block(definition, definition->states[state - 1u].onexit_handlers)
                              : (ta_span32){0};
        if (block.length != 0) {
            return ta_unit_at(definition, view, (ta_micro_position){ .phase = TA_PHASE_EXIT, .state = state }, block);
        }
    }
    ta_micro_position transitions = { .phase = TA_PHASE_TRANSITIONS };
    for (uint32_t index = 0; index < view->enabled_count; index += 1u) {
        ta_span32 block = ta_enabled_actions(definition, view->enabled[index]);
        if (block.length != 0) {
            transitions.state = index;
            return ta_unit_at(definition, view, transitions, block);
        }
    }
    for (ta_state_id state = 0; state < definition->state_count; state += 1u) {
        if (!ta_bits_get(view->to_enter, state)) {
            continue;
        }
        const ta_state_def *def = &definition->states[state];
        ta_span32 block = ta_first_block(definition, def->onentry_handlers);
        if (block.length == 0 && ta_bits_get(view->default_entry, state)) {
            block = definition->initial_transitions[def->initial].actions;
        }
        ta_state_id history = view->default_history[state];
        if (block.length == 0 && history != TA_STATE_NONE) {
            block = definition->transitions[definition->states[history].transitions.offset].actions;
        }
        if (block.length != 0) {
            return ta_unit_at(definition, view, (ta_micro_position){ .phase = TA_PHASE_ENTER, .state = state }, block);
        }
    }
    return ta_unit_at(definition, view, (ta_micro_position){ .phase = TA_PHASE_TRANSITIONS }, (ta_span32){0});
}

/*
 * Where a stopped run stands (ADR 0083): its phase, and the unit of
 * executable content it runs next. With a <foreach> on top of the frames,
 * that unit is the loop's next iteration: `action` is the <foreach> and
 * `iteration` the one it binds next. Otherwise it is the next action of the
 * innermost open block, in the iteration of the innermost <foreach> whose
 * body that block is. With no open block there is no action. At a
 * microstep's start it is the first unit the microstep runs (ADR 0086).
 */
static ta_trace_position ta_run_position(const ta_definition *definition, const ta_run_view *view) {
    if (view->at.phase == TA_PHASE_START) {
        return ta_start_position(definition, view);
    }
    ta_trace_position position = ta_phase_position(definition, view);
    if (view->frame_count == 0) {
        return position;
    }
    const ta_exec_frame *top = &view->frames[view->frame_count - 1u];
    if (top->kind == TA_EXEC_FRAME_FOREACH) {
        position.action = top->cursor;
        position.iteration = top->iteration;
        return position;
    }
    if (top->cursor < top->actions.length) {
        position.action = top->actions.offset + top->cursor;
    }
    position.iteration = ta_frames_iteration(view->frames, view->frame_count - 1u);
    return position;
}

static ta_run_view ta_interpreter_view(const ta_interpreter *it) {
    return (ta_run_view){ .at = it->at, .enabled = it->enabled, .enabled_count = it->enabled_count,
                          .frames = it->exec_frames, .frame_count = it->exec_count, .to_exit = it->to_exit,
                          .to_enter = it->to_enter, .default_entry = it->default_entry,
                          .default_history = it->default_history };
}

TA_PRIVATE ta_trace_position ta_actor_position(const ta_actor *actor) {
    ta_continuation *saved = actor->sparse == NULL ? NULL : actor->sparse->paused;
    if (!actor->macrostep || saved == NULL) {
        return ta_trace_nowhere();
    }
    /* The sets in their order: active, to exit, to enter, default entry. */
    const uint64_t *sets = ta_continuation_sets(saved);
    ta_run_view view = { .at = saved->at, .enabled = ta_continuation_enabled(saved),
                         .enabled_count = saved->enabled_count, .frames = ta_continuation_frames(saved),
                         .frame_count = saved->frame_count, .to_exit = sets + saved->words,
                         .to_enter = sets + 2u * (size_t)saved->words,
                         .default_entry = sets + 3u * (size_t)saved->words,
                         .default_history = ta_continuation_history(saved) };
    return ta_run_position(actor->definition, &view);
}

/* Where the interpreter stands while a unit runs: the phase, the unit
 * running now, and the iteration its innermost <foreach> runs. */
static ta_trace_position ta_trace_position_now(const ta_interpreter *it) {
    ta_run_view view = ta_interpreter_view(it);
    ta_trace_position position = ta_phase_position(it->definition, &view);
    position.action = it->trace_action;
    position.iteration = ta_frames_iteration(it->exec_frames, it->exec_count);
    return position;
}

/* A record of `kind` at the step and position running now. */
static ta_trace_record ta_trace_record_now(const ta_interpreter *it, ta_trace_kind kind) {
    ta_trace_record record = ta_trace_record_blank(kind);
    record.key = ta_trace_key(it);
    record.position = ta_trace_position_now(it);
    record.context = it->trace_context;
    record.actor = it->actor->record.id;
    record.definition = it->definition->id;
    record.now = ta_actor_now(it->system, it->actor);
    return record;
}

static void ta_trace_emit(const ta_interpreter *it, const ta_trace_record *record) {
    ta_trace_write(&it->system->trace, record);
}

/* A fault's detail and code; its data went into the error event. */
static void ta_trace_fault(ta_trace_record *record, const ta_fault *fault) {
    record->has_fault = true;
    if (fault != NULL) {
        /* The data moved into the error event; ACTION_FAILED borrows it back. */
        record->fault = (ta_fault){ .detail = fault->detail, .code = fault->code, .message = fault->message };
    }
}

/* Counts one error event by its kind (ADR 0072); other events count nothing. */
static void ta_count_error(ta_system *system, const ta_actor *actor, ta_event_name_id name) {
    if (ta_event_name_matches_internal(&system->event_names, name, TA_EVENT_NAME_ERROR_EXECUTION)) {
        TA_COUNT(system, actor->definition, errors_execution);
    } else if (ta_event_name_matches_internal(&system->event_names, name, TA_EVENT_NAME_ERROR_COMMUNICATION)) {
        TA_COUNT(system, actor->definition, errors_communication);
    } else if (ta_event_name_matches_internal(&system->event_names, name, TA_EVENT_NAME_ERROR_PLATFORM)) {
        TA_COUNT(system, actor->definition, errors_platform);
    }
}

/* Counts the errors a unit queued after `mark`, when it commits. */
static void ta_count_raised(ta_interpreter *it, uint32_t mark) {
    const ta_event_block *queue = it->actor->event_block;
    for (uint32_t index = mark; queue != NULL && index < queue->count; index += 1u) {
        ta_count_error(it->system, it->actor, queue->events[index].event.name);
    }
}

static void ta_trace_raised_records(ta_interpreter *it, uint32_t mark) {
    const ta_event_block *queue = it->actor->event_block;
    if (!ta_tracing(it, TA_TRACE_RAISED) || queue == NULL) {
        return;
    }
    for (uint32_t index = mark; index < queue->count; index += 1u) {
        ta_trace_record record = ta_trace_record_now(it, TA_TRACE_RAISED);
        record.event = &queue->events[index].event;
        ta_trace_emit(it, &record);
    }
}

TA_PRIVATE void ta_interpreter_trace_raised(ta_interpreter *it, uint32_t mark) {
    ta_count_raised(it, mark);
    ta_trace_raised_records(it, mark);
}

TA_PRIVATE void ta_interpreter_trace_invoke(ta_interpreter *it, ta_trace_kind kind, ta_invoke_id invoke) {
    if (kind == TA_TRACE_INVOKED) {
        TA_COUNT(it->system, it->actor->definition, invocations_started);
    } else if (kind == TA_TRACE_INVOKE_CANCELLED) {
        TA_COUNT(it->system, it->actor->definition, invocations_ended);
    }
    if (!ta_tracing(it, kind)) {
        return;
    }
    ta_trace_record record = ta_trace_record_now(it, kind);
    record.invoke = invoke;
    ta_trace_emit(it, &record);
}

TA_PRIVATE void ta_interpreter_trace_forward(ta_interpreter *it, ta_invoke_id invoke, const ta_event *event) {
    if (!ta_tracing(it, TA_TRACE_FORWARDED)) {
        return;
    }
    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_FORWARDED);
    record.invoke = invoke;
    record.event = event;
    ta_trace_emit(it, &record);
}

/* An action of the running unit failed; its error is queued. */
static void ta_trace_action_failed(ta_interpreter *it, const ta_fault *fault) {
    if (!ta_tracing(it, TA_TRACE_ACTION_FAILED)) {
        return;
    }
    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_ACTION_FAILED);
    record.action = it->trace_action;
    ta_trace_fault(&record, fault);
    /* The error the failure just queued is the queue's last event, and its
     * data is what the operation reported: borrow it for the record. */
    const ta_event_block *queue = it->actor->event_block;
    if (queue != NULL && queue->count != 0) {
        record.fault.data = queue->events[queue->count - 1u].event.data;
    }
    ta_trace_emit(it, &record);
}

/* The states of `set` into `out`, in document order or its reverse. */
static uint32_t ta_trace_states(const ta_interpreter *it, const uint64_t *set, ta_state_id *out, bool reverse) {
    uint32_t count = 0;
    uint32_t states = it->definition->state_count;
    for (uint32_t index = 0; index < states; index += 1u) {
        ta_state_id state = reverse ? states - 1u - index : index;
        if (ta_bits_get(set, state)) {
            out[count++] = state;
        }
    }
    return count;
}

/*
 * The macrostep's first step has committed: taking its event can no longer
 * be undone. Its counter moves, and MACROSTEP_BEGIN is written, once. The
 * actor changes, so an image taken before no longer describes it.
 */
static void ta_trace_begin(ta_interpreter *it) {
    ta_actor *actor = it->actor;
    if (actor->trace_begun) {
        return;
    }
    ta_actor_touch(actor);
    actor->trace_begun = true;
    actor->trace_macrostep += 1u;
    TA_COUNT(it->system, actor->definition, macrosteps);
    actor->trace_microstep = 0;
    ta_trace_parent parent = ta_actor_macrostep_parent(actor);
    it->trace_context = parent.context;
    it->trace_kinds = ta_trace_macrostep_kinds(&it->system->trace, &parent.context);
    if (!ta_tracing(it, TA_TRACE_MACROSTEP_BEGIN)) {
        return;
    }
    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_MACROSTEP_BEGIN);
    record.position = ta_trace_nowhere();
    record.cause = parent.cause;
    const ta_event_block *block = actor->event_block;
    if (block != NULL && block->slots.external == TA_EXTERNAL_TAKEN) {
        record.event = &block->slots.external_event;
    }
    ta_trace_emit(it, &record);
}

/* The macrostep ended, stable or finished: MACROSTEP_END, with the active
 * configuration. */
static void ta_trace_end(ta_interpreter *it) {
    ta_actor *actor = it->actor;
    if (!actor->trace_begun) {
        return;
    }
    if (ta_tracing(it, TA_TRACE_MACROSTEP_END) && it->trace_states != NULL) {
        ta_trace_record record = ta_trace_record_now(it, TA_TRACE_MACROSTEP_END);
        record.position = ta_trace_nowhere();
        record.states = it->trace_states;
        record.state_count = ta_trace_states(it, it->active, it->trace_states, false);
        ta_trace_emit(it, &record);
    }
    actor->trace_begun = false;
}

/*
 * A microstep leaves selection and becomes visible: its counter moves, and
 * one MICROSTEP record names its event, its transitions in selection order,
 * the exit set in exit order, the entry set in entry order, and the history
 * states the exit recorded. It is written before its actions run.
 */
static void ta_trace_microstep_record(ta_interpreter *it, const ta_event *event) {
    if (!ta_tracing(it, TA_TRACE_MICROSTEP) || it->trace_states == NULL) {
        return;
    }
    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_MICROSTEP);
    record.position = ta_trace_nowhere();
    record.event = event;
    if (it->enabled_count != 0 && ta_enabled_authored(it->enabled[0])) {
        record.transitions = it->enabled;
        record.transition_count = it->enabled_count;
    } else if (it->jump != NULL) {
        /* An operator transition: its source and the targets it named. */
        record.state = it->jump->source;
        record.states = it->jump->targets;
        record.state_count = it->jump->target_count;
    }
    ta_state_id *lists = it->trace_states;
    record.exited = lists;
    record.exited_count = ta_trace_states(it, it->to_exit, lists, true);
    record.entered = lists + record.exited_count;
    record.entered_count = ta_trace_states(it, it->to_enter, lists + record.exited_count, false);
    ta_state_id *history = lists + record.exited_count + record.entered_count;
    const ta_definition *definition = it->definition;
    for (ta_state_id state = 0; state < definition->state_count; state += 1u) {
        const ta_state_def *def = &definition->states[state];
        if ((def->kind == TA_STATE_HISTORY_SHALLOW || def->kind == TA_STATE_HISTORY_DEEP) &&
            ta_bits_get(it->to_exit, def->parent)) {
            history[record.history_count++] = state;
        }
    }
    record.history = history;
    ta_trace_emit(it, &record);
}

/*
 * The microstep that reached a tenth of the limit, or the limit itself
 * (ADR 0075): a counter, and a record with its transitions and the
 * configuration before it. Counts move only here, on commit, so a retry
 * never counts twice.
 */
static void ta_trace_runaway_mark(ta_interpreter *it) {
    uint32_t limit = it->microstep_limit;
    uint32_t count = it->actor->trace_microstep;
    uint32_t warning = limit / 10u == 0 ? 1u : limit / 10u;
    if (limit == 0 || (count != warning && count != limit)) {
        return;
    }
    ta_trace_kind kind = count == limit ? TA_TRACE_RUNAWAY : TA_TRACE_LONG_MACROSTEP;
    if (kind == TA_TRACE_RUNAWAY) {
        TA_COUNT(it->system, it->actor->definition, runaways);
    } else {
        TA_COUNT(it->system, it->actor->definition, macrosteps_long);
    }
    if (!ta_tracing(it, kind) || it->trace_states == NULL) {
        return;
    }
    ta_trace_record record = ta_trace_record_now(it, kind);
    record.position = ta_trace_nowhere();
    if (it->enabled_count != 0 && it->enabled[0] != TA_ENABLED_BOOTSTRAP) {
        record.transitions = it->enabled;
        record.transition_count = it->enabled_count;
    }
    record.states = it->trace_states;
    record.state_count = ta_trace_states(it, it->active, it->trace_states, false);
    ta_trace_emit(it, &record);
}

/* A microstep commits: its counter moves, its record is written, and the
 * runaway marks are checked. */
static void ta_trace_microstep(ta_interpreter *it, const ta_event *event) {
    it->actor->trace_microstep += 1u;
    TA_COUNT(it->system, it->actor->definition, microsteps);
    ta_trace_microstep_record(it, event);
    ta_trace_runaway_mark(it);
}

/* Keeps a guard the selection evaluated, to be written when it completes. */
static void ta_trace_guard(ta_interpreter *it, const struct ta_trace_condition *guard) {
    if (it->conditions != NULL && it->condition_count < it->condition_capacity) {
        it->conditions[it->condition_count++] = *guard;
    }
}

/*
 * A selection completed: its guards, in evaluation order, each followed by
 * the error it raised, or, without guards to write, the events it queued
 * after `mark`.
 */
static void ta_trace_selection(ta_interpreter *it, uint32_t mark) {
    uint32_t count = it->condition_count;
    it->condition_count = 0;
    ta_count_raised(it, mark);
    if (it->conditions == NULL || !ta_tracing(it, TA_TRACE_CONDITION)) {
        ta_trace_raised_records(it, mark);
        return;
    }
    const ta_event_block *queue = it->actor->event_block;
    for (uint32_t index = 0; index < count; index += 1u) {
        const struct ta_trace_condition *guard = &it->conditions[index];
        ta_trace_record record = ta_trace_record_now(it, TA_TRACE_CONDITION);
        record.position = ta_trace_nowhere();
        record.position.at = TA_TRACE_AT_SELECT;
        record.position.transition = guard->transition;
        record.position.state = it->definition->transitions[guard->transition].source;
        record.state = record.position.state;
        record.expr = guard->expr;
        record.result = guard->result;
        if (guard->failed) {
            record.has_fault = true;
            record.fault = (ta_fault){ .detail = guard->detail, .code = guard->code };
        }
        ta_trace_emit(it, &record);
        if (guard->raised != UINT32_MAX && queue != NULL && guard->raised < queue->count &&
            ta_tracing(it, TA_TRACE_RAISED)) {
            ta_trace_record raised = ta_trace_record_now(it, TA_TRACE_RAISED);
            raised.position = record.position;
            raised.event = &queue->events[guard->raised].event;
            ta_trace_emit(it, &raised);
        }
    }
}

/* The most guards one selection can evaluate: each atomic state walks its
 * ancestors' transitions, so shared ancestors count once per atomic state. */
static uint32_t ta_trace_guard_capacity(const ta_definition *definition) {
    uint64_t total = 0;
    for (ta_state_id atomic = 0; atomic < definition->state_count; atomic += 1u) {
        if (!ta_definition_is_atomic(definition, atomic)) {
            continue;
        }
        for (ta_state_id state = atomic; state != TA_STATE_NONE; state = definition->states[state].parent) {
            ta_span32 transitions = definition->states[state].transitions;
            for (uint32_t index = 0; index < transitions.length; index += 1u) {
                total += definition->transitions[transitions.offset + index].condition != UINT32_MAX ? 1u : 0u;
            }
        }
    }
    return total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
}

/* ------------------------------------------------------------------------
 * Events and errors
 */

static ta_actor_sparse_state *ta_sparse(ta_system *system, ta_actor *actor) {
    if (actor->sparse == NULL) {
        actor->sparse = ta_mem_alloc(system->allocator, sizeof(ta_actor_sparse_state),
                                     _Alignof(ta_actor_sparse_state));
        if (actor->sparse == NULL) {
            return NULL;
        }
        *actor->sparse = (ta_actor_sparse_state){ .final_state = TA_STATE_NONE };
    }
    return actor->sparse;
}

/* ------------------------------------------------------------------------
 * Event blocks (ADR 0064, ADR 0069)
 */

/* The first capacity of a block's queue, which is also the largest a block
 * the system caches may have: the cache holds small blocks only. */
#define TA_EVENT_BLOCK_QUEUE_INITIAL 8u

TA_PRIVATE size_t ta_event_block_bytes(uint32_t capacity) {
    return offsetof(ta_event_block, events) + (size_t)capacity * sizeof(ta_queued_event);
}

static void ta_block_free(ta_system *system, ta_event_block *block) {
    ta_mem_free(system->allocator, block,
                &(ta_allocator_free_params){ .size = ta_event_block_bytes(block->capacity),
                                             .alignment = _Alignof(ta_event_block) });
}

/* An empty block: the newest cached one, else a new one with room for
 * `capacity` events. Null when there is no memory for one. */
static ta_event_block *ta_block_take(ta_system *system, uint32_t capacity) {
    ta_event_block *block = system->block_cache;
    if (block != NULL) {
        system->block_cache = block->next_cached;
        system->block_cache_count -= 1u;
    } else {
        block = ta_mem_alloc(system->allocator, ta_event_block_bytes(capacity), _Alignof(ta_event_block));
        if (block == NULL) {
            return NULL;
        }
        block->capacity = capacity;
    }
    block->count = 0;
    block->next_cached = NULL;
    block->slots = (ta_event_slots){0};
    return block;
}

TA_PRIVATE ta_result ta_interpreter_hold_block(ta_system *system, ta_actor *actor) {
    if (actor->event_block == NULL) {
        /* A delivered event needs no queue room: a new block is a header
         * only, which grows when the first internal event is queued. */
        actor->event_block = ta_block_take(system, 0);
        if (actor->event_block == NULL) {
            return TA_ERR_NOMEM;
        }
    }
    return TA_OK;
}

/*
 * Makes sure the actor holds a block with room for one more internal event:
 * its own, else one from the system's cache, else a new one; a full queue
 * doubles. TA_ERR_NOMEM leaves the actor as it was, apart from a block
 * taken from the cache, which it may keep.
 */
static ta_result ta_queue_room(ta_system *system, ta_actor *actor) {
    ta_event_block *block = actor->event_block;
    if (block == NULL) {
        block = ta_block_take(system, TA_EVENT_BLOCK_QUEUE_INITIAL);
        if (block == NULL) {
            return TA_ERR_NOMEM;
        }
        actor->event_block = block;
    }
    if (block->count < block->capacity) {
        return TA_OK;
    }
    if (block->capacity > UINT32_MAX / 2u) {
        return TA_ERR_NOMEM;
    }
    uint32_t capacity = block->capacity == 0 ? TA_EVENT_BLOCK_QUEUE_INITIAL : block->capacity * 2u;
    ta_event_block *grown = ta_mem_alloc(system->allocator, ta_event_block_bytes(capacity),
                                         _Alignof(ta_event_block));
    if (grown == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(grown, block, ta_event_block_bytes(block->count));
    grown->capacity = capacity;
    ta_block_free(system, block);
    actor->event_block = grown;
    return TA_OK;
}

/*
 * Gives an actor's block back when the actor no longer needs it: it holds
 * no event, no microstep is paused, and no running macrostep's trace parent
 * (ADR 0067). It goes to the system's cache, or is freed when the cache is
 * full or the block grew past the first capacity. Cannot fail.
 */
static void ta_block_settle(ta_system *system, ta_actor *actor) {
    ta_event_block *block = actor->event_block;
    if (block == NULL || block->count != 0 || block->slots.external != TA_EXTERNAL_NONE || block->slots.has_kept ||
        (actor->sparse != NULL && actor->sparse->paused != NULL) || actor->macrostep) {
        return;
    }
    actor->event_block = NULL;
    if (system->block_cache_count < system->block_cache_limit && block->capacity <= TA_EVENT_BLOCK_QUEUE_INITIAL) {
        block->next_cached = system->block_cache;
        system->block_cache = block;
        system->block_cache_count += 1u;
        return;
    }
    ta_block_free(system, block);
}

TA_PRIVATE void ta_interpreter_block_cache_trim(ta_system *system) {
    while (system->block_cache_count > system->block_cache_limit) {
        ta_event_block *block = system->block_cache;
        system->block_cache = block->next_cached;
        system->block_cache_count -= 1u;
        ta_block_free(system, block);
    }
}

TA_PRIVATE ta_result ta_interpreter_enqueue_internal(ta_system *system, ta_actor *actor, ta_event *event) {
    ta_actor_touch(actor);
    ta_result result = ta_queue_room(system, actor);
    if (result != TA_OK) {
        ta_block_settle(system, actor);
        ta_event_release(event);
        return result;
    }
    ta_event_block *queue = actor->event_block;
    queue->events[queue->count] = (ta_queued_event){
        .event = *event,
        .flags = TA_EVENT_FLAG_OWNS_DATA | (event->type == TA_EVENT_PLATFORM ? TA_EVENT_FLAG_GENERATED : 0u),
    };
    queue->count += 1u;
    return TA_OK;
}

/* Removes the queue's head; its event is the caller's now. */
static ta_event ta_queue_pop(ta_actor *actor) {
    ta_event_block *queue = actor->event_block;
    ta_event head = queue->events[0].event;
    for (uint32_t index = 1; index < queue->count; index += 1) {
        queue->events[index - 1u] = queue->events[index];
    }
    queue->count -= 1u;
    return head;
}

/* How many events the actor's queue holds, a taken head included. */
static uint32_t ta_queue_mark(const ta_actor *actor) {
    return actor->event_block == NULL ? 0 : actor->event_block->count;
}

/*
 * Takes back, and releases, the events queued after `mark`: a step that ran
 * out of memory after queueing some is done again, and queues them again
 * (ADR 0070). Only events of that step lie after the mark.
 */
static void ta_queue_truncate(ta_actor *actor, uint32_t mark) {
    ta_event_block *queue = actor->event_block;
    while (queue != NULL && queue->count > mark) {
        queue->count -= 1u;
        ta_event_release(&queue->events[queue->count].event);
    }
}

/* Makes `event`, and its references, the kept event (ADR 0069). */
static void ta_keep_event(ta_event_slots *slots, const ta_event *event) {
    if (slots->has_kept) {
        ta_event_release(&slots->kept_event);
    }
    slots->kept_event = *event;
    slots->has_kept = true;
}

/*
 * Binds `_event` to an event just taken (SCXML 5.10.1); the actor holds a
 * block, since it holds the event. Nothing moves but a taken queue head,
 * which leaves the queue for the kept slot, so that a retry can bind it
 * again.
 */
static void ta_bind_event(ta_actor *actor, ta_event_binding taken) {
    ta_event_slots *slots = &actor->event_block->slots;
    if (slots->bound == TA_BOUND_INTERNAL) {
        ta_event head = ta_queue_pop(actor);
        ta_keep_event(slots, &head);
        slots->bound = TA_BOUND_KEPT;
    }
    slots->bound_before = slots->bound;
    slots->bound = (uint8_t)taken;
    if (taken == TA_BOUND_EXTERNAL) {
        slots->external = TA_EXTERNAL_TAKEN;
    }
}

/*
 * Gives back the event of a microstep that could not start (ADR 0047): it
 * never left its slot or the queue, so only the marks go back.
 */
static void ta_unbind_event(ta_actor *actor) {
    ta_event_slots *slots = &actor->event_block->slots;
    if (slots->bound == TA_BOUND_EXTERNAL) {
        slots->external = TA_EXTERNAL_WAITING;
    }
    slots->bound = slots->bound_before;
}

/* Frees the operator transition a waiting external event came with. */
static void ta_jump_free(ta_system *system, ta_event_slots *slots) {
    if (slots->jump != NULL) {
        ta_mem_free(system->allocator, slots->jump,
                    &(ta_allocator_free_params){ .size = slots->jump->bytes,
                                                 .alignment = _Alignof(ta_operator_jump) });
        slots->jump = NULL;
    }
}

/* Notes, as its macrostep ends, whether the external event it took enabled
 * nothing, for ta_actor_resume_params.ignored (ADR 0089 section 8). */
static void ta_note_ignored(ta_interpreter *it) {
    ta_event_block *block = it->actor->event_block;
    it->ended_ignored = block != NULL && block->slots.external_ignored;
    if (block != NULL) {
        block->slots.timed = false;
    }
}

/*
 * The logical time of the actor's macrostep (ADR 0089 section 9): the one
 * its first resume took, so it does not move while a macrostep spans
 * several resumes; the system's time when no macrostep has one.
 */
TA_PRIVATE ta_time ta_interpreter_macrostep_time(const ta_system *system, const ta_actor *actor) {
    const ta_event_block *block = actor->event_block;
    if (actor->macrostep && block != NULL && block->slots.timed) {
        return block->slots.time;
    }
    return ta_actor_now(system, actor);
}

/* The first resume of a macrostep takes its time, in a block the actor then
 * holds until the macrostep ends (ta_block_settle). */
static ta_result ta_take_macrostep_time(ta_system *system, ta_actor *actor) {
    ta_result result = ta_interpreter_hold_block(system, actor);
    if (result == TA_OK && !actor->event_block->slots.timed) {
        actor->event_block->slots.time = ta_actor_now(system, actor);
        actor->event_block->slots.timed = true;
    }
    return result;
}

/* Releases the external event, and the operator transition it came with. */
static void ta_release_external(ta_system *system, ta_event_slots *slots) {
    if (slots->external != TA_EXTERNAL_NONE) {
        ta_event_release(&slots->external_event);
        slots->external = TA_EXTERNAL_NONE;
    }
    slots->external_ignored = false;
    ta_jump_free(system, slots);
}

/* Releases every event the actor holds: its queue, taken head included,
 * its kept event and its external event. */
static void ta_release_events(ta_system *system, ta_actor *actor) {
    ta_event_block *block = actor->event_block;
    if (block == NULL) {
        return;
    }
    while (block->count != 0) {
        ta_event event = ta_queue_pop(actor);
        ta_event_release(&event);
    }
    ta_event_slots *slots = &block->slots;
    if (slots->has_kept) {
        ta_event_release(&slots->kept_event);
        slots->has_kept = false;
    }
    ta_release_external(system, slots);
    slots->bound = TA_BOUND_NONE;
}

/*
 * A macrostep ended with no internal event waiting. An invoked session keeps
 * the event `_event` names, and so its block, since cancelling it runs its
 * <onexit> between its macrosteps (SCXML 6.4); any other session lets go of
 * every event, and its block goes back when the run returns.
 */
static void ta_end_macrostep_events(ta_system *system, ta_actor *actor) {
    ta_event_block *block = actor->event_block;
    if (block == NULL || actor->sparse == NULL || !actor->sparse->has_parent) {
        ta_release_events(system, actor);
        return;
    }
    ta_event_slots *slots = &block->slots;
    if (slots->bound == TA_BOUND_INTERNAL) {
        ta_event head = ta_queue_pop(actor);
        ta_keep_event(slots, &head);
        slots->bound = TA_BOUND_KEPT;
    } else if (slots->bound == TA_BOUND_EXTERNAL) {
        ta_keep_event(slots, &slots->external_event);
        slots->external = TA_EXTERNAL_NONE;
        slots->bound = TA_BOUND_KEPT;
    }
    ta_release_external(system, slots);
}

typedef struct {
    ta_event_name_id name;
    ta_send_id send_id;
    ta_fault *fault;
    /* A communication error that makes an idle actor run: what its
     * macrostep runs under, the failed send's parent. Null for none. */
    const ta_trace_parent *trace;
} ta_error_params;

/* Queues a platform error event; the fault's data becomes the event data. */
static ta_result ta_raise_error(ta_system *system, ta_actor *actor, const ta_error_params *params) {
    ta_event event = {
        .name = params->name,
        .type = TA_EVENT_PLATFORM,
        .send_id = params->send_id,
        .invoke_id = TA_INVOKE_ID_NONE,
    };
    if (params->fault != NULL) {
        event.data = params->fault->data;
        params->fault->data = (ta_value){0};
    }
    return ta_interpreter_enqueue_internal(system, actor, &event);
}

static ta_result ta_execution_error(ta_interpreter *it, ta_fault *fault) {
    ta_result result = ta_raise_error(it->system, it->actor,
                                      &(ta_error_params){ .name = TA_EVENT_NAME_ERROR_EXECUTION, .fault = fault });
    return result == TA_OK ? TA_EXEC_FAILED : result;
}

/*
 * What a failed step of evaluation is. A data-model or plugin operation
 * that fails is an execution error. One that ran out of memory and changed
 * nothing (TA_OP_NOMEM), like core's own allocation failing while it
 * interns a name or takes scratch, is TA_ERR_NOMEM: it stops the step with
 * nothing visible, never an error event (ADR 0047, ADR 0070).
 */
static ta_result ta_op_result(ta_op_status status) {
    return status == TA_OP_OK ? TA_OK : status == TA_OP_NOMEM ? TA_ERR_NOMEM : TA_EXEC_FAILED;
}

/* A failed operation: TA_ERR_NOMEM for TA_OP_NOMEM, which drops the fault,
 * otherwise error.execution with the fault's data (SCXML 5.10). */
static ta_result ta_op_failed(ta_interpreter *it, ta_op_status status, ta_fault *fault) {
    if (status == TA_OP_NOMEM) {
        ta_value_release(&fault->data);
        *fault = (ta_fault){0};
        return TA_ERR_NOMEM;
    }
    return ta_execution_error(it, fault);
}

static ta_result ta_core_result(ta_result result) {
    return result == TA_OK || result == TA_ERR_NOMEM ? result : TA_EXEC_FAILED;
}

typedef struct {
    ta_result result; /* the step's result */
    int32_t code;     /* the fault's code when core itself failed the step */
    const char *message;
} ta_core_fault_params;

/*
 * A step core failed itself, not a data model: give the fault a code and a
 * static message, so the error event and the ACTION_FAILED record say why
 * (ADR 0072's failure logs). A fault a data model already filled stays as it
 * is, and a result that is not TA_EXEC_FAILED passes through.
 */
static ta_result ta_core_fault(ta_fault *fault, const ta_core_fault_params *params) {
    if (params->result == TA_EXEC_FAILED && fault != NULL && fault->code == 0 && fault->message.len == 0) {
        fault->code = params->code;
        fault->message = ta_str_from_cstr(params->message);
    }
    return params->result;
}

/* ------------------------------------------------------------------------
 * Data-model calls
 */

typedef struct {
    ta_value_source source;
    ta_value *out;
    ta_fault *fault;
} ta_value_params;

static ta_op_status ta_eval_source(ta_interpreter *it, const ta_value_params *params) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    *params->out = (ta_value){0};
    switch (params->source.kind) {
        case TA_VALUE_SOURCE_NONE:
            return TA_OP_OK;
        case TA_VALUE_SOURCE_EXPR:
            if (v->eval_value == NULL) {
                return TA_OP_ERROR;
            }
            return v->eval_value(definition->datamodel.self,
                                 &(ta_datamodel_eval_value_params){
                                     .shared = definition->datamodel_shared,
                                     .instance = it->actor->datamodel_instance,
                                     .expr = params->source.as.expr,
                                     .host = &it->actor->host,
                                     .out = params->out,
                                     .fault = params->fault,
                                 });
        case TA_VALUE_SOURCE_LOCATION:
            if (v->read_location == NULL) {
                return TA_OP_ERROR;
            }
            return v->read_location(definition->datamodel.self,
                                    &(ta_datamodel_read_location_params){
                                        .shared = definition->datamodel_shared,
                                        .instance = it->actor->datamodel_instance,
                                        .location = params->source.as.location,
                                        .host = &it->actor->host,
                                        .out = params->out,
                                        .fault = params->fault,
                                    });
        case TA_VALUE_SOURCE_CONTENT:
            if (v->import_content == NULL) {
                return TA_OP_ERROR;
            }
            return v->import_content(definition->datamodel.self,
                                     &(ta_datamodel_import_content_params){
                                         .shared = definition->datamodel_shared,
                                         .instance = it->actor->datamodel_instance,
                                         .content = definition->contents[params->source.as.content].bytes,
                                         .host = &it->actor->host,
                                         .out = params->out,
                                         .fault = params->fault,
                                     });
        default:
            return TA_OP_ERROR;
    }
}

/*
 * Evaluates an expression and reads its value as text through value_text.
 * `*text` is borrowed from `*holder`, which the caller releases after use.
 */
typedef struct {
    ta_expr_id expr;
    ta_value *holder;
    ta_str *text;
    ta_fault *fault;
} ta_text_eval_params;

static ta_op_status ta_eval_text(ta_interpreter *it, const ta_text_eval_params *params) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    ta_op_status status = ta_eval_source(it, &(ta_value_params){
                                                 .source = { .kind = TA_VALUE_SOURCE_EXPR, .as.expr = params->expr },
                                                 .out = params->holder,
                                                 .fault = params->fault,
                                             });
    if (status != TA_OP_OK) {
        return status;
    }
    if (v->value_text == NULL) {
        return TA_OP_ERROR;
    }
    *params->text = (ta_str){0};
    status = v->value_text(definition->datamodel.self,
                           &(ta_datamodel_value_text_params){
                               .shared = definition->datamodel_shared,
                               .instance = it->actor->datamodel_instance,
                               .value = params->holder,
                               .host = &it->actor->host,
                               .out = params->text,
                               .fault = params->fault,
                           });
    return status == TA_OP_OK && params->text->ptr == NULL && params->text->len != 0 ? TA_OP_ERROR : status;
}

static ta_op_status ta_eval_condition(ta_interpreter *it, ta_expr_id expr, bool *out, ta_fault *fault) {
    const ta_definition *definition = it->definition;
    if (definition->datamodel.v->eval_condition == NULL) {
        return TA_OP_ERROR;
    }
    return definition->datamodel.v->eval_condition(definition->datamodel.self,
                                                   &(ta_datamodel_eval_condition_params){
                                                       .shared = definition->datamodel_shared,
                                                       .instance = it->actor->datamodel_instance,
                                                       .expr = expr,
                                                       .host = &it->actor->host,
                                                       .out = out,
                                                       .fault = fault,
                                                   });
}

/*
 * Initializes `owner`'s data from declaration `*cursor` on, and leaves
 * `*cursor` at the declaration whose error could not be queued when it
 * returns TA_ERR_NOMEM: that declaration's initializer failed, so running
 * it again changes nothing that ran before (ADR 0070).
 */
static ta_result ta_initialize_data(ta_interpreter *it, ta_state_id owner, uint32_t *cursor) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    for (; *cursor < definition->data_decl_count; *cursor += 1u) {
        uint32_t index = *cursor;
        const ta_data_decl *decl = &definition->data_decls[index];
        if (decl->owner_state != owner || decl->initializer == TA_INITIALIZER_NONE ||
            (it->at.phase == TA_PHASE_DATA && ta_bits_get(it->bound_decls, index))) {
            continue;
        }
        ta_fault fault = {0};
        ta_value value = {0};
        uint32_t mark = ta_queue_mark(it->actor);
        const ta_value_source *source = &definition->initializers[decl->initializer].value;
        ta_op_status status = ta_eval_source(it, &(ta_value_params){ .source = *source, .out = &value,
                                                                     .fault = &fault });
        if (status == TA_OP_OK) {
            status = v->set_named == NULL
                         ? TA_OP_ERROR
                         : v->set_named(definition->datamodel.self,
                                        &(ta_datamodel_set_named_params){
                                            .shared = definition->datamodel_shared,
                                            .instance = it->actor->datamodel_instance,
                                            .name = ta_atom_text(&it->system->atoms, decl->name),
                                            .value = &value,
                                            .host = &it->actor->host,
                                            .fault = &fault,
                                        });
        }
        ta_result result = TA_OK;
        if (status != TA_OP_OK) {
            /* The binding stays declared; the error does not stop other initializers. */
            result = ta_op_failed(it, status, &fault);
            if (result != TA_EXEC_FAILED) {
                ta_value_release(&value);
                return result;
            }
        }
        if (ta_tracing(it, TA_TRACE_DATA_INIT)) {
            ta_trace_record record = ta_trace_record_now(it, TA_TRACE_DATA_INIT);
            record.state = owner;
            record.name = ta_atom_text(&it->system->atoms, decl->name);
            record.expr = source->kind == TA_VALUE_SOURCE_EXPR ? source->as.expr : TA_EXPR_ID_NONE;
            if (result == TA_OK) {
                record.has_value = true;
                record.value = value;
            } else {
                ta_trace_fault(&record, &fault);
            }
            ta_trace_emit(it, &record);
        }
        ta_value_release(&value);
        ta_interpreter_trace_raised(it, mark);
    }
    return TA_OK;
}

/*
 * Declares every datum from declaration `*cursor` on, and records the ones
 * the instance's binding supplies. On TA_ERR_NOMEM `*cursor` names the
 * declaration whose error could not be queued; it failed, so declaring it
 * again repeats nothing (ADR 0070).
 */
static ta_result ta_declare_data(ta_interpreter *it, uint32_t *cursor) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    for (; *cursor < definition->data_decl_count && v->declare != NULL; *cursor += 1u) {
        uint32_t index = *cursor;
        ta_fault fault = {0};
        bool bound = false;
        ta_op_status status = v->declare(definition->datamodel.self,
                                         &(ta_datamodel_declare_params){
                                             .shared = definition->datamodel_shared,
                                             .instance = it->actor->datamodel_instance,
                                             .name = ta_atom_text(&it->system->atoms,
                                                                  definition->data_decls[index].name),
                                             .host = &it->actor->host,
                                             .fault = &fault,
                                             .bound = &bound,
                                         });
        if (bound) {
            ta_bits_put(it->bound_decls, index, true);
        }
        if (status != TA_OP_OK) {
            uint32_t mark = ta_queue_mark(it->actor);
            ta_result result = ta_op_failed(it, status, &fault);
            if (result != TA_EXEC_FAILED) {
                return result;
            }
            /* A declaration that failed: DATA_INIT with its fault. */
            if (ta_tracing(it, TA_TRACE_DATA_INIT)) {
                ta_trace_record record = ta_trace_record_now(it, TA_TRACE_DATA_INIT);
                record.state = definition->data_decls[index].owner_state;
                record.name = ta_atom_text(&it->system->atoms, definition->data_decls[index].name);
                ta_trace_fault(&record, &fault);
                ta_trace_emit(it, &record);
            }
            ta_interpreter_trace_raised(it, mark);
        }
    }
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Sends
 */

static ta_send_id ta_literal_send_id(ta_atom atom) {
    return (ta_send_id)atom + 1u;
}

/*
 * Send IDs as text. A literal ID is the author's text (its atom); a generated
 * ID is TA_GENERATED_SEND_ID_PREFIX followed by the actor-local counter. Both
 * forms round-trip, so idlocation, sendidexpr, and _event.sendid agree.
 */
#define TA_GENERATED_SEND_ID_PREFIX "#_send."

TA_PRIVATE void ta_send_id_write(const ta_system *system, ta_send_id id, ta_text_writer *writer) {
    if (id & TA_GENERATED_SEND_ID_BIT) {
        ta_text_put(writer, ta_str_from_cstr(TA_GENERATED_SEND_ID_PREFIX));
        ta_text_put_u64(writer, id & ~TA_GENERATED_SEND_ID_BIT);
    } else if (id != 0) {
        ta_text_put(writer, ta_atom_text(&system->atoms, (ta_atom)(id - 1u)));
    }
}

TA_PRIVATE ta_send_id ta_send_id_from_text(const ta_system *system, ta_str text) {
    ta_str prefix = ta_str_from_cstr(TA_GENERATED_SEND_ID_PREFIX);
    if (text.len > prefix.len && ta_str_equal((ta_str){ .ptr = text.ptr, .len = prefix.len }, prefix)) {
        uint64_t number = 0;
        bool digits = true;
        for (uint32_t index = prefix.len; index < text.len && digits; index += 1) {
            char digit = text.ptr[index];
            digits = digit >= '0' && digit <= '9' && number <= (UINT64_MAX >> 1) / 10u;
            number = number * 10u + (uint64_t)(digit - '0');
        }
        if (digits && number != 0 && (number & TA_GENERATED_SEND_ID_BIT) == 0) {
            return TA_GENERATED_SEND_ID_BIT | number;
        }
    }
    ta_atom atom = 0;
    return ta_atom_find(&system->atoms, text, &atom) == TA_OK ? ta_literal_send_id(atom) : 0;
}

TA_API ta_result ta_send_id_parse(const ta_system *system, ta_str text, ta_send_id *out) {
    if (system == NULL || out == NULL || text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_send_id id = ta_send_id_from_text(system, text);
    if (id == 0) {
        return TA_ERR_NOT_FOUND;
    }
    *out = id;
    return TA_OK;
}

TA_API ta_result ta_send_id_intern(ta_system *system, ta_str text, ta_send_id *out) {
    if (system == NULL || out == NULL || text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_send_id id = ta_send_id_from_text(system, text);
    if (id == 0) {
        ta_atom atom = 0;
        ta_result result = ta_atom_intern(&system->atoms, text, &atom);
        if (result != TA_OK) {
            return result;
        }
        id = ta_send_id_from_text(system, text);
    }
    if (id == 0) {
        return TA_ERR_INVALID;
    }
    *out = id;
    return TA_OK;
}

TA_API ta_result ta_send_id_format(const ta_system *system, const ta_send_id_format_params *params) {
    if (system == NULL || params == NULL || params->needed == NULL) {
        return TA_ERR_INVALID;
    }
    if (params->send_id == 0) {
        *params->needed = 0;
        return TA_ERR_NOT_FOUND;
    }
    ta_text_writer writer = { .buffer = params->buffer, .capacity = params->capacity };
    ta_send_id_write(system, params->send_id, &writer);
    return ta_text_finish(&writer, params->needed);
}

typedef struct {
    ta_actor_ref origin;
    ta_invoke_id invoke_id;
    ta_actor_ref destination;
    ta_event_name_id event;
    ta_send_id send_id;
    const ta_value *data;
    ta_fault *fault;
    ta_trace_parent trace;
    ta_time due;
} ta_submit_params;

static ta_external_event_submit_status ta_submit_external(ta_system *system, const ta_submit_params *params) {
    if (system->external_events.v == NULL) {
        return TA_EXTERNAL_EVENT_ERROR;
    }
    /* A session ID that is not a resident runtime ID resolves here (ADR 0066). */
    ta_actor_ref destination;
    bool routed_by_name = ta_session_route(system, &params->destination, &destination);
    ta_external_delivery delivery = {
        .destination = destination,
        .event = {
            .name = params->event,
            .type = TA_EVENT_EXTERNAL,
            .send_id = params->send_id,
            .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = params->origin },
            .invoke_id = params->invoke_id,
            .data = *params->data,
        },
        .trace = params->trace,
        .due = params->due,
    };
    ta_external_event_submit_status status =
        system->external_events.v->submit(system->external_events.self,
                                          &(ta_external_event_submit_params){
                                              .delivery = &delivery,
                                              .fault = params->fault,
                                          });
    if (routed_by_name) {
        ta_actor_ref_release(&destination);
    }
    return status;
}

TA_PRIVATE void ta_interpreter_raised_outside(ta_system *system, ta_actor *actor, ta_event_name_id name,
                                   const ta_trace_parent *parent) {
    ta_count_error(system, actor, name);
    ta_trace_context none = { 0 };
    const ta_trace_context *context = parent != NULL ? &parent->context : &none;
    if ((ta_trace_macrostep_kinds(&system->trace, context) & TA_TRACE_BIT(TA_TRACE_RAISED)) == 0) {
        return;
    }
    const ta_event_block *queue = actor->event_block;
    if (queue == NULL || queue->count == 0) {
        return;
    }
    ta_trace_record record = ta_trace_record_blank(TA_TRACE_RAISED);
    record.key = (ta_step_key){ .session = actor->session_id, .family = actor->definition->family };
    record.actor = actor->record.id;
    record.definition = actor->definition->id;
    record.now = ta_actor_now(system, actor);
    record.event = &queue->events[queue->count - 1u].event;
    if (parent != NULL) {
        record.context = parent->context;
        record.cause = parent->cause;
    }
    ta_trace_write(&system->trace, &record);
}

static ta_result ta_communication_error(ta_system *system, ta_actor *actor, const ta_error_params *params) {
    if (actor->finished) {
        /* A send submitted before termination failed; the session can no longer observe it. */
        if (params->fault != NULL) {
            ta_value_release(&params->fault->data);
            params->fault->data = (ta_value){0};
        }
        return TA_OK;
    }
    ta_error_params error = *params;
    error.name = TA_EVENT_NAME_ERROR_COMMUNICATION;
    ta_result result = ta_raise_error(system, actor, &error);
    if (result == TA_OK && params->trace != NULL) {
        /* Queued outside a run, by a delayed send that failed: no unit
         * commits it, so it is counted and recorded here (ADR 0072). */
        ta_interpreter_raised_outside(system, actor, TA_EVENT_NAME_ERROR_COMMUNICATION, params->trace);
    }
    if (result == TA_OK) {
        /* An asynchronous failure makes an idle actor runnable again. */
        ta_interpreter_start_macrostep(actor, params->trace);
    }
    return result;
}

TA_PRIVATE void ta_interpreter_start_macrostep(ta_actor *actor, const ta_trace_parent *trace) {
    if (actor->macrostep) {
        return;
    }
    actor->macrostep = true;
    actor->record.residency = TA_ACTOR_RUNNING;
    if (actor->event_block != NULL) {
        actor->event_block->slots.trace = trace != NULL ? *trace : (ta_trace_parent){0};
    }
}

TA_PRIVATE void ta_interpreter_literal_content(const ta_definition *definition, ta_content_id content, ta_str *media_type,
                                    ta_bytes *bytes) {
    static const uint8_t empty[1] = { 0 };
    const ta_content_def *def = &definition->contents[content];
    *media_type = def->media_type;
    *bytes = def->bytes.ptr != NULL ? def->bytes : (ta_bytes){ .ptr = empty, .len = 0 };
}

TA_PRIVATE ta_external_event_submit_status ta_interpreter_dispatch_delayed(ta_system *system, ta_delayed_send *send, bool in_run) {
    ta_actor *sender = ta_actor_lookup(system, send->owner);
    ta_fault fault = {0};
    ta_external_event_submit_status status = TA_EXTERNAL_EVENT_ERROR;
    /*
     * A submitted send was already sent: it is logically in the receiver's
     * queue and keeps its envelope even when the sending session is gone.
     */
    ta_actor_ref origin = send->submitted || sender == NULL ? send->origin : sender->record.self;
    /* Invoke: events from an invoked child to its parent carry the invoke ID. */
    ta_invoke_id invoke_id = send->submitted || sender == NULL
                                 ? send->invoke_id
                                 : ta_invoke_parent_id(sender, &send->destination);
    if (sender == NULL && !send->submitted) {
        status = TA_EXTERNAL_EVENT_ACCEPTED; /* the sender's session is gone; drop the send */
    } else if (!send->submitted && !ta_invoke_parent_accepts(system, sender, &send->destination)) {
        /* The parent already cancelled this invoked child. */
        status = TA_EXTERNAL_EVENT_ACCEPTED;
    } else if (ta_system_is_scxml_io(system, send->io_type)) {
        status = ta_submit_external(system, &(ta_submit_params){
                                                .origin = origin,
                                                .invoke_id = invoke_id,
                                                .destination = send->destination,
                                                .event = send->event,
                                                .send_id = send->send_id,
                                                .data = &send->data,
                                                .fault = &fault,
                                                .trace = send->trace,
                                                .due = send->due,
                                            });
    } else {
        const ta_io *io = ta_system_io_of(system, send->io_type);
        ta_send request = {
            .source = origin,
            .send_id = send->send_id,
            .event = send->event,
            .io_type = send->io_type,
            .target = send->target,
            .data = send->data,
            .trace = send->trace,
            .due = send->due,
        };
        /* A send to a host processor is dispatched while its sender lives,
         * so the content resolves against the sender's definition. */
        if (send->has_content && sender != NULL) {
            ta_interpreter_literal_content(sender->definition, send->content, &request.content_type,
                                           &request.content);
        }
        if (send->has_target_text && sender != NULL) {
            ta_str media_type = { 0 };
            ta_bytes text = { 0 };
            ta_interpreter_literal_content(sender->definition, send->target_text, &media_type, &text);
            request.target_text = (ta_str){ .ptr = (const char *)text.ptr, .len = text.len };
        }
        ta_op_status sent = io == NULL ? TA_OP_ERROR : io->v->send(io->self, &request, &fault);
        /* A processor that ran out of memory accepted nothing: the send
         * waits, due now, like one a router answered RETRY for (ADR 0070). */
        status = sent == TA_OP_OK      ? TA_EXTERNAL_EVENT_ACCEPTED
                 : sent == TA_OP_NOMEM ? TA_EXTERNAL_EVENT_RETRY
                                       : TA_EXTERNAL_EVENT_ERROR;
    }
    if (status == TA_EXTERNAL_EVENT_RETRY) {
        /*
         * The router accepted nothing (ADR 0040). The send is due, so it is
         * sent: it waits to be offered again with the envelope it has now,
         * <cancel> no longer reaches it, and it outlives its sender
         * (ADR 0054 D3, D5). The caller keeps it in the deadline index.
         */
        if (!send->submitted) {
            /* The stored send now owns its envelope's origin (ADR 0065). */
            ta_actor_ref_retain(&origin);
            ta_actor_ref_release(&send->origin);
            send->origin = origin;
            send->submitted = true;
            send->invoke_id = invoke_id;
        }
        ta_value_release(&fault.data);
        return status;
    }
    if (status == TA_EXTERNAL_EVENT_ERROR && sender != NULL) {
        (void)ta_communication_error(system, sender,
                                     &(ta_error_params){ .send_id = send->send_id, .fault = &fault,
                                                         .trace = in_run ? NULL : &send->trace });
    }
    ta_value_release(&fault.data);
    ta_value_release(&send->target);
    ta_value_release(&send->data);
    ta_actor_ref_release(&send->destination);
    ta_actor_ref_release(&send->origin);
    return status;
}

/*
 * A <send> being evaluated. `event` and `delay` are its evaluated name and
 * delay; the values are owned until the send is committed.
 */
typedef struct {
    ta_value data;
    ta_value target;
    ta_scxml_target scxml;
    ta_atom io_type;
    ta_send_id send_id;
    ta_event_name_id event;
    ta_time delay;
    bool scxml_io;
    bool has_content;
    bool generated_id;
    ta_content_id content;
    /* A custom processor's literal target (ta_send.target_text). */
    bool has_target_text;
    ta_content_id target_text;
    /* The send left the actor: dispatched, stored to fire later, or held
     * for an unpublished actor's activation (ADR 0067's SENT). */
    bool sent;
} ta_send_state;

static void ta_send_state_release(ta_send_state *state) {
    ta_value_release(&state->data);
    ta_value_release(&state->target);
    ta_scxml_target_release(&state->scxml);
}

/*
 * A content-sized temporary that does not fit the render area (ADR 0092
 * section 3): an execution error that names the parameter, and fails the
 * same way for the same content, where the arena used to grow.
 */
TA_PRIVATE void ta_interpreter_render_fault(ta_fault *fault) {
    static const char text[] = "the render area is full (renderBytes)";
    *fault = (ta_fault){ .code = TA_ERR_NOMEM, .message = { .ptr = text, .len = (uint32_t)(sizeof text - 1u) } };
}

static ta_result ta_send_payload(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                 ta_fault *fault) {
    if (!state->scxml_io && send->data.kind == TA_VALUE_SOURCE_CONTENT) {
        /*
         * A host I/O processor receives literal content as bytes: its
         * interpretation is platform-specific (SCXML 6.4.2), and only the
         * built-in SCXML processor, whose receiver reads _event.data, imports
         * it (ADR 0053).
         */
        state->has_content = true;
        state->content = send->data.as.content;
        return TA_OK;
    }
    if (send->params.length == 0) {
        return ta_op_result(
            ta_eval_source(it, &(ta_value_params){ .source = send->data, .out = &state->data, .fault = fault }));
    }
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->make_params == NULL) {
        return TA_EXEC_FAILED;
    }
    ta_named_value *values = ta_arena_alloc(&it->system->render, send->params.length * sizeof(ta_named_value),
                                            _Alignof(ta_named_value));
    if (values == NULL) {
        ta_interpreter_render_fault(fault);
        return TA_EXEC_FAILED;
    }
    uint32_t evaluated = 0;
    ta_op_status status = TA_OP_OK;
    for (; evaluated < send->params.length && status == TA_OP_OK; evaluated += 1) {
        const ta_param_def *param = &definition->params[send->params.offset + evaluated];
        values[evaluated].name = ta_atom_text(&it->system->atoms, param->name);
        status = ta_eval_source(it, &(ta_value_params){ .source = param->value, .out = &values[evaluated].value,
                                                        .fault = fault });
    }
    if (status == TA_OP_OK) {
        status = v->make_params(definition->datamodel.self,
                                &(ta_datamodel_make_params_params){
                                    .shared = definition->datamodel_shared,
                                    .instance = it->actor->datamodel_instance,
                                    .values = values,
                                    .count = send->params.length,
                                    .host = &it->actor->host,
                                    .out = &state->data,
                                    .fault = fault,
                                });
    }
    for (uint32_t index = 0; index < evaluated; index += 1) {
        ta_value_release(&values[index].value);
    }
    return ta_op_result(status);
}

static ta_result ta_send_target(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                ta_fault *fault) {
    if (!state->scxml_io) {
        /*
         * A custom I/O processor receives a literal target as the text the
         * chart wrote, without the data model, and a targetexpr as the value
         * it evaluates to, not as a URI.
         */
        if (send->target.kind == TA_VALUE_SOURCE_CONTENT) {
            state->has_target_text = true;
            state->target_text = send->target.as.content;
            return TA_OK;
        }
        return send->target.kind == TA_VALUE_SOURCE_NONE
                   ? TA_OK
                   : ta_op_result(ta_eval_source(it, &(ta_value_params){ .source = send->target,
                                                                         .out = &state->target, .fault = fault }));
    }
    if (send->target.kind == TA_VALUE_SOURCE_NONE) {
        state->scxml = (ta_scxml_target){ .kind = TA_SCXML_TARGET_SELF };
        return TA_OK;
    }
    /*
     * One grammar for both spellings (ADR 0044): a literal target is the
     * definition's text, and an evaluated one is the text of its value. A
     * named address borrows that text, which the send record keeps alive by
     * retaining the value.
     */
    ta_str text;
    if (send->target.kind == TA_VALUE_SOURCE_CONTENT) {
        ta_bytes content = it->definition->contents[send->target.as.content].bytes;
        text = (ta_str){ .ptr = (const char *)content.ptr, .len = content.len };
    } else {
        const ta_definition *definition = it->definition;
        const ta_datamodel_vtable *v = definition->datamodel.v;
        ta_op_status status = ta_eval_source(it, &(ta_value_params){ .source = send->target, .out = &state->target,
                                                                     .fault = fault });
        if (status != TA_OP_OK) {
            return ta_op_result(status);
        }
        if (v->value_text == NULL) {
            return ta_core_fault(fault, &(ta_core_fault_params){
                                            .result = TA_EXEC_FAILED, .code = TA_ERR_UNSUPPORTED,
                                            .message = "the data model cannot turn the <send> target into text" });
        }
        status = v->value_text(definition->datamodel.self,
                               &(ta_datamodel_value_text_params){
                                   .shared = definition->datamodel_shared,
                                   .instance = it->actor->datamodel_instance,
                                   .value = &state->target,
                                   .host = &it->actor->host,
                                   .out = &text,
                                   .fault = fault,
                               });
        if (status != TA_OP_OK) {
            return ta_op_result(status);
        }
    }
    /* Interning a named address may run out of memory. */
    return ta_core_fault(fault, &(ta_core_fault_params){
                                    .result = ta_core_result(ta_scxml_target_parse(it->system, text, &state->scxml)),
                                    .code = TA_ERR_INVALID,
                                    .message = "the <send> target is not a target SCXML can send to" });
}

/* Resolves an SCXML target to an external destination; false means inaccessible. */
static bool ta_send_destination(ta_interpreter *it, const ta_scxml_target *target, ta_actor_ref *out) {
    switch (target->kind) {
        case TA_SCXML_TARGET_SELF:
            /*
             * A send to self names *this* session by its session ID (ADR
             * 0066), which a reload does not change. While the session is
             * resident and never reloaded it is also the runtime ID, so it
             * needs no directory; otherwise submission resolves it through
             * the session table (ta_session_route). What a receiver replies
             * to is the origin, which for a named actor is the address.
             */
            *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME,
                                   .family = it->definition->family,
                                   .as.runtime = it->actor->session_id };
            return true;
        case TA_SCXML_TARGET_PARENT:
            if (it->actor->sparse == NULL || !it->actor->sparse->has_parent) {
                return false;
            }
            *out = ta_actor_runtime_ref(it->system, it->actor->sparse->parent.actor);
            return true;
        case TA_SCXML_TARGET_ACTOR:
            *out = target->as.actor;
            return true;
        case TA_SCXML_TARGET_INVOKE:
            if (!ta_invoke_destination(it->actor, target->as.invoke, out)) {
                return false;
            }
            out->family = ta_actor_family_of(it->system, out->as.runtime);
            return true;
        default:
            return false;
    }
}

/*
 * A replaying actor's invocations run nothing, so `#_<invokeid>` names no
 * child (ADR 0089 section 3). While the invocation is active the target is
 * still one the run being replayed could reach: it resolves to no session,
 * which the held send carries until it is published or dropped.
 */
static bool ta_send_replayed_invoke(ta_interpreter *it, const ta_scxml_target *target, ta_actor_ref *out) {
    ta_actor_ref none = { .kind = TA_ACTOR_REF_RUNTIME };
    if (target->kind != TA_SCXML_TARGET_INVOKE || !ta_invoke_is_active(it->actor, target->as.invoke)) {
        return false;
    }
    *out = none;
    return true;
}

/* Resolves `type` / `typeexpr`; an unknown processor is an execution error. */
static ta_result ta_send_io_type(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                 ta_fault *fault) {
    ta_system *system = it->system;
    ta_atom type = system->scxml_io_type;
    if (send->type.kind == TA_TEXT_SOURCE_ATOM) {
        type = send->type.as.atom;
    } else if (send->type.kind == TA_TEXT_SOURCE_EXPR) {
        ta_value holder = {0};
        ta_str text = {0};
        ta_result result = ta_op_result(ta_eval_text(it, &(ta_text_eval_params){
                                                             .expr = send->type.as.expr, .holder = &holder,
                                                             .text = &text, .fault = fault,
                                                         }));
        if (result == TA_OK) {
            result = text.len == 0 ? ta_core_fault(fault, &(ta_core_fault_params){
                                                              .result = TA_EXEC_FAILED, .code = TA_ERR_INVALID,
                                                              .message = "the <send> typeexpr gave empty text" })
                                   : ta_core_result(ta_atom_intern(&system->atoms, text, &type));
        }
        ta_value_release(&holder);
        if (result != TA_OK) {
            return result;
        }
    }
    state->scxml_io = ta_system_is_scxml_io(system, type);
    state->io_type = state->scxml_io ? system->scxml_io_type : type;
    return state->scxml_io || ta_system_io_of(system, type) != NULL
               ? TA_OK
               : ta_core_fault(fault, &(ta_core_fault_params){
                                          .result = TA_EXEC_FAILED, .code = TA_ERR_NOT_FOUND,
                                          .message = "the <send> type names no I/O processor this system has" });
}

/* Resolves `event` / `eventexpr`; the expression's text is a dotted event name. */
static ta_result ta_send_event_name(ta_interpreter *it, const ta_send_action *send, ta_event_name_id *out,
                                    ta_fault *fault) {
    if (send->event.kind == TA_EVENT_SOURCE_NAME) {
        *out = send->event.as.name;
        return TA_OK;
    }
    ta_value holder = {0};
    ta_str text = {0};
    ta_result result = ta_op_result(ta_eval_text(it, &(ta_text_eval_params){
                                                         .expr = send->event.as.expr, .holder = &holder,
                                                         .text = &text, .fault = fault,
                                                     }));
    if (result == TA_OK) {
        result = ta_core_fault(fault, &(ta_core_fault_params){
                                          .result = ta_core_result(ta_event_name_intern_dotted(&it->system->event_names,
                                                                                               text, out)),
                                          .code = TA_ERR_INVALID,
                                          .message = "the <send> eventexpr is not a dotted event name" });
    }
    ta_value_release(&holder);
    return result;
}

/* Resolves `delay` / `delayexpr`; the expression's text is an SCXML duration. */
static ta_result ta_send_delay(ta_interpreter *it, const ta_send_action *send, ta_time *out, ta_fault *fault) {
    *out = 0;
    if (send->delay.kind == TA_DELAY_SOURCE_LITERAL) {
        *out = send->delay.as.literal;
        return TA_OK;
    }
    if (send->delay.kind != TA_DELAY_SOURCE_EXPR) {
        return TA_OK;
    }
    ta_value holder = {0};
    ta_str text = {0};
    ta_op_status status = ta_eval_text(it, &(ta_text_eval_params){
                                               .expr = send->delay.as.expr, .holder = &holder,
                                               .text = &text, .fault = fault,
                                           });
    bool unparsed = status == TA_OP_OK && ta_duration_parse(text, out) != TA_OK;
    ta_value_release(&holder);
    if (unparsed) {
        return ta_core_fault(fault, &(ta_core_fault_params){
                                        .result = TA_EXEC_FAILED, .code = TA_ERR_INVALID,
                                        .message = "the <send> delayexpr is not an SCXML duration" });
    }
    return ta_op_result(status);
}

/* Stores generated text, such as an ID, through a location (`idlocation`). */
typedef struct {
    ta_expr_id location;
    ta_str text;
    ta_fault *fault;
} ta_store_text_params;

static ta_op_status ta_store_text(ta_interpreter *it, const ta_store_text_params *params) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->text_value == NULL || v->assign == NULL) {
        return TA_OP_ERROR;
    }
    ta_value value = {0};
    ta_op_status status = v->text_value(definition->datamodel.self,
                                        &(ta_datamodel_text_value_params){
                                            .shared = definition->datamodel_shared,
                                            .instance = it->actor->datamodel_instance,
                                            .text = params->text,
                                            .host = &it->actor->host,
                                            .out = &value,
                                            .fault = params->fault,
                                        });
    if (status == TA_OP_OK) {
        status = v->assign(definition->datamodel.self,
                           &(ta_datamodel_assign_params){
                               .shared = definition->datamodel_shared,
                               .instance = it->actor->datamodel_instance,
                               .location = params->location,
                               .value = &value,
                               .host = &it->actor->host,
                               .fault = params->fault,
                           });
    }
    ta_value_release(&value);
    return status;
}

/*
 * Assigns the send ID. Without `id` or `idlocation` the send has no ID, so
 * its events carry a blank sendid (SCXML 5.10.1). A generated ID is the
 * next value of the actor's counter, which moves only once the send is done
 * (ta_exec_send), so a send that runs out of memory and runs again stores
 * the same ID through idlocation (ADR 0070).
 */
static ta_result ta_send_assign_id(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                   ta_fault *fault) {
    state->send_id = 0;
    if (send->id.kind == TA_ID_SOURCE_LITERAL) {
        state->send_id = ta_literal_send_id(send->id.as.literal);
        return TA_OK;
    }
    if (send->id.kind != TA_ID_SOURCE_LOCATION) {
        return TA_OK;
    }
    state->generated_id = true;
    state->send_id = TA_GENERATED_SEND_ID_BIT | (it->actor->next_send_id + 1u);
    char buffer[48];
    ta_text_writer writer = { .buffer = buffer, .capacity = sizeof(buffer) };
    ta_send_id_write(it->system, state->send_id, &writer);
    return ta_op_result(ta_store_text(it, &(ta_store_text_params){
                                              .location = send->id.as.location,
                                              .text = { .ptr = buffer, .len = writer.length },
                                              .fault = fault,
                                          }));
}

/* An error while evaluating a <send> carries that send's ID (SCXML 5.10.1). */
static ta_result ta_send_execution_error(ta_interpreter *it, ta_send_id send_id, ta_fault *fault) {
    ta_result result = ta_raise_error(it->system, it->actor,
                                      &(ta_error_params){ .name = TA_EVENT_NAME_ERROR_EXECUTION,
                                                          .send_id = send_id, .fault = fault });
    return result == TA_OK ? TA_EXEC_FAILED : result;
}

/* Evaluates every attribute of a <send>, in the order SCXML 6.2 gives them. */
static ta_result ta_send_evaluate(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                  ta_fault *fault) {
    ta_result result = ta_send_assign_id(it, send, state, fault);
    if (result == TA_OK) {
        result = ta_send_event_name(it, send, &state->event, fault);
    }
    if (result == TA_OK) {
        result = ta_send_io_type(it, send, state, fault);
    }
    if (result == TA_OK) {
        result = ta_send_delay(it, send, &state->delay, fault);
    }
    if (result == TA_OK) {
        result = ta_send_payload(it, send, state, fault);
    }
    if (result == TA_OK) {
        result = ta_send_target(it, send, state, fault);
    }
    if (result == TA_OK && state->scxml_io && state->scxml.kind == TA_SCXML_TARGET_INTERNAL && state->delay != 0) {
        /* A delayed send cannot target the internal queue. */
        result = ta_core_fault(fault, &(ta_core_fault_params){
                                          .result = TA_EXEC_FAILED, .code = TA_ERR_UNSUPPORTED,
                                          .message = "a <send> with a delay cannot target #_internal" });
    }
    if (result == TA_OK && !(state->scxml_io && state->scxml.kind == TA_SCXML_TARGET_INTERNAL)) {
        /*
         * The data leaves the session (ADR 0097, SCXML C.1), to another
         * session, an I/O processor or this session's external queue: what
         * the receiver gets is a copy made now, when the <send> runs (SCXML
         * 6.2), which nothing this session does later can change. An event
         * for #_internal stays in the session and is not copied.
         */
        result = ta_core_fault(fault, &(ta_core_fault_params){
                                          .result = ta_op_result(ta_interpreter_copy_out(it, &state->data, fault)),
                                          .code = TA_ERR_UNSUPPORTED,
                                          .message = "the data model cannot copy the <send> data" });
    }
    return result;
}

/*
 * Queues, dispatches, holds or stores an evaluated <send>, which owns the
 * state's values from here. It runs out of memory only where nothing is
 * visible yet: holding it for an unpublished actor, or keeping one the
 * router refused with RETRY, which accepted nothing (ADR 0040).
 */
static ta_result ta_send_commit(ta_interpreter *it, ta_send_state *state) {
    ta_system *system = it->system;
    ta_actor *actor = it->actor;
    if (state->scxml_io && state->scxml.kind == TA_SCXML_TARGET_INTERNAL) {
        ta_value_release(&state->target);
        ta_event event = {
            .name = state->event,
            .type = TA_EVENT_INTERNAL,
            .send_id = state->send_id,
            .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = actor->record.self },
            .invoke_id = TA_INVOKE_ID_NONE,
            .data = state->data,
        };
        /* The queued event owns its origin reference (ADR 0065). */
        ta_actor_ref_retain(&event.origin.as.actor);
        return ta_interpreter_enqueue_internal(system, actor, &event);
    }
    bool replaying = ta_actor_replaying(actor);
    if (replaying && state->delay == 0) {
        /* A replay crosses no border (ADR 0089 section 3): the run being
         * replayed sent this, so it counts as sent, and goes nowhere. */
        ta_send_state_release(state);
        state->sent = true;
        return TA_OK;
    }

    ta_delayed_send record = {
        .owner = actor->record.id,
        .send_id = state->send_id,
        .due = ta_actor_now(system, actor) + state->delay,
        .event = state->event,
        .io_type = state->io_type,
        .target = state->target,
        .data = state->data,
        .has_content = state->has_content,
        .content = state->content,
        .has_target_text = state->has_target_text,
        .target_text = state->target_text,
        .trace = ta_interpreter_trace_here(it),
    };
    /* A named target's reference passes to the send record (ADR 0065). */
    if (state->scxml_io && !ta_send_destination(it, &state->scxml, &record.destination) &&
        !(replaying && ta_send_replayed_invoke(it, &state->scxml, &record.destination))) {
        ta_send_state_release(state);
        return ta_communication_error(system, actor, &(ta_error_params){ .send_id = state->send_id });
    }
    if (!actor->published) {
        ta_result result = ta_actor_hold_send(
            system, actor, &(ta_activation_send){ .send = record, .immediate = state->delay == 0 });
        if (result != TA_OK) {
            ta_value_release(&record.data);
            ta_value_release(&record.target);
            ta_actor_ref_release(&record.destination);
        }
        state->sent = result == TA_OK;
        return result;
    }
    if (state->delay != 0 && actor->sparse != NULL &&
        actor->sparse->timers.count >= system->params[TA_PARAM_MAX_PENDING_SENDS]) {
        /* A quota (ADR 0092 section 4): the send is not made, and the
         * sender hears it as an execution error. */
        ta_value_release(&record.data);
        ta_value_release(&record.target);
        ta_actor_ref_release(&record.destination);
        system->counters.quota_refusals += 1u;
        static const char text[] = "too many pending delayed sends (maxPendingSends)";
        ta_fault fault = { .code = TA_ERR_NOMEM, .message = { .ptr = text, .len = (uint32_t)(sizeof text - 1u) } };
        ta_result raised = ta_execution_error(it, &fault);
        state->sent = false;
        return raised == TA_EXEC_FAILED ? TA_OK : raised;
    }
    if (state->delay == 0) {
        ta_external_event_submit_status submitted = ta_interpreter_dispatch_delayed(system, &record, true);
        if (submitted != TA_EXTERNAL_EVENT_RETRY) {
            state->sent = submitted == TA_EXTERNAL_EVENT_ACCEPTED;
            return TA_OK;
        }
        /*
         * The router accepted nothing, typically because the destination is
         * this busy actor. The send, now marked submitted, waits in the
         * deadline index, due now, and keeps its order among later sends.
         */
    }
    ta_result result = ta_actor_schedule_send(system, actor, &record);
    if (result != TA_OK) {
        ta_value_release(&record.data);
        ta_value_release(&record.target);
        ta_actor_ref_release(&record.destination);
        ta_actor_ref_release(&record.origin);
    }
    state->sent = result == TA_OK;
    return result;
}

/*
 * The SENT record of a committed send: its ID, and the event as it left,
 * whose data the record holds a reference to, since committing gave the
 * send's own away (ADR 0067).
 */
static void ta_trace_sent(ta_interpreter *it, const ta_send_state *state, const ta_value *data) {
    ta_event event = {
        .name = state->event,
        .type = TA_EVENT_EXTERNAL,
        .send_id = state->send_id,
        .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = it->actor->record.self },
        .invoke_id = TA_INVOKE_ID_NONE,
        .data = *data,
    };
    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_SENT);
    record.send = state->send_id;
    record.event = &event;
    ta_trace_emit(it, &record);
}

/*
 * <send> in two parts (ADR 0070): evaluate every attribute, which may run
 * out of memory interning a name or taking scratch, then commit. A send
 * that runs out of memory is run again from the start: what it did before
 * is evaluation, and storing the same generated ID again, so the actor's
 * counter moves here, once the send has an outcome.
 */
static ta_result ta_exec_send(ta_interpreter *it, const ta_send_action *send) {
    ta_send_state state = { .io_type = it->system->scxml_io_type, .scxml_io = true };
    ta_fault fault = {0};
    ta_result result = ta_send_evaluate(it, send, &state, &fault);
    if (result == TA_OK) {
        /* Committing gives the data away; SENT reports it afterwards. */
        bool traced = ta_tracing(it, TA_TRACE_SENT);
        ta_value data = state.data;
        if (traced) {
            ta_value_retain(&data);
        }
        result = ta_send_commit(it, &state);
        if (result == TA_OK) {
            TA_COUNT(it->system, it->actor->definition, sends);
            if (state.delay != 0) {
                TA_COUNT(it->system, it->actor->definition, sends_delayed);
            }
        }
        if (traced && result == TA_OK && state.sent) {
            ta_trace_sent(it, &state, &data);
        }
        if (traced) {
            ta_value_release(&data);
        }
    } else {
        ta_send_state_release(&state);
        if (result == TA_EXEC_FAILED) {
            result = ta_send_execution_error(it, state.send_id, &fault);
            if (result == TA_EXEC_FAILED) {
                ta_trace_action_failed(it, &fault);
            }
        } else {
            ta_value_release(&fault.data);
        }
    }
    if (result != TA_ERR_NOMEM && state.generated_id) {
        it->actor->next_send_id += 1u;
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Executable content
 */

/*
 * <foreach> iterates a shallow copy made by the data model. A failed
 * snapshot or binding, or an error in the body, stops the foreach and the
 * enclosing block (SCXML 4.6). The loop is a frame of the executor (see
 * ta_run_block): `actions` is the body, `cursor` the <foreach> action
 * itself, and the snapshot and position are the frame's.
 */
static ta_result ta_foreach_begin(ta_interpreter *it, ta_exec_frame *loop) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->foreach_snapshot == NULL || v->foreach_bind == NULL) {
        return ta_execution_error(it, NULL);
    }
    ta_fault fault = {0};
    ta_op_status snapshot = v->foreach_snapshot(definition->datamodel.self,
                            &(ta_datamodel_foreach_snapshot_params){
                                .shared = definition->datamodel_shared,
                                .instance = it->actor->datamodel_instance,
                                .array = definition->action_defs[loop->cursor].as.foreach.array,
                                .host = &it->actor->host,
                                .out = &loop->iteration_snapshot,
                                .count = &loop->iteration_count,
                                .fault = &fault,
                            });
    if (snapshot == TA_OP_OK) {
        return TA_OK;
    }
    ta_result result = ta_op_failed(it, snapshot, &fault);
    if (result == TA_EXEC_FAILED) {
        ta_trace_action_failed(it, &fault);
    }
    return result;
}

/* Binds the item and index for the loop's next position. */
static ta_result ta_foreach_bind(ta_interpreter *it, ta_exec_frame *loop) {
    const ta_definition *definition = it->definition;
    const ta_foreach_action *foreach = &definition->action_defs[loop->cursor].as.foreach;
    ta_str item = ta_atom_text(&it->system->atoms, foreach->item);
    ta_str index = foreach->has_index ? ta_atom_text(&it->system->atoms, foreach->index) : (ta_str){0};
    ta_fault fault = {0};
    ta_op_status bound = definition->datamodel.v->foreach_bind(definition->datamodel.self,
                                                               &(ta_datamodel_foreach_bind_params){
                                                                   .shared = definition->datamodel_shared,
                                                                   .instance = it->actor->datamodel_instance,
                                                                   .snapshot = &loop->iteration_snapshot,
                                                                   .index = loop->iteration,
                                                                   .item = item,
                                                                   .index_name = index,
                                                                   .host = &it->actor->host,
                                                                   .fault = &fault,
                                                               });
    ta_result result = bound == TA_OP_OK ? TA_OK : ta_op_failed(it, bound, &fault);
    if (result == TA_OK && ta_tracing(it, TA_TRACE_FOREACH)) {
        ta_trace_record record = ta_trace_record_now(it, TA_TRACE_FOREACH);
        record.iteration = loop->iteration;
        record.position.iteration = loop->iteration;
        record.has_value = true;
        record.value = loop->iteration_snapshot;
        ta_trace_emit(it, &record);
    } else if (result == TA_EXEC_FAILED) {
        ta_trace_action_failed(it, &fault);
    }
    if (result != TA_ERR_NOMEM) {
        loop->iteration += 1u; /* an iteration whose error could not be queued runs again */
    }
    return result;
}

static ta_result ta_exec_action(ta_interpreter *it, const ta_action_def *action) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    ta_fault fault = {0};
    ta_op_status status = TA_OP_OK;
    switch (action->kind) {
        case TA_ACTION_RAISE: {
            ta_value data = {0};
            status = ta_eval_source(it, &(ta_value_params){ .source = action->as.raise.data, .out = &data,
                                                            .fault = &fault });
            if (status != TA_OP_OK) {
                break;
            }
            ta_event event = {
                .name = action->as.raise.event,
                .type = TA_EVENT_INTERNAL,
                .invoke_id = TA_INVOKE_ID_NONE,
                .data = data,
            };
            return ta_interpreter_enqueue_internal(it->system, it->actor, &event);
        }
        case TA_ACTION_SEND:
            return ta_exec_send(it, &action->as.send);
        case TA_ACTION_CANCEL: {
            ta_send_id send_id = 0;
            if (action->as.cancel.sendid.kind == TA_TEXT_SOURCE_ATOM) {
                send_id = ta_literal_send_id(action->as.cancel.sendid.as.atom);
            } else {
                ta_value holder = {0};
                ta_str text = {0};
                status = ta_eval_text(it, &(ta_text_eval_params){
                                              .expr = action->as.cancel.sendid.as.expr, .holder = &holder,
                                              .text = &text, .fault = &fault,
                                          });
                if (status == TA_OP_OK) {
                    send_id = ta_send_id_from_text(it->system, text);
                }
                ta_value_release(&holder);
            }
            if (status == TA_OP_OK && send_id != 0) {
                (void)ta_actor_cancel_held_send(it->actor, send_id);
                if (ta_timer_cancel(&it->system->timers, ta_actor_timers(it->actor), send_id) == TA_OK) {
                    TA_COUNT(it->system, it->actor->definition, sends_cancelled);
                }
                if (ta_tracing(it, TA_TRACE_CANCELLED)) {
                    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_CANCELLED);
                    record.send = send_id;
                    ta_trace_emit(it, &record);
                }
            }
            break;
        }
        case TA_ACTION_ASSIGN: {
            ta_value value = {0};
            status = ta_eval_source(it, &(ta_value_params){ .source = action->as.assign.value, .out = &value,
                                                            .fault = &fault });
            if (status == TA_OP_OK) {
                status = v->assign == NULL
                             ? TA_OP_ERROR
                             : v->assign(definition->datamodel.self,
                                         &(ta_datamodel_assign_params){
                                             .shared = definition->datamodel_shared,
                                             .instance = it->actor->datamodel_instance,
                                             .location = action->as.assign.location,
                                             .value = &value,
                                             .host = &it->actor->host,
                                             .fault = &fault,
                                         });
                if (status == TA_OP_OK && ta_tracing(it, TA_TRACE_ASSIGN)) {
                    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_ASSIGN);
                    record.expr = action->as.assign.location;
                    record.has_value = true;
                    record.value = value;
                    ta_trace_emit(it, &record);
                }
                ta_value_release(&value);
            }
            break;
        }
        case TA_ACTION_SCRIPT:
            status = v->execute_script == NULL
                         ? TA_OP_ERROR
                         : v->execute_script(definition->datamodel.self,
                                             &(ta_datamodel_execute_script_params){
                                                 .shared = definition->datamodel_shared,
                                                 .instance = it->actor->datamodel_instance,
                                                 .script = action->as.script,
                                                 .host = &it->actor->host,
                                                 .fault = &fault,
                                             });
            if (status != TA_OP_NOMEM) {
                /* A <script> reports its own run: SCRIPT, with the fault if it failed. */
                ta_result result = TA_OK;
                if (status != TA_OP_OK) {
                    result = ta_op_failed(it, status, &fault);
                    if (result == TA_ERR_NOMEM) {
                        return result;
                    }
                }
                if (ta_tracing(it, TA_TRACE_SCRIPT)) {
                    ta_trace_record record = ta_trace_record_now(it, TA_TRACE_SCRIPT);
                    record.expr = action->as.script;
                    if (status != TA_OP_OK) {
                        ta_trace_fault(&record, &fault);
                    }
                    ta_trace_emit(it, &record);
                }
                /* Like every other action that fails, it also writes
                 * ACTION_FAILED, which a host may want without SCRIPT. */
                if (result == TA_EXEC_FAILED) {
                    ta_trace_action_failed(it, &fault);
                }
                return result;
            }
            break;
        case TA_ACTION_LOG: {
            /*
             * SCXML 4.7: evaluating the expression can fail, which is an
             * ordinary execution error; writing the record cannot, so a
             * <log> never changes interpretation.
             */
            ta_log_record record = { .source = it->actor->record.self, .key = ta_trace_key(it),
                                     .context = it->trace_context };
            if (action->as.log.has_label) {
                record.label = ta_atom_text(&it->system->atoms, action->as.log.label);
            }
            if (action->as.log.has_expr) {
                status = ta_eval_source(it, &(ta_value_params){
                                                .source = { .kind = TA_VALUE_SOURCE_EXPR, .as.expr = action->as.log.expr },
                                                .out = &record.value,
                                                .fault = &fault,
                                            });
                record.has_value = status == TA_OP_OK;
            }
            if (status == TA_OP_OK && it->system->log_sink.v != NULL) {
                if (it->actor->published) {
                    it->system->log_sink.v->write(it->system->log_sink.self, &record);
                } else if (ta_actor_replaying(it->actor)) {
                    /* The run being replayed wrote it (ADR 0089 section 3). */
                } else {
                    ta_result result = ta_actor_hold_log(it->system, it->actor, &record);
                    if (result != TA_OK) {
                        ta_value_release(&record.value);
                        return result;
                    }
                }
            }
            ta_value_release(&record.value);
            break;
        }
        /* <if> and <foreach> open blocks, which ta_run_block runs. */
        default:
            status = TA_OP_ERROR;
            break;
    }
    if (status == TA_OP_OK) {
        return TA_OK;
    }
    ta_result result = ta_op_failed(it, status, &fault);
    if (result == TA_EXEC_FAILED) {
        ta_trace_action_failed(it, &fault);
    }
    return result;
}

static ta_result ta_exec_push(ta_interpreter *it, ta_exec_frame frame) {
    if (it->exec_count == it->exec_capacity) {
        return TA_ERR_INVALID; /* unreachable: the definition's depth sizes the frames */
    }
    it->exec_frames[it->exec_count++] = frame;
    return TA_OK;
}

static ta_result ta_exec_block(ta_interpreter *it, ta_span32 block) {
    return ta_exec_push(it, (ta_exec_frame){ .kind = TA_EXEC_FRAME_ACTIONS, .actions = block });
}

/*
 * Whether the executor stops before its next unit (ADR 0061): only in a
 * microstep's own block of a bootstrapped actor, never in <finalize> or at
 * termination, and only once this call has run its action budget.
 */
static bool ta_exec_should_pause(const ta_interpreter *it, uint32_t floor) {
    return it->pausable && it->in_microstep && it->actor->bootstrapped && floor == 0 && it->action_budget != 0 &&
           it->actions >= it->action_budget;
}

/*
 * What a unit of executable content needs before it runs (ADR 0070): room
 * for the one internal event it may queue, its own or the error it raises,
 * and for a published actor's <send> with a delay, room to store the send.
 * Running out of memory here stops the run before the unit, with nothing
 * of it done; so a <script> that changed the data model and then failed
 * never runs twice because its error could not be queued.
 */
static ta_result ta_unit_reserve(ta_interpreter *it, const ta_action_def *action) {
    /* Content-sized temporaries live for one unit (ADR 0092 section 3): a
     * <foreach> of many sends reuses the render area instead of filling it. */
    ta_arena_reset(&it->system->render);
    ta_result result = ta_queue_room(it->system, it->actor);
    if (result == TA_OK && action != NULL && action->kind == TA_ACTION_SEND &&
        action->as.send.delay.kind != TA_DELAY_SOURCE_NONE && it->actor->published) {
        result = ta_actor_reserve_send(it->system, it->actor);
    }
    return result;
}

/*
 * Runs a block of executable content and every block nested in it, without
 * recursion (ADR 0060). Each open block is an ACTIONS frame; an <if> opens
 * the block its condition picks, and a <foreach> is a FOREACH frame that
 * opens its body once per position. The frames come from the interpreter's
 * scratch, so nothing here allocates them. An error anywhere stops the whole
 * block, as SCXML 4.9 requires, and every snapshot still held is released.
 *
 * A pause (ADR 0061) and running out of memory (ADR 0070) both stop before
 * a unit and leave the frames open, `at.block_open` set, for the run to
 * save. A unit that runs out of memory had no effect a second run of it
 * would repeat, so its frame is put back to before it.
 */
static ta_result ta_run_block(ta_interpreter *it, ta_span32 block) {
    const ta_definition *definition = it->definition;
    uint32_t floor = it->exec_count;
    ta_result result = TA_OK;
    if (it->at.block_open) {
        /* Resuming: the frames the stop left, from the bottom, are this
         * block's, which a stop only ever leaves at the step's own level. */
        floor = 0;
        it->at.block_open = false;
    } else {
        result = ta_exec_block(it, block);
    }
    while (result == TA_OK && it->exec_count > floor) {
        uint32_t top = it->exec_count - 1u;
        ta_exec_frame *frame = &it->exec_frames[top];
        if (frame->kind == TA_EXEC_FRAME_FOREACH) {
            if (frame->iteration == frame->iteration_count) {
                ta_value_release(&frame->iteration_snapshot);
                it->exec_count -= 1u;
                continue;
            }
            if (ta_exec_should_pause(it, floor)) {
                it->at.block_open = true;
                return TA_EXEC_PAUSED;
            }
            result = ta_unit_reserve(it, NULL);
            if (result == TA_OK) {
                it->actions += 1u;
                it->trace_action = frame->cursor;
                uint32_t mark = ta_queue_mark(it->actor);
                result = ta_foreach_bind(it, frame);
                if (result != TA_ERR_NOMEM) {
                    ta_interpreter_trace_raised(it, mark);
                }
            }
            if (result == TA_OK) {
                result = ta_exec_block(it, frame->actions);
            }
            continue;
        }
        if (frame->cursor == frame->actions.length) {
            it->exec_count -= 1u;
            continue;
        }
        if (ta_exec_should_pause(it, floor)) {
            it->at.block_open = true;
            return TA_EXEC_PAUSED;
        }
        uint32_t index = frame->actions.offset + frame->cursor;
        const ta_action_def *action = &definition->action_defs[index];
        result = ta_unit_reserve(it, action);
        if (result != TA_OK) {
            break;
        }
        it->actions += 1u;
        frame->cursor += 1u;
        it->trace_action = index;
        uint32_t mark = ta_queue_mark(it->actor);
        if (action->kind == TA_ACTION_IF) {
            bool condition = false;
            ta_fault fault = {0};
            ta_op_status status = ta_eval_condition(it, action->as.conditional.condition, &condition, &fault);
            result = status != TA_OP_OK ? ta_op_failed(it, status, &fault) : TA_OK;
            if (result != TA_ERR_NOMEM && ta_tracing(it, TA_TRACE_CONDITION)) {
                ta_trace_record record = ta_trace_record_now(it, TA_TRACE_CONDITION);
                record.expr = action->as.conditional.condition;
                record.result = condition && result == TA_OK;
                if (result != TA_OK) {
                    ta_trace_fault(&record, &fault);
                }
                ta_trace_emit(it, &record);
            }
            if (result == TA_OK) {
                result = ta_exec_block(it, condition ? action->as.conditional.then_actions
                                                     : action->as.conditional.else_actions);
            }
        } else if (action->kind == TA_ACTION_FOREACH) {
            ta_exec_frame loop = { .kind = TA_EXEC_FRAME_FOREACH, .actions = action->as.foreach.body_actions,
                                   .cursor = index };
            result = ta_foreach_begin(it, &loop);
            if (result == TA_OK) {
                result = ta_exec_push(it, loop);
                if (result != TA_OK) {
                    ta_value_release(&loop.iteration_snapshot);
                }
            }
        } else {
            result = ta_exec_action(it, action);
        }
        if (result != TA_ERR_NOMEM) {
            /* The unit committed: the events it queued, after its own record. */
            ta_interpreter_trace_raised(it, mark);
        }
        if (result == TA_ERR_NOMEM && it->exec_count == top + 1u) {
            /* Nothing was opened above it: the unit runs again from its start. */
            it->exec_frames[top].cursor -= 1u;
            it->actions -= 1u;
        }
    }
    if (result == TA_ERR_NOMEM && floor == 0 && it->exec_count != 0) {
        it->at.block_open = true;
        return result;
    }
    while (it->exec_count > floor) {
        ta_exec_frame *frame = &it->exec_frames[--it->exec_count];
        if (frame->kind == TA_EXEC_FRAME_FOREACH) {
            ta_value_release(&frame->iteration_snapshot);
        }
    }
    return result;
}

/* A failed block has already queued its error; execution continues with the next block. */
static ta_result ta_run_block_isolated(ta_interpreter *it, ta_span32 block) {
    ta_result result = ta_run_block(it, block);
    it->trace_action = TA_ACTION_NONE;
    return result == TA_EXEC_FAILED ? TA_OK : result;
}

/*
 * Runs a state's <onentry> or <onexit> handlers in document order. Each
 * handler is a separate block (SCXML 3.8, 3.9): an error skips the rest of
 * its own handler only.
 */
static ta_result ta_run_handlers(ta_interpreter *it, ta_span32 handlers) {
    /* `at.handler` is where a paused microstep resumes; it is 0 otherwise. */
    for (; it->at.handler < handlers.length; it->at.handler += 1u) {
        ta_result result = ta_run_block_isolated(it, it->definition->handlers[handlers.offset + it->at.handler]);
        if (result != TA_OK) {
            if (result != TA_EXEC_PAUSED) {
                it->at.handler = 0;
            }
            return result;
        }
    }
    it->at.handler = 0;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Transition selection
 */

/*
 * Adds a span of targets to `out`: a target itself, or for a history state
 * its recorded configuration, or else the targets of its default
 * transition, which may name history states again. Those are expanded from
 * a worklist, each at most once, so a history default that leads back to
 * itself ends instead of looping.
 */
static void ta_effective_span(ta_interpreter *it, ta_state_list targets, uint64_t *out, uint32_t *pending) {
    for (uint32_t index = 0; index < targets.count; index += 1) {
        ta_state_id target = targets.ids[index];
        if (!ta_is_history(it, target)) {
            ta_bits_put(out, target, true);
        } else if (!ta_bits_get(it->history_seen, target)) {
            ta_bits_put(it->history_seen, target, true);
            it->history_stack[(*pending)++] = target;
        }
    }
}

static void ta_effective_targets(ta_interpreter *it, ta_state_list targets, uint64_t *out) {
    ta_bits_clear(it, it->history_seen);
    uint32_t pending = 0;
    ta_effective_span(it, targets, out, &pending);
    while (pending != 0) {
        ta_state_id history = it->history_stack[--pending];
        const uint64_t *recorded = ta_history_slot_words(&it->actor->history, ta_state(it, history)->history_slot);
        if (ta_bits_any(it, recorded)) {
            for (uint32_t word = 0; word < it->words; word += 1) {
                out[word] |= recorded[word];
            }
        } else {
            const ta_transition_def *fallback = &it->definition->transitions[ta_state(it, history)->transitions.offset];
            ta_effective_span(it, ta_targets(it, fallback->targets), out, &pending);
        }
    }
}

/* False when the transition is targetless and so has no domain. */
static bool ta_transition_domain(ta_interpreter *it, ta_transition_id transition, ta_state_id *out) {
    ta_transition_view view = ta_view(it, transition);
    if (view.targets.count == 0) {
        return false;
    }
    if (view.source == TA_STATE_NONE) {
        *out = TA_STATE_NONE;
        return true;
    }
    uint64_t *targets = it->domain_targets;
    ta_bits_clear(it, targets);
    ta_effective_targets(it, view.targets, targets);
    const ta_definition *definition = it->definition;
    if (view.type == TA_TRANSITION_INTERNAL && ta_state(it, view.source)->kind == TA_STATE_COMPOUND) {
        bool inside = true;
        for (ta_state_id state = 0; state < definition->state_count && inside; state += 1) {
            inside = !ta_bits_get(targets, state) || ta_descends(it, state, view.source);
        }
        if (inside) {
            *out = view.source;
            return true;
        }
    }
    /* The least common compound ancestor of the source and every target. */
    for (ta_state_id ancestor = ta_state(it, view.source)->parent; ancestor != TA_STATE_NONE;
         ancestor = ta_state(it, ancestor)->parent) {
        if (ta_state(it, ancestor)->kind != TA_STATE_COMPOUND) {
            continue;
        }
        bool common = true;
        for (ta_state_id state = 0; state < definition->state_count && common; state += 1) {
            common = !ta_bits_get(targets, state) || ta_descends(it, state, ancestor);
        }
        if (common) {
            *out = ancestor;
            return true;
        }
    }
    *out = TA_STATE_NONE;
    return true;
}

static void ta_exit_set(ta_interpreter *it, ta_transition_id transition, uint64_t *out) {
    ta_state_id domain = TA_STATE_NONE;
    if (!ta_transition_domain(it, transition, &domain)) {
        return;
    }
    for (ta_state_id state = 0; state < it->definition->state_count; state += 1) {
        if (ta_bits_get(it->active, state) && ta_descends(it, state, domain)) {
            ta_bits_put(out, state, true);
        }
    }
}

static void ta_remove_conflicts(ta_interpreter *it) {
    uint32_t kept = 0;
    for (uint32_t index = 0; index < it->enabled_count; index += 1) {
        ta_transition_id candidate = it->enabled[index];
        ta_bits_clear(it, it->exit_left);
        ta_exit_set(it, candidate, it->exit_left);
        bool preempted = false;
        for (uint32_t other = 0; other < kept && !preempted; other += 1) {
            it->remove[other] = false;
            ta_bits_clear(it, it->exit_right);
            ta_exit_set(it, it->filtered[other], it->exit_right);
            bool overlap = false;
            for (uint32_t word = 0; word < it->words && !overlap; word += 1) {
                overlap = (it->exit_left[word] & it->exit_right[word]) != 0;
            }
            if (!overlap) {
                continue;
            }
            if (ta_descends(it, ta_view(it, candidate).source, ta_view(it, it->filtered[other]).source)) {
                it->remove[other] = true;
            } else {
                preempted = true;
            }
        }
        if (preempted) {
            continue;
        }
        uint32_t write = 0;
        for (uint32_t other = 0; other < kept; other += 1) {
            if (!it->remove[other]) {
                it->filtered[write++] = it->filtered[other];
            }
        }
        kept = write;
        it->filtered[kept++] = candidate;
    }
    for (uint32_t index = 0; index < kept; index += 1) {
        it->enabled[index] = it->filtered[index];
    }
    it->enabled_count = kept;
}

static bool ta_transition_matches(ta_interpreter *it, const ta_transition_def *transition, const ta_event *event) {
    if (event == NULL) {
        return transition->event_patterns.length == 0;
    }
    for (uint32_t index = 0; index < transition->event_patterns.length; index += 1) {
        if (ta_event_pattern_matches(it->definition,
                                     &it->definition->event_patterns[transition->event_patterns.offset + index],
                                     event->name)) {
            return true;
        }
    }
    return false;
}

/* Selects the optimal enabled transition set; event is null for eventless selection. */
static ta_result ta_select_transitions(ta_interpreter *it, const ta_event *event) {
    const ta_definition *definition = it->definition;
    it->enabled_count = 0;
    it->condition_count = 0;
    for (ta_state_id atomic = 0; atomic < definition->state_count; atomic += 1) {
        if (!ta_bits_get(it->active, atomic) || !ta_definition_is_atomic(definition, atomic)) {
            continue;
        }
        bool found = false;
        for (ta_state_id state = atomic; state != TA_STATE_NONE && !found; state = ta_state(it, state)->parent) {
            ta_span32 transitions = ta_state(it, state)->transitions;
            for (uint32_t index = 0; index < transitions.length && !found; index += 1) {
                ta_transition_id id = transitions.offset + index;
                const ta_transition_def *transition = &definition->transitions[id];
                if (!ta_transition_matches(it, transition, event)) {
                    continue;
                }
                bool enabled = true;
                if (transition->condition != UINT32_MAX) {
                    ta_fault fault = {0};
                    ta_op_status status = ta_eval_condition(it, transition->condition, &enabled, &fault);
                    struct ta_trace_condition guard = { .transition = id, .expr = transition->condition,
                                                        .result = enabled, .raised = UINT32_MAX };
                    if (status != TA_OP_OK) {
                        enabled = false;
                        ta_result result = ta_op_failed(it, status, &fault);
                        if (result != TA_EXEC_FAILED) {
                            return result;
                        }
                        guard = (struct ta_trace_condition){ .transition = id, .expr = transition->condition,
                                                             .failed = true, .detail = fault.detail,
                                                             .code = fault.code,
                                                             .raised = ta_queue_mark(it->actor) - 1u };
                    }
                    ta_trace_guard(it, &guard);
                }
                if (!enabled) {
                    continue;
                }
                found = true;
                bool duplicate = false;
                for (uint32_t other = 0; other < it->enabled_count && !duplicate; other += 1) {
                    duplicate = it->enabled[other] == id;
                }
                if (!duplicate) {
                    it->enabled[it->enabled_count++] = id;
                }
            }
        }
    }
    ta_remove_conflicts(it);
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Microsteps
 */

static bool ta_entering_below(ta_interpreter *it, ta_state_id ancestor) {
    for (ta_state_id state = 0; state < it->definition->state_count; state += 1) {
        if (ta_bits_get(it->to_enter, state) && ta_descends(it, state, ancestor)) {
            return true;
        }
    }
    return false;
}

/*
 * The entry set (SCXML addDescendantStatesToEnter and
 * addAncestorStatesToEnter), without recursion (ADR 0060). Each frame is one
 * activation of the recursive algorithm, resumed where it left off, so the
 * order of every step, which ta_entering_below and default_history depend
 * on, is the recursion's:
 *
 *   DESCENDANTS  addDescendantStatesToEnter(state), not yet started
 *   TARGETS      for each target: descendants; then for each: ancestors
 *   RECORDED     the same over a history state's recorded configuration
 *   ANCESTORS    addAncestorStatesToEnter, `state` the next ancestor
 *   REGIONS      a parallel state's regions, `cursor` the next child
 *
 * A history frame marks its history state busy until it ends. A history
 * default that leads back to a busy history state would recurse forever;
 * it is skipped. Every other chain of descendant frames goes to strictly
 * deeper states. So a chain holds at most one descendants frame per level
 * of nesting and one per history state, and each of those has at most two
 * frames above it (a loop, and an ancestors walk with its regions).
 */
typedef enum {
    TA_ENTRY_DESCENDANTS = 0,
    TA_ENTRY_TARGETS,
    TA_ENTRY_RECORDED,
    TA_ENTRY_ANCESTORS,
    TA_ENTRY_REGIONS
} ta_entry_kind;

struct ta_entry_frame {
    ta_entry_kind kind;
    bool ancestors;         /* TARGETS, RECORDED: the second loop */
    ta_state_id state;
    ta_state_id ancestor;
    ta_state_id history;    /* the history state that owns this frame, or TA_STATE_NONE */
    ta_state_list targets;
    uint32_t cursor;
};

/* The frame capacity ta_interpreter_init reserves: three per level of
 * nesting and per history state, and room for the first frame. */
static uint32_t ta_entry_capacity(const ta_definition *definition) {
    return 3u * (definition->max_state_depth + definition->history.slot_count + 1u) + 1u;
}

static bool ta_entry_push(ta_interpreter *it, uint32_t *top, struct ta_entry_frame frame) {
    if (*top == it->entry_capacity) {
        return false; /* unreachable: the bound above holds */
    }
    it->entry_frames[(*top)++] = frame;
    return true;
}

static struct ta_entry_frame ta_entry_descendants(ta_state_id state) {
    return (struct ta_entry_frame){ .kind = TA_ENTRY_DESCENDANTS, .state = state, .history = TA_STATE_NONE };
}

static struct ta_entry_frame ta_entry_ancestors(ta_interpreter *it, ta_state_id state, ta_state_id ancestor) {
    return (struct ta_entry_frame){ .kind = TA_ENTRY_ANCESTORS, .state = ta_state(it, state)->parent,
                                    .ancestor = ancestor, .history = TA_STATE_NONE };
}

/* Starts addDescendantStatesToEnter(frame->state): rewrites the frame into
 * the loop the recursion would run, or pops it. */
static void ta_entry_start(ta_interpreter *it, uint32_t *top) {
    struct ta_entry_frame *frame = &it->entry_frames[*top - 1u];
    ta_state_id state = frame->state;
    const ta_state_def *def = ta_state(it, state);
    if (ta_is_history(it, state)) {
        if (ta_bits_get(it->history_busy, state)) {
            *top -= 1u;
            return;
        }
        ta_bits_put(it->history_busy, state, true);
        const uint64_t *recorded = ta_history_slot_words(&it->actor->history, def->history_slot);
        if (ta_bits_any(it, recorded)) {
            *frame = (struct ta_entry_frame){ .kind = TA_ENTRY_RECORDED, .state = state, .ancestor = def->parent,
                                              .history = state };
        } else {
            it->default_history[def->parent] = state;
            ta_span32 fallback = it->definition->transitions[def->transitions.offset].targets;
            *frame = (struct ta_entry_frame){ .kind = TA_ENTRY_TARGETS, .ancestor = def->parent, .history = state,
                                              .targets = ta_targets(it, fallback) };
        }
        return;
    }
    ta_bits_put(it->to_enter, state, true);
    if (def->kind == TA_STATE_COMPOUND) {
        ta_bits_put(it->default_entry, state, true);
        ta_span32 initial = it->definition->initial_transitions[def->initial].targets;
        *frame = (struct ta_entry_frame){ .kind = TA_ENTRY_TARGETS, .ancestor = state, .history = TA_STATE_NONE,
                                          .targets = ta_targets(it, initial) };
    } else if (def->kind == TA_STATE_PARALLEL) {
        *frame = (struct ta_entry_frame){ .kind = TA_ENTRY_REGIONS, .state = state, .history = TA_STATE_NONE };
    } else {
        *top -= 1u;
    }
}

static void ta_entry_pop(ta_interpreter *it, uint32_t *top) {
    ta_state_id history = it->entry_frames[*top - 1u].history;
    if (history != TA_STATE_NONE) {
        ta_bits_put(it->history_busy, history, false);
    }
    *top -= 1u;
}

/* The next recorded state at or after `from`, or state_count. */
static ta_state_id ta_entry_next_recorded(ta_interpreter *it, const struct ta_entry_frame *frame, ta_state_id from) {
    const uint64_t *recorded =
        ta_history_slot_words(&it->actor->history, ta_state(it, frame->state)->history_slot);
    while (from < it->definition->state_count && !ta_bits_get(recorded, from)) {
        from += 1u;
    }
    return from;
}

/* One step of the frame on top of the stack. */
static bool ta_entry_step(ta_interpreter *it, uint32_t *top) {
    struct ta_entry_frame *frame = &it->entry_frames[*top - 1u];
    switch (frame->kind) {
        case TA_ENTRY_DESCENDANTS:
            ta_entry_start(it, top);
            return true;
        case TA_ENTRY_TARGETS: {
            if (frame->cursor == frame->targets.count) {
                if (frame->ancestors) {
                    ta_entry_pop(it, top);
                    return true;
                }
                frame->ancestors = true;
                frame->cursor = 0;
                return true;
            }
            ta_state_id target = frame->targets.ids[frame->cursor++];
            return ta_entry_push(it, top, frame->ancestors ? ta_entry_ancestors(it, target, frame->ancestor)
                                                           : ta_entry_descendants(target));
        }
        case TA_ENTRY_RECORDED: {
            ta_state_id next = ta_entry_next_recorded(it, frame, frame->cursor);
            if (next == it->definition->state_count) {
                if (frame->ancestors) {
                    ta_entry_pop(it, top);
                    return true;
                }
                frame->ancestors = true;
                frame->cursor = 0;
                return true;
            }
            frame->cursor = next + 1u;
            return ta_entry_push(it, top, frame->ancestors ? ta_entry_ancestors(it, next, frame->ancestor)
                                                           : ta_entry_descendants(next));
        }
        case TA_ENTRY_ANCESTORS: {
            ta_state_id at = frame->state;
            if (at == frame->ancestor || at == TA_STATE_NONE) {
                ta_entry_pop(it, top);
                return true;
            }
            ta_bits_put(it->to_enter, at, true);
            frame->state = ta_state(it, at)->parent;
            if (ta_state(it, at)->kind == TA_STATE_PARALLEL) {
                return ta_entry_push(it, top, (struct ta_entry_frame){ .kind = TA_ENTRY_REGIONS, .state = at,
                                                                       .history = TA_STATE_NONE });
            }
            return true;
        }
        case TA_ENTRY_REGIONS: {
            ta_state_id parallel = frame->state;
            while (frame->cursor < ta_state(it, parallel)->children.length) {
                ta_state_id child = ta_child_at(it, parallel, frame->cursor++);
                if (!ta_is_history(it, child) && !ta_entering_below(it, child)) {
                    return ta_entry_push(it, top, ta_entry_descendants(child));
                }
            }
            ta_entry_pop(it, top);
            return true;
        }
    }
    return false;
}

static void ta_entry_run(ta_interpreter *it, struct ta_entry_frame first) {
    uint32_t top = 0;
    if (!ta_entry_push(it, &top, first)) {
        return;
    }
    while (top != 0 && ta_entry_step(it, &top)) {
    }
}

static void ta_add_descendants(ta_interpreter *it, ta_state_id state) {
    ta_entry_run(it, ta_entry_descendants(state));
}

static void ta_add_ancestors(ta_interpreter *it, ta_state_id state, ta_state_id ancestor) {
    ta_entry_run(it, ta_entry_ancestors(it, state, ancestor));
}

/* The first state after `state`'s subtree: states are numbered in document
 * pre-order, so a subtree is a contiguous range of IDs. */
static ta_state_id ta_subtree_end(ta_interpreter *it, ta_state_id state) {
    ta_state_id last = state;
    while (ta_state(it, last)->children.length != 0) {
        last = ta_child_at(it, last, ta_state(it, last)->children.length - 1u);
    }
    return last + 1u;
}

/*
 * SCXML isInFinalState, bottom up in one pass (ADR 0060 section 4): every
 * child has a higher ID than its parent, so visiting the subtree in reverse
 * ID order answers each child before its parent.
 */
static bool ta_in_final_state(ta_interpreter *it, ta_state_id state) {
    for (ta_state_id at = ta_subtree_end(it, state); at > state;) {
        at -= 1u;
        const ta_state_def *def = ta_state(it, at);
        bool final = false;
        if (def->kind == TA_STATE_COMPOUND) {
            for (uint32_t index = 0; index < def->children.length && !final; index += 1) {
                ta_state_id child = ta_child_at(it, at, index);
                final = ta_state(it, child)->kind == TA_STATE_FINAL && ta_bits_get(it->active, child);
            }
        } else if (def->kind == TA_STATE_PARALLEL) {
            final = true;
            for (uint32_t index = 0; index < def->children.length && final; index += 1) {
                ta_state_id child = ta_child_at(it, at, index);
                final = ta_is_history(it, child) || ta_bits_get(it->final_states, child);
            }
        }
        ta_bits_put(it->final_states, at, final);
    }
    return ta_bits_get(it->final_states, state);
}

/* Queues done.state.<state>; takes ownership of data, which may be empty. */
static ta_result ta_raise_done(ta_interpreter *it, ta_state_id state, ta_value *data) {
    ta_event event = {
        .name = ta_state(it, state)->done_event,
        .type = TA_EVENT_INTERNAL,
        .invoke_id = TA_INVOKE_ID_NONE,
        .data = *data,
    };
    *data = (ta_value){0};
    return ta_interpreter_enqueue_internal(it->system, it->actor, &event);
}

/*
 * <donedata> <param>s: a param whose value fails is ignored after queueing
 * error.execution (SCXML 5.7); the rest still form the payload.
 */
static ta_result ta_done_data_params(ta_interpreter *it, ta_span32 params, ta_value *out) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    ta_named_value *values = ta_arena_alloc(&it->system->render, params.length * sizeof(ta_named_value),
                                            _Alignof(ta_named_value));
    if (values == NULL) {
        ta_fault full = {0};
        ta_interpreter_render_fault(&full);
        ta_result raised = ta_execution_error(it, &full);
        return raised == TA_EXEC_FAILED ? TA_OK : raised;
    }
    uint32_t count = 0;
    ta_result result = TA_OK;
    for (uint32_t index = 0; index < params.length && result == TA_OK; index += 1) {
        const ta_param_def *param = &definition->params[params.offset + index];
        ta_fault fault = {0};
        values[count].name = ta_atom_text(&it->system->atoms, param->name);
        ta_op_status status = ta_eval_source(it, &(ta_value_params){ .source = param->value,
                                                                     .out = &values[count].value, .fault = &fault });
        if (status == TA_OP_OK) {
            count += 1;
        } else {
            result = ta_op_failed(it, status, &fault);
            result = result == TA_EXEC_FAILED ? TA_OK : result;
        }
    }
    ta_fault fault = {0};
    if (result == TA_OK && count != 0) {
        ta_op_status status = v->make_params == NULL
                                  ? TA_OP_ERROR
                                  : v->make_params(definition->datamodel.self,
                                                   &(ta_datamodel_make_params_params){
                                                       .shared = definition->datamodel_shared,
                                                       .instance = it->actor->datamodel_instance,
                                                       .values = values,
                                                       .count = count,
                                                       .host = &it->actor->host,
                                                       .out = out,
                                                       .fault = &fault,
                                                   });
        if (status != TA_OP_OK) {
            *out = (ta_value){0};
            result = ta_op_failed(it, status, &fault);
            result = result == TA_EXEC_FAILED ? TA_OK : result;
        }
    }
    for (uint32_t index = 0; index < count; index += 1) {
        ta_value_release(&values[index].value);
    }
    return result;
}

/*
 * Evaluates a final state's <donedata> into `out`. A failure queues
 * error.execution and leaves the data empty; the done event still follows.
 */
static ta_result ta_done_data_value(ta_interpreter *it, ta_done_data_id id, ta_value *out) {
    *out = (ta_value){0};
    if (id == TA_DONE_DATA_NONE) {
        return TA_OK;
    }
    const ta_definition *definition = it->definition;
    const ta_done_data_def *done = &definition->done_data[id];
    if (done->params.length != 0) {
        return ta_done_data_params(it, done->params, out);
    }
    ta_fault fault = {0};
    ta_op_status status = TA_OP_OK;
    if (done->content.kind == TA_VALUE_SOURCE_CONTENT) {
        const ta_datamodel_vtable *v = definition->datamodel.v;
        status = v->import_content == NULL
                     ? TA_OP_ERROR
                     : v->import_content(definition->datamodel.self,
                                         &(ta_datamodel_import_content_params){
                                             .shared = definition->datamodel_shared,
                                             .instance = it->actor->datamodel_instance,
                                             .content = definition->contents[done->content.as.content].bytes,
                                             .host = &it->actor->host,
                                             .out = out,
                                             .fault = &fault,
                                         });
    } else {
        status = ta_eval_source(it, &(ta_value_params){ .source = done->content, .out = out, .fault = &fault });
    }
    if (status == TA_OP_OK) {
        return TA_OK;
    }
    *out = (ta_value){0};
    ta_result result = ta_op_failed(it, status, &fault);
    return result == TA_EXEC_FAILED ? TA_OK : result;
}

/* Keeps a top-level final state's done data as the actor's completion; the
 * actor holds its sparse state already. */
static void ta_store_completion(ta_interpreter *it, ta_value *data) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    ta_value_release(&sparse->completion.data);
    sparse->completion = (ta_actor_completion){ .has_data = data->v != NULL, .data = *data };
    *data = (ta_value){0};
}

/* Computes the exit set and records history, once, as a microstep begins. */
static void ta_exit_begin(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_bits_clear(it, it->to_exit);
    for (uint32_t index = 0; index < it->enabled_count; index += 1) {
        ta_exit_set(it, it->enabled[index], it->to_exit);
    }
    /* Record history before any exit changes the configuration. */
    for (ta_state_id history = 0; history < definition->state_count; history += 1) {
        const ta_state_def *def = ta_state(it, history);
        if (!ta_is_history(it, history) || !ta_bits_get(it->to_exit, def->parent)) {
            continue;
        }
        uint64_t *recorded = ta_history_slot_words(&it->actor->history, def->history_slot);
        ta_bits_clear(it, recorded);
        for (ta_state_id state = 0; state < definition->state_count; state += 1) {
            if (!ta_bits_get(it->active, state)) {
                continue;
            }
            bool keep = def->kind == TA_STATE_HISTORY_DEEP
                            ? ta_definition_is_atomic(definition, state) && ta_descends(it, state, def->parent)
                            : ta_state(it, state)->parent == def->parent;
            if (keep) {
                ta_bits_put(recorded, state, true);
            }
        }
    }
}

/* Exits the exit set, highest first, from where `at` stands. */
static ta_result ta_exit_states(ta_interpreter *it) {
    while (it->at.state > 0) {
        ta_state_id state = it->at.state - 1u;
        if (!ta_bits_get(it->to_exit, state)) {
            it->at.state -= 1u;
            continue;
        }
        ta_result result = TA_OK;
        if (it->at.stage == TA_EXIT_HANDLERS) {
            result = ta_run_handlers(it, ta_state(it, state)->onexit_handlers);
            if (result != TA_OK) {
                return result;
            }
            it->at.stage = TA_EXIT_HANDLERS_DONE;
        }
        result = ta_invoke_state_exited(it, state); /* Invoke: cancel this state's invocations. */
        if (result != TA_OK) {
            return result;
        }
        ta_bits_put(it->active, state, false);
        it->at.state -= 1u;
        it->at.stage = TA_EXIT_HANDLERS;
    }
    return TA_OK;
}

/*
 * Computes the entry set, once, as the microstep begins, after the exit set
 * and the history it records, which is all the entry set depends on: the
 * MICROSTEP record names it before any action runs (ADR 0067).
 */
static void ta_enter_set(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_bits_clear(it, it->to_enter);
    ta_bits_clear(it, it->default_entry);
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        it->default_history[state] = TA_STATE_NONE;
    }
    for (uint32_t index = 0; index < it->enabled_count; index += 1) {
        ta_transition_id transition = it->enabled[index];
        ta_transition_view view = ta_view(it, transition);
        ta_state_id ancestor = TA_STATE_NONE;
        if (!ta_transition_domain(it, transition, &ancestor)) {
            continue;
        }
        for (uint32_t target = 0; target < view.targets.count; target += 1) {
            ta_add_descendants(it, view.targets.ids[target]);
        }
        ta_bits_clear(it, it->entry_targets);
        ta_effective_targets(it, view.targets, it->entry_targets);
        for (ta_state_id state = 0; state < definition->state_count; state += 1) {
            if (ta_bits_get(it->entry_targets, state)) {
                ta_add_ancestors(it, state, ancestor);
            }
        }
    }
}

/*
 * Done data and done.state events for a final state just entered, or for a
 * top-level one the session's completion. It runs again from its start
 * when it runs out of memory (ADR 0070): its caller takes back the errors
 * and events it queued, and nothing else it does is visible until it
 * succeeds.
 */
static ta_result ta_enter_final(ta_interpreter *it, ta_state_id state) {
    const ta_state_def *def = ta_state(it, state);
    if (def->parent == TA_STATE_NONE && ta_sparse(it->system, it->actor) == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_value data = {0};
    ta_result result = ta_done_data_value(it, def->done_data, &data);
    if (result != TA_OK) {
        return result;
    }
    if (def->parent == TA_STATE_NONE) {
        ta_store_completion(it, &data);
        it->actor->sparse->final_state = state; /* ADR 0056 */
        it->actor->finished = true;
        return TA_OK;
    }
    result = ta_raise_done(it, def->parent, &data);
    ta_state_id grandparent = ta_state(it, def->parent)->parent;
    if (result == TA_OK && grandparent != TA_STATE_NONE &&
        ta_state(it, grandparent)->kind == TA_STATE_PARALLEL && ta_in_final_state(it, grandparent)) {
        ta_value none = {0};
        result = ta_raise_done(it, grandparent, &none);
    }
    return result;
}

/*
 * Enters the entry set, lowest first, from where `at` stands. Each stage
 * of a state happens once; `at.stage` records the ones done, so a paused
 * or stopped microstep never activates a state or starts its data twice
 * (ADR 0061, ADR 0070).
 */
static ta_result ta_enter_states(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    while (it->at.state < definition->state_count) {
        ta_state_id state = it->at.state;
        if (!ta_bits_get(it->to_enter, state)) {
            it->at.state += 1u;
            continue;
        }
        const ta_state_def *def = ta_state(it, state);
        ta_result result = TA_OK;
        if (it->at.stage == TA_ENTER_ACTIVATE) {
            /* Invoke: start after the macrostep. Recording it is the only
             * part that allocates, and it changes nothing when it fails. */
            result = ta_invoke_state_entered(it, state);
            if (result != TA_OK) {
                return result;
            }
            ta_bits_put(it->active, state, true);
            it->at.stage = TA_ENTER_DATA;
            it->at.handler = 0;
        }
        if (it->at.stage == TA_ENTER_DATA) {
            /* Late binding: the state's data, once, from the declaration
             * `at.handler` names. */
            if (definition->binding_mode == TA_BINDING_LATE && !ta_bits_get(it->actor->late_initialized, state)) {
                result = ta_initialize_data(it, state, &it->at.handler);
                if (result != TA_OK) {
                    return result;
                }
                ta_bits_put(it->actor->late_initialized, state, true);
            }
            it->at.handler = 0;
            it->at.stage = TA_ENTER_HANDLERS;
        }
        if (it->at.stage == TA_ENTER_HANDLERS) {
            result = ta_run_handlers(it, def->onentry_handlers);
            if (result != TA_OK) {
                return result;
            }
            it->at.stage = TA_ENTER_INITIAL;
        }
        if (it->at.stage == TA_ENTER_INITIAL) {
            if (ta_bits_get(it->default_entry, state)) {
                result = ta_run_block_isolated(it, definition->initial_transitions[def->initial].actions);
                if (result != TA_OK) {
                    return result;
                }
            }
            it->at.stage = TA_ENTER_HISTORY;
        }
        if (it->at.stage == TA_ENTER_HISTORY) {
            if (it->default_history[state] != TA_STATE_NONE) {
                const ta_state_def *history = ta_state(it, it->default_history[state]);
                result = ta_run_block_isolated(it, definition->transitions[history->transitions.offset].actions);
                if (result != TA_OK) {
                    return result;
                }
            }
            it->at.stage = TA_ENTER_DONE;
        }
        if (def->kind == TA_STATE_FINAL) {
            uint32_t mark = ta_queue_mark(it->actor);
            result = ta_enter_final(it, state);
            if (result != TA_OK) {
                if (result == TA_ERR_NOMEM) {
                    ta_queue_truncate(it->actor, mark);
                }
                return result;
            }
            ta_interpreter_trace_raised(it, mark);
        }
        it->at.state += 1u;
        it->at.stage = TA_ENTER_ACTIVATE;
    }
    return TA_OK;
}

static void ta_load_active(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_bits_clear(it, it->active);
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (!ta_configuration_contains_atomic(&it->actor->configuration, state)) {
            continue;
        }
        for (ta_state_id at = state; at != TA_STATE_NONE; at = ta_state(it, at)->parent) {
            ta_bits_put(it->active, at, true);
        }
    }
}

TA_PRIVATE void ta_interpreter_occupancy(ta_actor *actor, bool arrive) {
    ta_definition *definition = (ta_definition *)actor->definition;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (ta_configuration_contains_atomic(&actor->configuration, state)) {
            uint32_t *count = &definition->occupancy[state];
            *count = arrive ? *count + 1u : (*count == 0 ? 0 : *count - 1u);
        }
    }
}

static void ta_store_active(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_interpreter_occupancy(it->actor, false);
    ta_configuration_clear(&it->actor->configuration);
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (ta_bits_get(it->active, state) && ta_definition_is_atomic(definition, state)) {
            ta_configuration_set_atomic(&it->actor->configuration, state, true);
        }
    }
    ta_interpreter_occupancy(it->actor, true);
}

static void ta_compact_sparse(ta_interpreter *it) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    if (sparse == NULL || sparse->paused != NULL || sparse->invoke_count != 0 ||
        sparse->pending_invokes.count != 0 || sparse->activation_effects != NULL || sparse->has_parent ||
        sparse->completion_pending || sparse->completion.has_data) {
        return;
    }
    ta_invoke_release(it->system, it->actor);
    if (sparse->timers.count != 0) {
        /* Pending delayed sends keep the sparse state (ADR 0054). */
        return;
    }
    ta_timer_list_destroy(&it->system->timers, &sparse->timers);
    ta_mem_free(it->system->allocator, sparse,
                &(ta_allocator_free_params){ .size = sizeof(ta_actor_sparse_state),
                                             .alignment = _Alignof(ta_actor_sparse_state) });
    it->actor->sparse = NULL;
}

/* ------------------------------------------------------------------------
 * Running a microstep
 */

/* Opens the microstep of the enabled transitions: the exit set, history
 * recorded before anything exits, and the entry set; then its MICROSTEP
 * record. It stands at its start (ADR 0086). It changes the actor, so an
 * image taken before it no longer describes it. `event` is the event it
 * takes, or null. */
static void ta_microstep_begin(ta_interpreter *it, const ta_event *event) {
    ta_actor_touch(it->actor);
    it->at = (ta_micro_position){ .phase = TA_PHASE_START };
    ta_exit_begin(it);
    ta_enter_set(it);
    ta_trace_microstep(it, event);
}

/* Passes a microstep's start: its first state to exit is next. */
static void ta_microstep_pass_start(ta_interpreter *it) {
    it->at = (ta_micro_position){ .phase = TA_PHASE_EXIT, .state = it->definition->state_count,
                                  .stage = TA_EXIT_HANDLERS };
}

/*
 * Runs the open microstep from where `at` stands. A pause (TA_EXEC_PAUSED)
 * or a failed allocation (TA_ERR_NOMEM) leaves it open: the position, the
 * sets and the frames stay in the interpreter for the run to save, and the
 * actor's configuration is still the one before the microstep (ADR 0061,
 * ADR 0070).
 */
static ta_result ta_microstep(ta_interpreter *it) {
    if (it->at.phase == TA_PHASE_START) {
        /* A microstep this call began; one it resumed at its start has
         * passed it already. Nothing has run, so stopping here needs no
         * block open (ADR 0086). */
        if (it->pause_at_start && it->actor->bootstrapped) {
            return TA_EXEC_PAUSED;
        }
        ta_microstep_pass_start(it);
    }
    ta_result result = TA_OK;
    it->in_microstep = true;
    if (it->at.phase == TA_PHASE_EXIT) {
        result = ta_exit_states(it);
        if (result == TA_OK) {
            it->at.phase = TA_PHASE_TRANSITIONS;
            it->at.state = 0;
        }
    }
    if (result == TA_OK && it->at.phase == TA_PHASE_TRANSITIONS) {
        for (; result == TA_OK && it->at.state < it->enabled_count; it->at.state += 1u) {
            result = ta_run_block_isolated(it, ta_view(it, it->enabled[it->at.state]).actions);
            if (result != TA_OK) {
                break;
            }
        }
        if (result == TA_OK) {
            it->at.phase = TA_PHASE_ENTER;
            it->at.state = 0;
            it->at.stage = TA_ENTER_ACTIVATE;
        }
    }
    if (result == TA_OK && it->at.phase == TA_PHASE_ENTER) {
        result = ta_enter_states(it);
    }
    it->in_microstep = false;
    if (result == TA_EXEC_PAUSED || result == TA_ERR_NOMEM) {
        return result;
    }
    it->at = (ta_micro_position){0};
    ta_store_active(it);
    it->steps += 1u;
    /* The first microstep an actor completes is its bootstrap's. */
    it->actor->bootstrapped = true;
    return result;
}

/*
 * A selection for an event completed: its guards are written, then the
 * microstep of its transitions, or, when none is enabled, the event is
 * consumed.
 */
static void ta_after_selection(ta_interpreter *it, uint32_t mark) {
    ta_trace_selection(it, mark);
    ta_actor *actor = it->actor;
    bool external = actor->event_block != NULL && actor->event_block->slots.bound == TA_BOUND_EXTERNAL;
    if (external) {
        TA_COUNT(it->system, actor->definition, events_taken);
    }
    if (it->enabled_count != 0) {
        ta_microstep_begin(it, ta_actor_bound_event(actor));
        return;
    }
    /* The event is consumed and enabled nothing (ADR 0072). */
    if (external) {
        actor->event_block->slots.external_ignored = true;
        TA_COUNT(it->system, actor->definition, events_ignored);
    } else {
        TA_COUNT(it->system, actor->definition, internal_ignored);
    }
    if (ta_tracing(it, TA_TRACE_IGNORED)) {
        ta_trace_record record = ta_trace_record_now(it, TA_TRACE_IGNORED);
        record.event = ta_actor_bound_event(actor);
        ta_trace_emit(it, &record);
    }
    it->at = (ta_micro_position){0};
    it->steps += 1u;
}

/*
 * Selects the transitions for an event just taken. Nothing has run for it
 * yet, so running out of memory gives it back, and takes back the errors
 * its guards queued: the next run takes it again (ADR 0069, ADR 0070). A
 * selection that completes commits the take, and so the macrostep.
 */
static ta_result ta_select_taken(ta_interpreter *it) {
    /* A copy: an error.execution a guard raises can grow the queue, which
     * moves a bound queue head. */
    ta_event taken = *ta_actor_bound_event(it->actor);
    uint32_t mark = ta_queue_mark(it->actor);
    ta_result result = ta_select_transitions(it, &taken);
    if (result != TA_OK) {
        if (result == TA_ERR_NOMEM) {
            ta_queue_truncate(it->actor, mark);
            ta_unbind_event(it->actor);
        }
        return result;
    }
    ta_trace_begin(it);
    ta_after_selection(it, mark);
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Phases outside a microstep (ADR 0070)
 */

/*
 * The bootstrap's data (SCXML 5.2): declares every datum, then initializes
 * the top-level data and, with early binding, every state's, from where
 * `at` stands; then the top-level <script>.
 */
static ta_result ta_run_data(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_result result = TA_OK;
    if (it->at.stage == TA_DATA_DECLARE) {
        result = ta_declare_data(it, &it->at.handler);
        if (result != TA_OK) {
            return result;
        }
        it->at.stage = TA_DATA_INITIALIZE;
        it->at.state = 0;
        it->at.handler = 0;
    }
    uint32_t owners = definition->binding_mode == TA_BINDING_EARLY ? definition->state_count + 1u : 1u;
    for (; it->at.state < owners; it->at.state += 1u, it->at.handler = 0) {
        ta_state_id owner = it->at.state == 0 ? TA_STATE_NONE : it->at.state - 1u;
        result = ta_initialize_data(it, owner, &it->at.handler);
        if (result != TA_OK) {
            return result;
        }
    }
    it->at = (ta_micro_position){ .phase = TA_PHASE_SCRIPT };
    return TA_OK;
}

/* The bootstrap's top-level <script> (SCXML 5.8), then its initial microstep. */
static ta_result ta_run_script(ta_interpreter *it) {
    ta_result result = ta_run_block_isolated(it, it->definition->global_script_actions);
    if (result != TA_OK) {
        return result;
    }
    it->enabled[0] = TA_ENABLED_BOOTSTRAP;
    it->enabled_count = 1;
    ta_microstep_begin(it, NULL);
    return TA_OK;
}

/*
 * An external event was taken (SCXML mainEventLoop): the <finalize> of the
 * invocation it came from, then autoforwarding it to every invocation that
 * asks for that; its transitions are selected after both.
 */
static ta_result ta_run_finalize(ta_interpreter *it) {
    /* A copy: <finalize> may grow the internal queue, which can move the
     * bound event (ADR 0069). The slot keeps the data retained. */
    const ta_event event = *ta_actor_bound_event(it->actor);
    if (it->at.stage == TA_FINALIZE_BLOCK) {
        ta_span32 finalize = {0};
        if (!it->at.block_open) {
            (void)ta_invoke_finalize_actions(it->actor, &event, &finalize);
        }
        ta_result result = ta_run_block_isolated(it, finalize);
        if (result != TA_OK) {
            return result;
        }
        it->at.stage = TA_FINALIZE_FORWARD;
        it->at.state = 0;
    }
    ta_result result = ta_invoke_autoforward(it, &event, &it->at.state);
    if (result != TA_OK) {
        return result;
    }
    it->at = (ta_micro_position){ .phase = TA_PHASE_SELECT };
    return TA_OK;
}

/*
 * Selects the transitions for the external event <finalize> ran for. The
 * event cannot be given back any more, so running out of memory stops the
 * run here, and the next one selects again.
 */
static ta_result ta_run_select(ta_interpreter *it) {
    ta_event taken = *ta_actor_bound_event(it->actor);
    uint32_t mark = ta_queue_mark(it->actor);
    ta_result result = ta_select_transitions(it, &taken);
    if (result != TA_OK) {
        if (result == TA_ERR_NOMEM) {
            ta_queue_truncate(it->actor, mark);
        }
        return result;
    }
    ta_after_selection(it, mark);
    return TA_OK;
}

/* Starts the invocations of the states the macrostep entered, from the
 * state and <invoke> `at` names. */
static ta_result ta_run_invokes(ta_interpreter *it) {
    ta_trace_begin(it);
    ta_result result = ta_invoke_start_pending(it, &it->at.state, &it->at.handler);
    if (result != TA_OK) {
        return result;
    }
    it->at = (ta_micro_position){0};
    it->after_invokes = true;
    return TA_OK;
}

/*
 * A top-level final state was entered (SCXML exitInterpreter): every
 * active state is exited, highest first, running its <onexit>; then the
 * session ends, and an invoked one reports done.invoke to its parent. The
 * end runs again when it runs out of memory storing that report, and
 * repeats nothing visible: the invocations it cancelled are gone.
 */
static ta_result ta_run_finish(ta_interpreter *it) {
    if (it->at.stage == TA_FINISH_EXIT) {
        for (; it->at.state > 0; it->at.state -= 1u) {
            ta_state_id state = it->at.state - 1u;
            if (!ta_bits_get(it->active, state)) {
                continue;
            }
            ta_result result = ta_run_handlers(it, ta_state(it, state)->onexit_handlers);
            if (result != TA_OK) {
                return result;
            }
            ta_bits_put(it->active, state, false);
        }
        ta_store_active(it);
        it->at.stage = TA_FINISH_END;
    }
    /* Sends the router already refused once stay queued: they were sent before termination. */
    ta_timer_cancel_unsubmitted(&it->system->timers, ta_actor_timers(it->actor));
    ta_note_ignored(it);
    ta_release_events(it->system, it->actor);
    /* Invoke: cancel this session's invocations and report done.invoke to its parent. */
    ta_result result = ta_invoke_finished(it);
    if (result != TA_OK) {
        return result;
    }
    it->at = (ta_micro_position){0};
    ta_trace_end(it);
    ta_actor *actor = it->actor;
    /* A lifecycle record: its own key, and the step that finished as cause. */
    ta_step_key step = ta_trace_key(it);
    ta_step_key key = ta_trace_lifecycle_key(actor);
    TA_COUNT(it->system, actor->definition, finished);
    if (ta_trace_lifecycle_wanted(&it->system->trace, TA_TRACE_FINISHED, &it->trace_context)) {
        ta_trace_record record = ta_trace_record_now(it, TA_TRACE_FINISHED);
        record.key = key;
        record.cause = step;
        record.position = (ta_trace_position){ .at = TA_TRACE_AT_NONE, .state = TA_STATE_NONE,
                                               .transition = TA_TRANSITION_NONE, .action = TA_ACTION_NONE };
        record.state = actor->sparse != NULL ? actor->sparse->final_state : TA_STATE_NONE;
        ta_trace_emit(it, &record);
    }
    actor->macrostep = false;
    actor->record.residency = TA_ACTOR_DONE;
    return TA_OK;
}

/* Continues the phase `at` names. */
static ta_result ta_continue_phase(ta_interpreter *it) {
    switch (it->at.phase) {
        case TA_PHASE_START:
        case TA_PHASE_EXIT:
        case TA_PHASE_TRANSITIONS:
        case TA_PHASE_ENTER:
            return ta_microstep(it);
        case TA_PHASE_DATA:
            return ta_run_data(it);
        case TA_PHASE_SCRIPT:
            return ta_run_script(it);
        case TA_PHASE_FINALIZE:
            return ta_run_finalize(it);
        case TA_PHASE_SELECT:
            return ta_run_select(it);
        case TA_PHASE_INVOKES:
            return ta_run_invokes(it);
        case TA_PHASE_FINISH:
            return ta_run_finish(it);
        case TA_PHASE_NONE:
        default:
            return TA_ERR_INVALID;
    }
}

/*
 * Takes an external event that came with an operator transition (ADR 0082):
 * the transition, not selection, takes it, as the macrostep's first step.
 * Nothing here allocates, so there is nothing to give back, and taking it
 * commits the macrostep. <finalize> and autoforwarding do not run, since
 * no chart transition sees the event. Once the microstep has begun, its
 * sets hold all it needs, and the operator transition is freed.
 */
static void ta_take_jump(ta_interpreter *it) {
    ta_actor *actor = it->actor;
    ta_bind_event(actor, TA_BOUND_EXTERNAL);
    ta_trace_begin(it);
    TA_COUNT(it->system, actor->definition, events_taken);
    ta_event_slots *slots = &actor->event_block->slots;
    it->jump = slots->jump;
    it->enabled[0] = TA_ENABLED_OPERATOR;
    it->enabled_count = 1;
    ta_microstep_begin(it, ta_actor_bound_event(actor));
    it->jump = NULL;
    ta_jump_free(it->system, slots);
}

/*
 * Nothing is open: the next step of the macrostep (SCXML mainEventLoop).
 * An eventless transition first, then the next internal event, then the
 * invocations of the states the macrostep entered, then the next external
 * event. Right after the invocations started, eventless transitions are
 * selected again only if they queued an internal event. A macrostep that
 * starts with a delivered event takes it first: the last one ended stable,
 * so SCXML selects no eventless transition before it. With nothing left,
 * the macrostep has ended and `*stable` is set.
 */
static ta_result ta_next_step(ta_interpreter *it, bool *stable) {
    ta_actor *actor = it->actor;
    bool external_waiting =
        actor->event_block != NULL && actor->event_block->slots.external == TA_EXTERNAL_WAITING;
    if (external_waiting && !actor->trace_begun && actor->event_block->slots.jump != NULL) {
        /* An operator transition is the first microstep of its macrostep,
         * before internal events queued since it was delivered, so it runs
         * from the configuration it was checked against. */
        ta_take_jump(it);
        return TA_OK;
    }
    bool skip_to_external = !ta_interpreter_internal_pending(actor) &&
                            (it->after_invokes || (external_waiting && !actor->trace_begun));
    it->after_invokes = false;
    if (!skip_to_external) {
        uint32_t mark = ta_queue_mark(actor);
        ta_result result = ta_select_transitions(it, NULL);
        if (result != TA_OK) {
            if (result == TA_ERR_NOMEM) {
                ta_queue_truncate(actor, mark);
            }
            return result;
        }
        /* A selection that completes is the macrostep's first step, if
         * nothing else was. */
        ta_trace_begin(it);
        ta_trace_selection(it, mark);
        if (it->enabled_count != 0) {
            ta_microstep_begin(it, NULL);
            return TA_OK;
        }
        if (ta_interpreter_internal_pending(actor)) {
            ta_bind_event(actor, TA_BOUND_INTERNAL);
            return ta_select_taken(it);
        }
        if ((actor->published || ta_actor_replaying(actor)) && ta_invoke_has_pending(actor)) {
            it->at = (ta_micro_position){ .phase = TA_PHASE_INVOKES };
            return TA_OK;
        }
    }
    if (actor->event_block != NULL && actor->event_block->slots.external == TA_EXTERNAL_WAITING) {
        /* It stays in its slot, where inspection reports it, until the
         * macrostep ends. */
        ta_bind_event(actor, TA_BOUND_EXTERNAL);
        /* Invoke: <finalize> and autoforwarding. The event cannot be given
         * back once they start, so taking it commits the macrostep. */
        if (ta_invoke_before_external(actor, ta_actor_bound_event(actor))) {
            it->at = (ta_micro_position){ .phase = TA_PHASE_FINALIZE, .stage = TA_FINALIZE_BLOCK };
            ta_trace_begin(it);
            return TA_OK;
        }
        return ta_select_taken(it);
    }
    ta_compact_sparse(it);
    ta_trace_end(it);
    ta_note_ignored(it);
    ta_end_macrostep_events(it->system, actor);
    actor->macrostep = false;
    actor->record.residency = TA_ACTOR_IDLE;
    *stable = true;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Stopping inside a phase (ADR 0061, ADR 0070)
 */

/* The executor's frames: an open block per level, and a <foreach> frame
 * under each body (ADR 0060). */
static uint32_t ta_exec_capacity(const ta_definition *definition) {
    return 2u * definition->max_action_depth + 1u;
}

/* The words of a set over the definition's data declarations; never 0. */
static uint32_t ta_decl_words(const ta_definition *definition) {
    return ta_bits_words(definition->data_decl_count) + 1u;
}

/*
 * ta_continuation_capacity describes the largest continuation a run of this
 * definition can save: every set, every enabled transition, and every frame
 * the executor can open. It fills `*shape` with those counts and the offsets
 * of its arrays, and `*bytes` with its size. It answers TA_ERR_NOMEM, with
 * the outputs untouched, when that does not fit (ta_continuation_bytes). It
 * never allocates; it is called where a continuation is allocated or loaded
 * (ta_reserve_stop, the restore of an image), and the result is stored.
 */
TA_PRIVATE ta_result ta_continuation_capacity(const ta_definition *definition, ta_continuation *shape, size_t *bytes) {
    ta_continuation capacity = {
        .words = ta_bits_words(definition->state_count),
        .decl_words = ta_decl_words(definition),
        .state_count = definition->state_count,
        .enabled_count = definition->state_count + 1u,
        .frame_count = ta_exec_capacity(definition),
    };
    size_t total = 0;
    ta_result result = ta_continuation_bytes(&capacity, &total, &capacity.offsets);
    if (result == TA_OK) {
        *shape = capacity;
        *bytes = total;
    }
    return result;
}

static void ta_continuation_forget(ta_system *system, ta_continuation *continuation) {
    /* Its frames moved into scratch or were never there: nothing to release. */
    ta_mem_free(system->allocator, continuation,
                &(ta_allocator_free_params){ .size = continuation->bytes, .alignment = TA_CONTINUATION_ALIGN });
}

/* Gives a continuation whose frames moved back into scratch to the system
 * as its spare, keeping the larger of the two. */
static void ta_continuation_recycle(ta_system *system, ta_continuation *continuation) {
    ta_continuation *spare = system->spare_continuation;
    if (spare != NULL && spare->bytes >= continuation->bytes) {
        ta_continuation_forget(system, continuation);
        return;
    }
    if (spare != NULL) {
        ta_continuation_forget(system, spare);
    }
    system->spare_continuation = continuation;
}

TA_PRIVATE void ta_interpreter_release_spares(ta_system *system) {
    if (system->spare_continuation != NULL) {
        ta_continuation_forget(system, system->spare_continuation);
        system->spare_continuation = NULL;
    }
    if (system->spare_sparse != NULL) {
        ta_mem_free(system->allocator, system->spare_sparse,
                    &(ta_allocator_free_params){ .size = sizeof(ta_actor_sparse_state),
                                                 .alignment = _Alignof(ta_actor_sparse_state) });
        system->spare_sparse = NULL;
    }
}

/*
 * Makes sure a run of the actor can stop anywhere without allocating
 * (ADR 0070): a continuation large enough for its definition and, for an
 * actor without one, a sparse state to hang it from. An actor that stopped
 * before stops into its own continuation again. TA_ERR_NOMEM here happens
 * before the run does anything.
 */
static ta_result ta_reserve_stop(ta_system *system, ta_actor *actor) {
    if (actor->sparse != NULL && actor->sparse->paused != NULL) {
        return TA_OK;
    }
    ta_continuation shape;
    size_t bytes = 0;
    ta_result result = ta_continuation_capacity(actor->definition, &shape, &bytes);
    if (result != TA_OK) {
        return result;
    }
    ta_continuation *spare = system->spare_continuation;
    if (spare == NULL || spare->bytes < bytes) {
        ta_continuation *grown = ta_mem_alloc(system->allocator, bytes, TA_CONTINUATION_ALIGN);
        if (grown == NULL) {
            return TA_ERR_NOMEM;
        }
        *grown = (ta_continuation){ .bytes = bytes };
        if (spare != NULL) {
            ta_continuation_forget(system, spare);
        }
        system->spare_continuation = grown;
        spare = grown;
    }
    /* A spare that is reused may have been laid out for another definition:
     * its offsets are rewritten for this one, and fit because spare->bytes
     * is at least the bytes this shape needs. */
    spare->offsets = shape.offsets;
    if (actor->sparse == NULL && system->spare_sparse == NULL) {
        system->spare_sparse = ta_mem_alloc(system->allocator, sizeof(ta_actor_sparse_state),
                                            _Alignof(ta_actor_sparse_state));
        if (system->spare_sparse == NULL) {
            return TA_ERR_NOMEM;
        }
    }
    return TA_OK;
}

/*
 * Stops the run where `at` stands: the position, the sets and the frames
 * move out of scratch into a continuation the actor owns, the one it
 * resumed from or the system's spare, which ta_reserve_stop provided, so
 * stopping cannot fail. The frames' snapshots move with them.
 */
static void ta_stop(ta_interpreter *it) {
    ta_system *system = it->system;
    ta_actor *actor = it->actor;
    if (actor->sparse == NULL) {
        actor->sparse = system->spare_sparse;
        system->spare_sparse = NULL;
        *actor->sparse = (ta_actor_sparse_state){ .final_state = TA_STATE_NONE };
    }
    ta_continuation *saved = it->resumed;
    it->resumed = NULL;
    if (saved == NULL) {
        saved = system->spare_continuation;
        system->spare_continuation = NULL;
    }
    /* The offsets stay: they were set for the capacity shape when the
     * continuation was reserved or loaded, and the counts written here never
     * exceed it. */
    *saved = (ta_continuation){ .bytes = saved->bytes, .at = it->at, .words = it->words,
                                .decl_words = it->decl_words, .state_count = it->definition->state_count,
                                .enabled_count = it->enabled_count, .frame_count = it->exec_count,
                                .offsets = saved->offsets };
    ta_mem_copy(ta_continuation_decls(saved), it->bound_decls, (size_t)it->decl_words * sizeof(uint64_t));
    uint64_t *sets = ta_continuation_sets(saved);
    const uint64_t *from[] = { it->active, it->to_exit, it->to_enter, it->default_entry };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(from); index += 1u) {
        ta_mem_copy(sets + (size_t)index * it->words, from[index], (size_t)it->words * sizeof(uint64_t));
    }
    ta_mem_copy(ta_continuation_history(saved), it->default_history,
                (size_t)saved->state_count * sizeof(ta_state_id));
    ta_mem_copy(ta_continuation_enabled(saved), it->enabled, (size_t)saved->enabled_count * sizeof(ta_transition_id));
    ta_mem_copy(ta_continuation_frames(saved), it->exec_frames, (size_t)saved->frame_count * sizeof(ta_exec_frame));
    actor->sparse->paused = saved;
    it->exec_count = 0;
    it->at = (ta_micro_position){0};
}

/* Moves a stopped run back into scratch. The actor no longer holds the
 * continuation; the run keeps it, to stop into again or to recycle. */
static void ta_stop_restore(ta_interpreter *it) {
    ta_continuation *saved = it->actor->sparse->paused;
    const uint64_t *sets = ta_continuation_sets(saved);
    uint64_t *to[] = { it->active, it->to_exit, it->to_enter, it->default_entry };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(to); index += 1u) {
        ta_mem_copy(to[index], sets + (size_t)index * it->words, (size_t)it->words * sizeof(uint64_t));
    }
    ta_mem_copy(it->bound_decls, ta_continuation_decls(saved), (size_t)saved->decl_words * sizeof(uint64_t));
    ta_mem_copy(it->default_history, ta_continuation_history(saved), (size_t)saved->state_count * sizeof(ta_state_id));
    ta_mem_copy(it->enabled, ta_continuation_enabled(saved), (size_t)saved->enabled_count * sizeof(ta_transition_id));
    ta_mem_copy(it->exec_frames, ta_continuation_frames(saved), (size_t)saved->frame_count * sizeof(ta_exec_frame));
    it->enabled_count = saved->enabled_count;
    it->exec_count = saved->frame_count;
    it->at = saved->at;
    it->actor->sparse->paused = NULL;
    it->resumed = saved;
}

/* Drops a restored stop without finishing it: its snapshots are released,
 * and its continuation goes back to the system. */
static void ta_stop_drop(ta_interpreter *it) {
    while (it->exec_count > 0) {
        ta_exec_frame *frame = &it->exec_frames[--it->exec_count];
        if (frame->kind == TA_EXEC_FRAME_FOREACH) {
            ta_value_release(&frame->iteration_snapshot);
        }
    }
    it->at = (ta_micro_position){0};
    if (it->resumed != NULL) {
        ta_continuation_recycle(it->system, it->resumed);
        it->resumed = NULL;
    }
}

/* One allocation of ta_interpreter_init, added to a layout the way an
 * arena lays it out: `count` items of `type`, at its alignment. */
#define TA_SCRATCH_TAKE(layout, type, items)                                                      \
    ta_layout_add((layout), &(ta_layout_entry){ .size = sizeof(type), .alignment = _Alignof(type), \
                                                .count = (items) })

/*
 * The bytes of interpreter scratch a definition needs (ADR 0092 section 3):
 * everything ta_interpreter_init takes, in its order and alignment, with
 * both trace buffers, whether or not a sink wants them. The compiler checks
 * it against interpreterBytes, so a definition that registers never runs
 * out of structural scratch.
 *
 * The figure is a pure computation with call lifetime: it allocates nothing
 * and keeps nothing. It must stay in lock-step with ta_interpreter_init,
 * entry for entry, because the arena pads each allocation to its alignment
 * from the start of its data; a transposed pair would let a chart register
 * with too little scratch. When a size does not fit in a size_t it answers
 * TA_ERR_NOMEM and writes nothing to `*out`; the compiler treats that as
 * "needs more than any system has".
 */
TA_PRIVATE ta_result ta_interpreter_scratch_need(const ta_definition *definition, size_t *out) {
    /* Positions are counted from the block's data, which starts aligned to
     * the block's own alignment, as ta_arena_init_fixed lays it out. */
    size_t start = offsetof(ta_arena_block, data);
    ta_layout layout;
    ta_layout_init(&layout, start);
    uint32_t words = ta_bits_words(definition->state_count);
    for (uint32_t index = 0; index < 11u; index += 1u) {
        TA_SCRATCH_TAKE(&layout, uint64_t, words);
    }
    size_t capacity = (size_t)definition->state_count + 1u;
    TA_SCRATCH_TAKE(&layout, ta_state_id, capacity);
    TA_SCRATCH_TAKE(&layout, ta_transition_id, capacity);
    TA_SCRATCH_TAKE(&layout, ta_transition_id, capacity);
    TA_SCRATCH_TAKE(&layout, bool, capacity);
    TA_SCRATCH_TAKE(&layout, struct ta_entry_frame, ta_entry_capacity(definition));
    TA_SCRATCH_TAKE(&layout, ta_state_id, (size_t)definition->history.slot_count + 1u);
    TA_SCRATCH_TAKE(&layout, ta_exec_frame, ta_exec_capacity(definition));
    TA_SCRATCH_TAKE(&layout, uint64_t, ta_decl_words(definition));
    /* 3 * state_count + 1 items, as four entries so that no count wraps: same alignment, no gap between. */
    for (uint32_t index = 0; index < 3u; index += 1u) {
        TA_SCRATCH_TAKE(&layout, ta_state_id, definition->state_count);
    }
    TA_SCRATCH_TAKE(&layout, ta_state_id, 1u);
    TA_SCRATCH_TAKE(&layout, struct ta_trace_condition, (size_t)ta_trace_guard_capacity(definition) + 1u);
    size_t end;
    ta_result result = ta_layout_finish(&layout, &end, NULL);
    if (result == TA_OK) {
        *out = end - start;
    }
    return result;
}

static ta_result ta_interpreter_init(ta_interpreter *it, ta_system *system, ta_actor *actor) {
    const ta_definition *definition = actor->definition;
    uint32_t words = ta_bits_words(definition->state_count);
    *it = (ta_interpreter){ .system = system, .actor = actor, .definition = definition, .words = words };
    ta_arena_reset(&system->scratch);
    ta_arena_reset(&system->render);
    uint64_t **sets[] = {
        &it->active, &it->to_exit, &it->to_enter, &it->default_entry,
        &it->domain_targets, &it->entry_targets, &it->exit_left, &it->exit_right,
        &it->history_seen, &it->history_busy, &it->final_states,
    };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(sets); index += 1) {
        *sets[index] = ta_arena_alloc(&system->scratch, words * sizeof(uint64_t), _Alignof(uint64_t));
        if (*sets[index] == NULL) {
            return TA_ERR_NOMEM;
        }
    }
    /* At most one transition per atomic state, plus the bootstrap transition. */
    uint32_t capacity = definition->state_count + 1u;
    it->default_history = ta_arena_alloc(&system->scratch, capacity * sizeof(ta_state_id), _Alignof(ta_state_id));
    it->enabled = ta_arena_alloc(&system->scratch, capacity * sizeof(ta_transition_id), _Alignof(ta_transition_id));
    it->filtered = ta_arena_alloc(&system->scratch, capacity * sizeof(ta_transition_id), _Alignof(ta_transition_id));
    it->remove = ta_arena_alloc(&system->scratch, capacity * sizeof(bool), _Alignof(bool));
    if (it->default_history == NULL || it->enabled == NULL || it->filtered == NULL || it->remove == NULL) {
        return TA_ERR_NOMEM;
    }
    it->entry_capacity = ta_entry_capacity(definition);
    it->entry_frames = ta_arena_alloc(&system->scratch, it->entry_capacity * sizeof(struct ta_entry_frame),
                                      _Alignof(struct ta_entry_frame));
    /* Each history state is expanded at most once. */
    it->history_stack = ta_arena_alloc(&system->scratch, (definition->history.slot_count + 1u) * sizeof(ta_state_id),
                                       _Alignof(ta_state_id));
    it->exec_capacity = ta_exec_capacity(definition);
    it->exec_frames = ta_arena_alloc(&system->scratch, it->exec_capacity * sizeof(ta_exec_frame), _Alignof(ta_exec_frame));
    it->decl_words = ta_decl_words(definition);
    it->bound_decls = ta_arena_alloc(&system->scratch, (size_t)it->decl_words * sizeof(uint64_t), _Alignof(uint64_t));
    if (it->entry_frames == NULL || it->history_stack == NULL || it->exec_frames == NULL || it->bound_decls == NULL) {
        return TA_ERR_NOMEM;
    }
    /* Tracing (ADR 0067): what this macrostep builds, and the scratch its
     * records need, taken only when the sink wants those records. */
    ta_trace_parent parent = ta_actor_macrostep_parent(actor);
    it->trace_context = parent.context;
    it->trace_kinds = ta_trace_macrostep_kinds(&system->trace, &parent.context);
    it->trace_action = TA_ACTION_NONE;
    uint32_t wanted = ta_trace_any_kinds(&system->trace);
    if ((wanted & (TA_TRACE_BIT(TA_TRACE_MICROSTEP) | TA_TRACE_BIT(TA_TRACE_MACROSTEP_END) |
                   TA_TRACE_BIT(TA_TRACE_LONG_MACROSTEP) | TA_TRACE_BIT(TA_TRACE_RUNAWAY))) != 0) {
        it->trace_states = ta_arena_alloc(&system->scratch, (3u * (size_t)definition->state_count + 1u) *
                                                                sizeof(ta_state_id),
                                          _Alignof(ta_state_id));
        if (it->trace_states == NULL) {
            return TA_ERR_NOMEM;
        }
    }
    if ((wanted & TA_TRACE_BIT(TA_TRACE_CONDITION)) != 0) {
        it->condition_capacity = ta_trace_guard_capacity(definition);
        it->conditions = ta_arena_alloc(&system->scratch,
                                        ((size_t)it->condition_capacity + 1u) * sizeof(struct ta_trace_condition),
                                        _Alignof(struct ta_trace_condition));
        if (it->conditions == NULL) {
            return TA_ERR_NOMEM;
        }
    }
    ta_bits_clear(it, it->history_busy);
    ta_load_active(it);
    if (actor->sparse != NULL && actor->sparse->paused != NULL) {
        ta_stop_restore(it); /* the live set, too, is the stopped run's */
    }
    actor->live_active = it->active;
    return TA_OK;
}

/* PAUSED, RESUMED or STOPPED, for a macrostep that has begun, at the
 * position the run stands at: the one ta_actor_inspect() reports while it
 * is stopped (ADR 0083). */
static void ta_trace_control(ta_interpreter *it, ta_trace_kind kind) {
    if (kind == TA_TRACE_PAUSED) {
        TA_COUNT(it->system, it->actor->definition, runs_paused);
    } else if (kind == TA_TRACE_STOPPED) {
        TA_COUNT(it->system, it->actor->definition, runs_stopped);
    }
    if (it->actor->trace_begun && ta_tracing(it, kind)) {
        ta_trace_record record = ta_trace_record_now(it, kind);
        ta_run_view view = ta_interpreter_view(it);
        record.position = ta_run_position(it->definition, &view);
        ta_trace_emit(it, &record);
    }
}

/*
 * Runs the actor until its macrostep ends, the microstep budget is spent,
 * or a pause or a failed allocation stops it inside a phase, where the
 * next run continues (ADR 0047, ADR 0061, ADR 0070). A failed allocation
 * with nothing open, while selecting transitions, leaves nothing to
 * continue: the next run selects again.
 */
static ta_result ta_run(ta_interpreter *it, uint32_t budget, ta_run_status *status) {
    ta_actor *actor = it->actor;
    for (;;) {
        ta_result result = TA_OK;
        if (it->at.phase != TA_PHASE_NONE) {
            result = ta_continue_phase(it);
        } else if (!actor->bootstrapped) {
            it->at = (ta_micro_position){ .phase = TA_PHASE_DATA, .stage = TA_DATA_DECLARE };
            ta_mem_zero(it->bound_decls, (size_t)it->decl_words * sizeof(uint64_t));
            /* The bootstrap has no event to give back: it begins here. */
            ta_trace_begin(it);
        } else if (actor->finished) {
            if (actor->record.residency == TA_ACTOR_DONE) {
                *status = TA_RUN_DONE;
                return TA_OK;
            }
            it->at = (ta_micro_position){ .phase = TA_PHASE_FINISH, .state = it->definition->state_count,
                                          .stage = TA_FINISH_EXIT };
        } else if (it->microstep_limit != 0 && actor->trace_begun && actor->trace_microstep >= it->microstep_limit) {
            /* Between microsteps, nothing open: the actor stays mid-macrostep
             * and a later resume continues it (ADR 0075). */
            *status = TA_RUN_ERROR;
            return TA_ERR_RUNAWAY;
        } else if (it->steps >= budget) {
            ta_trace_control(it, TA_TRACE_PAUSED);
            *status = TA_RUN_PAUSED;
            return TA_OK;
        } else {
            bool stable = false;
            result = ta_next_step(it, &stable);
            if (result == TA_OK && stable) {
                *status = TA_RUN_STABLE;
                return TA_OK;
            }
        }
        if (result == TA_OK) {
            continue;
        }
        if (result == TA_EXEC_PAUSED) {
            ta_trace_control(it, TA_TRACE_PAUSED);
            ta_stop(it);
            *status = TA_RUN_PAUSED;
            return TA_OK;
        }
        if (result == TA_ERR_NOMEM) {
            ta_trace_control(it, TA_TRACE_STOPPED);
        }
        if (result == TA_ERR_NOMEM && it->at.phase != TA_PHASE_NONE) {
            ta_stop(it);
        }
        *status = TA_RUN_ERROR;
        return result;
    }
}

TA_PRIVATE ta_result ta_interpreter_resume(ta_system *system, ta_actor *actor, const ta_actor_resume_params *params) {
    ta_run_status *status = params->status;
    if (actor->finished && (actor->sparse == NULL || actor->sparse->paused == NULL)) {
        *status = TA_RUN_DONE;
        return TA_OK;
    }
    if (!actor->macrostep) {
        /* Idle: its last macrostep ended stable, and nothing waits. */
        *status = TA_RUN_STABLE;
        return TA_OK;
    }
    ta_interpreter interpreter;
    ta_interpreter *it = &interpreter;
    ta_result result = ta_reserve_stop(system, actor);
    if (result == TA_OK) {
        result = ta_take_macrostep_time(system, actor);
    }
    if (result == TA_OK) {
        result = ta_interpreter_init(it, system, actor);
    }
    if (result != TA_OK) {
        *status = TA_RUN_ERROR;
        return result;
    }
    it->pausable = true;
    it->action_budget = params->action_budget;
    uint32_t limit = actor->definition->microstep_limit == 0 ? system->params[TA_PARAM_MICROSTEP_LIMIT]
                                                            : actor->definition->microstep_limit;
    it->microstep_limit = params->no_microstep_limit || limit == TA_MICROSTEP_LIMIT_NONE ? 0 : limit;
    it->pause_at_start = params->pause_at_microstep_start;
    if (it->at.phase != TA_PHASE_NONE) {
        /* Continuing a stopped run changes the actor again, so an image
         * taken while it was stopped no longer describes it. */
        ta_actor_touch(actor);
    }
    ta_trace_control(it, TA_TRACE_RESUMED);
    if (it->at.phase == TA_PHASE_START) {
        /* It stopped at a microstep's start: this call passes it. */
        ta_microstep_pass_start(it);
    }
    result = ta_run(it, params->budget, status);
    if (params->ignored != NULL && (*status == TA_RUN_STABLE || *status == TA_RUN_DONE)) {
        *params->ignored = it->ended_ignored;
    }
    /* The live set lives in scratch memory; between calls In() derives from the configuration. */
    actor->live_active = NULL;
    if (it->resumed != NULL) {
        ta_continuation_recycle(system, it->resumed);
    }
    /* An empty queue goes back; queued events or a stopped run keep it. */
    ta_block_settle(system, actor);
    return result;
}

/*
 * One session being exited by ta_interpreter_exit_invoked: its interpreter,
 * the scratch arena that interpreter lives in, the states it still has to
 * exit, and, while a child it invoked is being exited above it, the
 * invocation to cancel once that child is done.
 */
typedef struct {
    ta_actor *actor;
    ta_interpreter it;
    ta_arena arena;
    ta_arena render;
    ta_state_id next;     /* states left to exit: those below `next`, highest first */
    ta_state_id exiting;  /* the state whose invocations are being cancelled */
    uint32_t cursor;      /* its invocations left: those below `cursor` */
    bool waiting;         /* `taken`'s child is being exited above this frame */
    ta_invoke_taken taken;
} ta_exit_frame;

/*
 * Starts exiting a session: a nested interpreter region of its own (ADR
 * 0092 section 3), region `frames->count + 1`, so the interpreter that
 * cancelled it keeps its scratch, and an interpreter over it. The region is
 * allocated the first time an exit reaches this depth, and kept. Nothing
 * happens when the session is gone or finished, when the depth passes
 * interpreterNesting, or when there is no memory for the region.
 */
static void ta_exit_open(ta_system *system, ta_vec *frames, ta_actor_id id) {
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL || actor->finished) {
        return;
    }
    ta_exit_frame frame = { .actor = actor, .exiting = TA_STATE_NONE };
    if (ta_system_nested_region(system, frames->count + 1u, &frame.arena, &frame.render) != TA_OK) {
        return;
    }
    ta_arena previous = system->scratch;
    ta_arena previous_render = system->render;
    system->scratch = frame.arena;
    system->render = frame.render;
    ta_result result = ta_interpreter_init(&frame.it, system, actor);
    /* A session stopped inside a microstep exits its live set; the rest of
     * the stop is dropped (ADR 0061 section 6). */
    ta_stop_drop(&frame.it);
    frame.arena = system->scratch;
    frame.render = system->render;
    frame.next = actor->definition->state_count;
    if (result == TA_OK) {
        result = ta_vec_push(frames, &frame);
    }
    if (result != TA_OK) {
        actor->live_active = NULL;
        system->scratch = previous;
        system->render = previous_render;
    }
}

/*
 * One step of the session on top: exit its next state, cancel that state's
 * next invocation, or finish. `system->scratch` is the session's arena while
 * it runs; a step that keeps the session saves the arena back to its frame.
 */
static void ta_exit_step(ta_system *system, ta_vec *frames) {
    ta_exit_frame *frame = ta_vec_at(frames, frames->count - 1u);
    ta_interpreter *it = &frame->it;
    if (frame->waiting) {
        frame->waiting = false;
        ta_invoke_cancel_service(&frame->taken);
        frame->arena = system->scratch;
        frame->render = system->render;
        return;
    }
    if (frame->exiting != TA_STATE_NONE) {
        if (!ta_invoke_take(frame->actor, frame->exiting, &frame->cursor, &frame->taken)) {
            ta_bits_put(it->active, frame->exiting, false);
            frame->exiting = TA_STATE_NONE;
            it->at = (ta_micro_position){0};
            frame->arena = system->scratch;
            frame->render = system->render;
        frame->render = system->render;
            return;
        }
        ta_interpreter_trace_invoke(it, TA_TRACE_INVOKE_CANCELLED, frame->taken.active.id);
        if (frame->taken.meta.has_child) {
            frame->waiting = true;
            frame->arena = system->scratch;
            frame->render = system->render;
        frame->render = system->render;
            ta_exit_open(system, frames, frame->taken.meta.child); /* may move `frame` */
            return;
        } else {
            ta_invoke_cancel_service(&frame->taken);
        }
        frame->arena = system->scratch;
        frame->render = system->render;
        return;
    }
    while (frame->next > 0 && !ta_bits_get(it->active, frame->next - 1u)) {
        frame->next -= 1u;
    }
    if (frame->next == 0) {
        ta_store_active(it);
        frame->actor->live_active = NULL;
        /* The region stays allocated for the next exit this deep. */
        frames->count -= 1u;
        return;
    }
    ta_state_id state = --frame->next;
    /* The position records report: the <onexit> of `state` (ADR 0067). */
    it->at = (ta_micro_position){ .phase = TA_PHASE_EXIT, .state = state + 1u };
    ta_result result = ta_run_handlers(it, ta_state(it, state)->onexit_handlers);
    frame->arena = system->scratch;
    frame->render = system->render;
    if (result != TA_OK) {
        /* Nothing continues an exit: a block that ran out of memory left its
         * frames open for a stop, and they are dropped. */
        ta_stop_drop(it);
        ta_bits_put(it->active, state, false);
        frame->next = 0; /* exitInterpreter stops at a failure, as it did */
        return;
    }
    ta_invoke_forget_pending(frame->actor, state);
    frame->exiting = state;
    frame->cursor = UINT32_MAX;
}

TA_PRIVATE void ta_interpreter_exit_invoked(ta_system *system, ta_actor_id id) {
    /* Cancelling an invocation happens inside the parent's interpreter:
     * every exited session gets its own scratch, and the parent's comes back. */
    ta_arena outer = system->scratch;
    ta_arena outer_render = system->render;
    ta_vec frames;
    ta_vec_init(&frames, system->allocator, TA_VEC_LAYOUT(ta_exit_frame));
    ta_exit_open(system, &frames, id);
    while (frames.count != 0) {
        ta_exit_frame *frame = ta_vec_at(&frames, frames.count - 1u);
        system->scratch = frame->arena;
        system->render = frame->render;
        ta_exit_step(system, &frames);
    }
    ta_vec_destroy(&frames);
    system->scratch = outer;
    system->render = outer_render;
}

/* ------------------------------------------------------------------------
 * Services for invoke.c
 */

TA_PRIVATE ta_actor_sparse_state *ta_interpreter_sparse(ta_system *system, ta_actor *actor) {
    return ta_sparse(system, actor);
}

TA_PRIVATE ta_op_status ta_interpreter_eval(ta_interpreter *it, const ta_interpreter_value_params *params) {
    return ta_eval_source(it, &(ta_value_params){ .source = params->source, .out = params->out,
                                                  .fault = params->fault });
}

TA_PRIVATE ta_op_status ta_interpreter_copy_out(ta_interpreter *it, ta_value *value, ta_fault *fault) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (value->v == NULL || v->copy_out == NULL) {
        return TA_OP_OK;
    }
    ta_value copy = {0};
    ta_op_status status = v->copy_out(definition->datamodel.self, &(ta_datamodel_copy_out_params){
                                                                       .shared = definition->datamodel_shared,
                                                                       .instance = it->actor->datamodel_instance,
                                                                       .value = value,
                                                                       .host = &it->actor->host,
                                                                       .out = &copy,
                                                                       .fault = fault,
                                                                   });
    if (status == TA_OP_OK) {
        ta_value_release(value);
        *value = copy;
    }
    return status;
}

TA_PRIVATE ta_result ta_interpreter_execution_error(ta_interpreter *it, ta_fault *fault) {
    ta_result result = ta_execution_error(it, fault);
    return result == TA_EXEC_FAILED ? TA_OK : result;
}

TA_PRIVATE ta_result ta_interpreter_queue_room(ta_system *system, ta_actor *actor) {
    return ta_queue_room(system, actor);
}

TA_PRIVATE ta_result ta_interpreter_queue_reserve(ta_system *system, ta_actor *actor, uint32_t count) {
    ta_result result = ta_queue_room(system, actor);
    while (result == TA_OK && actor->event_block->capacity - actor->event_block->count < count) {
        ta_event_block *block = actor->event_block;
        if (block->capacity > UINT32_MAX / 2u) {
            return TA_ERR_NOMEM;
        }
        uint32_t capacity = block->capacity * 2u;
        ta_event_block *grown = ta_mem_alloc(system->allocator, ta_event_block_bytes(capacity),
                                             _Alignof(ta_event_block));
        if (grown == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_copy(grown, block, ta_event_block_bytes(block->count));
        grown->capacity = capacity;
        ta_block_free(system, block);
        actor->event_block = grown;
    }
    return result;
}

TA_PRIVATE uint32_t ta_interpreter_queue_mark(const ta_actor *actor) {
    return ta_queue_mark(actor);
}

TA_PRIVATE bool ta_interpreter_internal_pending(const ta_actor *actor) {
    return ta_actor_waiting_internal(actor) != 0;
}

TA_PRIVATE void ta_interpreter_release_actor_state(ta_system *system, ta_actor *actor) {
    ta_release_events(system, actor);
    /* The actor goes: its block goes too, whatever it ran under, and even
     * mid-macrostep, which otherwise keeps it (ta_block_settle). */
    actor->macrostep = false;
    if (actor->event_block != NULL) {
        actor->event_block->slots.trace = (ta_trace_parent){0};
    }
    if (actor->sparse == NULL) {
        ta_block_settle(system, actor);
        return;
    }
    ta_invoke_release(system, actor); /* Invoke: cancel services, free bookkeeping. */
    ta_continuation_free(system->allocator, &actor->sparse->paused);
    ta_block_settle(system, actor); /* empty, and no longer paused */
    /* The owner heap must not keep pointing into the freed sparse state. */
    ta_timer_list_destroy(&system->timers, &actor->sparse->timers);
    ta_value_release(&actor->sparse->completion.data);
    ta_mem_free(system->allocator, actor->sparse,
                &(ta_allocator_free_params){ .size = sizeof(ta_actor_sparse_state),
                                             .alignment = _Alignof(ta_actor_sparse_state) });
    actor->sparse = NULL;
}

/* ------------------------------------------------------------------------
 * Operator transitions and abandoning a macrostep (ADR 0082)
 */

/* Where a target sits for the legality check: a history state stands for
 * its parent, whose region it restores. */
static ta_state_id ta_jump_place(const ta_definition *definition, ta_state_id target) {
    ta_state_kind kind = definition->states[target].kind;
    bool history = kind == TA_STATE_HISTORY_SHALLOW || kind == TA_STATE_HISTORY_DEEP;
    return history ? definition->states[target].parent : target;
}

/*
 * Whether two targets can be in one legal configuration: neither is the
 * other or its ancestor, and the first state above both is a <parallel>.
 * The root counts as a compound state.
 */
static bool ta_jump_compatible(const ta_definition *definition, ta_state_id first, ta_state_id second) {
    if (first == TA_STATE_NONE || second == TA_STATE_NONE || first == second ||
        ta_definition_is_descendant(definition, first, second) ||
        ta_definition_is_descendant(definition, second, first)) {
        return false;
    }
    ta_state_id meet = definition->states[first].parent;
    while (meet != TA_STATE_NONE && !ta_definition_is_descendant(definition, second, meet)) {
        meet = definition->states[meet].parent;
    }
    return meet != TA_STATE_NONE && definition->states[meet].kind == TA_STATE_PARALLEL;
}

TA_PRIVATE bool ta_interpreter_jump_valid(const ta_definition *definition, const ta_operator_transition *transition) {
    uint32_t count = transition->target_count;
    if (count == 0 || transition->targets == NULL ||
        (transition->source != TA_STATE_NONE && transition->source >= definition->state_count)) {
        return false;
    }
    for (uint32_t index = 0; index < count; index += 1u) {
        if (transition->targets[index] >= definition->state_count) {
            return false;
        }
    }
    for (uint32_t index = 0; index < count; index += 1u) {
        ta_state_id first = ta_jump_place(definition, transition->targets[index]);
        for (uint32_t other = index + 1u; other < count; other += 1u) {
            if (!ta_jump_compatible(definition, first, ta_jump_place(definition, transition->targets[other]))) {
                return false;
            }
        }
    }
    return true;
}

TA_PRIVATE ta_result ta_interpreter_abandon(ta_system *system, ta_actor *actor) {
    if (actor->finished) {
        return TA_ERR_DONE;
    }
    if (!actor->macrostep) {
        return TA_ERR_INVALID;
    }
    /* A run stopped inside a step, or a bootstrap whose microstep has not
     * completed, holds no configuration an idle actor could keep. */
    if (!actor->bootstrapped || (actor->sparse != NULL && actor->sparse->paused != NULL)) {
        return TA_ERR_BUSY;
    }
    ta_interpreter interpreter;
    ta_interpreter *it = &interpreter;
    ta_result result = ta_interpreter_init(it, system, actor);
    actor->live_active = NULL;
    if (result != TA_OK) {
        return result;
    }
    uint32_t dropped = ta_actor_waiting_internal(actor);
    if (ta_tracing(it, TA_TRACE_ABANDONED)) {
        ta_trace_record record = ta_trace_record_now(it, TA_TRACE_ABANDONED);
        record.position = ta_trace_nowhere();
        record.event = ta_actor_external_event(actor);
        record.dropped = dropped;
        ta_trace_emit(it, &record);
    }
    ta_trace_end(it);
    /* The waiting internal events go; `_event` and the external event end
     * as at the end of any macrostep (ADR 0069). */
    ta_queue_truncate(actor, ta_queue_mark(actor) - dropped);
    ta_end_macrostep_events(system, actor);
    ta_actor_touch(actor);
    actor->macrostep = false;
    actor->record.residency = TA_ACTOR_IDLE;
    ta_compact_sparse(it);
    ta_block_settle(system, actor);
    return TA_OK;
}
