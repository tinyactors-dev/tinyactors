/* access-reconciler — a k8s-style controller for employee access, simulated.
 *
 * One SCXML actor per person (charts/access.scxml) reconciles the HR record
 * with what six SaaS apps actually grant. This file is the HOST: everything
 * outside the actor system lives here, in a deterministic discrete-event
 * simulation on a simulated clock:
 *
 *   HR feed     joiners, movers, leavers (end dates, legal holds, rescinds), a reorg
 *   SaaS apps   latency, outages, error spikes, rate limits, lost responses,
 *               idempotency keys, a limited license pool, rogue admins (drift)
 *               whose watch events are sometimes lost
 *   approvals   managers approve / deny / ignore privileged access
 *   memory      idle controllers are hibernated to a blob file and woken on demand
 *
 * At the end it checks invariants: active people converged to policy, leavers
 * lost access within an hour of their end date, nobody got privileged access
 * without approval, drift was corrected within a resync period, finalizers ran.
 *
 *   access-reconciler [--employees N] [--days D] [--seed S] [--check] [--quiet]
 *
 * TRACE=1 prints every event the host delivers (and chart logs) to stderr.
 */
#define _POSIX_C_SOURCE 200809L /* strdup */
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyactors.h>
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/runtime.h>

#ifndef CHART_PATH
#define CHART_PATH "charts/access.scxml"
#endif

/* ---- time (simulated, milliseconds) ---------------------------------------------------- */
#define SECOND ((int64_t)1000)
#define MIN    (60 * SECOND)
#define HOUR   (60 * MIN)
#define DAY    (24 * HOUR)
static int64_t now;

/* ---- deterministic randomness ---------------------------------------------------------- */
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}
static double frand(void) {
    return (double)(rnd() >> 11) / 9007199254740992.0;
}
static int64_t urange(int64_t lo, int64_t hi) {
    return lo + (int64_t)(frand() * (double)(hi - lo));
}

/* ---- the domain ------------------------------------------------------------------------ */
enum { GITHUB, AWS, GOOGLE, SLACK, SALESFORCE, ZENDESK, N_APPS };
static const char *const APP[N_APPS] = {"github", "aws",        "google",
                                        "slack",  "salesforce", "zendesk"};
enum {
    L_NONE,
    L_MEMBER,
    L_ADMIN,
    L_READONLY,
    L_ACTIVE,
    L_SUSPENDED,
    L_LICENSED,
    L_AGENT,
    N_LEVELS
};
static const char *const LEVEL[N_LEVELS] = {"none",   "member",    "admin",    "readonly",
                                            "active", "suspended", "licensed", "agent"};
enum { ENG, SALES, SUPPORT, FINANCE, N_DEPTS };
static const char *const DEPT[N_DEPTS] = {"eng", "sales", "support", "finance"};
enum { ACTIVE, LEAVING, LEFT };
static const char *const STATUS[3] = {"active", "leaving", "left"};

typedef struct employee {
    uint32_t actor; /* controller handle; 0 once deleted */
    int status, department, lead, legal_hold, deleted;
    int64_t end_at;           /* LEAVING/LEFT: access must be gone from here (+1 h) */
    int64_t revoked_at;       /* when the apps first showed nothing left (offboarding) */
    uint8_t level[N_APPS];    /* ground truth inside the apps */
    uint8_t approved[N_APPS]; /* approvals registry */
    uint8_t asked[N_APPS];
    int64_t drift_at[N_APPS]; /* a rogue change waiting to be corrected */
    /* last status the controller reported */
    int ready, progressing, awaiting, degraded, blocked;
} employee;

static employee *E;
static uint32_t employee_count, cap_emp;
static uint32_t *by_slot; /* actor slot -> employee index + 1 */
static uint32_t by_slot_capacity;

typedef struct app_state {
    int64_t down_from, down_until;   /* outage window */
    int64_t error_from, error_until; /* error spike window */
    double error_rate;
    int rate_per_min, tokens; /* rate limit (0 = none) */
    int64_t minute;
    int license_capacity, licenses;
    uint64_t *keys; /* idempotency keys seen (open addressing) */
    uint32_t key_capacity, key_count;
    uint64_t calls, deduped, failed, lost;
} app_state;
static app_state A[N_APPS];

/* ---- metrics + invariant violations ---------------------------------------------------- */
static uint64_t m_hires, m_leavers, m_movers, m_rescinds, m_holds, m_drift, m_drift_lost,
    m_finalized, m_alerts, m_status, m_approvals_asked, m_wakes, m_hibernations;
static uint64_t v_unapproved, v_finalize_with_access, v_late_revoke, v_late_excused,
    v_not_converged, v_drift_uncorrected, v_faults;
static int64_t max_revoke, max_drift_fix;
static int quiet;

static void say(const char *format, ...) {
    if (quiet) return;
    va_list ap;
    va_start(ap, format);
    printf("[day %5.2f] ", (double)now / DAY);
    vprintf(format, ap);
    printf("\n");
    va_end(ap);
}

/* ---- the system ------------------------------------------------------------------------ */
enum { IO_SAAS, IO_APPROVALS, IO_HR, N_IO };
static ta_system *simulation_system;
static ta_datamodel_ecmascript *simulation_datamodel;
static ta_image simulation_image;

