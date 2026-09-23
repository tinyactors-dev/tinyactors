/*
 * Invariant 17, population-independent actor cost: creating, replacing, or
 * destroying one actor costs the same whether the system holds a hundred
 * actors or four million (tinyactors-core/docs/internal/invariants.md, and
 * "Per-actor operations cost the same at every population" in
 * docs/internal/memory-model.md). The same three measurements are the
 * benchmark behind the four-million-actor target of that page.
 *
 * The claim is about a population, so a fixed-size test cannot state it: it
 * is measured at two populations that differ by a factor of four, and the
 * per-actor cost must stay flat between them. A table walk inside a spawn
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
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "allocator.h"

#define STR(text) {.ptr = (text), .len = (uint32_t)(sizeof(text) - 1u)}

/*
 * The populations the ratio is taken over. Four million actors is the
 * target, not the test: SMALL and LARGE are the smallest pair that keeps
 * each measurement well clear of the clock's resolution while the whole
 * test stays under a second of a passing run.
 */
#define SMALL_POPULATION 50000u
#define LARGE_POPULATION 200000u
#define CHURN_ACTORS 20000u
#define INSPECTIONS 200000u
#define INSPECTION_SAMPLE 256u
#define REPETITIONS 3u

/*
 * A four-fold population may cost no more than twice as much per actor.
 * Flat costs measure at about 1.05; one table walk per spawn measures at
 * about 4.
 */
#define COST_RATIO_BOUND 2.0

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
} counting_allocator;

