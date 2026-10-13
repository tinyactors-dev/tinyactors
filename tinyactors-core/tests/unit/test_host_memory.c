/* The host's memory (docs/adr/0005-freestanding-host.md): every block through
 * the ta_allocator of its config, and a refusal is a defined outcome.
 *
 *   accounting   ta_host_free gives back every block it was given, each with
 *                the size it was allocated with.
 *   arena        a host over a fixed buffer, no malloc behind it: two
 *                segments, a send between them, a relocation, a migration.
 *   refusals     a workload that reaches every allocating path of the host
 *                (segments, relocation, routing between segments, the
 *                router's queue, migration, hibernation, export and adoption,
 *                delivery from a peer, expiry, restore) runs with an
 *                allocator that refuses its n-th allocation once. Each
 *                refusal comes back as TA_ERROR_NO_MEMORY or TA_RUN_EXHAUSTED,
 *                the step is retried, and the run ends with the same effects
 *                as one that was never refused: nothing lost or done twice.
 *                The fast pass refuses the first allocation of segment
 *                creation, of the router's queue growing and of relocation;
 *                --full refuses every allocation of the run, one per run. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include "../support/host_request.h"
#include "../support/host_config.h"
#include "../support/host_output.h"

#include <stdlib.h>
#include <string.h>

#define BASE "tinyactors://h/"
#define SC(name, datamodel, body)                                                                  \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='" datamodel "' name='" name "' "    \
    body "</scxml>"

/* ---- an allocator that counts, checks and refuses ------------------------------------------ */

#define BLOCK_MAGIC 0x7461626C6F636B21ull
#define MAX_PHASES  64

typedef struct block_header {
    uint64_t size, magic;
} block_header;

typedef struct test_memory {
    uint64_t count;    /* allocations asked for so far */
    uint64_t fail_at;  /* refuse the allocation with this number (1-based); 0: none */
    uint64_t failures; /* refusals so far */
    uint64_t live_blocks, live_bytes, peak_bytes;
    uint32_t bad_frees; /* unknown pointer, or not the size it was allocated with */
    uint32_t bad_aligns;
    const char *phase; /* what the workload is doing */
    /* the first allocation of each phase (in a run that refuses nothing) */
    const char *phase_names[MAX_PHASES];
    uint64_t phase_first[MAX_PHASES];
    uint32_t phase_count;
} test_memory;

static void *memory_alloc(void *context, size_t size, size_t align) {
    test_memory *m = (test_memory *)context;
    m->count++;
    if (align == 0 || align > 8 || (align & (align - 1))) m->bad_aligns++;
    if (m->phase) {
        uint32_t i = 0;
        while (i < m->phase_count && strcmp(m->phase_names[i], m->phase) != 0)
            i++;
        if (i == m->phase_count && i < MAX_PHASES) {
            m->phase_names[i] = m->phase;
            m->phase_first[i] = m->count;
            m->phase_count++;
        }
    }
    if (m->count == m->fail_at) {
        m->failures++;
        return NULL;
    }
    block_header *header = (block_header *)malloc(sizeof *header + size);
    if (!header) return NULL;
    header->size = size;
    header->magic = BLOCK_MAGIC;
    m->live_blocks++;
    m->live_bytes += size;
    if (m->live_bytes > m->peak_bytes) m->peak_bytes = m->live_bytes;
    return header + 1;
}

static void memory_free(void *context, void *pointer, size_t size) {
    test_memory *m = (test_memory *)context;
    if (!pointer) return;
    block_header *header = (block_header *)pointer - 1;
    if (header->magic != BLOCK_MAGIC || header->size != size) {
        m->bad_frees++;
        return;
    }
    header->magic = 0;
    m->live_blocks--;
    m->live_bytes -= size;
    free(header);
}

static ta_allocator memory_allocator(test_memory *m) {
    ta_allocator allocator = {m, memory_alloc, memory_free};
    return allocator;
}

static uint64_t phase_first(const test_memory *m, const char *phase) {
    for (uint32_t i = 0; i < m->phase_count; i++)
        if (strcmp(m->phase_names[i], phase) == 0) return m->phase_first[i];
    return 0;
}

/* Run `expr` (a ta_status) until it is not refused. A refusal must be
 * TA_ERROR_NO_MEMORY, and TA_ERROR_NO_MEMORY must be a refusal; whatever
 * else it returns must be `expected`. */
#define RETRY(m, expected, expr)                                                                   \
    do {                                                                                           \
        for (int ta__try = 0;; ta__try++) {                                                        \
            uint64_t ta__before = (m)->failures;                                                   \
            ta_status ta__status = (expr);                                                         \
            int ta__refused = (m)->failures != ta__before;                                         \
            CHECK_EQUAL(ta__refused, ta__status == TA_ERROR_NO_MEMORY);                            \
            if (ta__refused && ta__status == TA_ERROR_NO_MEMORY && ta__try < 4) continue;          \
            CHECK_EQUAL(ta__status, (expected));                                                   \
            break;                                                                                 \
        }                                                                                          \
    } while (0)