static employee *emp_of(uint32_t actor) {
    uint32_t slot = (actor & 0xFFFFFFu) - 1u;
    if (slot >= by_slot_capacity || !by_slot[slot]) return NULL;
    employee *e = &E[by_slot[slot] - 1];
    return e->actor == actor ? e : NULL;
}

static void index_actor(uint32_t actor, uint32_t index) {
    uint32_t slot = (actor & 0xFFFFFFu) - 1u;
    if (slot >= by_slot_capacity) {
        uint32_t nc = by_slot_capacity ? by_slot_capacity : 1024;
        while (nc <= slot)
            nc *= 2;
        by_slot = (uint32_t *)realloc(by_slot, sizeof(uint32_t) * nc);
        memset(by_slot + by_slot_capacity, 0, sizeof(uint32_t) * (nc - by_slot_capacity));
        by_slot_capacity = nc;
    }
    by_slot[slot] = index + 1;
}

static ta_payload json_payload(const char *json) {
    return ta_datamodel_ecmascript_payload_from_json(
        simulation_datamodel, (ta_string_view){json, (uint32_t)strlen(json)});
}

static void send_event(uint32_t actor, const char *name, const char *json) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = actor;
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    event.data = json ? json_payload(json) : 0;
    ta_status status = ta_system_send(simulation_system, &event);
    while (status == TA_ERROR_NO_MEMORY && ta_runtime_system_grow(&simulation_system) == TA_OK)
        status = ta_system_send(simulation_system,
                                &event); /* a failed send leaves the payload with us */
    if (status != TA_OK && event.data)
        ta_datamodel_ecmascript_vtable()->release(simulation_datamodel, event.data);
}

static void spec_json(const employee *e, char *buffer, size_t capacity) {
    snprintf(buffer, capacity,
             "{\"spec\":{\"id\":\"e%u\",\"status\":\"%s\",\"dept\":\"%s\",\"role\":\"%s\","
             "\"legalHold\":%s,\"endAt\":%" PRId64 ",\"now\":%" PRId64 "}}",
             (unsigned)(e - E), STATUS[e->status], DEPT[e->department], e->lead ? "lead" : "member",
             e->legal_hold ? "true" : "false", e->end_at / SECOND, now / SECOND);
}

static void push_spec(employee *e) {
    char buffer[512];
    spec_json(e, buffer, sizeof buffer);
    if (getenv("TRACE"))
        fprintf(stderr, "%.3f e%u <- spec %s\n", (double)now / DAY, (unsigned)(e - E), buffer);
    send_event(e->actor, "spec", buffer);
}

static employee *hire(int dept, int lead) {
    if (employee_count == cap_emp) {
        cap_emp = cap_emp ? cap_emp * 2 : 1024;
        E = (employee *)realloc(E, sizeof(employee) * cap_emp);
    }
    employee *e = &E[employee_count];
    memset(e, 0, sizeof *e);
    e->department = dept;
    e->lead = lead;
    char buffer[512];
    spec_json(e, buffer, sizeof buffer);
    uint32_t actor = 0;
    ta_status status;
    while ((status = ta_system_spawn(simulation_system, 0, (ta_string_view){"", 0},
                                     json_payload(buffer), &actor)) == TA_ERROR_NO_MEMORY &&
           ta_runtime_system_grow(&simulation_system) == TA_OK)
        ;
    if (status != TA_OK) {
        fprintf(stderr, "spawn failed (%d)\n", status);
        exit(1);
    }
    e->actor = actor;
    index_actor(actor, employee_count);
    employee_count++;
    return e;
}

/* ---- the policy, as the checker sees it (mirrors the chart) ---------------------------- */
static int offboarded(const employee *e) {
    return e->status != ACTIVE && now >= e->end_at;
}
static int expected(const employee *e, int app) {
    if (offboarded(e)) return app == GOOGLE && e->legal_hold ? L_SUSPENDED : L_NONE;
    switch (app) {
    case GOOGLE:
        return L_ACTIVE;
    case SLACK:
        return L_MEMBER;
    case GITHUB:
        return e->department != ENG ? L_NONE : e->lead && e->approved[GITHUB] ? L_ADMIN : L_MEMBER;
    case AWS:
        return e->department != ENG ? L_NONE : e->lead && e->approved[AWS] ? L_ADMIN : L_READONLY;
    case SALESFORCE:
        return e->department == SALES ? L_LICENSED : L_NONE;
    case ZENDESK:
        return e->department == SUPPORT ? L_AGENT : L_NONE;
    }
    return L_NONE;
}
static int residual_access(const employee *e) {
    for (int a = 0; a < N_APPS; a++)
        if (e->level[a] != L_NONE && !(a == GOOGLE && e->legal_hold && e->level[a] == L_SUSPENDED))
            return 1;
    return 0;
}

/* ---- host events (a binary heap on time) ----------------------------------------------- */
enum {
    H_REPLY,    /* deliver an event to a controller */
    H_DAY,      /* generate the day's HR activity */
    H_HR,       /* one HR change */
    H_LEFT,     /* HR marks a leaver as left */
    H_DRIFT,    /* a rogue admin changes an app */
    H_APPROVAL, /* a manager answers */
    H_SWEEP,    /* hibernate idle controllers */
    H_REPORT
};
typedef struct host_event {
    int64_t time;
    uint64_t sequence;
    int kind, arg;
    uint32_t employee;
    char *name, *json;
} host_event;
static host_event *heap;
static uint32_t heap_n, heap_cap;
static uint64_t heap_seq;