static void *counting_alloc(void *self, size_t size, size_t alignment) {
    counting_allocator *counts = self;
    void *block = counts->backing.v->alloc(counts->backing.self, size, alignment);
    if (block != NULL) {
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
    counts->live_bytes -= params->size;
    counts->live_blocks -= 1;
    counts->backing.v->free(counts->backing.self, params);
}

static const ta_allocator_vtable COUNTING_VTABLE = {.alloc = counting_alloc, .free = counting_free};

/* ------------------------------------------------------------------------
 * A directory, which named actors need and the host owns (ADR 0037).
 *
 * Open addressing over the name text, sized so it never rehashes during a
 * run: this is the population's index, and a host cost the measurements
 * must not mistake for core's.
 */

typedef struct {
    char *name;
    uint32_t length;
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

static void directory_destroy(directory *dir) {
    for (size_t index = 0; index <= dir->mask; index += 1) {
        free(dir->slots[index].name);
    }
    free(dir->slots);
    dir->slots = NULL;
}

static directory_slot *directory_find(directory *dir, const ta_actor_address *address) {
    size_t hash = 1469598103934665603u;
    for (uint32_t index = 0; index < address->name.len; index += 1) {
        hash ^= (unsigned char)address->name.ptr[index];
        hash *= 1099511628211u;
    }
    for (size_t at = hash & dir->mask;; at = (at + 1u) & dir->mask) {
        directory_slot *slot = &dir->slots[at];
        if (!slot->used || (slot->length == address->name.len &&
                            memcmp(slot->name, address->name.ptr, slot->length) == 0)) {
            return slot;
        }
    }
}

static ta_result directory_claim(void *self, const ta_actor_directory_claim_params *params) {
    directory_slot *slot = directory_find(self, params->address);
    if (slot->used) {
        return TA_ERR_STALE;
    }
    slot->name = malloc(params->address->name.len);
    require(slot->name != NULL, "directory name allocation");
    memcpy(slot->name, params->address->name.ptr, params->address->name.len);
    slot->length = params->address->name.len;
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
    /* The name stays claimed: probing must not break, and no name is reused. */
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

/* One delayed send far in the future, armed on entry: every timed actor holds one. */
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
    double timer_us;   /* per actor: spawn a timed ephemeral (arms a send), destroy it (cancels it) */
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

static double per_actor_us(double seconds, size_t actors) {
    return seconds * 1e6 / (double)actors;
}

static round_cost measure(size_t population) {
    counting_allocator counts = {.backing = tat_libc_allocator()};
    ta_allocator allocator = {.self = &counts, .v = &COUNTING_VTABLE};
    ta_system *system = NULL;
    require(ta_system_create(&(ta_system_config){.allocator = allocator}, &system) == TA_OK, "system creation");

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
    double started = cpu_seconds();
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
        ta_run_status status = TA_RUN_ERROR;
        require(ta_actor_resume(system, &(ta_actor_resume_params){
                                            .actor = actors[index],
                                            .budget = 100,
                                            .status = &status,
                                        }) == TA_OK &&
                    status == TA_RUN_STABLE,
                "bootstrapping a named actor");
    }
    double spawned = cpu_seconds();
    double actor_bytes = (double)(counts.live_bytes - empty_bytes) / (double)population;

    /* Upgrading every one of them to v2 under its own address (ADR 0043). */
    for (size_t index = 0; index < population; index += 1) {
        ta_actor_id replacement = 0;
        require(ta_actor_prepare_replacement(system, &(ta_actor_prepare_replacement_params){
                                                         .incumbent = actors[index],
                                                         .definition = v2,
                                                         .out = &replacement,
                                                     }) == TA_OK,
                "preparing a replacement");
        ta_run_status status = TA_RUN_ERROR;
        require(ta_actor_resume(system, &(ta_actor_resume_params){
                                            .actor = replacement,
                                            .budget = 100,
                                            .status = &status,
                                        }) == TA_OK &&
                    status == TA_RUN_STABLE,
                "bootstrapping a replacement");
        require(ta_actor_activate_replacement(system, &(ta_actor_activate_replacement_params){
                                                          .incumbent = actors[index],
                                                          .replacement = replacement,
                                                      }) == TA_OK,
                "activating a replacement");
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
        ta_run_status status = TA_RUN_ERROR;
        require(ta_actor_resume(system, &(ta_actor_resume_params){
                                            .actor = ephemeral,
                                            .budget = 100,
                                            .status = &status,
                                        }) == TA_OK &&
                    status == TA_RUN_STABLE,
                "bootstrapping an ephemeral actor");
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
        ta_run_status status = TA_RUN_ERROR;
        require(ta_actor_resume(system, &(ta_actor_resume_params){
                                            .actor = timed_actors[index],
                                            .budget = 100,
                                            .status = &status,
                                        }) == TA_OK &&
                    status == TA_RUN_STABLE,
                "bootstrapping a timed actor");
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
        ta_run_status status = TA_RUN_ERROR;
        require(ta_actor_resume(system, &(ta_actor_resume_params){
                                            .actor = ephemeral,
                                            .budget = 100,
                                            .status = &status,
                                        }) == TA_OK &&
                    status == TA_RUN_STABLE,
                "bootstrapping a timed ephemeral actor");
        require(ta_actor_destroy(system, ephemeral) == TA_OK, "destroying a timed ephemeral actor");
    }
    double timers_churned = cpu_seconds();
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
        .actor_bytes = actor_bytes,
        .peak_bytes = peak_bytes,
    };
}

/* The cheapest of several rounds: the one least interrupted by the machine. */
static round_cost measure_best(size_t population, unsigned repetitions) {
    round_cost best = measure(population);
    for (unsigned round = 1; round < repetitions; round += 1) {
        round_cost next = measure(population);
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
    }
    return best;
}

static void report(const char *label, size_t population, round_cost cost) {
    printf("%-10s %9zu   %8.2f %8.2f %8.2f %8.3f %8.2f   %7.1f %10.1f\n", label, population, cost.spawn_us,
           cost.upgrade_us, cost.churn_us, cost.inspect_us, cost.timer_us, cost.actor_bytes,
           (double)cost.peak_bytes / (1024.0 * 1024.0));
    /* A failure below writes to stderr; keep the table ahead of it. */
    fflush(stdout);
}

static void report_header(void) {
    printf("%-10s %9s   %8s %8s %8s %8s %8s   %7s %10s\n", "", "actors", "spawn", "upgrade", "churn", "inspect",
           "timer", "bytes", "peak MB");
    printf("%-10s %9s   %8s %8s %8s %8s %8s   %7s %10s\n", "", "", "us", "us", "us", "us", "us", "/actor", "");
}

/* Names the two costs whose ratio failed, so the report says which walk. */
static void require_flat(const char *what, double small, double large) {
    double ratio = small > 0.0 ? large / small : 0.0;
    if (ratio <= COST_RATIO_BOUND) {
        return;
    }
    fprintf(stderr,
            "scale test failed: %s costs %.2f us per actor against %u residents and %.2f us against %u, a ratio of "
            "%.2f against a bound of %.2f: the cost of one actor grows with the population (invariant 17)\n",
            what, small, SMALL_POPULATION, large, LARGE_POPULATION, ratio, COST_RATIO_BOUND);
    exit(1);
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--report") == 0) {
        size_t population = (size_t)strtoull(argv[2], NULL, 10);
        require(population != 0, "--report takes a population");
        report_header();
        report("report", population, measure(population));
        return 0;
    }
    if (argc != 1) {
        fprintf(stderr, "usage: scale_test [--report POPULATION]\n");
        return 2;
    }

    round_cost small = measure_best(SMALL_POPULATION, REPETITIONS);
    round_cost large = measure_best(LARGE_POPULATION, REPETITIONS);
    report_header();
    report("small", SMALL_POPULATION, small);
    report("large", LARGE_POPULATION, large);

    require_flat("spawning an actor", small.spawn_us, large.spawn_us);
    require_flat("upgrading an actor", small.upgrade_us, large.upgrade_us);
    require_flat("an ephemeral actor", small.churn_us, large.churn_us);
    require_flat("inspecting a timed actor", small.inspect_us, large.inspect_us);
    require_flat("arming and cancelling a delayed send", small.timer_us, large.timer_us);

    /*
     * Per-actor memory is a population claim too: it may not grow with the
     * population, and it must stay inside the budget the target implies.
     */
    require(large.actor_bytes <= small.actor_bytes + 1.0, "per-actor memory grows with the population");
    require(large.actor_bytes <= (double)ACTOR_BYTE_BUDGET, "a resident actor costs more than its byte budget");

    printf("scale test passed: per-actor cost is flat from %u to %u actors, %.0f bytes each\n", SMALL_POPULATION,
           LARGE_POPULATION, large.actor_bytes);
    return 0;
}
