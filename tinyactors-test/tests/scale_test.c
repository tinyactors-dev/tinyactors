/*
 * Invariant 17, population-independent actor cost: creating, replacing, or
 * destroying one actor costs the same whether the system holds a hundred
 * actors or four million (tinyactors-core/docs/internal/invariants.md, and
 * "Per-actor operations cost the same at every population" in
 * docs/internal/memory-model.md). The same three measurements are the
 * benchmark behind the four-million-actor target of that page.
 *
 * The claim is about a population, so a fixed-size test cannot state it: it
 * is measured at a series of populations, each about four times the last,
 * up to the four-million-actor target, and the per-actor cost must stay flat
 * from each one to the next. A table walk inside a spawn
 * costs one factor of four per factor of four in population, so a bug shows
 * up as a ratio near four against a bound of two, and machine noise cannot
 * cross that gap. Each population is measured three times and the cheapest
 * run counts, which is the standard defence against a scheduler that steals
 * a slice mid-measurement. Cost is CPU time (ISO C `clock()`), not wall
 * time, so a loaded machine slows the test down without changing what it
 * measures. What it never does is assert an absolute time: that would fail
 * on a slower machine and pass on a faster one while a real regression hid
 * behind the hardware.
 *
 * The same holds for delayed sends (ADR 0054): with every actor of the
 * population holding one, inspecting an actor — which lists its sends — and
 * arming then cancelling one more actor's send cost the same at both sizes.
 *
 * Catches: a spawn, upgrade, or destroy that walks the actor table (the
 * free-list-less ta_actor_claim_slot() this file was written for: 18 us to
 * spawn an actor against 100k residents, 36 us against 200k); a per-actor
 * allocation that grows with the population; an upgrade that leaks the
 * incumbent's memory or its definition; a replacement that does not take
 * over the logical address; a retired revision freed while its last
 * incumbent still runs, or never freed after it goes.
 *
 * Run it as a benchmark over one population of your choosing, with no
 * ratios and no bound:
 *
 *     tinyactors-test/build/scale_test --report 4000000
 *
 * It stays a "just run it" test that adapts to where it runs, and guards
 * against misjudging that place or failing by accident:
 * - memory it may use is what the OS offers *and* what a container allows:
 *   on Linux MemAvailable, capped by a cgroup v2 (memory.max) or v1
 *   (memory.limit_in_bytes) limit less what the cgroup already uses; on
 *   macOS free, inactive, speculative and purgeable pages, halved under
 *   the kernel's "warn" memory pressure and none under "critical";
 * - where memory cannot be read, only the two smallest populations run;
 * - one untimed warm-up population runs first, so CPU frequency scaling and
 *   cold caches do not inflate the smallest size;
 * - a population step whose ratio fails is measured once more, both sizes,
 *   before the test fails, so one stolen time slice cannot fail CI while a
 *   real walk (a ratio near four) still does;
 * - TAT_SCALE_MAX_POPULATION and --report reject values that are not a
 *   positive whole number, instead of reading them as zero;
 * - it prints the machine and its load first, and at the end which sizes
 *   ran and why larger ones did not.
 */
/* getrusage() and friends, which glibc hides under -std=c11. */
#if defined(__linux__)
#define _DEFAULT_SOURCE
#endif

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

#include "allocator.h"

#define STR(text) {.ptr = (text), .len = (uint32_t)(sizeof(text) - 1u)}

/*
 * The populations measured, smallest first. Each neighbouring pair is a
 * ratio check, so each step grows the population by roughly four and a
 * table walk shows up as a ratio near the step against a bound of two.
 * Four million actors is the target of docs/internal/memory-model.md.
 *
 * The test sizes itself to the machine. The first two populations always
 * run. Before each larger one it predicts the peak memory that population
 * needs, from the process's own peak so far, and runs it only when that
 * fits in three quarters of the memory available at that moment; otherwise
 * it stops there and says why. Up to four million actors the whole test
 * takes about 30 seconds and a peak of about 9.5 GB.
 *
 * TAT_SCALE_MAX_POPULATION overrides the sizing: every population up to it
 * runs, whatever memory the machine reports, and none above it. At least
 * two populations always run.
 */
static const uint32_t POPULATIONS[] = {50000u, 200000u, 1000000u, 4000000u};
#define POPULATION_COUNT (sizeof(POPULATIONS) / sizeof(POPULATIONS[0]))
#define CHURN_ACTORS 20000u
#define INSPECTIONS 200000u
#define INSPECTION_SAMPLE 256u
/* Pages of the actor walk (ADR 0062): their size, and how many are taken. */
#define WALK_PAGE_SIZE 64u
#define PAGES 10000u
#define REPETITIONS 3u
/* An untimed population measured first, so frequency scaling and cold caches
 * warm up on it rather than on the smallest measured population. */
#define WARMUP_POPULATION 20000u

/*
 * A four-fold population may cost no more than twice as much per actor.
 * Flat costs measure at about 1.05; one table walk per spawn measures at
 * about 4.
 */
#define COST_RATIO_BOUND 2.0

/*
 * A page of the actor walk has a looser bound. Each visit reads a different
 * actor's memory, so its cost follows the memory hierarchy: warm, the step
 * from 1M to 4M actors (about 9.5 GB) measures 1.75-1.9 from TLB and cache
 * misses alone, too close to 2 to be a stable test. The bug this check is
 * for, a page that costs its population or its cursor position, measures
 * the population step, 4 to 5 times, because the pages start spread over
 * the whole table. Measured on an Apple M-series machine: 1.4-1.6,
 * 2.25-2.57 and 1.56-1.72 for the three steps on correct code, and 3.7 to
 * 7.6 with a planted O(position) walk. The middle step is closest: it is
 * where the population outgrows the caches.
 */
#define WALK_COST_RATIO_BOUND 3.0

/*
 * What one resident named actor of the null data model may cost, in bytes
 * the core asked the allocator for: the actor, its session storage, and the
 * copy of its address ("scale" / "actor_<n>" here). The null data model
 * adds no instance (SCXML B.1), so this is core's own per-actor cost, and
 * the budget is what makes the four-million-actor target arithmetic rather
 * than aspiration: 4M x 768 B is under 3 GB.
 */
#define ACTOR_BYTE_BUDGET 768u

static void require(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "scale test failed: %s\n", message);
        exit(1);
    }
}

/* ------------------------------------------------------------------------
 * An allocator that counts what core holds, over the host's libc boundary.
 */

typedef struct {
    ta_allocator backing;
    uint64_t live_bytes;
    uint64_t peak_bytes;
    int64_t live_blocks;
    /* The largest single block asked for since it was last reset. */
    uint64_t largest_block;
    /* Allocator calls since they were last reset (ADR 0064). */
    uint64_t alloc_calls;
    uint64_t free_calls;
} counting_allocator;

static void *counting_alloc(void *self, size_t size, size_t alignment) {
    counting_allocator *counts = self;
    void *block = counts->backing.v->alloc(counts->backing.self, size, alignment);
    counts->alloc_calls += 1u;
    if (block != NULL) {
        if (size > counts->largest_block) {
            counts->largest_block = size;
        }
        counts->live_bytes += size;
        counts->live_blocks += 1;
        if (counts->live_bytes > counts->peak_bytes) {
            counts->peak_bytes = counts->live_bytes;
        }
    }
    return block;
}

static void counting_free(void *self, const ta_allocator_free_params *params) {
    counting_allocator *counts = self;
    if (params->ptr == NULL) {
        return;
    }
    counts->free_calls += 1u;
    counts->live_bytes -= params->size;
    counts->live_blocks -= 1;
    counts->backing.v->free(counts->backing.self, params);
}

static const ta_allocator_vtable COUNTING_VTABLE = {.alloc = counting_alloc, .free = counting_free};

/* ------------------------------------------------------------------------
 * A directory, which named actors need and the host owns (ADR 0037).
 *
 * Open addressing over the address handle (ADR 0065): equal addresses are
 * one pointer, so the directory hashes the pointer and never the text. It is
 * sized so it never rehashes during a run: this is the population's index,
 * and a host cost the measurements must not mistake for core's. A kept entry
 * retains its address; a removed one gives it back and leaves a tombstone,
 * so probing does not break.
 */