static int hev_less(const host_event *a, const host_event *b) {
    return a->time < b->time || (a->time == b->time && a->sequence < b->sequence);
}
static void schedule(int64_t t, int kind, uint32_t ei, int arg, const char *name,
                     const char *json) {
    if (heap_n == heap_cap) {
        heap_cap = heap_cap ? heap_cap * 2 : 4096;
        heap = (host_event *)realloc(heap, sizeof(host_event) * heap_cap);
    }
    host_event h = {
        t, heap_seq++, kind, arg, ei, name ? strdup(name) : NULL, json ? strdup(json) : NULL};
    uint32_t i = heap_n++;
    while (i && hev_less(&h, &heap[(i - 1) / 2])) {
        heap[i] = heap[(i - 1) / 2];
        i = (i - 1) / 2;
    }
    heap[i] = h;
}
static host_event pop(void) {
    host_event top = heap[0], last = heap[--heap_n];
    uint32_t i = 0;
    for (;;) {
        uint32_t c = 2 * i + 1;
        if (c >= heap_n) break;
        if (c + 1 < heap_n && hev_less(&heap[c + 1], &heap[c])) c++;
        if (!hev_less(&heap[c], &last)) break;
        heap[i] = heap[c];
        i = c;
    }
    if (heap_n) heap[i] = last;
    return top;
}
static void reply(employee *e, int64_t delay, const char *name, const char *json) {
    schedule(now + delay, H_REPLY, (uint32_t)(e - E), 0, name, json);
}

/* ---- the apps --------------------------------------------------------------------------- */
static uint64_t hash_str(const char *s) {
    uint64_t h = 1469598103934665603ull;
    while (*s)
        h = (h ^ (unsigned char)*s++) * 1099511628211ull;
    return h | 1;
}
static int key_seen(app_state *a, const char *key) { /* records it; 1 if seen before */
    if (a->key_count * 2 >= a->key_capacity) {
        uint32_t oc = a->key_capacity, nc = oc ? oc * 2 : 4096;
        uint64_t *old = a->keys;
        a->keys = (uint64_t *)calloc(nc, sizeof(uint64_t));
        a->key_capacity = nc;
        a->key_count = 0;
        for (uint32_t i = 0; i < oc; i++)
            if (old[i]) {
                uint32_t j = (uint32_t)(old[i] % nc);
                while (a->keys[j])
                    j = (j + 1) % nc;
                a->keys[j] = old[i];
                a->key_count++;
            }
        free(old);
    }
    uint64_t h = hash_str(key);
    uint32_t j = (uint32_t)(h % a->key_capacity);
    while (a->keys[j]) {
        if (a->keys[j] == h) return 1;
        j = (j + 1) % a->key_capacity;
    }
    a->keys[j] = h;
    a->key_count++;
    return 0;
}

static void note_change(employee *e, int app) {
    if (e->drift_at[app] && e->level[app] == expected(e, app)) {
        int64_t fix = now - e->drift_at[app];
        if (fix > max_drift_fix) max_drift_fix = fix;
        e->drift_at[app] = 0;
    }
    if (offboarded(e) && !e->revoked_at && !residual_access(e)) e->revoked_at = now;
}

static void set_level(employee *e, int app, int level) {
    if (app == SALESFORCE) A[app].licenses += (level == L_LICENSED) - (e->level[app] == L_LICENSED);
    e->level[app] = (uint8_t)level;
    note_change(e, app);
}

static int level_of(const char *name) {
    for (int l = 0; l < N_LEVELS; l++)
        if (!strcmp(LEVEL[l], name)) return l;
    return -1;
}
static int app_of(const char *name) {
    for (int a = 0; a < N_APPS; a++)
        if (!strcmp(APP[a], name)) return a;
    return -1;
}

/* tiny JSON field reader for the flat payloads the chart sends */
static int jget(const char *json, const char *key, char *out, size_t capacity) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = json ? strstr(json, pat) : NULL;
    if (!p) return 0;
    p += strlen(pat);
    size_t n = 0;
    if (*p == '"') {
        for (p++; *p && *p != '"' && n + 1 < capacity; p++)
            out[n++] = *p;
    } else {
        for (; *p && *p != ',' && *p != '}' && n + 1 < capacity; p++)
            out[n++] = *p;
    }
    out[n] = 0;
    return 1;
}