/* ---- the workload --------------------------------------------------------------------------- */

#define W1                                                                                         \
    SC("w", "ecmascript",                                                                          \
       "initial='a'><datamodel><data id='n' expr='0'/></datamodel>"                                \
       "<state id='a'><transition event='ask'><send targetexpr='_event.data.to' event='ping'/>"    \
       "</transition><transition event='pong'><assign location='n' expr='n + 1'/>"                 \
       "<log expr=\"'pong:' + n\"/></transition>"                                                  \
       "<transition event='error.communication'><log expr=\"'comm'\"/></transition></state>")
#define ECHO                                                                                       \
    SC("echo", "ecmascript",                                                                       \
       "initial='e'><state id='e'><transition event='ping'>"                                       \
       "<send targetexpr='_event.origin' event='pong'/></transition>"                              \
       "<transition event='poke'><log expr=\"'poked'\"/></transition></state>")
#define W2                                                                                         \
    SC("w", "ecmascript",                                                                          \
       "initial='a'><datamodel><data id='total' expr='0'/></datamodel>"                            \
       "<state id='a'><transition event='error.platform.upgrade'>"                                 \
       "<assign location='total' expr='_event.data.n'/>"                                           \
       "<log expr=\"'upgraded:' + total\"/></transition>"                                          \
       "<transition event='pong'><log expr=\"'v2 pong'\"/></transition>"                           \
       "<transition event='error.communication'><log expr=\"'v2 comm'\"/></transition></state>")

typedef struct images {
    void *bytes[3];
    ta_image image[3];
} images;

static void images_open(images *im, const char *const texts[3]) {
    for (int i = 0; i < 3; i++) {
        size_t length;
        im->bytes[i] = compile_chart("c.scxml", texts[i], NULL, &length, 0);
        CHECK(im->bytes[i] != NULL);
        CHECK_EQUAL(ta_image_open((ta_bytes){im->bytes[i], length}, &im->image[i]), TA_OK);
    }
}

static void images_close(images *im) {
    for (int i = 0; i < 3; i++)
        free(im->bytes[i]);
}

/* What a run did: its effects, as text, order-free (a refused run goes on in
 * the next, which may visit segments in another order). Message ids are left
 * out: a refused outbound send gets a new one when it is sent again. */
typedef struct transcript {
    char **lines;
    uint32_t count, capacity;
} transcript;

static void transcript_add(transcript *t, const ta_host_effect *effect) {
    char line[512];
    snprintf(line, sizeof line, "%u|%llu|%llu|%u|%.*s|%.*s|%.*s|%.*s|%u/%u/%u", effect->kind,
             (unsigned long long)effect->session, (unsigned long long)effect->root, effect->flags,
             (int)effect->target.length, effect->target.pointer, (int)effect->event.length,
             effect->event.pointer, (int)effect->sendid.length, effect->sendid.pointer,
             (int)effect->auxiliary.length, effect->auxiliary.pointer, effect->migrated,
             effect->failed, effect->pending);
    if (t->count == t->capacity) {
        t->capacity = t->capacity ? t->capacity * 2 : 64;
        t->lines = (char **)realloc(t->lines, sizeof *t->lines * t->capacity);
    }
    size_t length = strlen(line) + 1; /* (strdup is POSIX, not C11) */
    t->lines[t->count] = (char *)malloc(length);
    if (!t->lines[t->count]) abort();
    memcpy(t->lines[t->count++], line, length);
}