typedef struct {
    ta_address *address;
    ta_actor_directory_entry entry;
    bool used;
} directory_slot;

typedef struct {
    directory_slot *slots;
    size_t mask;
} directory;

static void directory_init(directory *dir, size_t population) {
    size_t capacity = 1;
    while (capacity < population * 4u) {
        capacity <<= 1;
    }
    dir->slots = calloc(capacity, sizeof(directory_slot));
    require(dir->slots != NULL, "directory allocation");
    dir->mask = capacity - 1u;
}

/* Gives back every address the directory holds; before the system goes. */
static void directory_release(directory *dir) {
    for (size_t index = 0; index <= dir->mask; index += 1) {
        ta_address_release(dir->slots[index].address);
        dir->slots[index].address = NULL;
    }
}

static void directory_destroy(directory *dir) {
    free(dir->slots);
    dir->slots = NULL;
}

static directory_slot *directory_find(directory *dir, const ta_address *address) {
    uint64_t bits = (uint64_t)(uintptr_t)address;
    bits ^= bits >> 33;
    bits *= 0xff51afd7ed558ccdULL;
    bits ^= bits >> 33;
    for (size_t at = (size_t)bits & dir->mask;; at = (at + 1u) & dir->mask) {
        directory_slot *slot = &dir->slots[at];
        if (!slot->used || slot->address == address) {
            return slot;
        }
    }
}

static ta_result directory_claim(void *self, const ta_actor_directory_claim_params *params) {
    directory_slot *slot = directory_find(self, params->address);
    if (slot->used) {
        return TA_ERR_STALE;
    }
    ta_address_retain(params->address);
    slot->address = params->address;
    slot->entry = params->entry;
    slot->used = true;
    return TA_OK;
}

static ta_result directory_resolve(void *self, const ta_actor_directory_resolve_params *params) {
    const directory_slot *slot = directory_find(self, params->address);
    if (!slot->used || slot->entry.kind == TA_ACTOR_DIRECTORY_NONE) {
        return TA_ERR_NOT_FOUND;
    }
    *params->out = slot->entry;
    return TA_OK;
}

/* replace and remove are compare-and-swaps against the caller's expectation. */
static ta_result directory_replace(void *self, const ta_actor_directory_replace_params *params) {
    directory_slot *slot = directory_find(self, params->address);
    if (!slot->used) {
        return TA_ERR_NOT_FOUND;
    }
    if (slot->entry.kind != params->expected.kind || slot->entry.actor != params->expected.actor) {
        return TA_ERR_STALE;
    }
    slot->entry = params->replacement;
    return TA_OK;
}

static ta_result directory_remove(void *self, const ta_actor_directory_remove_params *params) {
    directory_slot *slot = directory_find(self, params->address);
    if (!slot->used) {
        return TA_ERR_NOT_FOUND;
    }
    if (slot->entry.kind != params->expected.kind || slot->entry.actor != params->expected.actor) {
        return TA_ERR_STALE;
    }
    /* A tombstone: the slot stays used so probing does not break. */
    ta_address_release(slot->address);
    slot->address = NULL;
    slot->entry = (ta_actor_directory_entry){.kind = TA_ACTOR_DIRECTORY_NONE};
    return TA_OK;
}

static const ta_actor_directory_vtable DIRECTORY_VTABLE = {
    .claim = directory_claim,
    .resolve = directory_resolve,
    .replace = directory_replace,
    .remove = directory_remove,
};

/* ------------------------------------------------------------------------
 * Two revisions of one chart family. The upgrade path needs both: a
 * replacement is a new revision of the same family under the same address
 * (ADR 0038), so v2 is a different chart, not the same one registered twice.
 */

static const ta_scxml_state_spec STATES_V1[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("idle"),
        .transitions =
            (const ta_scxml_transition_spec[]){
                {.kind = TA_SCXML_TRANSITION_EXTERNAL,
                 .event = STR("go"),
                 .targets = (const ta_str[]){STR("busy"), {0}}},
                {0},
            },
    },
    {.kind = TA_SCXML_STATE_ATOMIC, .id = STR("busy")},
    {0},
};

static const ta_scxml_state_spec STATES_V2[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("idle"),
        .transitions =
            (const ta_scxml_transition_spec[]){
                {.kind = TA_SCXML_TRANSITION_EXTERNAL,
                 .event = STR("go"),
                 .targets = (const ta_str[]){STR("working"), {0}}},
                {0},
            },
    },
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("working"),
        .transitions =
            (const ta_scxml_transition_spec[]){
                {.kind = TA_SCXML_TRANSITION_EXTERNAL,
                 .event = STR("done"),
                 .targets = (const ta_str[]){STR("idle"), {0}}},
                {0},
            },
    },
    {0},
};

/*
 * One delayed send far in the future, armed on entry: every timed actor holds
 * one. `rearm` cancels it with <cancel> and arms it again, which is the
 * cancel path ADR 0054 D7 measures on a resident actor.
 */
static const ta_scxml_state_spec STATES_TIMED[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("waiting"),
        .onentry =
            (const ta_scxml_handler_spec[]){
                {.actions =
                     (const ta_scxml_action_spec[]){
                         {.kind = TA_SCXML_ACTION_SEND,
                          .as.send = {.id = {.kind = TA_SCXML_ID_LITERAL, .as.literal = STR("timeout")},
                                      .event = {.kind = TA_SCXML_EVENT_NAME, .as.name = STR("timeout")},
                                      .delay = {.kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("60s")}}},
                         {0},
                     }},
                {0},
            },
        .transitions =
            (const ta_scxml_transition_spec[]){
                {.kind = TA_SCXML_TRANSITION_INTERNAL,
                 .event = STR("rearm"),
                 .actions =
                     (const ta_scxml_action_spec[]){
                         {.kind = TA_SCXML_ACTION_CANCEL,
                          .as.cancel = {.sendid = {.kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("timeout")}}},
                         {.kind = TA_SCXML_ACTION_SEND,
                          .as.send = {.id = {.kind = TA_SCXML_ID_LITERAL, .as.literal = STR("timeout")},
                                      .event = {.kind = TA_SCXML_EVENT_NAME, .as.name = STR("timeout")},
                                      .delay = {.kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("60s")}}},
                         {0},
                     }},
                {0},
            },
    },
    {0},
};

typedef struct {
    const char *family;
    const char *revision;
    const ta_scxml_state_spec *states;
} revision_spec;

static ta_definition_id register_revision(ta_system *system, ta_allocator allocator, const revision_spec *spec) {
    const char *revision = spec->revision;
    const ta_scxml_state_spec *states = spec->states;
    ta_definition_identity identity = {
        .family = {.name = {.ptr = spec->family, .len = (uint32_t)strlen(spec->family)}},
        .revision = {.ptr = (const uint8_t *)revision, .len = (uint32_t)strlen(revision)},
    };
    ta_definition *definition = NULL;
    ta_result result = ta_definition_compile_spec(
        system,
        &(ta_definition_compile_params){
            .spec = &(ta_scxml_spec){.datamodel = STR("null"), .identity = &identity, .states = states},
            .datamodel = ta_null_datamodel(),
            .allocator = allocator,
            .out = &definition,
        });
    require(result == TA_OK, "compiling a revision");
    ta_definition_id id = 0;
    require(ta_system_register_definition(system, definition, &id) == TA_OK, "registering a revision");
    return id;
}

/* ------------------------------------------------------------------------
 * One measured round at one population.
 */

typedef struct {
    double spawn_us;   /* per actor: spawn and bootstrap a named actor */
    double upgrade_us; /* per actor: prepare, run, activate, destroy incumbent */
    double churn_us;   /* per actor: spawn, bootstrap and destroy an ephemeral */
    double inspect_us; /* per inspection of one timed actor, its sends listed */
    double timer_us;   /* per actor: spawn a timed ephemeral (arms a send), destroy it (discards it) */
    double cancel_us;  /* per event: <cancel> a resident actor's send and arm it again */
    double page_us;    /* per actor visited: one page of the actor walk, from where the last one ended */
    double median_spawn_us;  /* the median single spawn-and-bootstrap, thread CPU time */
    double longest_spawn_us; /* the longest single one, reported, not bounded: see below */
    uint64_t largest_spawn_block; /* the largest block core allocated while spawning (ADR 0063) */
    double actor_bytes;
    uint64_t peak_bytes;
} round_cost;