/* an apply request from a controller: decide now, answer after the app's latency */
static void saas_apply(employee *e, int app, const char *json) {
    app_state *a = &A[app];
    char lv[32], key[160], out[256];
    if (!jget(json, "level", lv, sizeof lv) || !jget(json, "key", key, sizeof key)) return;
    int level = level_of(lv);
    int64_t lat = urange(50, 1500);
    a->calls++;
    if (now >= a->down_from && now < a->down_until) {
        a->failed++;
        snprintf(out, sizeof out, "{\"app\":\"%s\",\"reason\":\"unavailable\"}", APP[app]);
        reply(e, lat, "apply.failed", out);
        return;
    }
    if (a->rate_per_min) {
        int64_t minute = now / MIN;
        if (minute != a->minute) {
            a->minute = minute;
            a->tokens = a->rate_per_min;
        }
        if (a->tokens <= 0) {
            a->failed++;
            snprintf(out, sizeof out,
                     "{\"app\":\"%s\",\"reason\":\"rate_limited\",\"retryAfter\":%" PRId64 "}",
                     APP[app], (MIN - now % MIN) / SECOND + 1);
            reply(e, lat, "apply.failed", out);
            return;
        }
        a->tokens--;
    }
    if (now >= a->error_from && now < a->error_until && frand() < a->error_rate) {
        a->failed++;
        snprintf(out, sizeof out, "{\"app\":\"%s\",\"reason\":\"internal_error\"}", APP[app]);
        reply(e, lat, "apply.failed", out);
        return;
    }
    if (key_seen(a, key)) {
        a->deduped++; /* a retry of something already done: no second effect */
    } else {
        if (level == L_LICENSED && e->level[app] != L_LICENSED &&
            a->licenses >= a->license_capacity) {
            a->failed++;
            snprintf(out, sizeof out, "{\"app\":\"%s\",\"reason\":\"no_license\"}", APP[app]);
            reply(e, lat, "apply.failed", out);
            return;
        }
        if (level == L_ADMIN && !e->approved[app]) v_unapproved++;
        set_level(e, app, level);
    }
    if (frand() < 0.005) { /* the response is lost; the controller retries with the same key */
        a->lost++;
        return;
    }
    snprintf(out, sizeof out, "{\"app\":\"%s\",\"level\":\"%s\"}", APP[app], LEVEL[e->level[app]]);
    reply(e, lat, "apply.ok", out);
}

static void saas_list(employee *e) {
    char out[512];
    size_t n = (size_t)snprintf(out, sizeof out, "{\"apps\":{");
    int first = 1;
    for (int a = 0; a < N_APPS; a++)
        if (e->level[a] != L_NONE) {
            n += (size_t)snprintf(out + n, sizeof out - n, "%s\"%s\":\"%s\"", first ? "" : ",",
                                  APP[a], LEVEL[e->level[a]]);
            first = 0;
        }
    snprintf(out + n, sizeof out - n, "}}");
    reply(e, urange(200, 3000), "inventory", out);
}

/* ---- hibernation (the host's side: storage + policy) ----------------------------------- */
typedef struct blob_ref {
    long offset;
    uint32_t length, actor;
} blob_ref;
static FILE *store;
static blob_ref *blobs; /* by actor slot */
static uint32_t blob_capacity;
static unsigned char blob_buffer[1 << 16];
static uint32_t sweep_cursor;

static void hibernate_idle(void) {
    static uint32_t cold[4096];
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(simulation_system, &stats);
    uint32_t budget = stats.actors; /* one revolution */
    while (budget) {
        uint32_t chunk = budget < 4096 ? budget : 4096;
        budget -= chunk;
        uint32_t n = ta_system_cold_actors(simulation_system, &sweep_cursor, chunk, cold, chunk);
        for (uint32_t i = 0; i < n; i++) {
            size_t length = 0;
            if (ta_actor_hibernate(simulation_system, cold[i], blob_buffer, sizeof blob_buffer,
                                   &length) != TA_OK)
                continue;
            uint32_t slot = (cold[i] & 0xFFFFFFu) - 1u;
            if (slot >= blob_capacity) {
                uint32_t nc = blob_capacity ? blob_capacity : 1024;
                while (nc <= slot)
                    nc *= 2;
                blobs = (blob_ref *)realloc(blobs, sizeof(blob_ref) * nc);
                memset(blobs + blob_capacity, 0, sizeof(blob_ref) * (nc - blob_capacity));
                blob_capacity = nc;
            }
            fseek(store, 0, SEEK_END);
            blobs[slot] = (blob_ref){ftell(store), (uint32_t)length, cold[i]};
            fwrite(blob_buffer, 1, length, store);
            m_hibernations++;
        }
    }
}

static void wake(uint32_t actor) {
    uint32_t slot = (actor & 0xFFFFFFu) - 1u;
    if (slot >= blob_capacity || blobs[slot].actor != actor) {
        fprintf(stderr, "no blob for actor %u\n", actor);
        exit(1);
    }
    fseek(store, blobs[slot].offset, SEEK_SET);
    if (fread(blob_buffer, 1, blobs[slot].length, store) != blobs[slot].length) exit(1);
    ta_status status;
    while ((status = ta_actor_wake(simulation_system, actor, blob_buffer, blobs[slot].length)) ==
               TA_ERROR_NO_MEMORY &&
           ta_runtime_system_grow(&simulation_system) == TA_OK)
        ;
    if (status != TA_OK) {
        fprintf(stderr, "wake failed (%d)\n", status);
        exit(1);
    }
    blobs[slot].actor = 0;
    m_wakes++;
}

/* ---- effects: what the controllers asked of the outside world -------------------------- */
static uint32_t *wakes_due; /* TA_EFFECT_WAKE: never drop one, the actor would wait forever */
static uint32_t wakes_due_count, cap_wakes_due;