static int line_order(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void transcript_free(transcript *t) {
    for (uint32_t i = 0; i < t->count; i++)
        free(t->lines[i]);
    free(t->lines);
    memset(t, 0, sizeof *t);
}

static int transcript_equal(transcript *a, transcript *b) {
    qsort(a->lines, a->count, sizeof *a->lines, line_order);
    qsort(b->lines, b->count, sizeof *b->lines, line_order);
    if (a->count != b->count) {
        fprintf(stderr, "  %u effects, expected %u\n", a->count, b->count);
        return 0;
    }
    for (uint32_t i = 0; i < a->count; i++)
        if (strcmp(a->lines[i], b->lines[i]) != 0) {
            fprintf(stderr, "  effect %s, expected %s\n", a->lines[i], b->lines[i]);
            return 0;
        }
    return 1;
}

typedef struct workload {
    test_memory *memory;
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    ta_host *host;
    const images *images;
    transcript effects;
} workload;

static void collect(workload *w, ta_host *host) {
    const ta_host_effect *effects;
    uint32_t n = ta_host_effects(host, &effects);
    for (uint32_t i = 0; i < n; i++)
        transcript_add(&w->effects, &effects[i]);
    ta_host_effects_ack(host);
}

/* Run until idle; a refusal must be TA_RUN_EXHAUSTED, and TA_RUN_EXHAUSTED a refusal. */
static void run_idle(workload *w, ta_host *host) {
    test_memory *m = w->memory;
    for (int round = 0; round < 1000; round++) {
        uint64_t before = m->failures;
        ta_run result = ta_host_run(host, 100000);
        collect(w, host);
        CHECK(result.status != TA_RUN_FAULT);
        CHECK_EQUAL(m->failures != before, result.status == TA_RUN_EXHAUSTED);
        if (result.status == TA_RUN_IDLE) return;
    }
    CHECK(0 && "the host did not become idle");
}

static ta_payload json(workload *w, const char *text) {
    return ta_datamodel_ecmascript_payload_eval(
        w->ecmascript, 0, (ta_string_view){text, (uint32_t)strlen(text)});
}

static ta_status send_event(workload *w, uint64_t session, const char *name, ta_payload data) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    event.data = data;
    event.session = session;
    return ta_host_send(w->host, &event);
}

static ta_status spawn(workload *w, const char *chart, uint64_t *session) {
    ta_host_chart_ref ref;
    ta_status status =
        ta_host_chart(w->host, (ta_string_view){chart, (uint32_t)strlen(chart)}, &ref);
    if (status != TA_OK) return status;
    return ta_host_spawn(
        w->host,
        &(ta_spawn){ref.segment, ref.chart, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}},
        session);
}

static test_host_config workload_config(workload *w) {
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.system.max_actors = 2; /* relocation from the start */
    config.system.max_events = 8;
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.migrate_quantum = 2;
    config.allocator = memory_allocator(w->memory);
    return config;
}

static void add_image(workload *w, int i) {
    w->memory->phase = "segment";
    uint32_t segment = 0;
    RETRY(w->memory, TA_OK, ta_host_add_image(w->host, ta_image_bytes(&w->images->image[i]), &segment));
    CHECK_EQUAL(segment, (uint32_t)i);
}