/*
 * CPU time, which is what the work costs; wall time is what the machine was
 * doing meanwhile. CLOCKS_PER_SEC is an ISO C constant, so this needs no
 * POSIX clock and no platform branch.
 */
static double cpu_seconds(void) {
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

/*
 * The calling thread's CPU time in nanoseconds, for timing one spawn: clock()
 * ticks in microseconds, longer than a spawn, and a thread clock does not
 * count the time the thread was preempted, which is most of the noise in a
 * single short measurement. Reading it costs about 0.1 us, paid equally at
 * every population.
 */
static uint64_t thread_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static int compare_u32(const void *left, const void *right) {
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

static double per_actor_us(double seconds, size_t actors) {
    return seconds * 1e6 / (double)actors;
}

/* Counts the actors a page visits. */
static ta_result count_visit(void *self, const ta_actor_inspection *inspection) {
    (void)inspection;
    *(size_t *)self += 1u;
    return TA_OK;
}


/*
 * Runs `actor`'s next input to its end: one ta_run() for that session (its
 * session filter), with room for its outcome. True when the input completed
 * with the actor stable.
 */
static bool run_input(ta_system *system, ta_actor_id actor) {
    ta_input_outcome outcome = {.kind = TA_INPUT_TAKEN};
    uint32_t count = 0;
    ta_run_status status = TA_RUN_MORE;
    return ta_run(system, &(ta_run_params){.budget = 100, .session = actor, .outcomes = &outcome,
                                           .outcome_capacity = 1, .outcome_count = &count, .status = &status}) ==
               TA_OK &&
           count == 1 && (outcome.kind == TA_INPUT_COMPLETED || outcome.kind == TA_INPUT_IGNORED);
}

static round_cost measure(size_t population) {
    counting_allocator counts = {.backing = tat_libc_allocator()};
    ta_allocator allocator = {.self = &counts, .v = &COUNTING_VTABLE};
    ta_system *system = NULL;
    require(ta_system_create(&(ta_system_config){.allocator = allocator}, &system) == TA_OK, "system creation");
    /* Measured with every trace mask zero, which a system starts with, so the
     * per-actor figures include what tracing costs every actor untraced:
     * the step counters (ADR 0067). */
    require(ta_system_set_trace_masks(system, &(ta_trace_masks){ 0 }) == TA_OK, "no trace kinds");

    directory dir;
    directory_init(&dir, population);
    require(ta_system_set_actor_directory(system, (ta_actor_directory){.self = &dir, .v = &DIRECTORY_VTABLE}) == TA_OK,
            "directory installation");

    ta_definition_id v1 = register_revision(system, allocator, &(revision_spec){"scale", "v1", STATES_V1});
    ta_definition_id v2 = register_revision(system, allocator, &(revision_spec){"scale", "v2", STATES_V2});
    ta_definition_id timed = register_revision(system, allocator, &(revision_spec){"timed", "v1", STATES_TIMED});

    ta_actor_id *actors = calloc(population, sizeof(ta_actor_id));
    char *names = calloc(population, 24);
    require(actors != NULL && names != NULL, "fixture allocation");
    for (size_t index = 0; index < population; index += 1) {
        snprintf(&names[index * 24], 24, "actor_%zu", index);
    }

    /* Spawning a named actor, from an empty system to the full population. */
    uint64_t empty_bytes = counts.live_bytes;
    uint32_t *spawn_ns = malloc(population * sizeof(uint32_t));
    require(spawn_ns != NULL, "fixture allocation");
    counts.largest_block = 0;
    double started = cpu_seconds();
    uint64_t last_ns = thread_ns();
    for (size_t index = 0; index < population; index += 1) {
        const char *name = &names[index * 24];
        ta_actor_address address = {.family = {STR("scale")},
                                    .name = {.ptr = name, .len = (uint32_t)strlen(name)}};
        require(ta_actor_spawn(system, &(ta_actor_spawn_params){
                                           .definition = v1,
                                           .address = &address,
                                           .out = &actors[index],
                                       }) == TA_OK,
                "spawning a named actor");
        require(run_input(system, actors[index]), "bootstrapping a named actor");
        uint64_t now_ns = thread_ns();
        uint64_t spent = now_ns - last_ns;
        spawn_ns[index] = spent > UINT32_MAX ? UINT32_MAX : (uint32_t)spent;
        last_ns = now_ns;
    }
    double spawned = cpu_seconds();
    uint64_t largest_spawn_block = counts.largest_block;
    qsort(spawn_ns, population, sizeof(uint32_t), compare_u32);
    double median_spawn_us = (double)spawn_ns[population / 2u] / 1000.0;
    double longest_spawn_us = (double)spawn_ns[population - 1u] / 1000.0;
    free(spawn_ns);
    double actor_bytes = (double)(counts.live_bytes - empty_bytes) / (double)population;

    /* Upgrading every one of them to v2 under its own address (ADR 0043). */
    for (size_t index = 0; index < population; index += 1) {
        ta_actor_id replacement = 0;
        require(ta_actor_prepare(system, &(ta_actor_prepare_params){
                                             .incumbent = actors[index],
                                             .definition = v2,
                                             .out = &replacement,
                                         }) == TA_OK,
                "preparing a replacement");
        require(run_input(system, replacement), "bootstrapping a replacement");
        require(ta_actor_publish(system, &(ta_actor_publish_params){
                                             .actor = replacement,
                                             .incumbent = actors[index],
                                         }) == TA_OK,
                "publishing a replacement");
        require(ta_actor_destroy(system, actors[index]) == TA_OK, "destroying an incumbent");
        actors[index] = replacement;
    }
    double upgraded = cpu_seconds();

    /*
     * Ephemeral actors against the live population: the children a long-lived
     * actor spawns and drops. They are anonymous, so this measures the core
     * and not the directory.
     */
    for (size_t index = 0; index < CHURN_ACTORS; index += 1) {
        ta_actor_id ephemeral = 0;
        require(ta_actor_spawn(system, &(ta_actor_spawn_params){.definition = v2, .out = &ephemeral}) == TA_OK,
                "spawning an ephemeral actor");
        require(run_input(system, ephemeral), "bootstrapping an ephemeral actor");
        require(ta_actor_destroy(system, ephemeral) == TA_OK, "destroying an ephemeral actor");
    }
    double churned = cpu_seconds();

    /*
     * Delayed sends against the live population (ADR 0054): as many timed
     * actors as there are residents, each holding one send in the system's
     * deadline index. Neither inspecting one of them nor arming and
     * cancelling one more send may walk that index.
     */
    ta_actor_id *timed_actors = calloc(population, sizeof(ta_actor_id));
    require(timed_actors != NULL, "fixture allocation");
    for (size_t index = 0; index < population; index += 1) {
        require(ta_actor_spawn(system, &(ta_actor_spawn_params){.definition = timed, .out = &timed_actors[index]}) ==
                    TA_OK,
                "spawning a timed actor");
        require(run_input(system, timed_actors[index]), "bootstrapping a timed actor");
    }
    /*
     * A fixed sample spread over the population, inspected round-robin: the
     * sample stays in cache at both sizes, so the ratio measures work done,
     * not cache misses on a four times larger actor table.
     */
    size_t stride = population / INSPECTION_SAMPLE;
    double inspect_started = cpu_seconds();
    for (size_t index = 0; index < INSPECTIONS; index += 1) {
        ta_actor_inspection timed_view;
        require(ta_actor_inspect(system, timed_actors[(index % INSPECTION_SAMPLE) * stride], &timed_view) == TA_OK &&
                    timed_view.delayed_send_count == 1 && timed_view.delayed_sends[0].due == 60000,
                "inspecting a timed actor lists its one send");
    }
    double inspected = cpu_seconds();
    for (size_t index = 0; index < CHURN_ACTORS; index += 1) {
        ta_actor_id ephemeral = 0;
        require(ta_actor_spawn(system, &(ta_actor_spawn_params){.definition = timed, .out = &ephemeral}) == TA_OK,
                "spawning a timed ephemeral actor");
        require(run_input(system, ephemeral), "bootstrapping a timed ephemeral actor");
        require(ta_actor_destroy(system, ephemeral) == TA_OK, "destroying a timed ephemeral actor");
    }
    double timers_churned = cpu_seconds();

    /*
     * A real <cancel> on a resident actor (ADR 0054 D7): `rearm` cancels the
     * actor's one send and arms it again, so the owner's list is emptied and
     * refilled and its heap entry re-keyed, against the whole population's
     * sends. The same round-robin sample as inspection keeps the ratio about
     * work, not cache misses.
     */
    ta_atom rearm_atom = 0;
    ta_event_name_id rearm_name = 0;
    require(ta_system_intern_atom(system, (ta_str)STR("rearm"), &rearm_atom) == TA_OK &&
                ta_event_name_intern(system, &(ta_atom_list){.atoms = &rearm_atom, .count = 1}, &rearm_name) == TA_OK,
            "interning rearm");
    for (size_t index = 0; index < CHURN_ACTORS; index += 1) {
        ta_actor_id timed_actor = timed_actors[(index % INSPECTION_SAMPLE) * stride];
        ta_event rearm = {.name = rearm_name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE};
        require(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = timed_actor, .event = &rearm }) == TA_OK, "delivering rearm");
        require(run_input(system, timed_actor), "cancelling and re-arming a send");
    }
    double rearmed = cpu_seconds();
    ta_actor_inspection rearmed_view;
    require(ta_actor_inspect(system, timed_actors[0], &rearmed_view) == TA_OK &&
                rearmed_view.delayed_send_count == 1 && rearmed_view.delayed_sends[0].due == 60000,
            "a re-armed actor holds exactly its one send");

    /*
     * Pages of the actor walk (ADR 0062): a page costs its own size wherever
     * it starts and whatever the population. The pages start evenly spread
     * over the whole table, page i at i x population / PAGES, so the average
     * start is half the population at every size: a walk whose page cost
     * grows with its start position (one that re-walks from slot 0, say)
     * grows with each population step, 4 to 5 times per step, and fails.
     * Walked from slot 0 instead, the positions stop growing once the pass
     * is shorter than the population, and such a bug hid at 1M and 4M.
     *
     * The same pages are walked twice and only the second walk is timed.
     * The first touch of a large population's memory can page-fault, or
     * decompress pages the OS compressed, and clock() counts that as CPU
     * time. Measured cold, the step from 1M to 4M actors reached ratios of
     * 2.0-2.4 from that alone.
     */
    size_t visited = 0;
    double warm_started = 0.0;
    for (unsigned pass = 0; pass < 2u; pass += 1u) {
        visited = 0;
        warm_started = cpu_seconds();
        for (unsigned page = 0; page < PAGES; page += 1u) {
            uint32_t from = (uint32_t)(((uint64_t)page * population) / PAGES);
            uint32_t next = 0;
            require(ta_system_visit_actors(system, &(ta_actor_visit_params){
                                                       .inspector = {.self = &visited, .visit = count_visit},
                                                       .from = from,
                                                       .limit = WALK_PAGE_SIZE,
                                                       .next = &next,
                                                   }) == TA_OK,
                    "a page of the actor walk");
            require(next == TA_ACTOR_VISIT_END || next > from, "a page moves the cursor forward");
        }
    }
    double paged = cpu_seconds();
    require(visited >= (size_t)PAGES * WALK_PAGE_SIZE / 2u, "the pages visited actors");
    free(timed_actors);

    /*
     * The upgrade really happened: the address names the replacement, the
     * replacement runs v2, and v1 is gone with its last incumbent (ADR 0043).
     * A benchmark that measured a refused upgrade would otherwise look fast.
     */
    const char *first = &names[0];
    ta_actor_address address = {.family = {STR("scale")}, .name = {.ptr = first, .len = (uint32_t)strlen(first)}};
    ta_actor_id found = 0;
    require(ta_actor_find(system, &(ta_actor_find_params){.address = &address, .out = &found}) == TA_OK &&
                found == actors[0],
            "the address names the replacement");
    ta_actor_inspection view;
    require(ta_actor_inspect(system, actors[0], &view) == TA_OK && view.definition_id == v2,
            "the replacement runs the new revision");
    ta_definition_inspection facts;
    require(ta_definition_inspect(system, v2, &facts) == TA_OK && facts.actor_count == population,
            "every actor moved to the new revision");
    require(ta_system_retire_definition(system, v1) == TA_OK, "retiring the old revision");
    require(ta_definition_inspect(system, v1, &facts) == TA_ERR_NOT_FOUND,
            "the old revision is freed with its last incumbent");

    uint64_t peak_bytes = counts.peak_bytes;
    directory_release(&dir);
    ta_system_destroy(system);
    directory_destroy(&dir);
    free(actors);
    free(names);
    require(counts.live_bytes == 0 && counts.live_blocks == 0, "the system returned every byte it was given");

    return (round_cost){
        .spawn_us = per_actor_us(spawned - started, population),
        .upgrade_us = per_actor_us(upgraded - spawned, population),
        .churn_us = per_actor_us(churned - upgraded, CHURN_ACTORS),
        .inspect_us = per_actor_us(inspected - inspect_started, INSPECTIONS),
        .timer_us = per_actor_us(timers_churned - inspected, CHURN_ACTORS),
        .cancel_us = per_actor_us(rearmed - timers_churned, CHURN_ACTORS),
        .page_us = per_actor_us(paged - warm_started, visited),
        .median_spawn_us = median_spawn_us,
        .longest_spawn_us = longest_spawn_us,
        .largest_spawn_block = largest_spawn_block,
        .actor_bytes = actor_bytes,
        .peak_bytes = peak_bytes,
    };
}