static void handle_effect(const ta_effect *effect) {
    employee *e = emp_of(effect->actor);
    char json[1024];
    json[0] = 0;
    if (effect->payload)
        ta_datamodel_ecmascript_payload_to_json(simulation_datamodel, effect->payload, json,
                                                sizeof json);
    char event[64];
    snprintf(event, sizeof event, "%.*s", (int)effect->event.length,
             effect->event.pointer ? effect->event.pointer : "");
    switch (effect->kind) {
    case TA_EFFECT_SEND:
        if (!e) return;
        if (effect->io_processor == IO_SAAS) {
            char target[32];
            snprintf(target, sizeof target, "%.*s", (int)effect->target.length,
                     effect->target.pointer);
            if (!strcmp(event, "list"))
                saas_list(e);
            else if (app_of(target) >= 0)
                saas_apply(e, app_of(target), json);
        } else if (effect->io_processor == IO_APPROVALS) {
            char app[32];
            if (!jget(json, "app", app, sizeof app) || app_of(app) < 0) return;
            int a = app_of(app);
            e->asked[a] = 1;
            m_approvals_asked++;
            double r = frand();
            int64_t when = now + urange(1 * HOUR, 36 * HOUR);
            if (r < 0.80)
                schedule(when, H_APPROVAL, (uint32_t)(e - E), a * 2 + 1, NULL, NULL);
            else if (r < 0.92)
                schedule(when, H_APPROVAL, (uint32_t)(e - E), a * 2, NULL, NULL);
            /* else: the manager never answers (the chart expires the request after 72 h) */
        } else if (effect->io_processor == IO_HR) {
            if (!strcmp(event, "status")) {
                char v[16];
                m_status++;
                e->ready = jget(json, "ready", v, sizeof v) && !strcmp(v, "true");
                e->progressing = jget(json, "progressing", v, sizeof v) && !strcmp(v, "true");
                e->awaiting = jget(json, "awaitingApproval", v, sizeof v) && !strcmp(v, "true");
                e->degraded = jget(json, "degraded", v, sizeof v) && !strcmp(v, "true");
                e->blocked = jget(json, "blocked", v, sizeof v) && !strcmp(v, "true");
            } else if (!strcmp(event, "finalized")) {
                m_finalized++;
                if (residual_access(e)) v_finalize_with_access++;
            } else if (!strcmp(event, "alert")) {
                m_alerts++;
            }
        }
        break;
    case TA_EFFECT_LOG:
        if (getenv("TRACE"))
            fprintf(stderr, "%.3f log %.*s: %.*s\n", (double)now / DAY,
                    (int)effect->auxiliary.length, effect->auxiliary.pointer,
                    (int)effect->event.length, effect->event.pointer);
        break;
    case TA_EFFECT_WAKE:
        if (wakes_due_count == cap_wakes_due) {
            cap_wakes_due = cap_wakes_due ? cap_wakes_due * 2 : 1024;
            wakes_due = (uint32_t *)realloc(wakes_due, sizeof(uint32_t) * cap_wakes_due);
        }
        wakes_due[wakes_due_count++] = effect->actor;
        break;
    case TA_EFFECT_DONE: /* the finalizer completed: the record is gone */
        if (e) {
            e->deleted = 1;
            e->actor = 0;
        }
        break;
    case TA_EFFECT_ERROR:
        v_faults++;
        break;
    default:
        break;
    }
}

static void run_until_idle(void) {
    for (;;) {
        ta_run result = ta_runtime_run(&simulation_system, 1u << 22);
        const ta_effect *effect;
        uint32_t n = ta_system_effects(simulation_system, &effect);
        wakes_due_count = 0;
        for (uint32_t i = 0; i < n; i++)
            handle_effect(&effect[i]);
        ta_system_effects_ack(simulation_system, n);
        for (uint32_t i = 0; i < wakes_due_count; i++)
            wake(wakes_due[i]); /* a real host could do this asynchronously */
        if (result.status == TA_RUN_IDLE && !wakes_due_count && !n) break;
        if (result.status == TA_RUN_FAULT) {
            fprintf(stderr, "system fault\n");
            exit(1);
        }
    }
}

/* ---- the HR feed, rogue admins, outages ------------------------------------------------ */
static uint32_t random_emp(int want_status) {
    for (int tries = 0; tries < 64; tries++) {
        uint32_t i = (uint32_t)(rnd() % employee_count);
        if (!E[i].deleted && E[i].status == want_status) return i;
    }
    return UINT32_MAX;
}

static int64_t days, quiet_from;

static void plan_day(int64_t day_start, uint32_t population) {
    if (day_start >= quiet_from) return; /* the last days are quiet: convergence check */
    uint32_t hires = 1 + population / 500, leavers = 1 + population / 650,
             movers = 1 + population / 400, drift = 1 + population / 300;
    for (uint32_t i = 0; i < hires; i++)
        schedule(day_start + urange(8 * HOUR, 18 * HOUR), H_HR, 0, 0, NULL, NULL);
    for (uint32_t i = 0; i < leavers; i++)
        schedule(day_start + urange(8 * HOUR, 18 * HOUR), H_HR, 0, 1, NULL, NULL);
    for (uint32_t i = 0; i < movers; i++)
        schedule(day_start + urange(8 * HOUR, 18 * HOUR), H_HR, 0, 2, NULL, NULL);
    schedule(day_start + urange(8 * HOUR, 18 * HOUR), H_HR, 0, 3, NULL, NULL); /* rescind */
    for (uint32_t i = 0; i < drift; i++)
        schedule(day_start + urange(0, DAY), H_DRIFT, 0, 0, NULL, NULL);
    if (day_start / DAY == days / 2) /* reorg: 5% of engineering moves to support */
        schedule(day_start + 10 * HOUR, H_HR, 0, 4, NULL, NULL);
}