/* The whole workload; its effects in w->effects. */
static void workload_run(workload *w) {
    test_memory *m = w->memory;
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    test_host_config config = workload_config(w);
    m->phase = "new";
    RETRY(m, TA_OK, test_host_new(&config, &w->host));
    if (!w->host) return;
    add_image(w, 0); /* w, version 1 */
    add_image(w, 1); /* echo */

    /* spawning past the configured capacity: relocation */
    m->phase = "relocation";
    uint64_t workers[4], echoes[3];
    for (int i = 0; i < 4; i++)
        RETRY(m, TA_OK, spawn(w, "w", &workers[i]));
    for (int i = 0; i < 3; i++)
        RETRY(m, TA_OK, spawn(w, "echo", &echoes[i]));
    /* more events than the pool holds: the run relocates */
    for (int k = 0; k < 12; k++)
        RETRY(m, TA_OK, send_event(w, echoes[k % 3], "poke", 0));
    run_idle(w, w->host);

    /* routing between segments, there and back */
    m->phase = "routing";
    for (int i = 0; i < 4; i++) {
        char to[96];
        snprintf(to, sizeof to, "{\"to\": \"" BASE "#_scxml_%llu\"}",
                 (unsigned long long)echoes[i % 3]);
        ta_payload data = json(w, to);
        RETRY(m, TA_OK, send_event(w, workers[i], "ask", data));
    }
    /* nobody has that session: error.communication, held while the sender's
     * segment is drained */
    ta_payload nobody = json(w, "{\"to\": \"" BASE "#_scxml_999\"}");
    RETRY(m, TA_OK, send_event(w, workers[0], "ask", nobody));
    run_idle(w, w->host);

    /* the router: a peer that is down queues (the ring grows past 16), then flushes */
    m->phase = "queue";
    ta_peer_config peer_config;
    memset(&peer_config, 0, sizeof peer_config);
    peer_config.size = sizeof peer_config;
    peer_config.flags = TA_PEER_QUEUE | TA_PEER_TRUSTED;
    peer_config.id = (ta_string_view){"far", 3};
    uint32_t far = 0;
    RETRY(m, TA_OK, ta_host_peer_add(w->host, &peer_config, &far));
    RETRY(m, TA_OK, ta_host_peer_up(w->host, far, 0));
    for (int k = 0; k < 20; k++) {
        ta_payload data = json(w, "{\"to\": \"tinyactors://far/@x\"}");
        RETRY(m, TA_OK, send_event(w, workers[k % 4], "ask", data));
    }
    run_idle(w, w->host);
    m->phase = "flush";
    RETRY(m, TA_OK, ta_host_peer_up(w->host, far, 1));
    collect(w, w->host);

    /* expiry: a peer with a TTL, whose envelope fails back to its sender */
    m->phase = "expiry";
    peer_config.id = (ta_string_view){"slow", 4};
    peer_config.queue_ttl = 10;
    uint32_t slow = 0;
    RETRY(m, TA_OK, ta_host_peer_add(w->host, &peer_config, &slow));
    RETRY(m, TA_OK, ta_host_route(w->host, (ta_string_view){"slower", 6}, slow));
    RETRY(m, TA_OK, ta_host_peer_up(w->host, slow, 0));
    ta_payload data = json(w, "{\"to\": \"tinyactors://slower/@y\"}");
    RETRY(m, TA_OK, send_event(w, workers[0], "ask", data));
    run_idle(w, w->host);
    ta_host_advance_time(w->host, (ta_host_time){100, 0}); /* (what it cannot report, the next run does) */
    run_idle(w, w->host);

    /* delivery from a peer, deduplicated */
    m->phase = "deliver";
    char target[96];
    snprintf(target, sizeof target, BASE "#_scxml_%llu", (unsigned long long)echoes[0]);
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = (ta_string_view){target, (uint32_t)strlen(target)};
    envelope.name = (ta_string_view){"poke", 4};
    envelope.origin = (ta_string_view){"tinyactors://far/#_scxml_9", 26};
    envelope.msgid = (ta_string_view){"far-1", 5};
    envelope.datamodel = TA_NIL;
    RETRY(m, TA_ROUTE_DELIVERED, ta_host_deliver(w->host, far, &envelope));
    RETRY(m, TA_ROUTE_DUPLICATE, ta_host_deliver(w->host, far, &envelope));
    /* to a name nobody has, longer than the decoding buffer on the stack: an
     * error back to the peer */
    char name[320] = BASE "@";
    size_t prefix = strlen(name);
    memset(name + prefix, 'n', 300);
    envelope.target = (ta_string_view){name, (uint32_t)(prefix + 300)};
    envelope.msgid = (ta_string_view){"far-2", 5};
    RETRY(m, TA_ROUTE_REFUSED, ta_host_deliver(w->host, far, &envelope));
    collect(w, w->host);
    run_idle(w, w->host);

    /* a new version, and every worker migrates to it */
    add_image(w, 2);
    m->phase = "migration";
    uint32_t job = 0;
    RETRY(
        m, TA_OK,
        ta_host_migrate(w->host, &(ta_host_migration){(ta_string_view){"w", 1}, 0, 0, 0, 0}, &job));
    run_idle(w, w->host);

    /* hibernation, export and adoption */
    m->phase = "hibernation";
    static unsigned char blob[1 << 16];
    size_t length = 0;
    ta_bytes written; /* the host's, until the next call into it */
    RETRY(m, TA_OK, ta_host_hibernate(w->host, workers[1], &written));
    length = output_copy(written, blob, sizeof blob);
    RETRY(m, TA_OK, ta_host_wake(w->host, workers[1], (ta_bytes){blob, length}));
    run_idle(w, w->host);
    m->phase = "adoption";
    RETRY(m, TA_OK, ta_host_export_actor(w->host, workers[2], &written));
    length = output_copy(written, blob, sizeof blob);
    uint64_t adopted = 0;
    RETRY(m, TA_OK, ta_host_adopt(w->host, &(ta_host_adoption){2, 0, {blob, length}}, &adopted));
    CHECK_EQUAL(adopted, workers[2]);
    run_idle(w, w->host);
    m->phase = "sweep";
    uint64_t cold[8];
    ta_host_sweep(w->host, 64, (ta_uint64_array){cold, 8});
    ta_host_sweep(w->host, 64, (ta_uint64_array){cold, 8});

    /* restore: a second host from a snapshot of this one */
    m->phase = "restore";
    size_t hl = 0, dl = 0;
    ta_buffer room = {NULL, 0, 0};
    CHECK_EQUAL(ta_host_snapshot(w->host, &room), TA_ERROR_NO_MEMORY);
    hl = room.length;
    void *hs = malloc(hl);
    room = (ta_buffer){hs, hl, 0};
    CHECK_EQUAL(ta_host_snapshot(w->host, &room), TA_OK);
    hl = room.length;
    room = (ta_buffer){NULL, 0, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(w->ecmascript, &room), TA_ERROR_NO_MEMORY);
    dl = room.length;
    void *ds = malloc(dl);
    room = (ta_buffer){ds, dl, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(w->ecmascript, &room), TA_OK);
    dl = room.length;
    workload copy = *w;
    copy.ecmascript = ta_datamodel_ecmascript_new(NULL);
    copy.binding = ta_datamodel_ecmascript_binding(copy.ecmascript);
    memset(&copy.effects, 0, sizeof copy.effects);
    test_host_config restored_config = workload_config(&copy);
    ta_bytes restored_images[3] = {ta_image_bytes(&w->images->image[0]),
                                   ta_image_bytes(&w->images->image[1]),
                                   ta_image_bytes(&w->images->image[2])};
    uint64_t live = m->live_blocks;
    ta_host *restored = NULL;
    { /* the wrong image for the last live segment: refused, and the host's copies of
         the images before it, and of the wrong one, go back to the allocator */
        uint64_t hashes[3] = {0, 0, 0};
        uint32_t count = ta_host_snapshot_segments((ta_bytes){hs, hl}, (ta_uint64_array){hashes, 3});
        uint32_t last = TA_NIL;
        for (uint32_t i = 0; i < count && i < 3; i++)
            if (hashes[i]) last = i;
        CHECK(last != TA_NIL);
        if (last != TA_NIL) {
            ta_bytes wrong[3] = {restored_images[0], restored_images[1], restored_images[2]};
            wrong[last] = restored_images[(last + 1) % 3];
            /* Allocation can be refused before image validation. The
             * existing retry checks that refusal, then still requires the
             * wrong-image error and the same leak-free cleanup below. */
            RETRY(m, TA_ERROR_INVALID,
                  test_host_restore(&restored_config,
                                    &(ta_host_snapshot_input){{0, 0}, {hs, hl}, wrong, 3, 0}, &restored));
            CHECK(restored == NULL);
            CHECK_EQUAL(m->live_blocks, live);
        }
    }
    RETRY(m, TA_OK,
          test_host_restore(&restored_config, &(ta_host_snapshot_input){{0, 0}, {hs, hl}, restored_images, 3, 0}, &restored));
    CHECK(restored != NULL && m->live_blocks > live);
    if (restored) {
        CHECK_EQUAL(ta_datamodel_ecmascript_restore(copy.ecmascript, (ta_bytes){ds, dl}), TA_OK);
        ta_host_stats a, b;
        a.size = b.size = sizeof a;
        RETRY(m, TA_OK, host_stats_fill(w->host, &a));
        RETRY(m, TA_OK, host_stats_fill(restored, &b));
        CHECK(a.actors == b.actors && a.segments == b.segments);
        run_idle(&copy, restored);
        ta_host_free(restored);
    }
    CHECK_EQUAL(m->live_blocks, live); /* a refused restore left nothing behind either */
    transcript_free(&copy.effects);
    ta_datamodel_ecmascript_free(copy.ecmascript);
    free(hs);
    free(ds);

    m->phase = "free";
    ta_host_free(w->host);
    w->host = NULL;
    ta_datamodel_ecmascript_free(w->ecmascript);
}