/* The cheapest of several rounds: the one least interrupted by the machine. */
/* Keeps, in `best`, the cheaper of each cost of `best` and `next`. */
static void keep_cheapest(round_cost *best_out, round_cost next) {
    round_cost best = *best_out;
    if (next.spawn_us < best.spawn_us) {
        best.spawn_us = next.spawn_us;
    }
    if (next.upgrade_us < best.upgrade_us) {
        best.upgrade_us = next.upgrade_us;
    }
    if (next.churn_us < best.churn_us) {
        best.churn_us = next.churn_us;
    }
    if (next.inspect_us < best.inspect_us) {
        best.inspect_us = next.inspect_us;
    }
    if (next.timer_us < best.timer_us) {
        best.timer_us = next.timer_us;
    }
    if (next.cancel_us < best.cancel_us) {
        best.cancel_us = next.cancel_us;
    }
    if (next.page_us < best.page_us) {
        best.page_us = next.page_us;
    }
    /* The smallest longest spawn of the rounds: a spike every round has
     * is structural; one that noise put in a single round is not. */
    if (next.longest_spawn_us < best.longest_spawn_us) {
        best.longest_spawn_us = next.longest_spawn_us;
    }
    if (next.median_spawn_us < best.median_spawn_us) {
        best.median_spawn_us = next.median_spawn_us;
    }
    if (next.largest_spawn_block > best.largest_spawn_block) {
        best.largest_spawn_block = next.largest_spawn_block;
    }
    *best_out = best;
}

static round_cost measure_best(size_t population, unsigned repetitions) {
    round_cost best = measure(population);
    for (unsigned round = 1; round < repetitions; round += 1) {
        keep_cheapest(&best, measure(population));
    }
    return best;
}

static void report(const char *label, size_t population, round_cost cost) {
    printf("%-10s %9zu   %8.2f %8.2f %8.2f %8.3f %8.2f %8.3f %8.3f   %8.2f %9.1f %8.1f   %7.1f %10.1f\n", label, population,
           cost.spawn_us, cost.upgrade_us, cost.churn_us, cost.inspect_us, cost.timer_us, cost.cancel_us, cost.page_us,
           cost.median_spawn_us, cost.longest_spawn_us, (double)cost.largest_spawn_block / 1024.0, cost.actor_bytes, (double)cost.peak_bytes / (1024.0 * 1024.0));
    /* A failure below writes to stderr; keep the table ahead of it. */
    fflush(stdout);
}