static void hr_change(int what) {
    static const int dept_mix[10] = {ENG,   ENG,     ENG,     ENG,     SALES,
                                     SALES, SUPPORT, SUPPORT, FINANCE, ENG};
    if (what == 0) { /* joiner */
        hire(dept_mix[rnd() % 10], frand() < 0.12);
        m_hires++;
        return;
    }
    if (what == 4) {
        uint32_t moved = 0;
        for (uint32_t i = 0; i < employee_count; i++)
            if (!E[i].deleted && E[i].status == ACTIVE && E[i].department == ENG &&
                frand() < 0.05) {
                E[i].department = SUPPORT;
                E[i].lead = 0;
                push_spec(&E[i]);
                moved++;
            }
        say("reorg: %u engineers move to support", moved);
        m_movers += moved;
        return;
    }
    uint32_t i = random_emp(what == 3 ? LEAVING : ACTIVE);
    if (i == UINT32_MAX) return;
    employee *e = &E[i];
    if (what == 1) { /* leaver: an end date within two weeks, sometimes under legal hold */
        e->status = LEAVING;
        e->end_at = (now / DAY + 1 + (int64_t)(rnd() % 14)) * DAY + 18 * HOUR;
        e->legal_hold = frand() < 0.15;
        m_holds += (uint64_t)e->legal_hold;
        m_leavers++;
        schedule(e->end_at + urange(1 * HOUR, 48 * HOUR), H_LEFT, i, (int)(e->end_at / HOUR), NULL,
                 NULL);
    } else if (what == 2) { /* mover: promotion, demotion, or another department */
        int lead = e->lead, dept = e->department;
        if (frand() < 0.5)
            e->lead = !e->lead;
        else
            e->department = (int)(rnd() % N_DEPTS);
        if (e->lead != lead || e->department != dept) /* a new role: privileges are re-approved */
            memset(e->approved, 0, sizeof e->approved);
        m_movers++;
    } else { /* rescinded resignation, before the end date */
        if (now >= e->end_at) return;
        e->status = ACTIVE;
        e->legal_hold = 0;
        e->end_at = 0;
        m_rescinds++;
    }
    push_spec(e);
}

static void drift_one(void) {
    uint32_t i = random_emp(ACTIVE);
    if (i == UINT32_MAX) return;
    employee *e = &E[i];
    int app;
    int level;
    switch (rnd() % 3) {
    case 0:
        app = GITHUB, level = L_ADMIN;
        break; /* "just for this deploy" */
    case 1:
        app = SLACK, level = L_NONE;
        break; /* removed by mistake */
    default:
        app = AWS, level = L_ADMIN;
        break;
    }
    if (e->level[app] == level || expected(e, app) == level) return;
    set_level(e, app, level);
    e->drift_at[app] = now;
    m_drift++;
    if (frand() < 0.05) { /* the watch event is lost: only the daily resync will notice */
        m_drift_lost++;
        return;
    }
    char out[128];
    snprintf(out, sizeof out, "{\"app\":\"%s\",\"level\":\"%s\"}", APP[app], LEVEL[level]);
    reply(e, urange(100, 5000), "changed", out);
}

static void plan_outages(void) {
    if (days < 8) return;
    A[SLACK].down_from = 4 * DAY + 9 * HOUR, A[SLACK].down_until = A[SLACK].down_from + 6 * HOUR;
    A[GOOGLE].error_from = 6 * DAY, A[GOOGLE].error_until = 6 * DAY + 12 * HOUR,
    A[GOOGLE].error_rate = 0.3;
    A[AWS].down_from = (days / 2 + 1) * DAY + 17 * HOUR,
    A[AWS].down_until = A[AWS].down_from + 3 * HOUR;
    A[GITHUB].rate_per_min = 120; /* always rate limited */
}

/* ---- report + checks ------------------------------------------------------------------- */
static void report(void) {
    uint32_t live = 0, ready = 0, progressing = 0, awaiting = 0, degraded = 0, blocked = 0;
    for (uint32_t i = 0; i < employee_count; i++) {
        const employee *e = &E[i];
        if (e->deleted) continue;
        live++;
        ready += (uint32_t)e->ready;
        progressing += (uint32_t)e->progressing;
        awaiting += (uint32_t)e->awaiting;
        degraded += (uint32_t)e->degraded;
        blocked += (uint32_t)e->blocked;
    }
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(simulation_system, &stats);
    say("people %u: ready %u, progressing %u, awaiting approval %u, degraded %u, blocked %u | "
        "controllers %u, hibernated %u (%.0f%%), wakes %" PRIu64,
        live, ready, progressing, awaiting, degraded, blocked, stats.actors, stats.hibernated,
        stats.actors ? 100.0 * stats.hibernated / stats.actors : 0.0, m_wakes);
}

static int outage_overlaps(int64_t from, int64_t to) {
    for (int a = 0; a < N_APPS; a++)
        if (A[a].down_until > from && A[a].down_from < to) return 1;
    return 0;
}

