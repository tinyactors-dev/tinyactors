/*
 * staticjs's benchmarks: the throughput and the memory of the parser, the
 * compiler, the evaluator, the writer and values, in one program.
 *
 * `mise run bench` builds it optimized (build/release/bench) and stores what
 * it prints by machine signature; `mise run bench-compare <commit>` runs it
 * against another commit's library, side by side. The debug build
 * (build/bench) runs `--smoke` with the tests: every case once, at its smoke
 * size, checked for its value and for leaks, without timing anything.
 *
 * A case is named group.subject[.variant] and has a unit: byte (MB/s), eval
 * (M eval/s), element (M elem/s) or op (M op/s). Sizes: L {1000, 10000,
 * 100000} for linear cases; Q {100, 1000, 10000} for cases that are
 * quadratic today, as objects and sets are searched in order, or were at
 * the commit bench-compare runs against; C {100, 300, 1000} for
 * eval.object.spread-reduce, quadratic now that the accumulator is updated
 * in place (each put searches the object in order) and cubic before (each
 * spread copied it). Inputs are deterministic
 * (xorshift64 from 0x9E3779B97F4A7C15), built through the public API:
 *
 *   xs    the numbers 0 to n-1, shuffled, times 0.5
 *   rows  n objects {id: "r%06u", kwh: (i*37 % 1000)/10, priority: i%4, tags: ["a","b"]}
 *   ids   the row ids, with every fifth one twice
 *   text  the row ids joined by commas: "r000000,r000001,...", 8n - 1 bytes
 *   json  the rows as JSON.stringify writes them, about 64n bytes
 *   a, b  string sets of n elements that overlap by half
 *
 * compile (unit byte; eventsPerOp): bytes pushed to sjs_parser_init ->
 * sjs_compiler_sink -> push*k -> finish -> sjs_compiler_finish ->
 * sjs_program_release, whole (.whole), 64 bytes (.chunk64) or one byte
 * (.chunk1) at a time.
 *   parse.module.power.whole            the parser alone, into a sink that counts
 *   compile.module.power.*              bench/modules/power.js
 *   compile.module.campaign.whole       bench/modules/campaign.js, importing power
 *   compile.module.synthetic.*          power.js repeated, exports renamed, to >= 32 KiB
 *   compile.guard.event.*               pending.has(_event.data.id) && _event.name === "notify.ack"
 *   compile.guard.counter.whole         ({...counts, acked: counts.acked + 1})
 *   compile.regex                       twelve typical patterns (an e-mail, a date,
 *                                       a version, a URL, ...), each tested once
 *
 * eval (unit eval; fuelPerOp): compiled once; each op starts a kept vm,
 * runs it, takes the result and releases it.
 *   eval.guard.size, .event, .window    guards from tests/spec/charts.spec
 *   eval.assign.counter                 the counter above
 *   eval.send.target                    `#_actor_progress/${campaignID}`
 *   eval.module.power                   evaluating power.js: loading a chart's module
 *   eval.call.waterFill                 waterFill(capacity, demand), n ports {3, 30, 300}
 *
 * eval (unit element): eval.array.map, .filter, .reduce, .reduce.key
 * ((a, r) => a + r.kwh, which gives 166.5 at 10, as Node does), .pipeline,
 * .toSorted, .toSorted.desc ((a, b) => b - a), .toSorted.rows,
 * .toSorted.rows.desc ((a, b) => b.kwh - a.kwh), .map.pluck (r => r.kwh),
 * .every (x => x >= 0), .some (r => r.kwh < 0), .findIndex (r => r.id
 * === "none") and .findLastIndex (r => r.kwh < 0), which scan every element and find nothing, so they give
 * their smoke value at every size, and .lastIndexOf (xs.lastIndexOf(-1), likewise), .toSpliced (n / 4 elements at n / 2 replaced by 1, 2, 3, then
 * the length and sum checked against the removed part: [3,6] at every size), eval.string.join (L);
 * eval.string.concat-reduce (acc + r.id + ",", which + extends in place:
 * STRING_APPEND, L); eval.string.append-let (let s = acc; s += r.id;
 * s += ",": each += moves the let to +, MUTATION, L); eval.string.padStart (text, the
 * row ids joined by commas, padded to twice its length with "ab") and
 * eval.string.repeat ("r000000,".repeat(n)) and eval.string.replaceAll
 * (text.replaceAll(",", ";;")) and eval.string.encodeURIComponent
 * (encodeURIComponent(text), each comma as %2C), each checked by its
 * length and its ends (L);
 * eval.regex.test (/^(?:r\d{6},)*r\d{6}$/ and /r\d{6};/ on text: [true,false]),
 * .replace (text.replace(/r(\d+)/g, "$1"), checked by its length and start),
 * .split (text.split(/[,;]/): n pieces) and .redos (/(a+)+$/ on n a's and a
 * b: false), each the same value at every size, Node's at the smoke size, and
 * .exec (each row's id matched with two groups, one named, and the named
 * group read from the match result: n strings) and .filter (the rows whose
 * id a literal pattern tests true, r => /^r\d{5}[05]$/.test(r.id): n / 5
 * rows) and .literal (/;r\d+/ and /Qr\d/i on text, whose first character
 * never occurs there: [false,false]) and .test.many (every row's id and a
 * comma, 32 times, tested by /^(?:r\d{5}[05],)+$/, which the cache of a
 * test takes, and by a pattern of 25 classes, which it took only from 32:
 * the ids ending in 0 or 5, and those ending in 5 below r100000, each count
 * times 5 or 10 less n: [0,0]) (L);
 * eval.object.spread-reduce (C);
 * eval.array.spread-reduce ([...acc, r.id]) and eval.array.concat-reduce
 * (acc.concat([r.kwh])), linear now that the accumulator is appended to in
 * place and quadratic before, eval.object.fromEntries, eval.set.new, .has,
 * .union, .intersection, .difference (Q). Mutation (docs/mutation.md, L):
 * eval.array.push-reduce (acc.push(r.kwh) into reduce's accumulator),
 * eval.array.sort and .sort.rows (a copy of xs or rows sorted in place, as
 * .toSorted and .toSorted.rows sort), eval.set.add-reduce (s.add(id)),
 * eval.map.set-reduce (m.set(r.id, r.kwh), the reduce idiom);
 * eval.object.assign-reduce (acc[r.id] = r.kwh) and eval.array.assign-reduce
 * (acc[acc.length] = r.kwh), assignment into reduce's accumulator, and
 * eval.array.assign-index (acc[i] = x * 2 + 1 over a copy of xs, as map);
 * eval.let.counter (let n = a; n += r.kwh), eval.array.reduce.key with a let;
 * eval.set.delete-reduce (s.delete(id) of every member of new Set(ids), each
 * the first: Q) and .last (each the last, over ids.toReversed());
 * eval.object.delete-reduce (delete acc[r.id] of every key of an object of
 * the rows' ids, each the first: Q) and .last (each the last);
 * eval.array.splice (n / 4 elements of a copy of xs at n / 2 replaced by 1,
 * 2, 3 in place, as .toSpliced does: 3 at every size) and .splice-reduce
 * (acc.splice(i, 1, x * 2 + 1) over a copy of xs, as map);
 * eval.object.push-path-reduce (acc.groups[r.priority].push(r.kwh), the
 * rows grouped into arrays under the accumulator's keys: a method through a
 * path, MUTATION); eval.number.parse (texts.map(Number), FIX_NUMBERS's doubles
 * as "%.15g", which Clinger's fast path reads) and .parse.17 (as "%.17g",
 * which the bignum reads, back to the doubles themselves) (L);
 * eval.bigint.sum-reduce (1n to n added in reduce: L) and eval.bigint.factorial
 * (1n to n multiplied, n! of up to 118,000 bits: Q); eval.map.get (each row's
 * kwh looked up by its id in a map of every row's, built through the API: L)
 * and eval.map.groupBy (the rows by r => r.priority, the PLUCK path, as
 * eval.object.groupBy.pluck: L); eval.json.parse (the rows' text read) and
 * eval.json.stringify (the rows written as text) (L).
 *
 * write (unit byte): write.rows (n {1000, 10000}, 64 KiB buffer),
 * write.rows.buf64 (10000 rows, a 64-byte buffer), write.numbers (100000
 * doubles), write.strings (10000 strings with quotes, newlines and
 * non-ASCII text).
 *
 * vm and values: vm.fresh, vm.reuse, vm.reset (unit op, the program `1`);
 * vm.fuel.1, .16, .1024 (eval.array.map at 10000 on that much fuel per
 * sjs_vm_run; slicesPerOp); values.rows (L) and values.set (Q), built
 * through the public API and released.
 *
 * Memory comes from a counting allocator under the context and the parser,
 * measured in one untimed op: setupBytes (live after setup), peakBytes (the
 * op's transient peak), heldBytes (live while the op holds its result),
 * allocsPerOp, freesPerOp, allocatedBytesPerOp, and retainedBytes, which
 * must be 0. Time: a warm-up, a calibration that doubles the iterations
 * until a sample takes --sample-ms and holds at least 8 ops (or takes ten
 * times --sample-ms), then --samples samples; the median ns/op
 * is the headline, with the best, p25, p75 and spread = (p75 - p25)/median.
 *
 *   bench [--jsonl] [--only=NAME[,NAME]] [--size=N] [--filter=PREFIX]
 *         [--samples=15] [--sample-ms=10] [--warmup-ms=100] [--max-case-s=10]
 *         [--quick] [--smoke] [--list]
 *
 * Exit status: 0; 1 when a case failed (a wrong value, a leak, a fault; a
 * case whose setup the library refuses, as an older build refuses newer
 * syntax, prints an error record and fails only --smoke); 2 on a usage error.
 * Run it from staticjs/: it reads bench/modules/.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "staticjs.h"

/* Every op folds what it saw in here, so no result is unused. */
static volatile uint64_t bench_sink;

/* ------------------------------------------------------------------------
 * The counting allocator
 */

typedef struct {
    size_t live;
    size_t peak;
    uint64_t allocs;
    uint64_t frees;
    uint64_t allocated;
} bench_heap;

static void *heap_alloc(void *self, size_t size, size_t alignment) {
    bench_heap *heap = self;
    void *ptr = NULL;
    if (alignment > _Alignof(max_align_t)) {
        if (posix_memalign(&ptr, alignment, size) != 0) return NULL;
    } else {
        ptr = malloc(size);
        if (ptr == NULL) return NULL;
    }
    heap->live += size;
    if (heap->live > heap->peak) heap->peak = heap->live;
    heap->allocs++;
    heap->allocated += size;
    return ptr;
}

static void heap_free(void *self, const sjs_allocator_free_params *params) {
    bench_heap *heap = self;
    heap->live -= params->size;
    heap->frees++;
    free(params->ptr);
}

static const sjs_allocator_vtable heap_vtable = { heap_alloc, heap_free };

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