static void report_header(void) {
    printf("%-10s %9s   %8s %8s %8s %8s %8s %8s %8s   %8s %9s %8s   %7s %10s\n", "", "actors", "spawn", "upgrade", "churn",
           "inspect", "timer", "cancel", "page", "median", "longest", "block", "bytes", "peak MB");
    printf("%-10s %9s   %8s %8s %8s %8s %8s %8s %8s   %8s %9s %8s   %7s %10s\n", "", "", "us", "us", "us", "us", "us", "us", "us",
           "us", "us", "KiB", "/actor", "");
}

/* The pair of populations a ratio is taken over, for the failure message. */
static uint32_t small_population = 0;
static uint32_t large_population = 0;

/* The first ratio that failed in a pair, named so the report says which walk. */
static char ratio_failure[512];

static void require_flat_within(const char *what, double small, double large, double bound) {
    double ratio = small > 0.0 ? large / small : 0.0;
    if (ratio <= bound || ratio_failure[0] != 0) {
        return;
    }
    snprintf(ratio_failure, sizeof ratio_failure,
             "%s measures %.2f against %u residents and %.2f against %u, a ratio of %.2f against a bound of "
             "%.2f: the cost of one actor grows with the population (invariant 17)",
             what, small, small_population, large, large_population, ratio, bound);
}

static void require_flat(const char *what, double small, double large) {
    require_flat_within(what, small, large, COST_RATIO_BOUND);
}

/*
 * Every cost, and per-actor memory, flat from one population to the next.
 * False, with the reason in ratio_failure, when a timed ratio failed;
 * per-actor memory is exact, so it fails at once.
 */
static bool flat_pair(uint32_t small_count, const round_cost *small, uint32_t large_count, const round_cost *large) {
    small_population = small_count;
    large_population = large_count;
    ratio_failure[0] = 0;
    require_flat("spawning an actor", small->spawn_us, large->spawn_us);
    require_flat("upgrading an actor", small->upgrade_us, large->upgrade_us);
    require_flat("an ephemeral actor", small->churn_us, large->churn_us);
    require_flat("inspecting a timed actor", small->inspect_us, large->inspect_us);
    require_flat("a timed ephemeral actor", small->timer_us, large->timer_us);
    require_flat("cancelling and re-arming a delayed send", small->cancel_us, large->cancel_us);
    require_flat_within("a page of the actor walk", small->page_us, large->page_us, WALK_COST_RATIO_BOUND);
/*
 * No spawn may need a block that grows with the population (ADR 0063): a
 * table that doubles allocates four times as much for four times the
 * actors, and one spawn pays for copying it. The longest single spawn
 * shows that too, but allocator and page-fault noise reaches 100 us or
 * more at millions of actors, as large as the copy below four million, so
 * a time bound would pass or fail by accident. The largest block is exact.
 */
require_flat("the largest block a spawn allocates", (double)small->largest_spawn_block,
             (double)large->largest_spawn_block);
/* Per-actor memory is a population claim too: it may not grow. */
    require(large->actor_bytes <= small->actor_bytes + 1.0, "per-actor memory grows with the population");
    return ratio_failure[0] == 0;
}

/*
 * The process's peak resident memory so far, in bytes: a high-water mark,
 * so after a population has run it covers that population and the test's
 * own arrays with it, not just what core allocated.
 */
static uint64_t peak_resident_bytes(void) {
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    return (uint64_t)usage.ru_maxrss; /* bytes on macOS */
#else
    return (uint64_t)usage.ru_maxrss * 1024u; /* kilobytes elsewhere */
#endif
}

#if defined(__linux__)
/* The first unsigned number in `path`, or false; "max" reads as no limit. */
static bool read_u64_file(const char *path, uint64_t *out, bool *unlimited) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    char text[64] = {0};
    bool read = fgets(text, sizeof text, file) != NULL;
    fclose(file);
    if (!read) {
        return false;
    }
    if (strncmp(text, "max", 3) == 0) {
        *unlimited = true;
        return true;
    }
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (end == text) {
        return false;
    }
    *out = (uint64_t)value;
    /* cgroup v1 writes a huge page-aligned number for "no limit". */
    *unlimited = value >= (1ull << 60);
    return true;
}

/* Room left under this process's cgroup memory limit, v2 then v1. */
static bool cgroup_room(uint64_t *out) {
    static const char *const limits[][2] = {
        {"/sys/fs/cgroup/memory.max", "/sys/fs/cgroup/memory.current"},
        {"/sys/fs/cgroup/memory/memory.limit_in_bytes", "/sys/fs/cgroup/memory/memory.usage_in_bytes"},
    };
    for (size_t at = 0; at < sizeof limits / sizeof limits[0]; at += 1) {
        uint64_t limit = 0;
        uint64_t usage = 0;
        bool unlimited = false;
        bool usage_unlimited = false;
        if (!read_u64_file(limits[at][0], &limit, &unlimited)) {
            continue;
        }
        if (unlimited) {
            return false;
        }
        if (!read_u64_file(limits[at][1], &usage, &usage_unlimited)) {
            usage = 0;
        }
        *out = limit > usage ? limit - usage : 0;
        return true;
    }
    return false;
}
#endif

/* Where available_bytes() got its answer, for the report. */
static const char *memory_source = "unknown";

/*
 * Memory the system could give this process now, in bytes: pages that are
 * free or can be reclaimed without swapping, within any container limit.
 * False when the platform does not say.
 */
static bool available_bytes(uint64_t *out) {
#if defined(__APPLE__)
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) != KERN_SUCCESS) {
        return false;
    }
    uint64_t pages = (uint64_t)vm.free_count + vm.inactive_count + vm.speculative_count + vm.purgeable_count;
    uint64_t bytes = pages * (uint64_t)vm_page_size;
    memory_source = "free, inactive, speculative and purgeable pages";
    /* 1 normal, 2 warn, 4 critical: under pressure, reclaimable pages are
     * already being reclaimed for someone else. */
    int pressure = 0;
    size_t length = sizeof pressure;
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &pressure, &length, NULL, 0) == 0) {
        if (pressure >= 4) {
            bytes = 0;
            memory_source = "critical memory pressure";
        } else if (pressure >= 2) {
            bytes /= 2u;
            memory_source = "reclaimable pages, halved under memory pressure";
        }
    }
    *out = bytes;
    return true;
#elif defined(__linux__)
    FILE *meminfo = fopen("/proc/meminfo", "r");
    if (meminfo == NULL) {
        return false;
    }
    char line[256];
    bool found = false;
    while (!found && fgets(line, sizeof(line), meminfo) != NULL) {
        unsigned long long kilobytes = 0;
        if (sscanf(line, "MemAvailable: %llu kB", &kilobytes) == 1) {
            *out = (uint64_t)kilobytes * 1024u;
            found = true;
        }
    }
    fclose(meminfo);
    if (!found) {
        return false;
    }
    memory_source = "MemAvailable";
    uint64_t room = 0;
    if (cgroup_room(&room) && room < *out) {
        *out = room;
        memory_source = "the cgroup memory limit";
    }
    return true;
#else
    (void)out;
    return false;
#endif
}

static double gigabytes(uint64_t bytes) {
    return (double)bytes / (1024.0 * 1024.0 * 1024.0);
}

/*
 * A positive whole number of actors from `text`, or exit 2 naming `what`.
 */
static uint64_t population_arg(const char *what, const char *text) {
    char *end = NULL;
    unsigned long long value = strtoull(text, &end, 10);
    if (text[0] < '0' || text[0] > '9' || end == text || *end != 0 || value == 0 || value > UINT32_MAX) {
        fprintf(stderr, "scale test: %s must be a positive whole number of actors up to %u, not \"%s\"\n", what,
                UINT32_MAX, text);
        exit(2);
    }
    return (uint64_t)value;
}

/* Why the populations stopped where they did, for the closing summary. */
static char stop_reason[256];

/*
 * Whether the population at `index` should run. The first two always do;
 * TAT_SCALE_MAX_POPULATION, when set, decides the rest; otherwise the
 * population runs when its predicted peak fits in three quarters of what
 * is available. The prediction scales the peak so far by population, which
 * overstates it a little, since the peak includes the process's baseline.
 */