static void final_checks(void) {
    for (uint32_t i = 0; i < employee_count; i++) {
        employee *e = &E[i];
        if (e->status != ACTIVE && e->end_at && e->end_at < now - DAY) {
            int64_t took = e->revoked_at         ? e->revoked_at - e->end_at
                           : !residual_access(e) ? 0 /* revoked before the end date */
                                                 : now - e->end_at;
            if (took > max_revoke) max_revoke = took;
            if (took > HOUR && getenv("TRACE"))
                fprintf(stderr, "late: e%u end %.2f revoked %.2f status %s hold %d\n", i,
                        (double)e->end_at / DAY, (double)e->revoked_at / DAY, STATUS[e->status],
                        e->legal_hold);
            if (took > HOUR) {
                if (outage_overlaps(e->end_at, e->end_at + HOUR))
                    v_late_excused++;
                else
                    v_late_revoke++;
            }
        }
        if (e->deleted) continue;
        for (int a = 0; a < N_APPS; a++) {
            int want = expected(e, a);
            if (e->drift_at[a] && e->level[a] != want) v_drift_uncorrected++;
            if (e->level[a] == want) continue;
            if (a == SALESFORCE && A[a].licenses >= A[a].license_capacity) continue; /* blocked */
            v_not_converged++;
            if (v_not_converged <= 5)
                fprintf(stderr, "not converged: e%u %s is %s, want %s (status %s)\n", i, APP[a],
                        LEVEL[e->level[a]], LEVEL[want], STATUS[e->status]);
        }
    }
}