/* One run of the workload, refusing allocation `fail_at` (0: none); checks
 * its effects against `reference` (when given) and that every byte came back. */
static void workload_once(const images *im, uint64_t fail_at, test_memory *out,
                          transcript *reference, transcript *effects) {
    test_memory memory;
    memset(&memory, 0, sizeof memory);
    memory.fail_at = fail_at;
    workload w;
    memset(&w, 0, sizeof w);
    w.memory = &memory;
    w.images = im;
    workload_run(&w);
    CHECK_EQUAL(memory.live_blocks, 0);
    CHECK_EQUAL(memory.live_bytes, 0);
    CHECK_EQUAL(memory.bad_frees, 0);
    CHECK_EQUAL(memory.bad_aligns, 0);
    CHECK_EQUAL(memory.failures, fail_at ? 1 : 0);
    if (reference && !transcript_equal(&w.effects, reference))
        CHECK(0 && "the effects differ from a run that was never refused");
    if (out) *out = memory;
    if (effects)
        *effects = w.effects;
    else
        transcript_free(&w.effects);
}

static int full_mode;

static void test_refusals(void) {
    images im;
    const char *const texts[3] = {W1, ECHO, W2};
    images_open(&im, texts);
    test_memory reference_memory;
    transcript reference;
    workload_once(&im, 0, &reference_memory, NULL, &reference);
    uint64_t total = reference_memory.count;
    /* every path the workload takes allocates */
    const char *phases[] = {"new",    "segment", "relocation", "routing", "queue",
                            "flush",  "expiry",  "deliver",    "migration", "restore"};
    for (size_t i = 0; i < sizeof phases / sizeof phases[0]; i++)
        if (!phase_first(&reference_memory, phases[i])) {
            fprintf(stderr, "  phase %s allocates nothing\n", phases[i]);
            CHECK(0 && "a phase of the workload allocates nothing");
        }
    if (full_mode) {
        for (uint64_t n = 1; n <= total; n++)
            workload_once(&im, n, NULL, &reference, NULL);
        fprintf(stderr, "  refused each of %llu allocations, one per run\n",
                (unsigned long long)total);
    } else {
        /* segment creation, the router's queue growing, relocation */
        const char *chosen[] = {"segment", "queue", "relocation"};
        for (size_t i = 0; i < 3; i++)
            workload_once(&im, phase_first(&reference_memory, chosen[i]), NULL, &reference, NULL);
    }
    transcript_free(&reference);
    images_close(&im);
}