static bool should_run(size_t index) {
    if (index < 2) {
        return true;
    }
    uint32_t population = POPULATIONS[index];
    const char *cap_text = getenv("TAT_SCALE_MAX_POPULATION");
    if (cap_text != NULL && cap_text[0] != 0) {
        uint64_t cap = population_arg("TAT_SCALE_MAX_POPULATION", cap_text);
        if (population > cap) {
            snprintf(stop_reason, sizeof stop_reason, "%u is above TAT_SCALE_MAX_POPULATION=%llu", population,
                     (unsigned long long)cap);
            return false;
        }
        return true;
    }
    uint64_t available = 0;
    if (!available_bytes(&available)) {
        snprintf(stop_reason, sizeof stop_reason,
                 "this platform does not report available memory, so only the two smallest sizes run; set "
                 "TAT_SCALE_MAX_POPULATION to go further");
        return false;
    }
    double per_actor = (double)peak_resident_bytes() / (double)POPULATIONS[index - 1];
    uint64_t needed = (uint64_t)(per_actor * (double)population);
    if (needed > available / 4u * 3u) {
        snprintf(stop_reason, sizeof stop_reason,
                 "%u actors would need about %.1f GB, and %.1f GB is available (%s; three quarters of it may be "
                 "used)",
                 population, gigabytes(needed), gigabytes(available), memory_source);
        return false;
    }
    printf("measuring %u actors: needs about %.1f GB, %.1f GB available (%s)\n", population, gigabytes(needed),
           gigabytes(available), memory_source);
    return true;
}

/* The machine and its load, first, so a failure can be read in context. */
static void print_machine(void) {
    struct utsname name;
    char cpu[128] = "unknown CPU";
#if defined(__APPLE__)
    size_t length = sizeof cpu;
    if (sysctlbyname("machdep.cpu.brand_string", cpu, &length, NULL, 0) != 0) {
        snprintf(cpu, sizeof cpu, "unknown CPU");
    }
#elif defined(__linux__)
    FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
    if (cpuinfo != NULL) {
        char line[256];
        while (fgets(line, sizeof line, cpuinfo) != NULL) {
            if (strncmp(line, "model name", 10) == 0 && strchr(line, ':') != NULL) {
                snprintf(cpu, sizeof cpu, "%s", strchr(line, ':') + 2);
                cpu[strcspn(cpu, "\n")] = 0;
                break;
            }
        }
        fclose(cpuinfo);
    }
#endif
    double load[3] = {0.0, 0.0, 0.0};
    bool has_load = getloadavg(load, 3) == 3;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (uname(&name) == 0) {
        printf("machine: %s %s %s, %s, %ld CPUs", name.sysname, name.release, name.machine, cpu, cpus);
    } else {
        printf("machine: %s, %ld CPUs", cpu, cpus);
    }
    if (has_load) {
        printf(", load %.2f %.2f %.2f", load[0], load[1], load[2]);
    }
    printf("\n");
    fflush(stdout);
}

/* ------------------------------------------------------------------------
 * Allocator calls per event (ADR 0064). An actor's internal queue is lent
 * to it while it runs, so once the system is warm an event that moves a
 * simple actor and brings it back to stable makes no allocator call. Nor
 * does one on an actor that keeps its sparse state for a pending delayed
 * send. Before ADR 0064 every such event allocated and freed a queue, and a
 * simple actor its sparse state too. This depends on no population.
 */

#define EVENT_ROUNDS 1000u

static ta_event_name_id intern_event(ta_system *system, ta_str text) {
    ta_atom atom = 0;
    ta_event_name_id name = 0;
    require(ta_system_intern_atom(system, text, &atom) == TA_OK &&
                ta_event_name_intern(system, &(ta_atom_list){.atoms = &atom, .count = 1}, &name) == TA_OK,
            "interning an event name");
    return name;
}

static void run_to_stable(ta_system *system, ta_actor_id actor) {
    require(run_input(system, actor), "running an actor to stable");
}

static void run_event(ta_system *system, ta_actor_id actor, ta_event_name_id name) {
    ta_event event = {.name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE};
    require(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor, .event = &event }) == TA_OK, "delivering an event");
    run_to_stable(system, actor);
}

static void check_event_allocations(void) {
    counting_allocator counts = {.backing = tat_libc_allocator()};
    ta_allocator allocator = {.self = &counts, .v = &COUNTING_VTABLE};
    ta_system *system = NULL;
    require(ta_system_create(&(ta_system_config){.allocator = allocator}, &system) == TA_OK, "system creation");
    ta_definition_id simple = register_revision(system, allocator, &(revision_spec){"events", "v2", STATES_V2});
    ta_definition_id timed = register_revision(system, allocator, &(revision_spec){"timed", "v1", STATES_TIMED});
    ta_actor_id simple_actor = 0;
    ta_actor_id timed_actor = 0;
    require(ta_actor_spawn(system, &(ta_actor_spawn_params){.definition = simple, .out = &simple_actor}) == TA_OK &&
                ta_actor_spawn(system, &(ta_actor_spawn_params){.definition = timed, .out = &timed_actor}) == TA_OK,
            "spawning the event actors");
    run_to_stable(system, simple_actor);
    run_to_stable(system, timed_actor);
    ta_event_name_id go = intern_event(system, (ta_str)STR("go"));
    ta_event_name_id done = intern_event(system, (ta_str)STR("done"));
    ta_event_name_id rearm = intern_event(system, (ta_str)STR("rearm"));
    /* Warm-up: the first run may fill the system's cache of queues. */
    run_event(system, simple_actor, go);
    run_event(system, simple_actor, done);
    run_event(system, timed_actor, rearm);

    counts.alloc_calls = 0;
    counts.free_calls = 0;
    for (unsigned round = 0; round < EVENT_ROUNDS; round += 1u) {
        run_event(system, simple_actor, go);
        run_event(system, simple_actor, done);
    }
    double simple_allocs = (double)counts.alloc_calls / (2.0 * EVENT_ROUNDS);
    double simple_frees = (double)counts.free_calls / (2.0 * EVENT_ROUNDS);
    counts.alloc_calls = 0;
    counts.free_calls = 0;
    for (unsigned round = 0; round < EVENT_ROUNDS; round += 1u) {
        run_event(system, timed_actor, rearm);
    }
    double timed_allocs = (double)counts.alloc_calls / EVENT_ROUNDS;
    double timed_frees = (double)counts.free_calls / EVENT_ROUNDS;
    printf("allocator calls per event after warm-up: %.2f allocations and %.2f frees for a simple actor, "
           "%.2f and %.2f for an actor with a pending delayed send\n",
           simple_allocs, simple_frees, timed_allocs, timed_frees);
    ta_system_destroy(system);
    require(counts.live_bytes == 0, "the event system returned every byte it was given");
    require(simple_allocs == 0.0 && simple_frees == 0.0, "an event to a simple actor calls the allocator (ADR 0064)");
    require(timed_allocs == 0.0 && timed_frees == 0.0,
            "an event to an actor with a pending delayed send calls the allocator (ADR 0064)");
}

/* ------------------------------------------------------------------------
 * Batched delivery (ADR 0069, ADR 0099). A delivered event waits in core's
 * pool, which holds no event block, and takes one from its session only
 * when its macrostep takes it, until that macrostep ends. So a host that
 * delivers a batch before it runs the actors holds one block at a time.
 * The cache fills on demand up to its bound (ADR 0093): with the default
 * bound of 1,024, or a bound of 4, a warm system makes no allocator call,
 * and with no cache every input takes a new block and frees it again.
 * Creation makes no block.
 */

#define DELIVERY_BATCH 256u
#define DELIVERY_ROUNDS 20u

static void deliver_batch(ta_system *system, const ta_actor_id *actors, ta_event_name_id name) {
    ta_event event = {.name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE};
    for (unsigned index = 0; index < DELIVERY_BATCH; index += 1u) {
        require(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actors[index], .event = &event }) == TA_OK, "delivering an event of the batch");
    }
    for (unsigned index = 0; index < DELIVERY_BATCH; index += 1u) {
        run_to_stable(system, actors[index]);
    }
}