static uint64_t xorshift(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

#define BENCH_SEED 0x9E3779B97F4A7C15ull

/* ------------------------------------------------------------------------
 * Growable text, for sources and written values
 */

typedef struct {
    char *ptr;
    size_t length;
    size_t capacity;
} bench_text;

static void text_append(bench_text *text, const char *bytes, size_t length) {
    if (text->length + length + 1 > text->capacity) {
        size_t capacity = text->capacity < 256 ? 256 : text->capacity;
        while (capacity < text->length + length + 1) capacity *= 2;
        char *grown = realloc(text->ptr, capacity);
        if (grown == NULL) {
            fputs("bench: out of memory\n", stderr);
            exit(1);
        }
        text->ptr = grown;
        text->capacity = capacity;
    }
    memcpy(text->ptr + text->length, bytes, length);
    text->length += length;
    text->ptr[text->length] = '\0';
}

static void text_free(bench_text *text) {
    free(text->ptr);
    *text = (bench_text){ 0 };
}

/* ------------------------------------------------------------------------
 * Cases
 */

typedef enum { UNIT_BYTE, UNIT_EVAL, UNIT_ELEMENT, UNIT_OP } bench_unit;

static const char *const unit_names[] = { "byte", "eval", "element", "op" };

typedef enum { OP_PARSE, OP_COMPILE, OP_EVAL, OP_WRITE, OP_VM_FRESH, OP_VM_RESET, OP_VALUES_ROWS, OP_VALUES_SET } bench_op;

/* Where a case's source comes from. */
typedef enum { TEXT_INLINE, TEXT_POWER, TEXT_CAMPAIGN, TEXT_SYNTHETIC } bench_text_kind;

/* The inputs a case binds as globals. */
typedef enum {
    FIX_NONE = 0,
    FIX_LITERAL,       /* evaluated from `literal` */
    FIX_XS,
    FIX_ROWS,
    FIX_IDS,
    FIX_SET_A,
    FIX_SET_B,
    FIX_PENDING_HALF,  /* the ids of the even rows */
    FIX_NUMBERS,
    FIX_STRINGS,
    FIX_TEXT,          /* the row ids joined by commas, 8n - 1 bytes */
    FIX_WATERFILL,     /* the function from power's namespace */
    FIX_CAPACITY,
    FIX_DEMAND,
    FIX_NUMBER_TEXTS,    /* FIX_NUMBERS as "%.15g": Clinger's fast path reads each */
    FIX_NUMBER_TEXTS_17, /* FIX_NUMBERS as "%.17g": each reads back as it was, past the fast path */
    FIX_BIGINTS,         /* 1n to n as BigInts */
    FIX_ID_MAP,          /* each row's id to its kwh, a map */
    FIX_ROWS_JSON        /* the rows as JSON.stringify writes them */
} bench_fixture;

typedef struct {
    const char *name;
    bench_fixture fixture;
    const char *literal;
} bench_global;

/* What a result is checked for at a size other than the smoke size. */
typedef enum {
    CHECK_NONE = 0,
    CHECK_ARRAY_N,
    CHECK_FILTER_QUARTER,
    CHECK_FILTER_FIFTH,  /* an array of n / 5 elements */
    CHECK_SUM_XS,
    CHECK_SUM_KWH,
    CHECK_PIPELINE,
    CHECK_SORTED,
    CHECK_SORTED_DESC,
    CHECK_SAME,
    CHECK_JOIN,
    CHECK_OBJECT_N,
    CHECK_SET_N,
    CHECK_HAS,
    CHECK_UNION,
    CHECK_INTERSECTION,
    CHECK_DIFFERENCE,
    CHECK_SYNTHETIC,
    CHECK_IDS_JOINED,   /* STRING_APPEND: every row's id and a comma, 8n bytes */
    CHECK_BIGINT_SUM,   /* n(n + 1) / 2 as a BigInt */
    CHECK_FACTORIAL,    /* n! as a BigInt: its digits, counted */
    CHECK_MAP_N,        /* MUTATION: a map of n entries */
    CHECK_ROWS_JSON     /* the rows as JSON.stringify writes them */
} bench_check;

#define MAX_GLOBALS 4

typedef struct {
    const char *name;
    bench_unit unit;
    bench_op op;
    const uint32_t *sizes;   /* 0-terminated; NULL: one size, the source's bytes */
    uint32_t smoke;          /* n of the smoke run, for sized cases */
    bench_text_kind text;
    const char *source;      /* TEXT_INLINE */
    sjs_goal goal;
    uint32_t chunk;          /* bytes per push; 0: whole */
    uint64_t fuel;           /* fuel per sjs_vm_run; 0: unlimited */
    uint32_t buffer;         /* write: the buffer's bytes */
    bench_global globals[MAX_GLOBALS];
    const char *expect;      /* the result as written, at the smoke size */
    bench_check check;
} bench_case;

static const uint32_t SIZES_ONE[] = { 1, 0 };
static const uint32_t SIZES_L[] = { 1000, 10000, 100000, 0 };
static const uint32_t SIZES_Q[] = { 100, 1000, 10000, 0 };
static const uint32_t SIZES_C[] = { 100, 300, 1000, 0 };
static const uint32_t SIZES_PORTS[] = { 3, 30, 300, 0 };
static const uint32_t SIZES_WRITE_ROWS[] = { 1000, 10000, 0 };
static const uint32_t SIZES_10000[] = { 10000, 0 };
static const uint32_t SIZES_100000[] = { 100000, 0 };

#define GUARD_EVENT "pending.has(_event.data.id) && _event.name === \"notify.ack\""
#define GUARD_COUNTER "({...counts, acked: counts.acked + 1})"
#define MAP_XS "xs.map(x => x * 2 + 1)"

#define EVENT_GLOBALS                                                                  \
    { { "_event", FIX_LITERAL, "{name: \"notify.ack\", data: {id: \"n2\"}}" },        \
      { "pending", FIX_LITERAL, "new Set([\"n1\", \"n2\"])" } }
#define COUNTS_GLOBALS { { "counts", FIX_LITERAL, "{sent: 4, acked: 1, rejected: 0, failed: 0}" } }

#define POWER_VALUE "{\"example\":{\"c\":50,\"b\":5,\"a\":5},\"fill\":<function>,\"waterFill\":<function>}"
#define COUNTER_VALUE "{\"sent\":4,\"acked\":2,\"rejected\":0,\"failed\":0}"
#define MAP_VALUE "[6,2,5,8,9,3,4,7,1,10]"
#define ROWS_VALUE                                                                                          \
    "[{\"id\":\"r000000\",\"kwh\":0,\"priority\":0,\"tags\":[\"a\",\"b\"]},"                                \
    "{\"id\":\"r000001\",\"kwh\":3.7,\"priority\":1,\"tags\":[\"a\",\"b\"]},"                               \
    "{\"id\":\"r000002\",\"kwh\":7.4,\"priority\":2,\"tags\":[\"a\",\"b\"]},"                               \
    "{\"id\":\"r000003\",\"kwh\":11.1,\"priority\":3,\"tags\":[\"a\",\"b\"]},"                              \
    "{\"id\":\"r000004\",\"kwh\":14.8,\"priority\":0,\"tags\":[\"a\",\"b\"]},"                              \
    "{\"id\":\"r000005\",\"kwh\":18.5,\"priority\":1,\"tags\":[\"a\",\"b\"]},"                              \
    "{\"id\":\"r000006\",\"kwh\":22.2,\"priority\":2,\"tags\":[\"a\",\"b\"]},"                              \
    "{\"id\":\"r000007\",\"kwh\":25.9,\"priority\":3,\"tags\":[\"a\",\"b\"]},"                              \
    "{\"id\":\"r000008\",\"kwh\":29.6,\"priority\":0,\"tags\":[\"a\",\"b\"]},"                              \
    "{\"id\":\"r000009\",\"kwh\":33.3,\"priority\":1,\"tags\":[\"a\",\"b\"]}]"

#define ROWS_JSON_TEXT \
    "\"[{\\\"id\\\":\\\"r000000\\\",\\\"kwh\\\":0,\\\"priority\\\":0,\\\"tags\\\":[\\\"a\\\",\\\"b\\\"]}," \
    "{\\\"id\\\":\\\"r000001\\\",\\\"kwh\\\":3.7,\\\"priority\\\":1,\\\"tags\\\":[\\\"a\\\",\\\"b\\\"]}," \
    "{\\\"id\\\":\\\"r000002\\\",\\\"kwh\\\":7.4,\\\"priority\\\":2,\\\"tags\\\":[\\\"a\\\",\\\"b\\\"]}," \
    "{\\\"id\\\":\\\"r000003\\\",\\\"kwh\\\":11.1,\\\"priority\\\":3,\\\"tags\\\":[\\\"a\\\",\\\"b\\\"]}," \
    "{\\\"id\\\":\\\"r000004\\\",\\\"kwh\\\":14.8,\\\"priority\\\":0,\\\"tags\\\":[\\\"a\\\",\\\"b\\\"]}," \
    "{\\\"id\\\":\\\"r000005\\\",\\\"kwh\\\":18.5,\\\"priority\\\":1,\\\"tags\\\":[\\\"a\\\",\\\"b\\\"]}," \
    "{\\\"id\\\":\\\"r000006\\\",\\\"kwh\\\":22.2,\\\"priority\\\":2,\\\"tags\\\":[\\\"a\\\",\\\"b\\\"]}," \
    "{\\\"id\\\":\\\"r000007\\\",\\\"kwh\\\":25.9,\\\"priority\\\":3,\\\"tags\\\":[\\\"a\\\",\\\"b\\\"]}," \
    "{\\\"id\\\":\\\"r000008\\\",\\\"kwh\\\":29.6,\\\"priority\\\":0,\\\"tags\\\":[\\\"a\\\",\\\"b\\\"]}," \
    "{\\\"id\\\":\\\"r000009\\\",\\\"kwh\\\":33.3,\\\"priority\\\":1,\\\"tags\\\":[\\\"a\\\",\\\"b\\\"]}]\""

#define ROWS_VALUE_DESC                                                                                     \
    "[{\"id\":\"r000009\",\"kwh\":33.3,\"priority\":1,\"tags\":[\"a\",\"b\"]},"                               \
    "{\"id\":\"r000008\",\"kwh\":29.6,\"priority\":0,\"tags\":[\"a\",\"b\"]},"                                \
    "{\"id\":\"r000007\",\"kwh\":25.9,\"priority\":3,\"tags\":[\"a\",\"b\"]},"                                \
    "{\"id\":\"r000006\",\"kwh\":22.2,\"priority\":2,\"tags\":[\"a\",\"b\"]},"                                \
    "{\"id\":\"r000005\",\"kwh\":18.5,\"priority\":1,\"tags\":[\"a\",\"b\"]},"                                \
    "{\"id\":\"r000004\",\"kwh\":14.8,\"priority\":0,\"tags\":[\"a\",\"b\"]},"                                \
    "{\"id\":\"r000003\",\"kwh\":11.1,\"priority\":3,\"tags\":[\"a\",\"b\"]},"                                \
    "{\"id\":\"r000002\",\"kwh\":7.4,\"priority\":2,\"tags\":[\"a\",\"b\"]},"                                 \
    "{\"id\":\"r000001\",\"kwh\":3.7,\"priority\":1,\"tags\":[\"a\",\"b\"]},"                                 \
    "{\"id\":\"r000000\",\"kwh\":0,\"priority\":0,\"tags\":[\"a\",\"b\"]}]"

static const bench_case cases[] = {
    /* compile */
    { .name = "parse.module.power.whole", .unit = UNIT_BYTE, .op = OP_PARSE, .text = TEXT_POWER,
      .goal = SJS_GOAL_MODULE },
    { .name = "compile.module.power.whole", .unit = UNIT_BYTE, .op = OP_COMPILE, .text = TEXT_POWER,
      .goal = SJS_GOAL_MODULE, .expect = POWER_VALUE },
    { .name = "compile.module.power.chunk64", .unit = UNIT_BYTE, .op = OP_COMPILE, .text = TEXT_POWER,
      .goal = SJS_GOAL_MODULE, .chunk = 64, .expect = POWER_VALUE },
    { .name = "compile.module.power.chunk1", .unit = UNIT_BYTE, .op = OP_COMPILE, .text = TEXT_POWER,
      .goal = SJS_GOAL_MODULE, .chunk = 1, .expect = POWER_VALUE },
    { .name = "compile.module.campaign.whole", .unit = UNIT_BYTE, .op = OP_COMPILE, .text = TEXT_CAMPAIGN,
      .goal = SJS_GOAL_MODULE,
      .expect = "{\"check\":4,\"drop\":<function>,\"take\":<function>,\"tracker\":<function>,\"unsent\":<function>,"
                "\"without\":<function>}" },
    { .name = "compile.module.synthetic.whole", .unit = UNIT_BYTE, .op = OP_COMPILE, .text = TEXT_SYNTHETIC,
      .goal = SJS_GOAL_MODULE,
      .expect = "{\"example_0\":{\"c\":50,\"b\":5,\"a\":5},\"fill_0\":<function>,\"waterFill_0\":<function>}",
      .check = CHECK_SYNTHETIC },
    { .name = "compile.module.synthetic.chunk64", .unit = UNIT_BYTE, .op = OP_COMPILE, .text = TEXT_SYNTHETIC,
      .goal = SJS_GOAL_MODULE, .chunk = 64,
      .expect = "{\"example_0\":{\"c\":50,\"b\":5,\"a\":5},\"fill_0\":<function>,\"waterFill_0\":<function>}",
      .check = CHECK_SYNTHETIC },
    { .name = "compile.guard.event.whole", .unit = UNIT_BYTE, .op = OP_COMPILE, .source = GUARD_EVENT,
      .globals = EVENT_GLOBALS, .expect = "true" },
    { .name = "compile.guard.event.chunk1", .unit = UNIT_BYTE, .op = OP_COMPILE, .source = GUARD_EVENT, .chunk = 1,
      .globals = EVENT_GLOBALS, .expect = "true" },
    { .name = "compile.guard.counter.whole", .unit = UNIT_BYTE, .op = OP_COMPILE, .source = GUARD_COUNTER,
      .globals = COUNTS_GLOBALS, .expect = COUNTER_VALUE },
    /* Lexing, classifying and compiling patterns; evaluated only to check them. */
    { .name = "compile.regex", .unit = UNIT_BYTE, .op = OP_COMPILE,
      .source = "[/^[\\w.+-]+@[\\w-]+(?:\\.[\\w-]+)+$/.test(\"ada.lovelace+notes@example.org\"), "
                "/^\\d{4}-\\d{2}-\\d{2}(?:T\\d{2}:\\d{2}(?::\\d{2}(?:\\.\\d+)?)?(?:Z|[+-]\\d{2}:\\d{2})?)?$/"
                ".test(\"2024-02-29T13:45:00.250Z\"), "
                "/^(?<major>0|[1-9]\\d*)\\.(?<minor>0|[1-9]\\d*)\\.(?<patch>0|[1-9]\\d*)$/.test(\"1.20.03\"), "
                "/^#?(?:[0-9a-f]{3}|[0-9a-f]{6})$/i.test(\"#C0FFEE\"), "
                "/^https?:\\/\\/[^\\s\\/$.?#][^\\s]*$/i.test(\"https://example.org/a?b=c#d\"), "
                "/^(?:\\+?\\d{1,3}[ -]?)?\\(?\\d{3}\\)?[ -]?\\d{3}[ -]?\\d{4}$/.test(\"+1 (555) 010-4477\"), "
                "/^[a-z][a-z0-9_]*(?:\\.[a-z][a-z0-9_]*)*$/.test(\"notify.ack\"), "
                "/^notify\\.(?:ack|nack|timeout)$/.test(\"notify.retry\"), "
                "/^[A-Z]{2}\\d{2}[A-Z0-9]{11,30}$/.test(\"DE89370400440532013000\"), "
                "/^\\s*$/.test(\" \\t \"), "
                "/\\b(?:error|fail(?:ed|ure)?)\\b/i.test(\"Job FAILED at step 3\"), "
                "/^[^,]*,[^,]*$/.test(\"a,b,c\")]",
      .expect = "[true,true,false,true,true,true,true,false,true,true,true,false]" },

    /* eval: the data model's hot path */
    { .name = "eval.guard.size", .unit = UNIT_EVAL, .op = OP_EVAL, .sizes = SIZES_ONE, .smoke = 1,
      .source = "pending.size === 0", .globals = { { "pending", FIX_LITERAL, "new Set()" } }, .expect = "true" },
    { .name = "eval.guard.event", .unit = UNIT_EVAL, .op = OP_EVAL, .sizes = SIZES_ONE, .smoke = 1,
      .source = GUARD_EVENT, .globals = EVENT_GLOBALS, .expect = "true" },
    { .name = "eval.guard.window", .unit = UNIT_EVAL, .op = OP_EVAL, .sizes = SIZES_ONE, .smoke = 1,
      .source = "batch.filter(n => pending.has(n.id)).slice(0, windowSize).map(n => n.id)",
      .globals = { { "batch", FIX_LITERAL, "[{id: \"a\"}, {id: \"b\"}, {id: \"c\"}, {id: \"d\"}, {id: \"e\"}]" },
                   { "pending", FIX_LITERAL, "new Set([\"a\", \"c\", \"d\", \"e\"])" },
                   { "windowSize", FIX_LITERAL, "2" } },
      .expect = "[\"a\",\"c\"]" },
    { .name = "eval.assign.counter", .unit = UNIT_EVAL, .op = OP_EVAL, .sizes = SIZES_ONE, .smoke = 1,
      .source = GUARD_COUNTER, .globals = COUNTS_GLOBALS, .expect = COUNTER_VALUE },
    { .name = "eval.send.target", .unit = UNIT_EVAL, .op = OP_EVAL, .sizes = SIZES_ONE, .smoke = 1,
      .source = "`#_actor_progress/${campaignID}`", .globals = { { "campaignID", FIX_LITERAL, "\"c-42\"" } },
      .expect = "\"#_actor_progress/c-42\"" },
    { .name = "eval.module.power", .unit = UNIT_EVAL, .op = OP_EVAL, .sizes = SIZES_ONE, .smoke = 1,
      .text = TEXT_POWER, .goal = SJS_GOAL_MODULE, .expect = POWER_VALUE },
    { .name = "eval.call.waterFill", .unit = UNIT_EVAL, .op = OP_EVAL, .sizes = SIZES_PORTS, .smoke = 3,
      .source = "waterFill(capacity, demand)",
      .globals = { { "waterFill", FIX_WATERFILL, NULL }, { "capacity", FIX_CAPACITY, NULL },
                   { "demand", FIX_DEMAND, NULL } },
      .expect = "{\"p002\":15,\"p001\":3,\"p000\":0}", .check = CHECK_OBJECT_N },

    /* eval: heavier work */
    { .name = "eval.array.map", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = MAP_XS, .globals = { { "xs", FIX_XS, NULL } }, .expect = MAP_VALUE, .check = CHECK_ARRAY_N },
    { .name = "eval.array.filter", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.filter(r => r.priority === 2)", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "[{\"id\":\"r000002\",\"kwh\":7.4,\"priority\":2,\"tags\":[\"a\",\"b\"]},"
                "{\"id\":\"r000006\",\"kwh\":22.2,\"priority\":2,\"tags\":[\"a\",\"b\"]}]",
      .check = CHECK_FILTER_QUARTER },
    { .name = "eval.array.reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "xs.reduce((a, x) => a + x, 0)", .globals = { { "xs", FIX_XS, NULL } }, .expect = "22.5",
      .check = CHECK_SUM_XS },
    { .name = "eval.array.reduce.key", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.reduce((a, r) => a + r.kwh, 0)", .globals = { { "rows", FIX_ROWS, NULL } }, .expect = "166.5",
      .check = CHECK_SUM_KWH },
    { .name = "eval.array.pipeline", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.filter(r => r.priority > 1).map(r => r.kwh).reduce((a, k) => a + k, 0)",
      .globals = { { "rows", FIX_ROWS, NULL } }, .expect = "66.6", .check = CHECK_PIPELINE },
    { .name = "eval.array.toSorted", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "xs.toSorted((a, b) => a - b)", .globals = { { "xs", FIX_XS, NULL } },
      .expect = "[0,0.5,1,1.5,2,2.5,3,3.5,4,4.5]", .check = CHECK_SORTED },
    { .name = "eval.array.toSorted.desc", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "xs.toSorted((a, b) => b - a)", .globals = { { "xs", FIX_XS, NULL } },
      .expect = "[4.5,4,3.5,3,2.5,2,1.5,1,0.5,0]", .check = CHECK_SORTED_DESC },
    { .name = "eval.array.toSorted.rows", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.toSorted((a, b) => a.kwh - b.kwh)", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = ROWS_VALUE, .check = CHECK_ARRAY_N },
    { .name = "eval.array.toSorted.rows.desc", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.toSorted((a, b) => b.kwh - a.kwh)", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = ROWS_VALUE_DESC, .check = CHECK_ARRAY_N },
    { .name = "eval.array.map.pluck", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.map(r => r.kwh)", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "[0,3.7,7.4,11.1,14.8,18.5,22.2,25.9,29.6,33.3]", .check = CHECK_ARRAY_N },
    { .name = "eval.array.from.pluck", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "Array.from(rows, r => r.kwh)", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "[0,3.7,7.4,11.1,14.8,18.5,22.2,25.9,29.6,33.3]", .check = CHECK_ARRAY_N },
    { .name = "eval.array.from.call", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "Array.from(rows, (r, i) => r.kwh)", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "[0,3.7,7.4,11.1,14.8,18.5,22.2,25.9,29.6,33.3]", .check = CHECK_ARRAY_N },
    { .name = "eval.object.groupBy.pluck", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "Object.keys(Object.groupBy(rows, r => r.priority)).join()", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "\"0,1,2,3\"", .check = CHECK_SAME },
    { .name = "eval.object.groupBy.call", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "Object.keys(Object.groupBy(rows, r => String(r.priority))).join()",
      .globals = { { "rows", FIX_ROWS, NULL } }, .expect = "\"0,1,2,3\"", .check = CHECK_SAME },
    { .name = "eval.array.every", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "xs.every(x => x >= 0)", .globals = { { "xs", FIX_XS, NULL } }, .expect = "true", .check = CHECK_SAME },
    { .name = "eval.array.some", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.some(r => r.kwh < 0)", .globals = { { "rows", FIX_ROWS, NULL } }, .expect = "false",
      .check = CHECK_SAME },
    { .name = "eval.array.findIndex", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.findIndex(r => r.id === \"none\")", .globals = { { "rows", FIX_ROWS, NULL } }, .expect = "-1",
      .check = CHECK_SAME },
    { .name = "eval.array.findLastIndex", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.findLastIndex(r => r.kwh < 0)", .globals = { { "rows", FIX_ROWS, NULL } }, .expect = "-1",
      .check = CHECK_SAME },
    { .name = "eval.array.lastIndexOf", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "xs.lastIndexOf(-1)", .globals = { { "xs", FIX_XS, NULL } }, .expect = "-1", .check = CHECK_SAME },
    { .name = "eval.array.toSpliced", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "((a, b, sum) => (r => [r.length - xs.length + b, sum(r) - sum(xs) + sum(xs.slice(a, a + b))])"
                "(xs.toSpliced(a, b, 1, 2, 3)))(Math.floor(xs.length / 2), Math.floor(xs.length / 4), "
                "v => v.reduce((s, x) => s + x, 0))",
      .globals = { { "xs", FIX_XS, NULL } }, .expect = "[3,6]", .check = CHECK_SAME },
    { .name = "eval.string.join", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.map(r => r.id).join(\",\")", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "\"r000000,r000001,r000002,r000003,r000004,r000005,r000006,r000007,r000008,r000009\"",
      .check = CHECK_JOIN },
    /* STRING_APPEND: + extends the accumulator, which the callback holds alone. */
    { .name = "eval.string.concat-reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.reduce((acc, r) => acc + r.id + \",\", \"\")", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "\"r000000,r000001,r000002,r000003,r000004,r000005,r000006,r000007,r000008,r000009,\"",
      .check = CHECK_IDS_JOINED },
    /* MUTATION: appending to a let, s += ..., moves it to +, which extends it. */
    { .name = "eval.string.append-let", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.reduce((acc, r) => { let s = acc; s += r.id; s += \",\"; return s; }, \"\")",
      .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "\"r000000,r000001,r000002,r000003,r000004,r000005,r000006,r000007,r000008,r000009,\"",
      .check = CHECK_IDS_JOINED },
    /* A string built by a method: padded to twice its length, repeated n
     * times, or with each of its n - 1 commas replaced. */
    { .name = "eval.string.padStart", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "(p => [p.length - 2 * text.length, p.startsWith(\"abab\"), p.endsWith(text)])"
                "(text.padStart(2 * text.length, \"ab\"))",
      .globals = { { "text", FIX_TEXT, NULL } }, .expect = "[0,true,true]", .check = CHECK_SAME },
    { .name = "eval.string.repeat", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "(r => [r.length - 8 * xs.length, r.endsWith(\"r000000,r000000,\")])(\"r000000,\".repeat(xs.length))",
      .globals = { { "xs", FIX_XS, NULL } }, .expect = "[0,true]", .check = CHECK_SAME },
    { .name = "eval.string.replaceAll", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "(r => [r.length - text.length - Math.floor(text.length / 8), r.startsWith(\"r000000;;r000001;;\")])"
                "(text.replaceAll(\",\", \";;\"))",
      .globals = { { "text", FIX_TEXT, NULL } }, .expect = "[0,true]", .check = CHECK_SAME },
    /* Each of the n - 1 commas escaped as %2C, every byte read. */
    { .name = "eval.string.encodeURIComponent", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "(r => [r.length - text.length - 2 * Math.floor(text.length / 8), r.startsWith(\"r000000%2Cr000001%2C\")])"
                "(encodeURIComponent(text))",
      .globals = { { "text", FIX_TEXT, NULL } }, .expect = "[0,true]", .check = CHECK_SAME },
    /* Regular expressions (docs/regex.md, 12.1), on the n row ids joined by
     * commas: a whole-string test, a replacement with g and $1, a split on
     * a class, and /(a+)+$/ on n a's and a b, exponential for a
     * backtracker and linear here. */
    { .name = "eval.regex.test", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "[/^(?:r\\d{6},)*r\\d{6}$/.test(text), /r\\d{6};/.test(text)]",
      .globals = { { "text", FIX_TEXT, NULL } }, .expect = "[true,false]", .check = CHECK_SAME },
    { .name = "eval.regex.replace", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "(r => [8 * r.length - 7 * text.length, r.startsWith(\"000000,000001,\")])(text.replace(/r(\\d+)/g, \"$1\"))",
      .globals = { { "text", FIX_TEXT, NULL } }, .expect = "[-1,true]", .check = CHECK_SAME },
    { .name = "eval.regex.split", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "(p => [8 * p.length - text.length, p[0], p[p.length - 1].length])(text.split(/[,;]/))",
      .globals = { { "text", FIX_TEXT, NULL } }, .expect = "[1,\"r000000\",7]", .check = CHECK_SAME },
    { .name = "eval.regex.exec", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.map(r => /^r(\\d{3})(?<lo>\\d{3})$/.exec(r.id).groups.lo)", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "[\"000\",\"001\",\"002\",\"003\",\"004\",\"005\",\"006\",\"007\",\"008\",\"009\"]",
      .check = CHECK_ARRAY_N },
    { .name = "eval.regex.filter", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.filter(r => /^r\\d{5}[05]$/.test(r.id))", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "[{\"id\":\"r000000\",\"kwh\":0,\"priority\":0,\"tags\":[\"a\",\"b\"]},"
                "{\"id\":\"r000005\",\"kwh\":18.5,\"priority\":1,\"tags\":[\"a\",\"b\"]}]",
      .check = CHECK_FILTER_FIFTH },
    { .name = "eval.regex.literal", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "[/;r\\d+/.test(text), /Qr\\d/i.test(text)]",
      .globals = { { "text", FIX_TEXT, NULL } }, .expect = "[false,false]", .check = CHECK_SAME },
    { .name = "eval.regex.redos", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "/(a+)+$/.test(\"a\".repeat(xs.length) + \"b\")",
      .globals = { { "xs", FIX_XS, NULL } }, .expect = "false", .check = CHECK_SAME },
    /* Many tests over medium strings (each row's id and a comma, 32 times:
     * long enough to run as loops), by one pattern of 7 classes and one of
     * 25, which the cache took only from 32: what each call pays besides
     * the search. */
    { .name = "eval.regex.test.many", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "(ls => [ls.filter(s => /^(?:r\\d{5}[05],)+$/.test(s)).length * 5 - ls.length,"
                " ls.filter(s => /,r(?:a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|0)\\d{4}5,/.test(s)).length * 10 - ls.length])"
                "(rows.map(r => (r.id + \",\").repeat(32)))",
      .globals = { { "rows", FIX_ROWS, NULL } }, .expect = "[0,0]", .check = CHECK_SAME },
    /* Strings read as numbers: 15 digits (Clinger's fast path), 17 (the bignum's). */
    { .name = "eval.number.parse", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "texts.map(Number)", .globals = { { "texts", FIX_NUMBER_TEXTS, NULL } },
      .expect = "[859.794120780817,394.301338356337,480.587874049492,188.949885827746,175.462179033066,"
                "590.089667194192,604.780272776143,850.257467037368,865.879924430228,560.770620425737]",
      .check = CHECK_ARRAY_N },
    { .name = "eval.number.parse.17", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "texts.map(Number)", .globals = { { "texts", FIX_NUMBER_TEXTS_17, NULL } },
      .expect = "[859.7941207808166,394.3013383563367,480.58787404949175,188.94988582774553,175.46217903306626,"
                "590.0896671941921,604.7802727761426,850.2574670373677,865.8799244302276,560.7706204257373]",
      .check = CHECK_ARRAY_N },
    { .name = "eval.object.spread-reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_C, .smoke = 10,
      .source = "rows.reduce((acc, r) => ({ ...acc, [r.id]: r.kwh }), {})", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "{\"r000000\":0,\"r000001\":3.7,\"r000002\":7.4,\"r000003\":11.1,\"r000004\":14.8,\"r000005\":18.5,"
                "\"r000006\":22.2,\"r000007\":25.9,\"r000008\":29.6,\"r000009\":33.3}",
      .check = CHECK_OBJECT_N },
    { .name = "eval.array.spread-reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_Q, .smoke = 10,
      .source = "rows.reduce((acc, r) => [...acc, r.id], [])", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "[\"r000000\",\"r000001\",\"r000002\",\"r000003\",\"r000004\",\"r000005\",\"r000006\",\"r000007\","
                "\"r000008\",\"r000009\"]",
      .check = CHECK_ARRAY_N },
    { .name = "eval.array.concat-reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_Q, .smoke = 10,
      .source = "rows.reduce((acc, r) => acc.concat([r.kwh]), [])", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "[0,3.7,7.4,11.1,14.8,18.5,22.2,25.9,29.6,33.3]", .check = CHECK_ARRAY_N },
    /* MUTATION */
    { .name = "eval.array.push-reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.reduce((acc, r) => { acc.push(r.kwh); return acc; }, [])", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "[0,3.7,7.4,11.1,14.8,18.5,22.2,25.9,29.6,33.3]", .check = CHECK_ARRAY_N },
    { .name = "eval.array.sort", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "(ys => { ys.sort((a, b) => a - b); return ys; })([...xs])", .globals = { { "xs", FIX_XS, NULL } },
      .expect = "[0,0.5,1,1.5,2,2.5,3,3.5,4,4.5]", .check = CHECK_SORTED },
    { .name = "eval.array.sort.rows", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "(ys => { ys.sort((a, b) => a.kwh - b.kwh); return ys; })([...rows])",
      .globals = { { "rows", FIX_ROWS, NULL } }, .expect = ROWS_VALUE, .check = CHECK_ARRAY_N },
    { .name = "eval.set.add-reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "ids.reduce((s, id) => s.add(id), new Set())", .globals = { { "ids", FIX_IDS, NULL } },
      .expect = "new Set([\"r000000\",\"r000001\",\"r000002\",\"r000003\",\"r000004\",\"r000005\",\"r000006\","
                "\"r000007\",\"r000008\",\"r000009\"])",
      .check = CHECK_SET_N },
    /* MUTATION: a map's set, each row's kwh under its id, into reduce's
     * accumulator, as the reduce idiom builds one in place (L). */
    { .name = "eval.map.set-reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.reduce((m, r) => m.set(r.id, r.kwh), new Map())", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "new Map([[\"r000000\",0],[\"r000001\",3.7],[\"r000002\",7.4],[\"r000003\",11.1],[\"r000004\",14.8],"
                "[\"r000005\",18.5],[\"r000006\",22.2],[\"r000007\",25.9],[\"r000008\",29.6],[\"r000009\",33.3]])",
      .check = CHECK_MAP_N },
    /* MUTATION: removal. delete-reduce removes each member while it is the
     * first, so every later one moves (Q); .last removes each while it is
     * the last, which moves none (L). */
    { .name = "eval.set.delete-reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_Q, .smoke = 10,
      .source = "ids.reduce((s, id) => { s.delete(id); return s; }, new Set(ids))", .globals = { { "ids", FIX_IDS, NULL } },
      .expect = "new Set([])", .check = CHECK_SAME },
    { .name = "eval.set.delete-reduce.last", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "ids.toReversed().reduce((s, id) => { s.delete(id); return s; }, new Set(ids))",
      .globals = { { "ids", FIX_IDS, NULL } }, .expect = "new Set([])", .check = CHECK_SAME },
    { .name = "eval.object.delete-reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_Q, .smoke = 10,
      .source = "rows.reduce((acc, r) => { delete acc[r.id]; return acc; }, Object.fromEntries(rows.map(r => [r.id, r.kwh])))",
      .globals = { { "rows", FIX_ROWS, NULL } }, .expect = "{}", .check = CHECK_SAME },
    { .name = "eval.object.delete-reduce.last", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.toReversed().reduce((acc, r) => { delete acc[r.id]; return acc; }, Object.fromEntries(rows.map(r => [r.id, r.kwh])))",
      .globals = { { "rows", FIX_ROWS, NULL } }, .expect = "{}", .check = CHECK_SAME },
    { .name = "eval.array.splice", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "(ys => { const r = ys.splice(Math.floor(ys.length / 2), Math.floor(ys.length / 4), 1, 2, 3); "
                "return r.length + ys.length - xs.length; })([...xs])",
      .globals = { { "xs", FIX_XS, NULL } }, .expect = "3", .check = CHECK_SAME },
    { .name = "eval.array.splice-reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "xs.reduce((acc, x, i) => { acc.splice(i, 1, x * 2 + 1); return acc; }, [...xs])",
      .globals = { { "xs", FIX_XS, NULL } }, .expect = MAP_VALUE, .check = CHECK_ARRAY_N },
    /* MUTATION: assignment */
    { .name = "eval.object.assign-reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.reduce((acc, r) => { acc[r.id] = r.kwh; return acc; }, {})", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "{\"r000000\":0,\"r000001\":3.7,\"r000002\":7.4,\"r000003\":11.1,\"r000004\":14.8,\"r000005\":18.5,"
                "\"r000006\":22.2,\"r000007\":25.9,\"r000008\":29.6,\"r000009\":33.3}",
      .check = CHECK_OBJECT_N },
    { .name = "eval.array.assign-reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.reduce((acc, r) => { acc[acc.length] = r.kwh; return acc; }, [])", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "[0,3.7,7.4,11.1,14.8,18.5,22.2,25.9,29.6,33.3]", .check = CHECK_ARRAY_N },
    { .name = "eval.array.assign-index", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "xs.reduce((acc, x, i) => { acc[i] = x * 2 + 1; return acc; }, [...xs])", .globals = { { "xs", FIX_XS, NULL } },
      .expect = MAP_VALUE, .check = CHECK_ARRAY_N },
    /* MUTATION: methods through a path. Each row's kwh pushed into its
     * priority's array, under the accumulator's keys; priority 2's group. */
    { .name = "eval.object.push-path-reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.reduce((acc, r) => { acc.groups[r.priority].push(r.kwh); return acc; }, "
                "{groups: {0: [], 1: [], 2: [], 3: []}}).groups[2]",
      .globals = { { "rows", FIX_ROWS, NULL } }, .expect = "[7.4,22.2]", .check = CHECK_FILTER_QUARTER },
    /* MUTATION: let */
    { .name = "eval.let.counter", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.reduce((a, r) => { let n = a; n += r.kwh; return n; }, 0)", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "166.5", .check = CHECK_SUM_KWH },
    { .name = "eval.object.fromEntries", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_Q, .smoke = 10,
      .source = "Object.fromEntries(rows.map(r => [r.id, r.kwh]))", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "{\"r000000\":0,\"r000001\":3.7,\"r000002\":7.4,\"r000003\":11.1,\"r000004\":14.8,\"r000005\":18.5,"
                "\"r000006\":22.2,\"r000007\":25.9,\"r000008\":29.6,\"r000009\":33.3}",
      .check = CHECK_OBJECT_N },
    { .name = "eval.set.new", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_Q, .smoke = 10,
      .source = "new Set(ids)", .globals = { { "ids", FIX_IDS, NULL } },
      .expect = "new Set([\"r000000\",\"r000001\",\"r000002\",\"r000003\",\"r000004\",\"r000005\",\"r000006\","
                "\"r000007\",\"r000008\",\"r000009\"])",
      .check = CHECK_SET_N },
    { .name = "eval.set.has", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_Q, .smoke = 10,
      .source = "ids.filter(id => pending.has(id))",
      .globals = { { "ids", FIX_IDS, NULL }, { "pending", FIX_PENDING_HALF, NULL } },
      .expect = "[\"r000000\",\"r000002\",\"r000004\",\"r000004\",\"r000006\",\"r000008\"]", .check = CHECK_HAS },
    { .name = "eval.set.union", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_Q, .smoke = 10,
      .source = "a.union(b)", .globals = { { "a", FIX_SET_A, NULL }, { "b", FIX_SET_B, NULL } },
      .expect = "new Set([\"s000000\",\"s000001\",\"s000002\",\"s000003\",\"s000004\",\"s000005\",\"s000006\","
                "\"s000007\",\"s000008\",\"s000009\",\"s000010\",\"s000011\",\"s000012\",\"s000013\",\"s000014\"])",
      .check = CHECK_UNION },
    { .name = "eval.set.intersection", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_Q, .smoke = 10,
      .source = "a.intersection(b)", .globals = { { "a", FIX_SET_A, NULL }, { "b", FIX_SET_B, NULL } },
      .expect = "new Set([\"s000005\",\"s000006\",\"s000007\",\"s000008\",\"s000009\"])",
      .check = CHECK_INTERSECTION },
    { .name = "eval.set.difference", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_Q, .smoke = 10,
      .source = "a.difference(b)", .globals = { { "a", FIX_SET_A, NULL }, { "b", FIX_SET_B, NULL } },
      .expect = "new Set([\"s000000\",\"s000001\",\"s000002\",\"s000003\",\"s000004\"])",
      .check = CHECK_DIFFERENCE },

    /* Maps: a lookup by each row's id in a map of every row's (L). */
    { .name = "eval.map.get", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "rows.map(r => byID.get(r.id))", .globals = { { "rows", FIX_ROWS, NULL }, { "byID", FIX_ID_MAP, NULL } },
      .expect = "[0,3.7,7.4,11.1,14.8,18.5,22.2,25.9,29.6,33.3]", .check = CHECK_ARRAY_N },
    /* The rows grouped by a pluck, as eval.object.groupBy.pluck (L). */
    { .name = "eval.map.groupBy", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "[...Map.groupBy(rows, r => r.priority).keys()].join()", .globals = { { "rows", FIX_ROWS, NULL } },
      .expect = "\"0,1,2,3\"", .check = CHECK_SAME },

    /* BigInts: small ones added, which is most of what a data model does with
     * them (L), and a product that grows by a limb every ten elements or so,
     * schoolbook by one limb at a time (Q). */
    { .name = "eval.bigint.sum-reduce", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "ns.reduce((a, x) => a + x, 0n)", .globals = { { "ns", FIX_BIGINTS, NULL } }, .expect = "55n",
      .check = CHECK_BIGINT_SUM },
    { .name = "eval.bigint.factorial", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_Q, .smoke = 10,
      .source = "ns.reduce((a, x) => a * x, 1n)", .globals = { { "ns", FIX_BIGINTS, NULL } }, .expect = "3628800n",
      .check = CHECK_FACTORIAL },

    /* JSON: the rows' text read, and the rows written as text (L). */
    { .name = "eval.json.parse", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "JSON.parse(text)", .globals = { { "text", FIX_ROWS_JSON, NULL } }, .expect = ROWS_VALUE,
      .check = CHECK_ARRAY_N },
    { .name = "eval.json.stringify", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_L, .smoke = 10,
      .source = "JSON.stringify(rows)", .globals = { { "rows", FIX_ROWS, NULL } }, .expect = ROWS_JSON_TEXT,
      .check = CHECK_ROWS_JSON },

    /* write */
    { .name = "write.rows", .unit = UNIT_BYTE, .op = OP_WRITE, .sizes = SIZES_WRITE_ROWS, .smoke = 10,
      .buffer = 65536, .globals = { { "value", FIX_ROWS, NULL } }, .expect = ROWS_VALUE },
    { .name = "write.rows.buf64", .unit = UNIT_BYTE, .op = OP_WRITE, .sizes = SIZES_10000, .smoke = 10,
      .buffer = 64, .globals = { { "value", FIX_ROWS, NULL } }, .expect = ROWS_VALUE },
    { .name = "write.numbers", .unit = UNIT_BYTE, .op = OP_WRITE, .sizes = SIZES_100000, .smoke = 10,
      .buffer = 65536, .globals = { { "value", FIX_NUMBERS, NULL } },
      .expect = "[859.7941207808166,394.3013383563367,480.58787404949175,188.94988582774553,175.46217903306626,"
                "590.0896671941921,604.7802727761426,850.2574670373677,865.8799244302276,560.7706204257373]" },
    { .name = "write.strings", .unit = UNIT_BYTE, .op = OP_WRITE, .sizes = SIZES_10000, .smoke = 2, .buffer = 65536,
      .globals = { { "value", FIX_STRINGS, NULL } },
      .expect = "[\"say \\\"hi\\\" #0\\nna\xc3\xafve caf\xc3\xa9 \xe2\x9c\x93\","
                "\"say \\\"hi\\\" #1\\nna\xc3\xafve caf\xc3\xa9 \xe2\x9c\x93\"]" },

    /* vm and values */
    { .name = "vm.fresh", .unit = UNIT_OP, .op = OP_VM_FRESH, .sizes = SIZES_ONE, .smoke = 1, .source = "1",
      .expect = "1" },
    { .name = "vm.reuse", .unit = UNIT_OP, .op = OP_EVAL, .sizes = SIZES_ONE, .smoke = 1, .source = "1",
      .expect = "1" },
    { .name = "vm.reset", .unit = UNIT_OP, .op = OP_VM_RESET, .sizes = SIZES_ONE, .smoke = 1, .source = "1" },
    { .name = "vm.fuel.1", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_10000, .smoke = 10, .source = MAP_XS,
      .fuel = 1, .globals = { { "xs", FIX_XS, NULL } }, .expect = MAP_VALUE, .check = CHECK_ARRAY_N },
    { .name = "vm.fuel.16", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_10000, .smoke = 10, .source = MAP_XS,
      .fuel = 16, .globals = { { "xs", FIX_XS, NULL } }, .expect = MAP_VALUE, .check = CHECK_ARRAY_N },
    { .name = "vm.fuel.1024", .unit = UNIT_ELEMENT, .op = OP_EVAL, .sizes = SIZES_10000, .smoke = 10,
      .source = MAP_XS, .fuel = 1024, .globals = { { "xs", FIX_XS, NULL } }, .expect = MAP_VALUE,
      .check = CHECK_ARRAY_N },
    { .name = "values.rows", .unit = UNIT_ELEMENT, .op = OP_VALUES_ROWS, .sizes = SIZES_L, .smoke = 10,
      .expect = ROWS_VALUE, .check = CHECK_ARRAY_N },
    { .name = "values.set", .unit = UNIT_ELEMENT, .op = OP_VALUES_SET, .sizes = SIZES_Q, .smoke = 3,
      .expect = "new Set([\"s000000\",\"s000001\",\"s000002\"])", .check = CHECK_SET_N },
};

#define CASE_COUNT (sizeof cases / sizeof cases[0])

/* ------------------------------------------------------------------------
 * A case at one size
 */

typedef struct {
    const bench_case *c;
    uint32_t n;
    bool at_smoke_size;      /* the expected text applies */
    bool verifying;          /* this op checks its result */
    bool measuring;          /* this op counts memory and events */
    bench_heap heap;
    sjs_allocator allocator;
    sjs_context *context;
    bench_text text;         /* the source compiled */
    uint32_t copies;         /* TEXT_SYNTHETIC: power.js's copies */
    char *ids;               /* "r%06u" of every row, 8 bytes apart */
    sjs_str names[MAX_GLOBALS];
    sjs_value globals[MAX_GLOBALS];
    uint32_t global_count;
    sjs_value power;         /* power's namespace, for `import ... from "power"` */
    sjs_program *program;
    sjs_vm *vm;
    uint8_t *buffer;         /* write */
    /* What the last op saw. */
    uint64_t fuel;
    uint64_t slices;
    uint64_t events;
    uint64_t written;
    size_t hold_base;
    size_t held;
    bench_text out;          /* write, verifying: the bytes written */
    bool failed;
    bool refused;            /* setup failed: the library refused the case */
    char problem[512];
} bench_run;

static bool fail(bench_run *run, const char *what) {
    if (!run->failed) snprintf(run->problem, sizeof run->problem, "%s", what);
    run->failed = true;
    return false;
}

static const char *code_name(sjs_error_code code) {
    switch (code) {
    case SJS_ERROR_SYNTAX: return "syntax";
    case SJS_ERROR_UNSUPPORTED: return "unsupported";
    case SJS_ERROR_NAME: return "name";
    case SJS_ERROR_LIMIT: return "limit";
    case SJS_ERROR_MEMORY: return "memory";
    case SJS_ERROR_FAULT: return "fault";
    default: return "none";
    }
}

static bool fail_error(bench_run *run, const char *stage, const sjs_error *error) {
    char what[512];
    snprintf(what, sizeof what, "%s: %s %.*s (%u:%u)", stage, code_name(error->code), (int)error->message.length,
             error->message.ptr, error->at.line, error->at.column);
    return fail(run, what);
}

static sjs_result find_module(void *self, sjs_str name, sjs_value *exports) {
    bench_run *run = self;
    if (name.length == 5 && memcmp(name.ptr, "power", 5) == 0 && run->power.kind == SJS_OBJECT) {
        *exports = run->power;
        return SJS_OK;
    }
    return SJS_ERR_NOT_FOUND;
}

static const sjs_modules_vtable modules_vtable = { find_module };

/* A sink that counts events, and passes them on when it has somewhere to. */
typedef struct {
    sjs_sink inner;
    uint64_t events;
} counting_sink;

static sjs_result count_event(void *self, const sjs_event *event) {
    counting_sink *sink = self;
    sink->events++;
    return sink->inner.v == NULL ? SJS_OK : sink->inner.v->event(sink->inner.self, event);
}

static const sjs_sink_vtable counting_vtable = { count_event };

/* A source to compile. */
typedef struct {
    const char *text;
    size_t length;
    sjs_goal goal;
    uint32_t chunk;          /* 0: whole */
    uint32_t global_count;   /* of run->names */
    bool parse_only;
} bench_source;

/* Push `source` to a parser, into the compiler or (parse_only) a counting
 * sink. The program is NULL when parse_only. */
static bool compile_source(bench_run *run, const bench_source *source, sjs_program **out) {
    sjs_compiler *compiler = NULL;
    sjs_sink sink = { NULL, NULL };
    *out = NULL;
    if (!source->parse_only) {
        sjs_compiler_config config = { .context = run->context, .goal = source->goal, .globals = run->names,
                                       .global_count = source->global_count, .modules = { run, &modules_vtable } };
        if (sjs_compiler_init(&compiler, &config) != SJS_OK) return fail(run, "sjs_compiler_init failed");
        sink = sjs_compiler_sink(compiler);
    }
    counting_sink counter = { sink, 0 };
    if (run->measuring || source->parse_only) sink = (sjs_sink){ &counter, &counting_vtable };
    sjs_parser *parser;
    sjs_parser_config config = { run->allocator, SJS_LIMITS_DEFAULT, source->goal, sink };
    sjs_result r = sjs_parser_init(&parser, &config);
    if (r != SJS_OK) {
        sjs_compiler_destroy(compiler);
        return fail(run, "sjs_parser_init failed");
    }
    size_t chunk = source->chunk == 0 ? source->length : source->chunk;
    for (size_t at = 0; at < source->length && r == SJS_OK; at += chunk) {
        size_t n = source->length - at < chunk ? source->length - at : chunk;
        r = sjs_parser_push(parser, (sjs_bytes){ (const uint8_t *)source->text + at, (uint32_t)n });
    }
    if (r == SJS_OK) r = sjs_parser_finish(parser);
    if (r == SJS_OK && compiler != NULL) r = sjs_compiler_finish(compiler, out);
    bool ok = r == SJS_OK;
    if (!ok) {
        sjs_error error = { 0 };
        if (sjs_parser_error(parser, &error) == SJS_OK || sjs_compiler_error(compiler, &error) == SJS_OK) {
            fail_error(run, "compile", &error);
        } else {
            char what[64];
            snprintf(what, sizeof what, "compile: result %d", (int)r);
            fail(run, what);
        }
    }
    sjs_parser_destroy(parser);
    sjs_compiler_destroy(compiler);
    run->events = counter.events;
    bench_sink += counter.events;
    return ok;
}

/* Evaluate to the end, untimed; the result is the caller's. */
static bool evaluate(bench_run *run, const sjs_run_params *params, sjs_value *out) {
    sjs_vm *vm;
    sjs_vm_config config = { run->context, false, false };
    if (sjs_vm_init(&vm, &config) != SJS_OK) return fail(run, "sjs_vm_init failed");
    bool ok = sjs_vm_start(vm, params) == SJS_OK;
    if (!ok) fail(run, "sjs_vm_start refused");
    if (ok && sjs_vm_run(vm, UINT64_MAX) != SJS_RUN_DONE) {
        sjs_error error = { 0 };
        sjs_vm_error(vm, &error);
        ok = fail_error(run, "evaluate", &error);
    }
    if (ok) sjs_vm_take_result(vm, out);
    sjs_vm_destroy(vm);
    return ok;
}

/* Compile and evaluate a source that binds no globals. */
static bool evaluate_source(bench_run *run, const bench_source *source, sjs_value *out) {
    sjs_program *program;
    if (!compile_source(run, source, &program)) return false;
    sjs_run_params params = { program, NULL, 0 };
    bool ok = evaluate(run, &params, out);
    sjs_program_release(program);
    return ok;
}

/* The value as sjs_writer writes it, with "<function>" for a function. */
static bool write_value(bench_run *run, sjs_value value, bench_text *out) {
    sjs_writer *writer;
    sjs_writer_config config = { run->context, value, { "<function>", 10 } };
    if (sjs_writer_init(&writer, &config) != SJS_OK) return fail(run, "sjs_writer_init failed");
    uint8_t chunk[4096];
    sjs_buffer buffer = { chunk, sizeof chunk, 0 };
    sjs_result r;
    while ((r = sjs_writer_write(writer, &buffer)) == SJS_NEED_SPACE) {
        text_append(out, (const char *)chunk, buffer.length);
        buffer.length = 0;
    }
    if (r == SJS_OK) text_append(out, (const char *)chunk, buffer.length);
    sjs_writer_destroy(writer);
    return r == SJS_OK ? true : fail(run, "sjs_writer_write failed");
}

/* ------------------------------------------------------------------------
 * Inputs
 */

static bool string_value(bench_run *run, const char *text, sjs_value *out) {
    if (sjs_string_new(run->context, (sjs_str){ text, (uint32_t)strlen(text) }, out) != SJS_OK) {
        return fail(run, "sjs_string_new failed");
    }
    return true;
}

static bool put(bench_run *run, sjs_value *object, const sjs_property *property) {
    if (sjs_object_put(run->context, object, property) != SJS_OK) return fail(run, "sjs_object_put failed");
    return true;
}

static bool append(bench_run *run, sjs_value *array, sjs_value element) {
    if (sjs_array_append(run->context, array, element) != SJS_OK) return fail(run, "sjs_array_append failed");
    return true;
}

static bool add(bench_run *run, sjs_value *set, sjs_value element) {
    if (sjs_set_add(run->context, set, element) != SJS_OK) {
        sjs_release(run->context, element);
        return fail(run, "sjs_set_add failed");
    }
    return true;
}

static const char *row_id(const bench_run *run, uint32_t i) {
    return run->ids + (size_t)i * 8u;
}

static double row_kwh(uint32_t i) {
    return (double)((uint64_t)i * 37u % 1000u) / 10.0;
}

static bool make_row(bench_run *run, uint32_t i, sjs_value *out) {
    sjs_value row, id, tags, a, b;
    if (sjs_object_new(run->context, 4, &row) != SJS_OK) return fail(run, "sjs_object_new failed");
    *out = row;
    bool ok = string_value(run, row_id(run, i), &id) && put(run, out, &(sjs_property){ SJS_STR("id"), id }) &&
              put(run, out, &(sjs_property){ SJS_STR("kwh"), sjs_number(row_kwh(i)) }) &&
              put(run, out, &(sjs_property){ SJS_STR("priority"), sjs_number((double)(i % 4u)) });
    if (!ok) return false;
    if (sjs_array_new(run->context, 2, &tags) != SJS_OK) return fail(run, "sjs_array_new failed");
    ok = string_value(run, "a", &a) && append(run, &tags, a) && string_value(run, "b", &b) && append(run, &tags, b);
    if (!ok) {
        sjs_release(run->context, tags);
        return false;
    }
    return put(run, out, &(sjs_property){ SJS_STR("tags"), tags });
}

static bool make_rows(bench_run *run, sjs_value *out) {
    if (sjs_array_new(run->context, run->n, out) != SJS_OK) return fail(run, "sjs_array_new failed");
    for (uint32_t i = 0; i < run->n; i++) {
        sjs_value row;
        if (!make_row(run, i, &row)) return false;
        if (!append(run, out, row)) return false;
    }
    return true;
}

static bool make_xs(bench_run *run, sjs_value *out) {
    uint32_t n = run->n;
    uint32_t *order = malloc((size_t)n * sizeof *order + 1);
    if (order == NULL) return fail(run, "out of memory");
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    uint64_t state = BENCH_SEED;
    for (uint32_t i = n; i > 1; i--) {
        uint32_t j = (uint32_t)(xorshift(&state) % i);
        uint32_t t = order[i - 1];
        order[i - 1] = order[j];
        order[j] = t;
    }
    bool ok = sjs_array_new(run->context, n, out) == SJS_OK;
    if (!ok) fail(run, "sjs_array_new failed");
    for (uint32_t i = 0; ok && i < n; i++) ok = append(run, out, sjs_number(order[i] * 0.5));
    free(order);
    return ok;
}

static bool make_ids(bench_run *run, sjs_value *out) {
    if (sjs_array_new(run->context, run->n + run->n / 5u, out) != SJS_OK) return fail(run, "sjs_array_new failed");
    for (uint32_t i = 0; i < run->n; i++) {
        for (uint32_t twice = 0; twice < (i % 5u == 4u ? 2u : 1u); twice++) {
            sjs_value id;
            if (!string_value(run, row_id(run, i), &id) || !append(run, out, id)) return false;
        }
    }
    return true;
}

/* FIX_ID_MAP: each row's id to its kwh, built through the API. */
static bool make_id_map(bench_run *run, sjs_value *out) {
    if (sjs_map_new(run->context, run->n, out) != SJS_OK) return fail(run, "sjs_map_new failed");
    for (uint32_t i = 0; i < run->n; i++) {
        sjs_value row;
        if (!make_row(run, i, &row)) return false;
        sjs_map_entry entry;
        bool ok = sjs_object_get(row, SJS_STR("id"), &entry.key) == SJS_OK && sjs_object_get(row, SJS_STR("kwh"), &entry.value) == SJS_OK;
        if (ok) sjs_retain(entry.key);
        sjs_release(run->context, row);
        if (!ok || sjs_map_set(run->context, out, &entry) != SJS_OK) return fail(run, "sjs_map_set failed");
    }
    return true;
}

/* Strings "s%06u" from `first`, `count` of them. */
typedef struct {
    uint32_t first;
    uint32_t count;
} bench_range;

static bool make_string_set(bench_run *run, bench_range range, sjs_value *out) {
    if (sjs_set_new(run->context, range.count, out) != SJS_OK) return fail(run, "sjs_set_new failed");
    for (uint32_t i = range.first; i < range.first + range.count; i++) {
        char text[16];
        snprintf(text, sizeof text, "s%06u", (unsigned)i);
        sjs_value s;
        if (!string_value(run, text, &s) || !add(run, out, s)) return false;
    }
    return true;
}

static bool make_pending_half(bench_run *run, sjs_value *out) {
    if (sjs_set_new(run->context, run->n / 2u + 1u, out) != SJS_OK) return fail(run, "sjs_set_new failed");
    for (uint32_t i = 0; i < run->n; i += 2) {
        sjs_value id;
        if (!string_value(run, row_id(run, i), &id) || !add(run, out, id)) return false;
    }
    return true;
}

static bool make_numbers(bench_run *run, sjs_value *out) {
    if (sjs_array_new(run->context, run->n, out) != SJS_OK) return fail(run, "sjs_array_new failed");
    uint64_t state = BENCH_SEED;
    for (uint32_t i = 0; i < run->n; i++) {
        double x = (double)(xorshift(&state) >> 11) / 9007199254740992.0 * 1000.0;
        if (!append(run, out, sjs_number(x))) return false;
    }
    return true;
}

/* FIX_BIGINTS: 1n to n. */
static bool make_bigints(bench_run *run, sjs_value *out) {
    if (sjs_array_new(run->context, run->n, out) != SJS_OK) return fail(run, "sjs_array_new failed");
    for (uint32_t i = 0; i < run->n; i++) {
        char text[16];
        snprintf(text, sizeof text, "%u", (unsigned)(i + 1u));
        sjs_value b;
        if (sjs_bigint_new(run->context, (sjs_str){ text, (uint32_t)strlen(text) }, &b) != SJS_OK) return fail(run, "sjs_bigint_new failed");
        if (!append(run, out, b)) return false;
    }
    return true;
}

/* FIX_NUMBERS's doubles as strings with `digits` significant digits. */
static bool make_number_texts(bench_run *run, int digits, sjs_value *out) {
    if (sjs_array_new(run->context, run->n, out) != SJS_OK) return fail(run, "sjs_array_new failed");
    uint64_t state = BENCH_SEED;
    for (uint32_t i = 0; i < run->n; i++) {
        double x = (double)(xorshift(&state) >> 11) / 9007199254740992.0 * 1000.0;
        char text[40];
        snprintf(text, sizeof text, "%.*g", digits, x);
        sjs_value s;
        if (!string_value(run, text, &s) || !append(run, out, s)) return false;
    }
    return true;
}

static bool make_strings(bench_run *run, sjs_value *out) {
    if (sjs_array_new(run->context, run->n, out) != SJS_OK) return fail(run, "sjs_array_new failed");
    for (uint32_t i = 0; i < run->n; i++) {
        char text[64];
        snprintf(text, sizeof text, "say \"hi\" #%u\nna\xc3\xafve caf\xc3\xa9 \xe2\x9c\x93", (unsigned)i);
        sjs_value s;
        if (!string_value(run, text, &s) || !append(run, out, s)) return false;
    }
    return true;
}

/* The rows as JSON.stringify writes them, in a buffer the caller frees:
 * kwh is a tenth of an integer below 1000, which Number::toString writes
 * with one decimal at most. */
static char *rows_json(bench_run *run, size_t *length) {
    bench_text t = { 0 };
    text_append(&t, "[", 1);
    for (uint32_t i = 0; i < run->n; i++) {
        uint32_t tenths = (uint32_t)((uint64_t)i * 37u % 1000u);
        char row[96];
        int n = tenths % 10u == 0
                    ? snprintf(row, sizeof row, "%s{\"id\":\"%.7s\",\"kwh\":%u,\"priority\":%u,\"tags\":[\"a\",\"b\"]}",
                               i == 0 ? "" : ",", row_id(run, i), tenths / 10u, i % 4u)
                    : snprintf(row, sizeof row, "%s{\"id\":\"%.7s\",\"kwh\":%u.%u,\"priority\":%u,\"tags\":[\"a\",\"b\"]}",
                               i == 0 ? "" : ",", row_id(run, i), tenths / 10u, tenths % 10u, i % 4u);
        text_append(&t, row, (size_t)n);
    }
    text_append(&t, "]", 1);
    *length = t.length;
    return t.ptr;
}

static bool make_rows_json(bench_run *run, sjs_value *out) {
    size_t length;
    char *text = rows_json(run, &length);
    bool ok = sjs_string_new(run->context, (sjs_str){ text, (uint32_t)length }, out) == SJS_OK;
    free(text);
    return ok ? true : fail(run, "sjs_string_new failed");
}

/* The row ids joined by commas: "r000000,r000001,...". */
static bool make_text(bench_run *run, sjs_value *out) {
    size_t length = (size_t)run->n * 8u - 1u;
    char *text = malloc(length + 1u);
    if (text == NULL) return fail(run, "out of memory");
    memcpy(text, run->ids, length);
    for (uint32_t i = 1; i < run->n; i++) text[(size_t)i * 8u - 1u] = ',';
    bool ok = sjs_string_new(run->context, (sjs_str){ text, (uint32_t)length }, out) == SJS_OK;
    free(text);
    return ok ? true : fail(run, "sjs_string_new failed");
}

/* {p000: {want_kw, priority}, ...}: n ports in three priorities. */
static bool make_demand(bench_run *run, sjs_value *out) {
    if (sjs_object_new(run->context, run->n, out) != SJS_OK) return fail(run, "sjs_object_new failed");
    for (uint32_t i = 0; i < run->n; i++) {
        sjs_value port;
        if (sjs_object_new(run->context, 2, &port) != SJS_OK) return fail(run, "sjs_object_new failed");
        bool ok = put(run, &port, &(sjs_property){ SJS_STR("want_kw"), sjs_number(1.0 + (double)(i * 7u % 20u)) }) &&
                  put(run, &port, &(sjs_property){ SJS_STR("priority"), sjs_number((double)(i % 3u)) });
        char key[16];
        snprintf(key, sizeof key, "p%03u", (unsigned)i);
        if (!ok || !put(run, out, &(sjs_property){ { key, (uint32_t)strlen(key) }, port })) return false;
    }
    return true;
}

static bool make_global(bench_run *run, const bench_global *global, sjs_value *out) {
    *out = sjs_undefined();
    switch (global->fixture) {
    case FIX_LITERAL: {
        bench_source source = { global->literal, strlen(global->literal), SJS_GOAL_EXPRESSION, 0, 0, false };
        return evaluate_source(run, &source, out);
    }
    case FIX_XS: return make_xs(run, out);
    case FIX_ROWS: return make_rows(run, out);
    case FIX_IDS: return make_ids(run, out);
    case FIX_ID_MAP: return make_id_map(run, out);
    case FIX_SET_A: return make_string_set(run, (bench_range){ 0, run->n }, out);
    case FIX_SET_B: return make_string_set(run, (bench_range){ run->n / 2u, run->n }, out);
    case FIX_PENDING_HALF: return make_pending_half(run, out);
    case FIX_NUMBERS: return make_numbers(run, out);
    case FIX_STRINGS: return make_strings(run, out);
    case FIX_NUMBER_TEXTS: return make_number_texts(run, 15, out);
    case FIX_NUMBER_TEXTS_17: return make_number_texts(run, 17, out);
    case FIX_BIGINTS: return make_bigints(run, out);
    case FIX_TEXT: return make_text(run, out);
    case FIX_ROWS_JSON: return make_rows_json(run, out);
    case FIX_CAPACITY: *out = sjs_number(6.0 * run->n); return true;
    case FIX_DEMAND: return make_demand(run, out);
    case FIX_WATERFILL:
        if (run->power.kind != SJS_OBJECT || sjs_object_get(run->power, SJS_STR("waterFill"), out) != SJS_OK) {
            return fail(run, "power exports no waterFill");
        }
        sjs_retain(*out);
        return true;
    default: return fail(run, "no such fixture");
    }
}

/* ------------------------------------------------------------------------
 * Sources
 */

static bool read_file(const char *path, bench_text *out) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, file)) > 0) text_append(out, chunk, n);
    fclose(file);
    return out->length > 0;
}

static bool is_name_byte(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$';
}

/* power.js once more, with fill, waterFill and example renamed name_k. */
static void append_renamed(bench_text *out, const bench_text *power, uint32_t k) {
    static const char *const renamed[] = { "fill", "waterFill", "example" };
    size_t at = 0;
    while (at < power->length) {
        if (!is_name_byte(power->ptr[at])) {
            text_append(out, power->ptr + at, 1);
            at++;
            continue;
        }
        size_t end = at;
        while (end < power->length && is_name_byte(power->ptr[end])) end++;
        text_append(out, power->ptr + at, end - at);
        for (size_t i = 0; i < sizeof renamed / sizeof renamed[0]; i++) {
            if (strlen(renamed[i]) == end - at && memcmp(renamed[i], power->ptr + at, end - at) == 0) {
                char suffix[16];
                int length = snprintf(suffix, sizeof suffix, "_%u", (unsigned)k);
                text_append(out, suffix, (size_t)length);
            }
        }
        at = end;
    }
}

#define POWER_PATH "bench/modules/power.js"
#define CAMPAIGN_PATH "bench/modules/campaign.js"
#define SYNTHETIC_BYTES (32u * 1024u)

/* The case's source into run->text; false when a module file is missing. */
static bool load_text(bench_run *run, bool smoke) {
    const bench_case *c = run->c;
    switch (c->text) {
    case TEXT_INLINE:
        if (c->source != NULL) text_append(&run->text, c->source, strlen(c->source));
        return true;
    case TEXT_POWER: return read_file(POWER_PATH, &run->text);
    case TEXT_CAMPAIGN: return read_file(CAMPAIGN_PATH, &run->text);
    case TEXT_SYNTHETIC: {
        bench_text power = { 0 };
        if (!read_file(POWER_PATH, &power)) return false;
        run->copies = 0;
        do {
            append_renamed(&run->text, &power, run->copies++);
        } while (!smoke && run->text.length < SYNTHETIC_BYTES);
        text_free(&power);
        return true;
    }
    }
    return false;
}

/* ------------------------------------------------------------------------
 * Checking a result
 */

typedef struct {
    sjs_kind kind;
    uint32_t length;
} bench_shape;

static bool expect_shape(bench_run *run, sjs_value value, bench_shape shape) {
    if (value.kind == shape.kind && sjs_length(value) == shape.length) return true;
    char what[160];
    snprintf(what, sizeof what, "expected a value of kind %d and length %u, got kind %d and length %u", (int)shape.kind,
             (unsigned)shape.length, (int)value.kind, (unsigned)sjs_length(value));
    return fail(run, what);
}

static bool expect_number(bench_run *run, sjs_value value, double want) {
    if (value.kind == SJS_NUMBER && value.as.number == want) return true;
    char what[160];
    snprintf(what, sizeof what, "expected %.17g, got %.17g (kind %d)", want,
             value.kind == SJS_NUMBER ? value.as.number : 0.0, (int)value.kind);
    return fail(run, what);
}

/* A BigInt's decimal text, checked against `want`, or its length. */
static bool expect_bigint(bench_run *run, sjs_value value, const char *want, uint32_t length) {
    if (value.kind != SJS_BIGINT) return fail(run, "expected a BigInt");
    uint32_t bound = sjs_bigint_text_bound(value);
    char *text = malloc(bound);
    sjs_buffer b = { (uint8_t *)text, bound, 0 };
    bool ok = text != NULL && sjs_bigint_text(run->context, value, &b) == SJS_OK &&
              (want != NULL ? b.length == strlen(want) && memcmp(text, want, b.length) == 0 : b.length == length);
    free(text);
    return ok ? true : fail(run, "the BigInt is not the one expected");
}

static bool check_value(bench_run *run, sjs_value value) {
    uint32_t n = run->n;
    uint32_t half = n / 2u;
    switch (run->c->check) {
    case CHECK_NONE: return true;
    case CHECK_ARRAY_N: return expect_shape(run, value, (bench_shape){ SJS_ARRAY, n });
    case CHECK_FILTER_QUARTER: return expect_shape(run, value, (bench_shape){ SJS_ARRAY, (n + 1u) / 4u });
    case CHECK_FILTER_FIFTH: return expect_shape(run, value, (bench_shape){ SJS_ARRAY, n / 5u });
    case CHECK_SUM_XS: return expect_number(run, value, (double)n * (double)(n - 1u) / 4.0);
    case CHECK_SUM_KWH: {
        /* In the order reduce adds them, so rounded the same way. */
        double sum = 0;
        for (uint32_t i = 0; i < n; i++) sum += row_kwh(i);
        return expect_number(run, value, sum);
    }
    case CHECK_PIPELINE: {
        double sum = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (i % 4u > 1u) sum += row_kwh(i);
        }
        return expect_number(run, value, sum);
    }
    case CHECK_SORTED:
        if (!expect_shape(run, value, (bench_shape){ SJS_ARRAY, n })) return false;
        for (uint32_t i = 0; i < n; i++) {
            if (sjs_array_at(value, i).as.number != i * 0.5) return fail(run, "toSorted: not in order");
        }
        return true;
    case CHECK_SAME: return true; /* verify_value compares it with the smoke value */
    case CHECK_BIGINT_SUM: {
        char want[32];
        snprintf(want, sizeof want, "%llu", (unsigned long long)n * (n + 1u) / 2u);
        return expect_bigint(run, value, want, 0);
    }
    case CHECK_FACTORIAL: {
        /* The digits of n!, from Node: String(f).length. */
        uint32_t digits = n == 10u ? 7u : n == 100u ? 158u : n == 1000u ? 2568u : n == 10000u ? 35660u : 0u;
        return digits == 0 ? fail(run, "no digit count for this n") : expect_bigint(run, value, NULL, digits);
    }
    case CHECK_SORTED_DESC:
        if (!expect_shape(run, value, (bench_shape){ SJS_ARRAY, n })) return false;
        for (uint32_t i = 0; i < n; i++) {
            if (sjs_array_at(value, i).as.number != (n - 1u - i) * 0.5) return fail(run, "toSorted: not in order");
        }
        return true;
    case CHECK_JOIN:
        if (value.kind != SJS_STRING || sjs_string_text(value).length != n * 8u - 1u) {
            return fail(run, "join: not the expected length");
        }
        return true;
    case CHECK_OBJECT_N: return expect_shape(run, value, (bench_shape){ SJS_OBJECT, n });
    case CHECK_SET_N: return expect_shape(run, value, (bench_shape){ SJS_SET, n });
    case CHECK_MAP_N: return expect_shape(run, value, (bench_shape){ SJS_MAP, n }); /* MUTATION */
    case CHECK_HAS: {
        uint32_t count = 0;
        for (uint32_t i = 0; i < n; i += 2) count += i % 5u == 4u ? 2u : 1u;
        return expect_shape(run, value, (bench_shape){ SJS_ARRAY, count });
    }
    case CHECK_UNION: return expect_shape(run, value, (bench_shape){ SJS_SET, half + n });
    case CHECK_INTERSECTION: return expect_shape(run, value, (bench_shape){ SJS_SET, n - half });
    case CHECK_DIFFERENCE: return expect_shape(run, value, (bench_shape){ SJS_SET, half });
    case CHECK_SYNTHETIC: return expect_shape(run, value, (bench_shape){ SJS_OBJECT, run->copies * 3u });
    case CHECK_ROWS_JSON: {
        size_t length;
        char *want = rows_json(run, &length);
        sjs_str got = sjs_string_text(value);
        bool same = value.kind == SJS_STRING && got.length == length && memcmp(got.ptr, want, length) == 0;
        free(want);
        return same ? true : fail(run, "JSON.stringify: not the rows' text");
    }
    case CHECK_IDS_JOINED:
        if (value.kind != SJS_STRING || sjs_string_text(value).length != n * 8u) return fail(run, "ids joined: not the expected length");
        return true;
    }
    return true;
}

/* Compare written text with the case's expected text. */
static bool expect_text(bench_run *run, const bench_text *text) {
    if (strcmp(text->ptr == NULL ? "" : text->ptr, run->c->expect) == 0) return true;
    char what[512];
    snprintf(what, sizeof what, "a different value: got %.200s, expected %.200s", text->ptr == NULL ? "" : text->ptr,
             run->c->expect);
    return fail(run, what);
}

static bool verify_value(bench_run *run, sjs_value value) {
    bool as_written = run->at_smoke_size || run->c->check == CHECK_SAME;
    if (!as_written || run->c->expect == NULL) return check_value(run, value);
    bench_text text = { 0 };
    bool ok = write_value(run, value, &text) && expect_text(run, &text);
    text_free(&text);
    return ok;
}

/* Called by each op while it holds its result: an evaluation's value, the
 * values built, or (undefined) a program or a writer, which their ops
 * verify themselves. */
static void bench_hold(bench_run *run, sjs_value value) {
    bench_op op = run->c->op;
    if (run->measuring) run->held = run->heap.live - run->hold_base;
    if (run->verifying && op != OP_PARSE && op != OP_COMPILE && op != OP_WRITE) verify_value(run, value);
}

/* ------------------------------------------------------------------------
 * The ops
 */

static void fold_value(sjs_value value) {
    uint64_t bits = 0;
    if (value.kind == SJS_NUMBER) memcpy(&bits, &value.as.number, sizeof bits);
    bench_sink += bits ^ value.kind ^ sjs_length(value);
}

static bool op_compile(bench_run *run) {
    const bench_case *c = run->c;
    bench_source source = { run->text.ptr, run->text.length, c->goal, c->chunk, run->global_count, c->op == OP_PARSE };
    sjs_program *program;
    if (!compile_source(run, &source, &program)) return false;
    bench_sink += (uintptr_t)program;
    bench_hold(run, sjs_undefined());
    bool ok = true;
    if (run->verifying && c->op == OP_PARSE && run->events == 0) ok = fail(run, "the parser sent no events");
    if (run->verifying && program != NULL) {
        sjs_value result;
        sjs_run_params params = { program, run->globals, run->global_count };
        ok = evaluate(run, &params, &result);
        if (ok) {
            ok = verify_value(run, result);
            sjs_release(run->context, result);
        }
    }
    if (program != NULL) sjs_program_release(program);
    return ok && !run->failed;
}

/* Start, run on the case's fuel, take the result, hold and release it. */
static bool run_vm(bench_run *run, sjs_vm *vm) {
    sjs_run_params params = { run->program, run->globals, run->global_count };
    if (sjs_vm_start(vm, &params) != SJS_OK) return fail(run, "sjs_vm_start refused");
    uint64_t fuel = run->c->fuel == 0 ? UINT64_MAX : run->c->fuel;
    uint64_t slices = 0;
    sjs_run state;
    do {
        state = sjs_vm_run(vm, fuel);
        slices++;
    } while (state == SJS_RUN_PAUSED);
    if (state != SJS_RUN_DONE) {
        sjs_error error = { 0 };
        sjs_vm_error(vm, &error);
        return fail_error(run, "evaluate", &error);
    }
    sjs_value result;
    if (sjs_vm_take_result(vm, &result) != SJS_OK) return fail(run, "sjs_vm_take_result failed");
    run->fuel = sjs_vm_fuel_used(vm);
    run->slices = slices;
    bench_sink += run->fuel ^ slices;
    fold_value(result);
    bench_hold(run, result);
    sjs_release(run->context, result);
    return !run->failed;
}

static bool op_vm_fresh(bench_run *run) {
    sjs_vm *vm;
    sjs_vm_config config = { run->context, false, false };
    if (sjs_vm_init(&vm, &config) != SJS_OK) return fail(run, "sjs_vm_init failed");
    bool ok = run_vm(run, vm);
    sjs_vm_destroy(vm);
    return ok;
}

static bool op_vm_reset(bench_run *run) {
    sjs_run_params params = { run->program, run->globals, run->global_count };
    if (sjs_vm_start(run->vm, &params) != SJS_OK) return fail(run, "sjs_vm_start refused");
    bench_hold(run, sjs_undefined());
    sjs_vm_reset(run->vm);
    return !run->failed;
}


/* Write the case's value into a buffer of c->buffer bytes, emptying it
 * whenever the writer asks for space. */
static bool op_write(bench_run *run) {
    sjs_writer *writer;
    sjs_writer_config config = { run->context, run->globals[0], { "<function>", 10 } };
    if (sjs_writer_init(&writer, &config) != SJS_OK) return fail(run, "sjs_writer_init failed");
    sjs_buffer buffer = { run->buffer, run->c->buffer, 0 };
    uint64_t written = 0;
    uint8_t last = 0;
    sjs_result r;
    while ((r = sjs_writer_write(writer, &buffer)) == SJS_NEED_SPACE || r == SJS_OK) {
        if (buffer.length > 0) last = buffer.ptr[buffer.length - 1];
        written += buffer.length;
        if (run->verifying) text_append(&run->out, (const char *)buffer.ptr, buffer.length);
        bool done = r == SJS_OK;
        if (!done && buffer.length == 0) break;
        buffer.length = 0;
        if (done) break;
    }
    bench_hold(run, sjs_undefined());
    sjs_writer_destroy(writer);
    if (r != SJS_OK) return fail(run, "sjs_writer_write failed");
    run->written = written;
    bench_sink += written ^ last;
    if (!run->verifying) return true;
    bool ok = run->at_smoke_size ? expect_text(run, &run->out) : written > 0 || fail(run, "nothing written");
    text_free(&run->out);
    return ok && !run->failed;
}

static bool op_values_rows(bench_run *run) {
    sjs_value rows = sjs_undefined();
    bool ok = make_rows(run, &rows);
    if (ok) {
        fold_value(rows);
        bench_hold(run, rows);
    }
    sjs_release(run->context, rows);
    return ok && !run->failed;
}

static bool op_values_set(bench_run *run) {
    sjs_value set = sjs_undefined();
    bool ok = make_string_set(run, (bench_range){ 0, run->n }, &set);
    if (ok) {
        fold_value(set);
        bench_hold(run, set);
    }
    sjs_release(run->context, set);
    return ok && !run->failed;
}

static bool do_op(bench_run *run) {
    switch (run->c->op) {
    case OP_PARSE:
    case OP_COMPILE: return op_compile(run);
    case OP_EVAL: return run_vm(run, run->vm);
    case OP_WRITE: return op_write(run);
    case OP_VM_FRESH: return op_vm_fresh(run);
    case OP_VM_RESET: return op_vm_reset(run);
    case OP_VALUES_ROWS: return op_values_rows(run);
    case OP_VALUES_SET: return op_values_set(run);
    }
    return fail(run, "no such op");
}

/* ------------------------------------------------------------------------
 * Setup and teardown
 */

static bool needs_power(const bench_case *c) {
    if (c->text == TEXT_CAMPAIGN) return true;
    for (uint32_t i = 0; i < MAX_GLOBALS; i++) {
        if (c->globals[i].fixture == FIX_WATERFILL) return true;
    }
    return false;
}

static bool needs_ids(const bench_case *c) {
    if (c->op == OP_VALUES_ROWS) return true;
    for (uint32_t i = 0; i < MAX_GLOBALS; i++) {
        bench_fixture f = c->globals[i].fixture;
        if (f == FIX_ROWS || f == FIX_IDS || f == FIX_PENDING_HALF || f == FIX_TEXT || f == FIX_ROWS_JSON) return true;
    }
    return false;
}

/* Everything the timed ops leave out. The source is in run->text already. A
 * failure here is the library refusing the case (run->refused). */
static bool setup(bench_run *run) {
    const bench_case *c = run->c;
    run->heap = (bench_heap){ 0 };
    run->allocator = (sjs_allocator){ &run->heap, &heap_vtable };
    run->power = sjs_undefined();
    sjs_context_config config = { run->allocator, SJS_LIMITS_DEFAULT };
    if (sjs_context_init(&run->context, &config) != SJS_OK) return fail(run, "sjs_context_init failed");
    if (needs_ids(c)) {
        run->ids = malloc((size_t)run->n * 8u + 1u);
        if (run->ids == NULL) return fail(run, "out of memory");
        for (uint32_t i = 0; i < run->n; i++) snprintf(run->ids + (size_t)i * 8u, 8, "r%06u", (unsigned)i);
    }
    if (needs_power(c)) {
        bench_text power = { 0 };
        if (!read_file(POWER_PATH, &power)) return fail(run, "cannot read " POWER_PATH);
        bench_source source = { power.ptr, power.length, SJS_GOAL_MODULE, 0, 0, false };
        bool ok = evaluate_source(run, &source, &run->power);
        text_free(&power);
        if (!ok) return false;
    }
    for (uint32_t i = 0; i < MAX_GLOBALS && c->globals[i].name != NULL; i++) {
        run->names[i] = (sjs_str){ c->globals[i].name, (uint32_t)strlen(c->globals[i].name) };
        if (!make_global(run, &c->globals[i], &run->globals[i])) {
            run->global_count = i + 1;
            return false;
        }
        run->global_count = i + 1;
    }
    if (c->op == OP_EVAL || c->op == OP_VM_FRESH || c->op == OP_VM_RESET) {
        bench_source source = { run->text.ptr, run->text.length, c->goal, 0, run->global_count, false };
        if (!compile_source(run, &source, &run->program)) return false;
    }
    if (c->op == OP_EVAL || c->op == OP_VM_RESET) {
        sjs_vm_config vm_config = { run->context, false, false };
        if (sjs_vm_init(&run->vm, &vm_config) != SJS_OK) return fail(run, "sjs_vm_init failed");
    }
    if (c->op == OP_WRITE) {
        run->buffer = malloc(c->buffer);
        if (run->buffer == NULL) return fail(run, "out of memory");
    }
    return true;
}

/* Release everything; false when anything the case allocated is left. */
static bool teardown(bench_run *run) {
    if (run->context == NULL) return true;
    for (uint32_t i = 0; i < run->global_count; i++) sjs_release(run->context, run->globals[i]);
    run->global_count = 0;
    sjs_release(run->context, run->power);
    run->power = sjs_undefined();
    sjs_vm_destroy(run->vm);
    run->vm = NULL;
    if (run->program != NULL) sjs_program_release(run->program);
    run->program = NULL;
    free(run->buffer);
    run->buffer = NULL;
    free(run->ids);
    run->ids = NULL;
    text_free(&run->out);
    size_t live = sjs_context_live_bytes(run->context);
    sjs_context_destroy(run->context);
    run->context = NULL;
    if (live != 0 || run->heap.live != 0) {
        char what[160];
        snprintf(what, sizeof what, "leak: %zu bytes live in the context, %zu in the allocator at teardown", live,
                 run->heap.live);
        return fail(run, what);
    }
    return true;
}

/* ------------------------------------------------------------------------
 * Timing and statistics
 */

typedef struct {
    bool jsonl;
    bool quick;
    bool smoke;
    bool list;
    const char *only;
    const char *filter;
    uint32_t size;          /* 0: every size */
    uint32_t samples;
    uint32_t sample_ms;
    uint32_t warmup_ms;
    uint32_t max_case_s;
} bench_options;

typedef struct {
    uint32_t samples;
    uint64_t iterations;
    double median;
    double best;
    double p25;
    double p75;
    double spread;
    double units_per_op;
    size_t setup_bytes;
    size_t peak_bytes;
    size_t held_bytes;
    uint64_t allocs;
    uint64_t frees;
    uint64_t allocated;
    int64_t retained;
} bench_result;

static bool time_ops(bench_run *run, uint64_t iterations, uint64_t *ns) {
    uint64_t start = now_ns();
    for (uint64_t i = 0; i < iterations; i++) {
        if (!do_op(run)) return false;
    }
    *ns = now_ns() - start;
    return true;
}

static int compare_doubles(const void *a, const void *b) {
    double x = *(const double *)a;
    double y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* The value at percentile p by nearest rank. */
static double nearest_rank(const double *sorted, uint32_t count, uint32_t p) {
    uint32_t rank = (p * count + 99u) / 100u;
    if (rank < 1) rank = 1;
    return sorted[rank - 1];
}

enum { MIN_OPS = 8, SLOW_SAMPLE = 10 };

static bool measure_time(bench_run *run, const bench_options *options, bench_result *result) {
    uint64_t sample_ns = (uint64_t)options->sample_ms * 1000000u;
    uint64_t start = now_ns();
    uint64_t warmed = 0;
    while (warmed == 0 || now_ns() - start < (uint64_t)options->warmup_ms * 1000000u) {
        if (!do_op(run)) return false;
        warmed++;
    }
    /* A sample takes at least --sample-ms and, unless that makes it longer
     * than SLOW_SAMPLE samples' worth, MIN_OPS ops: one or two ops of a few
     * milliseconds each leave a sample to the allocator and page faults. */
    uint64_t iterations = 1;
    uint64_t ns = 0;
    for (;;) {
        if (!time_ops(run, iterations, &ns)) return false;
        if (ns >= sample_ns && (iterations >= MIN_OPS || ns >= sample_ns * SLOW_SAMPLE)) break;
        iterations *= 2;
    }
    /* Long samples: as many as fit in --max-case-s, and at least 5. */
    uint32_t count = options->samples;
    uint64_t cap = (uint64_t)options->max_case_s * 1000000000u / (ns == 0 ? 1 : ns);
    if (cap < 5) cap = 5;
    if (cap < count) count = (uint32_t)cap;
    double *samples = malloc(sizeof *samples * count);
    if (samples == NULL) return fail(run, "out of memory");
    for (uint32_t i = 0; i < count; i++) {
        if (!time_ops(run, iterations, &ns)) {
            free(samples);
            return false;
        }
        samples[i] = (double)ns / (double)iterations;
    }
    qsort(samples, count, sizeof *samples, compare_doubles);
    result->samples = count;
    result->iterations = iterations;
    result->median = count % 2 ? samples[count / 2] : (samples[count / 2 - 1] + samples[count / 2]) / 2.0;
    result->best = samples[0];
    result->p25 = nearest_rank(samples, count, 25);
    result->p75 = nearest_rank(samples, count, 75);
    result->spread = result->median > 0 ? (result->p75 - result->p25) / result->median * 100.0 : 0;
    free(samples);
    return true;
}

/* One untimed op that counts memory. Allocation is deterministic. */
static bool measure_memory(bench_run *run, bench_result *result) {
    bench_heap *heap = &run->heap;
    size_t base = heap->live;
    uint64_t allocs = heap->allocs;
    uint64_t frees = heap->frees;
    uint64_t allocated = heap->allocated;
    heap->peak = base;
    run->hold_base = base;
    run->held = 0;
    run->measuring = true;
    bool ok = do_op(run);
    run->measuring = false;
    if (!ok) return false;
    result->peak_bytes = heap->peak - base;
    result->held_bytes = run->held;
    result->allocs = heap->allocs - allocs;
    result->frees = heap->frees - frees;
    result->allocated = heap->allocated - allocated;
    result->retained = (int64_t)heap->live - (int64_t)base;
    if (result->retained != 0) {
        char what[96];
        snprintf(what, sizeof what, "leak: one op retained %lld bytes", (long long)result->retained);
        return fail(run, what);
    }
    return true;
}

static double units_per_op(const bench_run *run) {
    switch (run->c->unit) {
    case UNIT_BYTE: return run->c->op == OP_WRITE ? (double)run->written : (double)run->text.length;
    case UNIT_ELEMENT: return (double)run->n;
    default: return 1;
    }
}

/* ------------------------------------------------------------------------
 * Output
 */

static void print_json_string(const char *text) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p < 0x20) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static void print_error_record(const bench_run *run) {
    fputs("{\"bench\":", stdout);
    print_json_string(run->c->name);
    printf(",\"n\":%u,\"unit\":\"%s\",\"error\":", (unsigned)run->n, unit_names[run->c->unit]);
    print_json_string(run->problem);
    puts("}");
}

static void print_optional(const char *key, bool present, uint64_t value) {
    if (present) printf(",\"%s\":%llu", key, (unsigned long long)value);
    else printf(",\"%s\":null", key);
}

static void print_record(const bench_run *run, const bench_result *r) {
    const bench_case *c = run->c;
    double ops = r->median > 0 ? 1e9 / r->median : 0;
    bool eval = c->op == OP_EVAL || c->op == OP_VM_FRESH;
    bool compile = c->op == OP_COMPILE || c->op == OP_PARSE;
    fputs("{\"bench\":", stdout);
    print_json_string(c->name);
    printf(",\"n\":%u,\"unit\":\"%s\",\"unitsPerOp\":%.0f,\"samples\":%u,\"itersPerSample\":%llu", (unsigned)run->n,
           unit_names[c->unit], r->units_per_op, (unsigned)r->samples, (unsigned long long)r->iterations);
    printf(",\"nsPerOp\":{\"median\":%.1f,\"best\":%.1f,\"p25\":%.1f,\"p75\":%.1f},\"spreadPercent\":%.2f", r->median,
           r->best, r->p25, r->p75, r->spread);
    printf(",\"opsPerSecond\":%.1f,\"unitsPerSecond\":%.1f", ops, ops * r->units_per_op);
    print_optional("fuelPerOp", eval, run->fuel);
    print_optional("slicesPerOp", eval, run->slices);
    print_optional("eventsPerOp", compile, run->events);
    printf(",\"memory\":{\"setupBytes\":%zu,\"peakBytes\":%zu,\"heldBytes\":%zu,\"allocsPerOp\":%llu,"
           "\"freesPerOp\":%llu,\"allocatedBytesPerOp\":%llu,\"retainedBytes\":%lld}}\n",
           r->setup_bytes, r->peak_bytes, r->held_bytes, (unsigned long long)r->allocs, (unsigned long long)r->frees,
           (unsigned long long)r->allocated, (long long)r->retained);
}

/* A number to about four significant digits, with k, M or G. */
static void human(char *out, size_t size, double value) {
    const char *suffix = "";
    if (value >= 1e9) {
        value /= 1e9;
        suffix = " G";
    } else if (value >= 1e6) {
        value /= 1e6;
        suffix = " M";
    } else if (value >= 1e4) {
        value /= 1e3;
        suffix = " k";
    }
    snprintf(out, size, value >= 100 ? "%.1f%s" : value >= 1 ? "%.2f%s" : "%.3f%s", value, suffix);
}

static const char *const throughput_units[] = { "MB/s", "M eval/s", "M elem/s", "M op/s" };

static void print_header(void) {
    printf("%-34s %7s %16s %10s %10s %7s %9s %10s %11s %10s %10s\n", "benchmark", "n", "throughput", "ops/s",
           "ns/unit", "spread", "fuel/op", "peak B", "alloc B/op", "allocs/op", "held B");
}

static void print_row(const bench_run *run, const bench_result *r) {
    const bench_case *c = run->c;
    double ops = r->median > 0 ? 1e9 / r->median : 0;
    char throughput[32], ops_text[32], fuel[32];
    snprintf(throughput, sizeof throughput, "%.3g %s", ops * r->units_per_op / 1e6, throughput_units[c->unit]);
    human(ops_text, sizeof ops_text, ops);
    if (c->op == OP_EVAL || c->op == OP_VM_FRESH) snprintf(fuel, sizeof fuel, "%llu", (unsigned long long)run->fuel);
    else snprintf(fuel, sizeof fuel, "-");
    printf("%-34s %7u %16s %10s %10.2f %6.1f%% %9s %10zu %11llu %10llu %10zu\n", c->name, (unsigned)run->n,
           throughput, ops_text, r->units_per_op > 0 ? r->median / r->units_per_op : 0, r->spread, fuel, r->peak_bytes,
           (unsigned long long)r->allocated, (unsigned long long)r->allocs, r->held_bytes);
}

/* ------------------------------------------------------------------------
 * Running the cases
 */

static bool selected(const bench_case *c, const bench_options *options) {
    if (options->filter != NULL && strncmp(c->name, options->filter, strlen(options->filter)) != 0) return false;
    if (options->only == NULL) return true;
    size_t length = strlen(c->name);
    for (const char *at = options->only; *at != '\0';) {
        const char *end = strchr(at, ',');
        size_t n = end == NULL ? strlen(at) : (size_t)(end - at);
        if (n == length && memcmp(at, c->name, n) == 0) return true;
        if (end == NULL) break;
        at = end + 1;
    }
    return false;
}

/* The sizes to run a case at, 0-terminated. */
static uint32_t case_sizes(const bench_case *c, const bench_options *options, uint32_t *out) {
    uint32_t count = 0;
    if (options->smoke) {
        out[count++] = c->smoke;
    } else if (c->sizes == NULL) {
        out[count++] = 0;
    } else {
        for (uint32_t i = 0; c->sizes[i] != 0 && (!options->quick || i < 2); i++) out[count++] = c->sizes[i];
    }
    out[count] = 0;
    return count;
}

typedef struct {
    uint32_t ran;
    uint32_t failed;
    uint32_t refused;
} bench_tally;

/* Prepare a run of `c` at `size` (0: the source's own); n is set from the
 * source for a case without sizes. */
static bool prepare(bench_run *run, const bench_options *options) {
    if (!load_text(run, options->smoke)) {
        snprintf(run->problem, sizeof run->problem, "cannot read the case's module (run from staticjs/)");
        run->failed = true;
        return false;
    }
    if (run->c->sizes == NULL) run->n = (uint32_t)run->text.length;
    run->at_smoke_size = run->c->sizes == NULL ? (run->c->text != TEXT_SYNTHETIC || options->smoke)
                                               : run->n == run->c->smoke;
    return true;
}

/* A run of every selected case: the options, and what has happened. */
typedef struct {
    const bench_options *options;
    bench_tally tally;
} bench_session;

static void run_case(bench_session *session, const bench_case *c, uint32_t size) {
    const bench_options *options = session->options;
    bench_tally *tally = &session->tally;
    bench_run run = { 0 };
    run.c = c;
    run.n = size;
    run.power = sjs_undefined();
    if (!prepare(&run, options)) {
        tally->failed++;
        print_error_record(&run);
        text_free(&run.text);
        return;
    }
    if (options->size != 0 && run.n != options->size) {
        text_free(&run.text);
        return;
    }
    tally->ran++;
    bench_result result = { 0 };
    bool ok = setup(&run);
    if (!ok) run.refused = true;
    result.setup_bytes = run.heap.live;
    if (ok) {
        run.verifying = true;
        ok = do_op(&run);
        run.verifying = false;
    }
    if (ok && !options->smoke) ok = measure_time(&run, options, &result);
    if (ok) ok = measure_memory(&run, &result);
    result.units_per_op = units_per_op(&run);
    bool clean = teardown(&run);
    ok = ok && clean;
    text_free(&run.text);
    if (!ok) {
        if (run.refused && !options->smoke) tally->refused++;
        else tally->failed++;
        if (options->smoke || !options->jsonl) {
            fprintf(stderr, "bench: %s (n=%u): %s\n", c->name, (unsigned)run.n, run.problem);
        }
        if (options->jsonl) print_error_record(&run);
        return;
    }
    if (options->smoke) return;
    if (options->jsonl) print_record(&run, &result);
    else print_row(&run, &result);
    fflush(stdout);
}

static void list_cases(const bench_options *options) {
    for (size_t i = 0; i < CASE_COUNT; i++) {
        const bench_case *c = &cases[i];
        if (!selected(c, options)) continue;
        uint32_t sizes[8];
        uint32_t count = case_sizes(c, options, sizes);
        printf("%s\t%s\t", c->name, unit_names[c->unit]);
        for (uint32_t s = 0; s < count; s++) {
            bench_run run = { 0 };
            run.c = c;
            run.n = sizes[s];
            if (!prepare(&run, options)) run.n = 0;
            printf("%s%u", s == 0 ? "" : ",", (unsigned)run.n);
            text_free(&run.text);
        }
        putchar('\n');
    }
}

static bool number_option(const char *arg, const char *name, uint32_t *out) {
    size_t length = strlen(name);
    if (strncmp(arg, name, length) != 0 || arg[length] != '=') return false;
    char *end;
    unsigned long value = strtoul(arg + length + 1, &end, 10);
    if (*end != '\0' || end == arg + length + 1 || value > UINT32_MAX) {
        fprintf(stderr, "bench: %s takes a number\n", name);
        exit(2);
    }
    *out = (uint32_t)value;
    return true;
}

static void usage(void) {
    fputs("usage: bench [--jsonl] [--only=NAME[,NAME]] [--size=N] [--filter=PREFIX] [--samples=15]\n"
          "             [--sample-ms=10] [--warmup-ms=100] [--max-case-s=10] [--quick] [--smoke] [--list]\n",
          stderr);
    exit(2);
}

static void parse_options(int argc, char **argv, bench_options *options) {
    *options = (bench_options){ .samples = 15, .sample_ms = 10, .warmup_ms = 100, .max_case_s = 10 };
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--jsonl") == 0) options->jsonl = true;
        else if (strcmp(arg, "--smoke") == 0) options->smoke = true;
        else if (strcmp(arg, "--list") == 0) options->list = true;
        else if (strcmp(arg, "--quick") == 0) {
            options->quick = true;
            options->samples = 5;
            options->sample_ms = 5;
            options->warmup_ms = 20;
        } else if (strncmp(arg, "--only=", 7) == 0) options->only = arg + 7;
        else if (strncmp(arg, "--filter=", 9) == 0) options->filter = arg + 9;
        else if (number_option(arg, "--size", &options->size) || number_option(arg, "--samples", &options->samples) ||
                 number_option(arg, "--sample-ms", &options->sample_ms) ||
                 number_option(arg, "--warmup-ms", &options->warmup_ms) ||
                 number_option(arg, "--max-case-s", &options->max_case_s)) {
            continue;
        } else {
            fprintf(stderr, "bench: unknown argument %s\n", arg);
            usage();
        }
    }
    if (options->samples == 0) usage();
}

int main(int argc, char **argv) {
    bench_options options;
    parse_options(argc, argv, &options);
    if (options.list) {
        list_cases(&options);
        return 0;
    }
    if (options.jsonl && !options.smoke) {
        struct timespec resolution;
        clock_getres(CLOCK_MONOTONIC, &resolution);
        uint32_t version = sjs_version();
        printf("{\"meta\":{\"version\":\"%u.%u.%u\",\"clockResolutionNs\":%lld,\"samples\":%u,\"sampleMs\":%u,"
               "\"warmupMs\":%u,\"maxCaseSeconds\":%u,\"quick\":%s}}\n",
               (unsigned)(version >> 16), (unsigned)((version >> 8) & 0xFFu), (unsigned)(version & 0xFFu),
               (long long)resolution.tv_sec * 1000000000ll + resolution.tv_nsec, (unsigned)options.samples,
               (unsigned)options.sample_ms, (unsigned)options.warmup_ms, (unsigned)options.max_case_s,
               options.quick ? "true" : "false");
    } else if (!options.smoke) {
        print_header();
    }
    fflush(stdout);
    bench_session session = { &options, { 0 } };
    for (size_t i = 0; i < CASE_COUNT; i++) {
        if (!selected(&cases[i], &options)) continue;
        uint32_t sizes[8];
        uint32_t count = case_sizes(&cases[i], &options, sizes);
        for (uint32_t s = 0; s < count; s++) run_case(&session, &cases[i], sizes[s]);
    }
    bench_tally tally = session.tally;
    if (options.smoke) {
        if (tally.failed > 0 || tally.refused > 0) {
            fprintf(stderr, "bench: %u of %u cases fail the smoke run\n", (unsigned)(tally.failed + tally.refused),
                    (unsigned)tally.ran);
            return 1;
        }
        printf("bench: %u cases pass the smoke run\n", (unsigned)tally.ran);
        return 0;
    }
    if (tally.ran == 0) {
        fputs("bench: no case matches\n", stderr);
        return 2;
    }
    return tally.failed > 0 ? 1 : 0;
}