/* ---- accounting ------------------------------------------------------------------------------ */

static void test_accounting(void) {
    images im;
    const char *const texts[3] = {W1, ECHO, W2};
    images_open(&im, texts);
    test_memory memory;
    workload_once(&im, 0, &memory, NULL, NULL); /* checks the balance at the end */
    CHECK(memory.count > 50 && memory.peak_bytes > 0);
    /* a host that only came and went */
    test_memory m;
    memset(&m, 0, sizeof m);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.allocator = memory_allocator(&m);
    ta_host *host = NULL;
    CHECK_EQUAL(test_host_new(&config, &host), TA_OK);
    ta_host_free(host);
    CHECK(m.count == 1 && m.live_blocks == 0 && m.bad_frees == 0);
    /* an allocator is required */
    config.allocator.alloc = NULL;
    CHECK_EQUAL(test_host_new(&config, &host), TA_ERROR_INVALID);
    /* so is an environment that says how big it is */
    ta_host_environment environment;
    memset(&environment, 0, sizeof environment);
    environment.allocator = memory_allocator(&m);
    CHECK_EQUAL(ta_host_create((ta_bytes){"\xa1\x68systemIDah", 12}, &environment, &host),
                TA_ERROR_INVALID);
    CHECK(m.count == 1 && m.live_blocks == 0);
    images_close(&im);
}

/* ---- a fixed arena --------------------------------------------------------------------------- */

/* A bump allocator over a static buffer: what an embedder without malloc
 * would pass. A freed block is reused when it is the last one. */
typedef struct arena {
    unsigned char *base;
    size_t capacity, used;
    uint64_t live;
} arena;

static void *arena_alloc(void *context, size_t size, size_t align) {
    arena *a = (arena *)context;
    size_t at = (a->used + (align - 1)) & ~(align - 1);
    if (at > a->capacity || size > a->capacity - at) return NULL;
    a->used = at + size;
    a->live++;
    return a->base + at;
}

static void arena_free(void *context, void *pointer, size_t size) {
    arena *a = (arena *)context;
    a->live--;
    if ((unsigned char *)pointer + size == a->base + a->used)
        a->used = (size_t)((unsigned char *)pointer - a->base);
}

#define PINGER                                                                                     \
    SC("p", "null",                                                                                \
       "initial='a'><state id='a'><transition event='go'>"                                         \
       "<send target='" BASE "#_scxml_4294967297' event='ping'/></transition>"                     \
       "<transition event='pong' target='b'/></state><state id='b'/>")
#define PONGER                                                                                     \
    SC("q", "null",                                                                                \
       "initial='a'><state id='a'><transition event='ping' target='b'>"                            \
       "<send target='" BASE "#_scxml_1' event='pong'/></transition></state><state id='b'/>")
#define PINGER2                                                                                    \
    SC("p", "null", "initial='a'><state id='a'/><state id='b'><onentry><log label='moved'/>"       \
                    "</onentry></state>")