/* Allocator calls per delivered event over DELIVERY_ROUNDS go/done rounds. */
static double batch_allocations(ta_system *system, const ta_actor_id *actors, counting_allocator *counts) {
    ta_event_name_id go = intern_event(system, (ta_str)STR("go"));
    ta_event_name_id done = intern_event(system, (ta_str)STR("done"));
    /* Warm-up: the first round fills the cache as far as it reaches. */
    deliver_batch(system, actors, go);
    deliver_batch(system, actors, done);
    counts->alloc_calls = 0;
    for (unsigned round = 0; round < DELIVERY_ROUNDS; round += 1u) {
        deliver_batch(system, actors, go);
        deliver_batch(system, actors, done);
    }
    return (double)counts->alloc_calls / (2.0 * DELIVERY_ROUNDS * DELIVERY_BATCH);
}

/* A system whose block cache keeps at most `cache` blocks (core ADR 0093),
 * with DELIVERY_BATCH actors of the batched chart; allocator calls per
 * delivered event. `created_bytes` is what creation allocated. */
static double batched_system(uint32_t cache, size_t *created_bytes) {
    counting_allocator counts = {.backing = tat_libc_allocator()};
    ta_allocator allocator = {.self = &counts, .v = &COUNTING_VTABLE};
    ta_system *system = NULL;
    ta_param_value params[] = {{.param = TA_PARAM_EVENT_BLOCK_CACHE, .value = cache}};
    require(ta_system_create(&(ta_system_config){.allocator = allocator, .params = params, .param_count = 1},
                             &system) == TA_OK,
            "system creation");
    *created_bytes = counts.live_bytes;
    ta_definition_id simple = register_revision(system, allocator, &(revision_spec){"batched", "v2", STATES_V2});
    ta_actor_id actors[DELIVERY_BATCH];
    for (unsigned index = 0; index < DELIVERY_BATCH; index += 1u) {
        require(ta_actor_spawn(system, &(ta_actor_spawn_params){.definition = simple, .out = &actors[index]}) == TA_OK,
                "spawning an actor of the batch");
        run_to_stable(system, actors[index]);
    }
    double calls = batch_allocations(system, actors, &counts);
    ta_system_destroy(system);
    require(counts.live_bytes == 0, "the batched system returned every byte it was given");
    return calls;
}

static void check_batched_delivery_allocations(void) {
    size_t default_bytes = 0;
    size_t small_bytes = 0;
    size_t empty_bytes = 0;
    double default_cache = batched_system(TA_PARAM_DEFAULT_EVENT_BLOCK_CACHE, &default_bytes);
    double small_cache = batched_system(4u, &small_bytes);
    double no_cache = batched_system(0, &empty_bytes);
    printf("allocator calls per event delivered in batches of %u: %.2f with the default block cache (%u), %.2f with "
           "a cache of 4, %.2f with none; creation allocated %zu bytes with either bound, %zu with none\n",
           DELIVERY_BATCH, default_cache, (unsigned)TA_PARAM_DEFAULT_EVENT_BLOCK_CACHE, small_cache, no_cache,
           default_bytes, empty_bytes);
    require(default_cache == 0.0, "a warm system with the default cache calls no allocator (ADR 0093)");
    require(small_cache == 0.0, "a batch waiting in the pool holds no event block, so a cache of 4 serves it (ADR 0099)");
    require(no_cache > 0.9, "with no cache every input taken takes a block (ADR 0069)");
    require(default_bytes == empty_bytes && small_bytes == empty_bytes,
            "creation makes no event block, whatever the bound (ADR 0093)");
}

/* ------------------------------------------------------------------------
 * Events that carry a MESSAGE I/O origin (ADR 0068). Core stores the URI in
 * a pooled record, shared by every copy of the event and returned to the
 * free list with the last: after warm-up a URI of up to 48 bytes costs no
 * allocator call, and a longer one one allocation and one free for its
 * spill. The sizes of the event types do not grow.
 */

static ta_op_status silent_send(void *self, const ta_send *send, ta_fault *fault) {
    (void)self;
    (void)send;
    (void)fault;
    return TA_OP_OK;
}

static const ta_io_vtable SILENT_IO = {.send = silent_send};

static void run_origin_event(ta_system *system, ta_actor_id actor, ta_event_name_id name, const ta_io_origin_make_params *origin) {
    ta_event event = {.name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE};
    ta_io_origin_make_params params = *origin;
    params.out = &event.origin;
    require(ta_io_origin_make(system, &params) == TA_OK, "making a message origin");
    require(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor, .event = &event }) == TA_OK, "delivering an event with an origin");
    ta_event_release(&event);
    run_to_stable(system, actor);
}

static void check_io_origin_allocations(void) {
    counting_allocator counts = {.backing = tat_libc_allocator()};
    ta_allocator allocator = {.self = &counts, .v = &COUNTING_VTABLE};
    ta_system *system = NULL;
    require(ta_system_create(&(ta_system_config){.allocator = allocator}, &system) == TA_OK, "system creation");
    ta_atom type = 0;
    uint32_t processor = 0;
    require(ta_system_intern_atom(system, (ta_str)STR("urn:scale:request"), &type) == TA_OK &&
                ta_system_register_io(system, &(ta_io_registration_params){.type = type,
                                                                           .io = {.v = &SILENT_IO},
                                                                           .index = &processor}) == TA_OK,
            "registering a processor");
    ta_definition_id simple = register_revision(system, allocator, &(revision_spec){"events", "v2", STATES_V2});
    ta_actor_id actor = 0;
    require(ta_actor_spawn(system, &(ta_actor_spawn_params){.definition = simple, .out = &actor}) == TA_OK,
            "spawning the origin actor");
    run_to_stable(system, actor);
    ta_event_name_id go = intern_event(system, (ta_str)STR("go"));
    ta_event_name_id done = intern_event(system, (ta_str)STR("done"));
    ta_io_origin_make_params short_uri = {.processor = processor,
                                          .origin_class = TA_IO_ORIGIN_MESSAGE,
                                          .uri = STR("http://client.example/requests/0123456789")};
    ta_io_origin_make_params long_uri = short_uri;
    long_uri.uri = (ta_str)STR("http://client.example/callbacks/requests/0123456789abcdef0123456789abcdef");
    /* Warm-up: the first origin takes a page of records. */
    run_origin_event(system, actor, go, &short_uri);
    run_origin_event(system, actor, done, &short_uri);

    counts.alloc_calls = 0;
    counts.free_calls = 0;
    for (unsigned round = 0; round < EVENT_ROUNDS; round += 1u) {
        run_origin_event(system, actor, go, &short_uri);
        run_origin_event(system, actor, done, &short_uri);
    }
    double short_allocs = (double)counts.alloc_calls / (2.0 * EVENT_ROUNDS);
    double short_frees = (double)counts.free_calls / (2.0 * EVENT_ROUNDS);
    counts.alloc_calls = 0;
    counts.free_calls = 0;
    for (unsigned round = 0; round < EVENT_ROUNDS; round += 1u) {
        run_origin_event(system, actor, go, &long_uri);
        run_origin_event(system, actor, done, &long_uri);
    }
    /* The actor keeps its last event as _event; the next one releases it. */
    run_origin_event(system, actor, go, &short_uri);
    double long_allocs = (double)counts.alloc_calls / (2.0 * EVENT_ROUNDS);
    double long_frees = (double)counts.free_calls / (2.0 * EVENT_ROUNDS);
    printf("sizes: ta_origin %zu, ta_event %zu, ta_queued_event %zu, ta_send %zu, ta_active_invoke %zu bytes\n",
           sizeof(ta_origin), sizeof(ta_event), sizeof(ta_queued_event), sizeof(ta_send), sizeof(ta_active_invoke));
    printf("allocator calls per event with a message origin after warm-up: %.2f allocations and %.2f frees for a "
           "%u-byte URI, %.2f and %.2f for a %u-byte URI\n",
           short_allocs, short_frees, short_uri.uri.len, long_allocs, long_frees, long_uri.uri.len);
    ta_system_destroy(system);
    require(counts.live_bytes == 0, "the origin system returned every byte it was given");
    require(short_allocs == 0.0 && short_frees == 0.0,
            "an event with a short message origin calls the allocator (ADR 0068)");
    require(long_allocs == 1.0 && long_frees == 1.0,
            "an event with a long message origin does more than spill its URI once (ADR 0068)");
    require(sizeof(ta_origin) == 24u && sizeof(ta_event) == 64u && sizeof(ta_queued_event) == 72u,
            "an event type grew (ADR 0068 keeps them at 24, 64 and 72 bytes)");
}