int main(int argc, char **argv) {
    uint32_t employees = 10000;
    int check = 0;
    const char *chart = CHART_PATH;
    days = 30;
    setvbuf(stdout, NULL, _IOLBF, 0);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--employees") && i + 1 < argc)
            employees = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--days") && i + 1 < argc)
            days = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            rng_state ^= (uint64_t)atoll(argv[++i]) * 0x2545F4914F6CDD1Dull;
        else if (!strcmp(argv[i], "--chart") && i + 1 < argc)
            chart = argv[++i];
        else if (!strcmp(argv[i], "--check"))
            check = 1;
        else if (!strcmp(argv[i], "--quiet"))
            quiet = 1;
        else {
            fprintf(stderr, "usage: %s [--employees N] [--days D] [--seed S] [--check] [--quiet]\n",
                    argv[0]);
            return 2;
        }
    }
    quiet_from = (days - 3) * DAY;

    /* compile the chart into an image and start the system */
    ta_compile_result *result;
    if (ta_runtime_compile_file(chart, 0, &result) != TA_OK) {
        const ta_diagnostic *d;
        uint32_t n = ta_compile_diagnostics(result, &d);
        for (uint32_t i = 0; i < n; i++)
            fprintf(stderr, "%s:%u: %.*s\n", chart, d[i].line, (int)d[i].message.length,
                    d[i].message.pointer);
        return 1;
    }
    ta_string_view bytes = ta_compile_image(result);
    void *image = malloc(bytes.length + 8);
    memcpy(image, bytes.pointer, bytes.length);
    if (ta_image_open(image, bytes.length, &simulation_image) != TA_OK) return 1;
    ta_compile_free(result);
    simulation_datamodel = ta_datamodel_ecmascript_new(NULL);
    ta_datamodel_binding binding = ta_datamodel_ecmascript_binding(simulation_datamodel);
    static const ta_io_processor_descriptor io_processor[N_IO] = {
        {{"saas", 4}, {"saas:", 5}, 0, 0},
        {{"approvals", 9}, {"approvals:", 10}, 0, 0},
        {{"hr", 2}, {"hr:", 3}, 0, 0}};
    ta_system_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.max_actors = 1024;
    config.max_events = 4096;
    config.max_timers = 4096;
    config.ticks_per_second = 1000; /* the clock is ours: simulated milliseconds */
    config.io_processors = io_processor;
    config.io_processor_count = N_IO;
    if (ta_runtime_system_new(&simulation_image, &config, &binding, 1, &simulation_system) != TA_OK)
        return 1;
    store = tmpfile();

    /* the company on day 0 */
    for (int a = 0; a < N_APPS; a++) {
        A[a].down_from = A[a].down_until = A[a].error_from = A[a].error_until = -1;
        A[a].minute = -1;
    }
    A[SALESFORCE].license_capacity = (int)(employees * 0.21); /* a little short, eventually */
    plan_outages();
    static const int mix[10] = {ENG, ENG, ENG, ENG, SALES, SALES, SUPPORT, SUPPORT, FINANCE, ENG};
    for (uint32_t i = 0; i < employees; i++) {
        hire(mix[rnd() % 10], frand() < 0.1);
        if ((i & 1023) == 1023) run_until_idle();
    }
    run_until_idle();
    say("%u people hired; controllers started", employees);
    for (int64_t d = 0; d < days; d++) {
        schedule(d * DAY, H_DAY, 0, 0, NULL, NULL);
        schedule(d * DAY + 23 * HOUR, H_REPORT, 0, 0, NULL, NULL);
    }
    for (int64_t t = HOUR; t < days * DAY; t += HOUR)
        schedule(t, H_SWEEP, 0, 0, NULL, NULL);

    /* the discrete-event loop: jump to whatever happens next (host event or system timer) */
    int64_t end = days * DAY;
    while (now < end) {
        int64_t next_system = (int64_t)ta_system_next_deadline(simulation_system);
        int64_t next_host = heap_n ? heap[0].time : INT64_MAX;
        int64_t t = next_host < next_system ? next_host : next_system;
        if (t > end || t == INT64_MAX) t = end;
        if (t > now) now = t;
        ta_system_advance_time(simulation_system, (ta_time)now);
        while (heap_n && heap[0].time <= now) {
            host_event h = pop();
            employee *e = &E[h.employee];
            switch (h.kind) {
            case H_REPLY:
                if (getenv("TRACE"))
                    fprintf(stderr, "%.3f e%u <- %s %s\n", (double)now / DAY, h.employee, h.name,
                            h.json ? h.json : "");
                if (!e->deleted) send_event(e->actor, h.name, h.json);
                break;
            case H_DAY: {
                uint32_t pop_n = 0;
                for (uint32_t i = 0; i < employee_count; i++)
                    pop_n += !E[i].deleted;
                plan_day(now, pop_n);
                break;
            }
            case H_HR:
                hr_change(h.arg);
                break;
            case H_LEFT: /* (for this resignation: a rescinded one's end date no longer counts) */
                if (!e->deleted && e->status == LEAVING && e->end_at / HOUR == h.arg) {
                    e->status = LEFT;
                    push_spec(e);
                }
                break;
            case H_DRIFT:
                drift_one();
                break;
            case H_APPROVAL:
                if (!e->deleted) {
                    int a = h.arg / 2;
                    char out[128];
                    snprintf(out, sizeof out, "{\"app\":\"%s\",\"level\":\"admin\"}", APP[a]);
                    if (h.arg & 1) e->approved[a] = 1;
                    send_event(e->actor, h.arg & 1 ? "approval.granted" : "approval.denied", out);
                }
                break;
            case H_SWEEP:
                hibernate_idle();
                break;
            case H_REPORT:
                report();
                break;
            }
            free(h.name);
            free(h.json);
            run_until_idle();
        }
        run_until_idle();
    }
    final_checks();

    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(simulation_system, &stats);
    uint64_t calls = 0, dedup = 0, failed = 0, lost = 0;
    for (int a = 0; a < N_APPS; a++)
        calls += A[a].calls, dedup += A[a].deduped, failed += A[a].failed, lost += A[a].lost;
    printf("\n%" PRId64 " simulated days, %u people ever employed\n", days, employee_count);
    printf("  HR: %" PRIu64 " joiners, %" PRIu64 " leavers (%" PRIu64 " legal holds), %" PRIu64
           " movers, %" PRIu64 " rescinded; %" PRIu64 " records finalized\n",
           m_hires + employees, m_leavers, m_holds, m_movers, m_rescinds, m_finalized);
    printf("  apps: %" PRIu64 " calls, %" PRIu64
           " failed (outages, errors, rate limits, licenses), "
           "%" PRIu64 " responses lost, %" PRIu64 " retries deduplicated by idempotency key\n",
           calls, failed, lost, dedup);
    printf("  drift: %" PRIu64 " rogue changes (%" PRIu64
           " watch events lost), slowest fix %.1f h\n",
           m_drift, m_drift_lost, (double)max_drift_fix / HOUR);
    printf("  offboarding: slowest revocation %.0f min after the end date, %" PRIu64
           " overdue alerts\n",
           (double)max_revoke / MIN, m_alerts);
    printf("  approvals asked %" PRIu64 "; salesforce licenses %d/%d\n", m_approvals_asked,
           A[SALESFORCE].licenses, A[SALESFORCE].license_capacity);
    printf("  memory: %u controllers, %u hibernated; %" PRIu64 " hibernations, %" PRIu64
           " wakes; region %.1f MB, JS heap %.1f MB\n",
           stats.actors, stats.hibernated, m_hibernations, m_wakes,
           (double)stats.region_bytes / 1048576,
           (double)ta_datamodel_ecmascript_heap_bytes(simulation_datamodel) / 1048576);
    printf("\ninvariants:\n");
    printf("  %-58s %" PRIu64 "\n", "privileged access granted without approval", v_unapproved);
    printf("  %-58s %" PRIu64 "\n", "records finalized while access remained",
           v_finalize_with_access);
    printf("  %-58s %" PRIu64 " (+%" PRIu64 " during outages)\n", "leavers revoked later than 1 h",
           v_late_revoke, v_late_excused);
    printf("  %-58s %" PRIu64 "\n", "people not converged after the quiet days", v_not_converged);
    printf("  %-58s %" PRIu64 "\n", "rogue changes not corrected", v_drift_uncorrected);
    printf("  %-58s %" PRIu64 "\n", "controller faults", v_faults);
    uint64_t bad = v_unapproved + v_finalize_with_access + v_late_revoke + v_not_converged +
                   v_drift_uncorrected + v_faults;
    printf("%s\n", bad ? "FAILED" : "all invariants hold");

    ta_runtime_system_free(simulation_system);
    ta_datamodel_ecmascript_free(simulation_datamodel);
    free(image);
    fclose(store);
    return check && bad ? 1 : 0;
}