static void test_fixed_arena(void) {
    static unsigned char buffer[8u << 20];
    arena a = {buffer, sizeof buffer, 0, 0};
    images im;
    const char *const texts[3] = {PINGER, PONGER, PINGER2};
    images_open(&im, texts);
    ta_datamodel_binding binding = {ta_datamodel_null(), NULL};
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.system.max_actors = 1;
    config.datamodels = &binding;
    config.datamodel_count = 1;
    config.allocator = (ta_allocator){&a, arena_alloc, arena_free};
    ta_host *host = NULL;
    CHECK_EQUAL(test_host_new(&config, &host), TA_OK);
    CHECK_EQUAL(ta_host_add_image(host, ta_image_bytes(&im.image[0]), NULL), TA_OK);
    CHECK_EQUAL(ta_host_add_image(host, ta_image_bytes(&im.image[1]), NULL), TA_OK);
    uint64_t p = 0, q = 0, extra = 0;
    CHECK_EQUAL(ta_host_spawn(
                    host, &(ta_spawn){0, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}}, &p),
                TA_OK);
    CHECK_EQUAL(ta_host_spawn(
                    host, &(ta_spawn){1, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}}, &q),
                TA_OK);
    CHECK(p == 1 && q == (1ull << 32) + 1);
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(ta_host_system(host, 0), &stats);
    CHECK_EQUAL(stats.actors_capacity, 1);
    CHECK_EQUAL(ta_host_spawn(host,
                              &(ta_spawn){0, 0, (ta_string_view){0, 0}, 0, (ta_string_view){0, 0}},
                              &extra),
                TA_OK);
    ta_system_stats_get(ta_host_system(host, 0), &stats);
    CHECK(stats.actors_capacity >= 2); /* relocated, inside the arena */
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){"go", 2};
    event.session = p;
    CHECK_EQUAL(ta_host_send(host, &event), TA_OK);
    for (int i = 0; i < 20 && ta_host_run(host, 100000).status != TA_RUN_IDLE; i++)
        ta_host_effects_ack(host);
    ta_host_stats host_stats;
    host_stats.size = sizeof host_stats;
    CHECK_EQUAL(host_stats_fill(host, &host_stats), TA_OK);
    CHECK_EQUAL(host_stats.routed, 2); /* ping and pong, between the segments */
    ta_actor_info info;
    info.size = sizeof info;
    uint32_t segment, actor;
    ta_host_location location;
    CHECK_EQUAL(ta_host_locate(host, p, &location), TA_OK);
    segment = location.segment;
    actor = location.actor;
    CHECK_EQUAL(ta_actor_info_get(ta_host_system(host, segment), actor, &info), TA_OK);
    /* a new version of p, and p moves to it in state b */
    CHECK_EQUAL(ta_host_add_image(host, ta_image_bytes(&im.image[2]), NULL), TA_OK);
    uint32_t job = 0;
    CHECK_EQUAL(
        ta_host_migrate(host, &(ta_host_migration){(ta_string_view){"p", 1}, 0, 0, 0, 0}, &job),
        TA_OK);
    int moved = 0;
    for (int i = 0; i < 20; i++) {
        ta_run result = ta_host_run(host, 100000);
        const ta_host_effect *effects;
        uint32_t n = ta_host_effects(host, &effects);
        for (uint32_t k = 0; k < n; k++)
            moved += effects[k].kind == TA_HOST_EFFECT_MIGRATED;
        ta_host_effects_ack(host);
        if (result.status == TA_RUN_IDLE) break;
    }
    CHECK_EQUAL(moved, 2); /* p and extra */

    CHECK_EQUAL(ta_host_locate(host, p, &location), TA_OK);
    segment = location.segment;
    CHECK_EQUAL(segment, 2);
    CHECK(a.used > 0 && a.used <= sizeof buffer);
    ta_host_free(host);
    CHECK_EQUAL(a.live, 0);
    /* an arena too small: refused, not a crash, and nothing is kept */
    static unsigned char small[4096];
    arena tiny = {small, sizeof small, 0, 0};
    config.allocator = (ta_allocator){&tiny, arena_alloc, arena_free};
    CHECK_EQUAL(test_host_new(&config, &host), TA_OK);
    CHECK_EQUAL(ta_host_add_image(host, ta_image_bytes(&im.image[0]), NULL), TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(ta_host_segment_count(host), 0);
    ta_host_free(host);
    CHECK_EQUAL(tiny.live, 0);
    images_close(&im);
}

/* ---- the borrow of published effects ------------------------------------------------------ */

/* An envelope from `peer` to a session nobody has: refused, and answered
 * with a TA_HOST_EFFECT_PEER_ERROR effect carrying origin, msgid and sendid. */
static void refused_delivery(ta_host *host, uint32_t peer, int i) {
    char msgid[96], sendid[64];
    snprintf(msgid, sizeof msgid, "message-%04d-padding-so-the-string-pool-grows-quickly", i);
    snprintf(sendid, sizeof sendid, "send-%04d", i);
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = (ta_string_view){"tinyactors://h/#_scxml_99", 25};
    envelope.name = (ta_string_view){"e", 1};
    envelope.origin = (ta_string_view){"tinyactors://far/#_scxml_7", 26};
    envelope.msgid = (ta_string_view){msgid, (uint32_t)strlen(msgid)};
    envelope.sendid = (ta_string_view){sendid, (uint32_t)strlen(sendid)};
    envelope.datamodel = TA_NIL;
    CHECK_EQUAL(ta_host_deliver(host, peer, &envelope), TA_ROUTE_REFUSED);
}

/* What ta_host_effects returns stays valid until ta_host_effects_ack or the
 * next ta_host_run (host.h), even when calls in between make so many effects
 * that the host's effect array and string pool have to grow. */