/* ------------------------------------------------------------------------
 * A router that keeps a delivery and releases it later (ADR 0065). The
 * delivery's named destination and origin are shared handles, so keeping
 * it retains three references and releasing it gives them back: once the
 * system is warm, neither calls the allocator. Before ADR 0065 keeping a
 * delivery copied its destination's strings, two allocations, and releasing
 * it freed them. The sender keeps a far delayed send, so its sparse state
 * stays (ADR 0064) and only the router's own work is counted.
 */

static const ta_scxml_state_spec STATES_PINGER[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("waiting"),
        .onentry =
            (const ta_scxml_handler_spec[]){
                {.actions =
                     (const ta_scxml_action_spec[]){
                         {.kind = TA_SCXML_ACTION_SEND,
                          .as.send = {.id = {.kind = TA_SCXML_ID_LITERAL, .as.literal = STR("timeout")},
                                      .event = {.kind = TA_SCXML_EVENT_NAME, .as.name = STR("timeout")},
                                      .delay = {.kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("60s")}}},
                         {0},
                     }},
                {0},
            },
        .transitions =
            (const ta_scxml_transition_spec[]){
                {.kind = TA_SCXML_TRANSITION_INTERNAL,
                 .event = STR("ping"),
                 .actions =
                     (const ta_scxml_action_spec[]){
                         {.kind = TA_SCXML_ACTION_SEND,
                          .as.send = {.event = {.kind = TA_SCXML_EVENT_NAME, .as.name = STR("pong")},
                                      .target = {.kind = TA_SCXML_STRING_LITERAL,
                                                 .as.literal = STR("#_actor_peer/b")}}},
                         {0},
                     }},
                {0},
            },
    },
    {0},
};

typedef struct {
    ta_external_delivery kept;
    bool holding;
    unsigned accepted;
} keeping_router;

static ta_external_event_submit_status keep_delivery(void *self, const ta_external_event_submit_params *params) {
    keeping_router *router = self;
    require(!router->holding, "the router holds one delivery at a time");
    ta_external_delivery_retain(params->delivery, &router->kept);
    router->holding = true;
    router->accepted += 1u;
    return TA_EXTERNAL_EVENT_ACCEPTED;
}

static const ta_external_event_router_vtable KEEPING_ROUTER = {.submit = keep_delivery};

static void check_kept_delivery_allocations(void) {
    counting_allocator counts = {.backing = tat_libc_allocator()};
    ta_allocator allocator = {.self = &counts, .v = &COUNTING_VTABLE};
    ta_system *system = NULL;
    require(ta_system_create(&(ta_system_config){.allocator = allocator}, &system) == TA_OK, "system creation");
    directory dir;
    directory_init(&dir, 16);
    keeping_router router = {0};
    require(ta_system_set_actor_directory(system, (ta_actor_directory){.self = &dir, .v = &DIRECTORY_VTABLE}) ==
                    TA_OK &&
                ta_system_set_external_event_router(
                    system, (ta_external_event_router){.self = &router, .v = &KEEPING_ROUTER}) == TA_OK,
            "installing the directory and the router");
    ta_definition_id pinger = register_revision(system, allocator, &(revision_spec){"pinger", "v1", STATES_PINGER});
    ta_definition_id peer = register_revision(system, allocator, &(revision_spec){"peer", "v1", STATES_V2});
    ta_actor_id sender = 0;
    ta_actor_id receiver = 0;
    require(ta_actor_spawn(system, &(ta_actor_spawn_params){
                                       .definition = pinger,
                                       .address = &(ta_actor_address){.family = {STR("pinger")}, .name = STR("a")},
                                       .out = &sender}) == TA_OK &&
                ta_actor_spawn(system, &(ta_actor_spawn_params){
                                           .definition = peer,
                                           .address = &(ta_actor_address){.family = {STR("peer")}, .name = STR("b")},
                                           .out = &receiver}) == TA_OK,
            "spawning the named sender and receiver");
    run_to_stable(system, sender);
    run_to_stable(system, receiver);
    ta_event_name_id ping = intern_event(system, (ta_str)STR("ping"));
    /* Warm-up. */
    run_event(system, sender, ping);
    require(router.holding, "the router kept the delivery");
    ta_external_delivery_release(&router.kept);
    router.holding = false;

    counts.alloc_calls = 0;
    counts.free_calls = 0;
    for (unsigned round = 0; round < EVENT_ROUNDS; round += 1u) {
        run_event(system, sender, ping);
        ta_external_delivery_release(&router.kept);
        router.holding = false;
    }
    double allocs = (double)counts.alloc_calls / EVENT_ROUNDS;
    double frees = (double)counts.free_calls / EVENT_ROUNDS;
    printf("allocator calls per kept-then-released delivery after warm-up: %.2f allocations and %.2f frees\n",
           allocs, frees);
    require(router.accepted == EVENT_ROUNDS + 1u, "every ping reached the router");
    directory_release(&dir);
    ta_system_destroy(system);
    directory_destroy(&dir);
    require(counts.live_bytes == 0, "the router system returned every byte it was given");
    require(allocs == 0.0 && frees == 0.0, "keeping and releasing a delivery calls the allocator (ADR 0065)");
}

int main(int argc, char **argv) {
    print_machine();
    if (argc == 3 && strcmp(argv[1], "--report") == 0) {
        size_t population = (size_t)population_arg("--report", argv[2]);
        report_header();
        report("report", population, measure(population));
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: scale_test [--report POPULATION]\n");
        return 2;
    }

    const char *cap_text = getenv("TAT_SCALE_MAX_POPULATION");
    if (cap_text != NULL && cap_text[0] != 0) {
        (void)population_arg("TAT_SCALE_MAX_POPULATION", cap_text);
    }
    (void)measure(WARMUP_POPULATION);

    size_t count = 0;
    round_cost costs[POPULATION_COUNT];
    while (count < POPULATION_COUNT && should_run(count)) {
        costs[count] = measure_best(POPULATIONS[count], REPETITIONS);
        count += 1;
    }
    report_header();
    for (size_t index = 0; index < count; index += 1) {
        report("measured", POPULATIONS[index], costs[index]);
    }
    for (size_t index = 1; index < count; index += 1) {
        if (flat_pair(POPULATIONS[index - 1], &costs[index - 1], POPULATIONS[index], &costs[index])) {
            continue;
        }
        /* One stolen time slice must not fail CI: measure both sizes once
         * more, keep the cheaper of each cost, and only then decide. */
        printf("measuring %u and %u actors again: %s\n", POPULATIONS[index - 1], POPULATIONS[index], ratio_failure);
        keep_cheapest(&costs[index - 1], measure_best(POPULATIONS[index - 1], REPETITIONS));
        keep_cheapest(&costs[index], measure_best(POPULATIONS[index], REPETITIONS));
        report("again", POPULATIONS[index - 1], costs[index - 1]);
        report("again", POPULATIONS[index], costs[index]);
        if (!flat_pair(POPULATIONS[index - 1], &costs[index - 1], POPULATIONS[index], &costs[index])) {
            fprintf(stderr, "scale test failed: %s\n", ratio_failure);
            exit(1);
        }
    }
    check_event_allocations();
    check_batched_delivery_allocations();
    check_kept_delivery_allocations();
    check_io_origin_allocations();
    /* And every population stays inside the budget the target implies. */
    for (size_t index = 0; index < count; index += 1) {
        require(costs[index].actor_bytes <= (double)ACTOR_BYTE_BUDGET, "a resident actor costs more than its byte budget");
    }

    printf("scale test passed: per-actor cost is flat from %u to %u actors, %.0f bytes each\n", POPULATIONS[0],
           POPULATIONS[count - 1], costs[count - 1].actor_bytes);
    if (count < POPULATION_COUNT) {
        printf("ran %zu of %zu populations; stopped before %u: %s\n", count, (size_t)POPULATION_COUNT,
               POPULATIONS[count], stop_reason);
    }
    return 0;
}