static void test_published_effects_stay_valid(void) {
    test_memory m;
    memset(&m, 0, sizeof m);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.allocator = memory_allocator(&m);
    ta_host *host = NULL;
    CHECK_EQUAL(test_host_new(&config, &host), TA_OK);
    ta_peer_config peer_config;
    memset(&peer_config, 0, sizeof peer_config);
    peer_config.size = sizeof peer_config;
    peer_config.flags = TA_PEER_TRUSTED;
    peer_config.id = (ta_string_view){"far", 3};
    uint32_t peer = 0;
    CHECK_EQUAL(ta_host_peer_add(host, &peer_config, &peer), TA_OK);
    for (int i = 0; i < 5; i++)
        refused_delivery(host, peer, i);
    const ta_host_effect *borrowed;
    uint32_t n = ta_host_effects(host, &borrowed);
    CHECK_EQUAL(n, 5);
    /* while they are borrowed: hundreds more, past the array's and the pool's capacity */
    for (int i = 5; i < 400; i++)
        refused_delivery(host, peer, i);
    for (uint32_t i = 0; i < n; i++) { /* (ASan: no read of freed memory) */
        char msgid[96], sendid[64];
        snprintf(msgid, sizeof msgid, "message-%04u-padding-so-the-string-pool-grows-quickly", i);
        snprintf(sendid, sizeof sendid, "send-%04u", i);
        CHECK_EQUAL(borrowed[i].kind, TA_HOST_EFFECT_PEER_ERROR);
        CHECK(borrowed[i].msgid.length == strlen(msgid) &&
              memcmp(borrowed[i].msgid.pointer, msgid, borrowed[i].msgid.length) == 0);
        CHECK(borrowed[i].sendid.length == strlen(sendid) &&
              memcmp(borrowed[i].sendid.pointer, sendid, borrowed[i].sendid.length) == 0);
    }
    ta_host_effects_ack(host); /* the five; the later ones stay for the next batch */
    const ta_host_effect *next;
    CHECK_EQUAL(ta_host_effects(host, &next), 395);
    CHECK(next[0].sendid.length == 9 && memcmp(next[0].sendid.pointer, "send-0005", 9) == 0);
    CHECK(next[394].sendid.length == 9 && memcmp(next[394].sendid.pointer, "send-0399", 9) == 0);
    ta_host_effects_ack(host);
    ta_host_free(host);
    CHECK(m.live_blocks == 0 && m.bad_frees == 0);
}

/* The string pool is reclaimed even when an effect is always left over at
 * the ack (one made after ta_host_effects): what the host holds stays
 * bounded by what is pending, not by everything ever produced. */
static void test_pool_is_reclaimed_with_effects_pending(void) {
    test_memory m;
    memset(&m, 0, sizeof m);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.allocator = memory_allocator(&m);
    ta_host *host = NULL;
    CHECK_EQUAL(test_host_new(&config, &host), TA_OK);
    ta_peer_config peer_config;
    memset(&peer_config, 0, sizeof peer_config);
    peer_config.size = sizeof peer_config;
    peer_config.flags = TA_PEER_TRUSTED;
    peer_config.id = (ta_string_view){"far", 3};
    uint32_t peer = 0;
    CHECK_EQUAL(ta_host_peer_add(host, &peer_config, &peer), TA_OK);
    refused_delivery(host, peer, 0);
    uint64_t settled = 0;
    for (int i = 1; i <= 4000; i++) {
        const ta_host_effect *effects;
        CHECK_EQUAL(ta_host_effects(host, &effects), 1);
        refused_delivery(host, peer, i); /* made while the batch is borrowed: kept */
        ta_host_effects_ack(host);
        if (i == 100) settled = m.live_bytes;
    }
    const ta_host_effect *last;
    CHECK_EQUAL(ta_host_effects(host, &last), 1); /* the one made last, intact */
    CHECK(last[0].sendid.length == 9 && memcmp(last[0].sendid.pointer, "send-4000", 9) == 0);
    char msgid[96];
    snprintf(msgid, sizeof msgid, "message-%04d-padding-so-the-string-pool-grows-quickly", 4000);
    CHECK(last[0].msgid.length == strlen(msgid) &&
          memcmp(last[0].msgid.pointer, msgid, last[0].msgid.length) == 0);
    if (m.live_bytes != settled)
        fprintf(stderr, "  held %llu bytes after 100 rounds, %llu after 4000\n",
                (unsigned long long)settled, (unsigned long long)m.live_bytes);
    CHECK_EQUAL(m.live_bytes, settled);
    ta_host_effects_ack(host);
    ta_host_free(host);
    CHECK(m.live_blocks == 0 && m.bad_frees == 0);
}

int main(int argc, char **argv) {
    full_mode = argc > 1 && strcmp(argv[1], "--full") == 0;
    RUN(test_accounting);
    RUN(test_fixed_arena);
    RUN(test_published_effects_stay_valid);
    RUN(test_pool_is_reclaimed_with_effects_pending);
    RUN(test_refusals);
    TEST_MAIN_END();
}
