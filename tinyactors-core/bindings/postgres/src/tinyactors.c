/* tinyactors.c — durable SCXML actors in PostgreSQL.
 *
 * Every actor is a row (tinyactors.actors). Processing an actor is one short
 * cycle inside the calling transaction:
 *
 *   lock the row ─▶ adopt its exported state into a fresh, small ta_system
 *   ─▶ deliver its inbox rows ─▶ run to a stable point (fuel-bounded)
 *   ─▶ route the effects: events for other actors → their inbox rows,
 *      sends to registered I/O processors → outbox rows, <log> → server log
 *   ─▶ export it again ─▶ UPDATE the row (state, configuration, datamodel,
 *      next_due_at)
 *
 * Nothing about actors is cached across statements, so a rollback leaves no
 * stale actor behind; what is cached is immutable: chart images, prepared
 * plans, and the datamodel's compiled expressions. A call costs what it
 * processes — each actor runs in an SPI level of its own (its memory goes
 * when it is done) and run() finds work through indexes — so the table can
 * hold millions of actors of which only some are ever hot. The first actor of a call waits for
 * its row lock; actors reached through sends are taken with SKIP LOCKED
 * (their events wait in the inbox for run() or their next send), so a
 * statement only ever waits for one lock: no deadlocks between actors.
 * Actor rows are locked FOR NO KEY UPDATE (what the UPDATE of a row takes
 * anyway: the key never changes), which does not conflict with the FOR KEY
 * SHARE lock an inbox row's foreign key takes — so queueing an event for a
 * busy actor never waits, and two senders to one actor cannot deadlock.
 *
 * The clock is epoch milliseconds (ticks_per_second = 1000): delayed sends
 * carry absolute due times, valid in any later transaction or process.
 * The ECMAScript datamodel (quickjs-ng) is created per backend and dropped
 * after an error (it may hold half-processed sessions then).
 */
#include "postgres.h"

#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "funcapi.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "utils/resowner.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "utils/array.h"
#include "utils/jsonb.h"

#include <tinyactors.h>
#include <tinyactors/compile.h>
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(ta_postgres_compile);
PG_FUNCTION_INFO_V1(ta_postgres_chart_datamodel);
PG_FUNCTION_INFO_V1(ta_postgres_spawn);
PG_FUNCTION_INFO_V1(ta_postgres_process);
PG_FUNCTION_INFO_V1(ta_postgres_run);
PG_FUNCTION_INFO_V1(ta_postgres_deliver);
PG_FUNCTION_INFO_V1(ta_postgres_now);
PG_FUNCTION_INFO_V1(ta_postgres_system_id);
PG_FUNCTION_INFO_V1(ta_postgres_address);
PG_FUNCTION_INFO_V1(ta_postgres_data_ok);
PG_FUNCTION_INFO_V1(ta_postgres_envelope);
PG_FUNCTION_INFO_V1(ta_postgres_repeer);
PG_FUNCTION_INFO_V1(ta_postgres_error);

void _PG_init(void);

/* ---- settings ---------------------------------------------------------------------------- */
static int fuel_per_actor = 1000000;
static int cascade_limit = 1000;
static int log_level = LOG;
static char *clock_setting = NULL;
static char *system_id_setting = NULL;
static char *dedup_ttl_setting = NULL;
static int inbox_batch = 1000;
static int max_failures = 10;
static bool store_datamodel = true;
static int js_memory_kb = 256 * 1024;
static int max_state_kb = 64 * 1024;
static int max_system_kb = 256 * 1024;
static int log_limit = 100;
static int max_chart_kb = 1024;
static bool notify_on = true;
static char *outbox_retention_setting = NULL;

static const struct config_enum_entry log_levels[] = {
    {"debug", DEBUG1, false},  {"log", LOG, false},         {"info", INFO, false},
    {"notice", NOTICE, false}, {"warning", WARNING, false}, {"off", -1, false},
    {NULL, 0, false}};

void _PG_init(void) {
    DefineCustomIntVariable(
        "tinyactors.fuel", "Fuel an actor may use per processing before its transaction fails.",
        "Bounds runaway eventless loops: the transaction is rolled back.", &fuel_per_actor, 1000000,
        1000, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomIntVariable("tinyactors.cascade",
                            "Actors processed per call after the first one (events they send).",
                            "The rest stays in the inbox for tinyactors.run().", &cascade_limit,
                            1000, 0, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomEnumVariable("tinyactors.log", "Where <log> output of actors goes.", NULL,
                             &log_level, LOG, log_levels, PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("tinyactors.now",
                               "The actors' clock (a timestamptz); empty: clock_timestamp().",
                               "For tests and simulations: set it, then call tinyactors.run().",
                               &clock_setting, "", PGC_SUSET, 0, NULL, NULL, NULL);
    DefineCustomStringVariable(
        "tinyactors.system_id",
        "This database's system id in actor addresses (tinyactors://<system_id>/...).",
        "Empty: derived from the database name (lowercase; other characters become \"-\"). Set it "
        "per database (ALTER DATABASE ... SET "
        "tinyactors.system_id = 'orders'); it must be a valid system id (docs/protocol.md).",
        &system_id_setting, "", PGC_SUSET, 0, NULL, NULL, NULL);
    DefineCustomStringVariable(
        "tinyactors.dedup_ttl",
        "How long tinyactors.deliver(envelope) remembers message ids (an interval).",
        "Envelopes from other systems repeat within this time (retries of at-least-once "
        "transports) are dropped as duplicates; older entries of tinyactors.seen are pruned.",
        &dedup_ttl_setting, "7 days", PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomIntVariable("tinyactors.inbox_batch",
                            "Queued events delivered to an actor per processing.",
                            "A backlog is worked off in bounded processings (memory stays flat); "
                            "the rest waits for the next processing or tinyactors.run().",
                            &inbox_batch, 1000, 1, 1000000, PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomIntVariable(
        "tinyactors.max_failures",
        "Failed processings in a row after which run() marks an actor failed (0: never).",
        "run() processes each actor on its own: a failure is recorded (failures, last_error, "
        "retry_at with exponential backoff) and the others go on.",
        &max_failures, 10, 0, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomBoolVariable("tinyactors.store_datamodel",
                             "Write the datamodel as jsonb to tinyactors.actors on every event.",
                             "Off: the column is NULL (the state column keeps everything); saves "
                             "the jsonb rewrite and its WAL for big datamodels.",
                             &store_datamodel, true, PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomIntVariable(
        "tinyactors.js_memory_limit", "Memory the backend's JavaScript engine may use.",
        "Beyond it the allocating script fails (error.execution in the chart); the backend, and "
        "the server, are safe from a chart that allocates without end.",
        &js_memory_kb, 256 * 1024, 8 * 1024, INT_MAX, PGC_USERSET, GUC_UNIT_KB, NULL, NULL, NULL);
    DefineCustomIntVariable("tinyactors.max_state_bytes",
                            "Largest stored state of one actor (state plus datamodel column).",
                            "An actor whose state grows beyond it fails its transaction.",
                            &max_state_kb, 64 * 1024, 64, INT_MAX, PGC_USERSET, GUC_UNIT_KB, NULL,
                            NULL, NULL);
    DefineCustomIntVariable("tinyactors.max_system_bytes",
                            "Largest in-memory system (actor, invoked sessions, queues) of one "
                            "processing.",
                            "A chart that keeps invoking or queueing beyond it fails its "
                            "transaction.",
                            &max_system_kb, 256 * 1024, 1024, INT_MAX, PGC_USERSET, GUC_UNIT_KB,
                            NULL, NULL, NULL);
    DefineCustomIntVariable("tinyactors.log_limit", "<log> lines per processing of an actor.",
                            "The rest are counted and reported in one line; each message is cut "
                            "at 1000 bytes.",
                            &log_limit, 100, 0, INT_MAX, PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomIntVariable("tinyactors.max_chart_bytes",
                            "Largest SCXML document define() compiles.", NULL, &max_chart_kb, 1024,
                            1, INT_MAX, PGC_SUSET, GUC_UNIT_KB, NULL, NULL, NULL);
    DefineCustomBoolVariable(
        "tinyactors.notify", "NOTIFY tinyactors_outbox for new outbox rows.",
        "Notifications are skipped anyway while the server's NOTIFY queue is more than half "
        "full, so a stalled listener cannot make effects fail; workers poll as well.",
        &notify_on, true, PGC_USERSET, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("tinyactors.outbox_retention",
                               "How long delivered and failed outbox rows are kept (an interval).",
                               "tinyactors.run() deletes older ones (and dedup entries older "
                               "than tinyactors.dedup_ttl), a bounded batch at a time.",
                               &outbox_retention_setting, "7 days", PGC_USERSET, 0, NULL, NULL,
                               NULL);
    MarkGUCPrefixReserved("tinyactors");
}

/* ---- small helpers ------------------------------------------------------------------------- */

static TimestampTz clock_now(void) {
    if (clock_setting && clock_setting[0])
        return DatumGetTimestampTz(
            DirectFunctionCall3(timestamptz_in, CStringGetDatum(clock_setting),
                                ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)));
    return GetCurrentTimestamp();
}

/* epoch milliseconds <-> timestamptz (PostgreSQL counts µs from 2000-01-01) */
#define POSTGRES_EPOCH_UNIX_MS                                                                     \
    ((int64)(POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE) * SECS_PER_DAY * 1000)
static ta_time to_ms(TimestampTz t) {
    return (ta_time)(t / 1000 + POSTGRES_EPOCH_UNIX_MS);
}
static TimestampTz from_ms(ta_time ms) {
    /* beyond timestamptz (a saturated, "never" delay): infinity, never due */
    if (ms > (ta_time)9000000000000000ull) return DT_NOEND;
    return ((TimestampTz)ms - POSTGRES_EPOCH_UNIX_MS) * 1000;
}

/* this database's system id: tinyactors.system_id, else derived from the
 * database name — lowercased, every other character than a-z 0-9 "-" "."
 * becoming "-", leading/trailing "-" of labels dropped ("My_App" -> "my-app") */
static char base_uri[TA_ADDRESS_SYSTEM_MAX + 16];
static char derived_id[TA_ADDRESS_SYSTEM_MAX + 1];
static ta_string_view system_id(void) {
    const char *id = system_id_setting;
    if (!id || !id[0]) {
        const char *db = get_database_name(MyDatabaseId);
        uint32_t n = 0;
        for (const char *p = db; *p && n < TA_ADDRESS_SYSTEM_MAX; p++) {
            char c = *p >= 'A' && *p <= 'Z' ? (char)(*p - 'A' + 'a') : *p;
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.')) c = '-';
            if (c == '-' && (n == 0 || derived_id[n - 1] == '.' || derived_id[n - 1] == '-'))
                continue; /* no label starts with "-", no runs */
            if (c == '.' && n > 0 && derived_id[n - 1] == '-') n--; /* nor ends with it */
            derived_id[n++] = c;
        }
        while (n > 0 && (derived_id[n - 1] == '-' || derived_id[n - 1] == '.'))
            n--;
        derived_id[n] = 0;
        id = n ? derived_id : "db";
    }
    ta_string_view s = {id, (uint32_t)strlen(id)};
    if (!ta_address_system_valid(s))
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("tinyactors: \"%s\" is not a valid system id", id),
                 errhint("System ids are lowercase a-z, 0-9, \"-\" and dots "
                         "(docs/protocol.md). Set tinyactors.system_id for this database.")));
    return s;
}
static const char *base(void) {
    ta_string_view id = system_id();
    snprintf(base_uri, sizeof base_uri, "tinyactors://%.*s/", (int)id.length, id.pointer);
    return base_uri;
}

/* ---- prepared statements --------------------------------------------------------------------
 * Every statement on the processing path is planned once per backend and
 * kept (SPI_keepplan; the plan cache revalidates it after DDL). Planning is
 * most of the cost of a short statement, and these run several times per
 * actor processed. */
enum {
    Q_ACTIVE,
    Q_INBOX_INSERT,
    Q_OUTBOX_INSERT,
    Q_ACTOR_INSERT,
    Q_ACTOR_UPDATE,
    Q_LOCK_WAIT,
    Q_LOCK_SKIP,
    Q_INBOX_TAKE,
    Q_INBOX_DROP,
    Q_DUE,
    Q_QUEUED,
    Q_SPAWN_INFO,
    Q_EXISTS,
    Q_TREE_OF,
    Q_SESSION_BLOCK,
    Q_BY_NAME,
    Q_OUTBOX_PEER,
    Q_FAILURE,
    Q_CHART_IMAGE,
    Q_PEERS,
    Q_SEEN,
    Q_N
};
static const struct {
    const char *sql;
    int count;
    Oid types[16];
} stmts[Q_N] = {
    [Q_ACTIVE] = {"SELECT 1 FROM tinyactors.actors WHERE id = $1 AND status = 'active'",
                  1,
                  {INT8OID}},
    [Q_INBOX_INSERT] =
        {"INSERT INTO tinyactors.inbox (actor, event, data, origin, sendid, session, origintype) "
         "VALUES ($1, $2, $3::jsonb, $4, nullif($5, ''), $6, nullif($7, ''))",
         7,
         {INT8OID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, INT8OID, TEXTOID}},
    /* the key is the sending session's: the actor's id, or an invoked session's */
    /* the key: <sending session>:<step>.<n> — the actor's step (processings so far, rolled
     * back with it) and the effect's index in it: unique, and the same when a rolled-back
     * event is processed again */
    [Q_OUTBOX_INSERT] = {"INSERT INTO tinyactors.outbox (actor, type, target, event, data, sendid, "
                         "session, idempotency_key) VALUES ($1, $2, nullif($3, ''), $4, $5::jsonb, "
                         "$6, $7, $8) ON CONFLICT (idempotency_key) DO NOTHING",
                         8,
                         {INT8OID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, INT8OID, TEXTOID}},
    [Q_ACTOR_INSERT] = {"INSERT INTO tinyactors.actors (id, status, configuration, datamodel, "
                        "output, next_due_at, state, events, sessions, chart, version, name, "
                        "steps) VALUES ($1, $2, $3::text[], $4::jsonb, $5::jsonb, $6, $7, $8, "
                        "$9::bigint[], $10, $11, $12, $14)",
                        14,
                        {INT8OID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, TIMESTAMPTZOID, BYTEAOID,
                         INT8OID, TEXTOID, TEXTOID, INT4OID, TEXTOID, BOOLOID, INT8OID}},
    /* $13: write the datamodel column ($4, NULL with tinyactors.store_datamodel = off);
     * false keeps the last one (a done or failed actor) */
    [Q_ACTOR_UPDATE] = {"UPDATE tinyactors.actors SET status = $2, configuration = $3::text[], "
                        "datamodel = CASE WHEN $13 THEN $4::jsonb ELSE datamodel END, output = "
                        "$5::jsonb, next_due_at = $6, state = $7, events = events + $8, sessions = "
                        "$9::bigint[], steps = $14, failures = 0, last_error = NULL, retry_at = "
                        "NULL, updated_at = now() WHERE id = $1",
                        14,
                        {INT8OID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, TIMESTAMPTZOID, BYTEAOID,
                         INT8OID, TEXTOID, TEXTOID, INT4OID, TEXTOID, BOOLOID, INT8OID}},
/* everything a processing reads, in the statement that locks the actor: its row, the
 * row version of its chart (image_of: is the cached image still that row?) and the
 * I/O processor types (load_processors) */
#define ACTOR_ROW                                                                                  \
    "SELECT a.chart, a.version, a.state, a.status, a.steps, (SELECT c.xmin::text FROM "            \
    "tinyactors.charts c WHERE c.name = a.chart AND c.version = a.version), " PROCESSORS           \
    " FROM tinyactors.actors a WHERE a.id = $1 FOR NO KEY UPDATE OF a"
#define PROCESSORS "ARRAY(SELECT p.type FROM tinyactors.processors p ORDER BY p.type)"
    [Q_LOCK_WAIT] = {ACTOR_ROW, 1, {INT8OID}},
    [Q_LOCK_SKIP] = {ACTOR_ROW " SKIP LOCKED", 1, {INT8OID}},
    /* the oldest $2 events (tinyactors.inbox_batch): a backlog is worked off in bounded
     * processings, the rest stays queued for run() */
    [Q_INBOX_TAKE] = {"DELETE FROM tinyactors.inbox WHERE id IN (SELECT id FROM tinyactors.inbox "
                      "WHERE actor = $1 ORDER BY id LIMIT $2) RETURNING id, event, data::text, "
                      "origin, sendid, kind, session, origintype",
                      2,
                      {INT8OID, INT4OID}},
    [Q_INBOX_DROP] = {"DELETE FROM tinyactors.inbox WHERE actor = $1", 1, {INT8OID}},
    /* run(): both walk an index from its start, so they cost what they find,
     * not what is stored — millions of idle actors are never looked at */
    [Q_DUE] = {"SELECT id FROM tinyactors.actors WHERE status = 'active' AND next_due_at <= $1 "
               "AND (retry_at IS NULL OR retry_at <= $1) ORDER BY next_due_at LIMIT $2 FOR NO "
               "KEY UPDATE SKIP LOCKED",
               2,
               {TIMESTAMPTZOID, INT4OID}},
    /* (actors that are also due were all taken by Q_DUE when it came back
     * short — the only time this runs — so they are left out here) */
    /* distinct actors with queued events, from actor $3 on (a loose scan of the
     * inbox_actor index: one probe per actor, however long its backlog), so one
     * busy or locked actor cannot fill the batch and starve the others */
    [Q_QUEUED] = {"WITH RECURSIVE q(actor) AS ((SELECT actor FROM tinyactors.inbox WHERE actor > "
                  "$3 ORDER BY actor LIMIT 1) UNION ALL SELECT (SELECT i.actor FROM "
                  "tinyactors.inbox i WHERE i.actor > q.actor ORDER BY i.actor LIMIT 1) FROM q "
                  "WHERE q.actor IS NOT NULL) SELECT a.id FROM q JOIN tinyactors.actors a ON a.id "
                  "= q.actor WHERE a.status = 'active' AND (a.next_due_at <= $2) IS NOT TRUE AND "
                  "(a.retry_at IS NULL OR a.retry_at <= $2) LIMIT $1 FOR NO KEY UPDATE OF a SKIP "
                  "LOCKED",
                  3,
                  {INT4OID, TIMESTAMPTZOID, INT8OID}},
    [Q_SPAWN_INFO] = {"SELECT v, nextval('tinyactors.actor_id_seq'), (SELECT c.xmin::text FROM "
                      "tinyactors.charts c WHERE c.name = $1 AND c.version = v), " PROCESSORS
                      " FROM (SELECT max(version) AS v FROM tinyactors.charts WHERE name = $1) m",
                      1,
                      {TEXTOID}},
    [Q_EXISTS] = {"SELECT 1 FROM tinyactors.actors WHERE id = $1", 1, {INT8OID}},
    /* the actor whose session tree holds an invoked session */
    [Q_TREE_OF] = {"SELECT id FROM tinyactors.actors WHERE sessions @> ARRAY[$1::bigint] AND "
                   "status = 'active'",
                   1,
                   {INT8OID}},
    [Q_SESSION_BLOCK] = {"SELECT nextval('tinyactors.session_seq')", 0, {0}},
    [Q_BY_NAME] = {"SELECT id FROM tinyactors.actors WHERE name = $1 AND status = 'active'",
                   1,
                   {TEXTOID}},
    /* an envelope for another system (a store link takes it, docs/protocol.md "Store
     * links"): `peer` is the next hop — the system itself, or the peer whose hello
     * announced that it reaches it */
    [Q_OUTBOX_PEER] = {"INSERT INTO tinyactors.outbox (actor, type, target, event, data, sendid, "
                       "session, idempotency_key, system, peer, envelope) VALUES ($1, 'scxml', "
                       "$2, $3, $4::jsonb, $5, $6, $9, $7, $10, $8::jsonb) ON CONFLICT "
                       "(idempotency_key) DO NOTHING",
                       10,
                       {INT8OID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, INT8OID, TEXTOID, TEXTOID,
                        TEXTOID, TEXTOID}},
    /* run(): a failed processing, recorded outside it; backoff 2^failures s (at most an hour);
     * after tinyactors.max_failures the actor is failed for good */
    [Q_FAILURE] =
        {"UPDATE tinyactors.actors SET failures = failures + 1, last_error = $2, "
         "retry_at = $3 + least(interval '1 hour', interval '1 second' * power(2, "
         "least(failures, 12))), next_due_at = CASE WHEN next_due_at IS NOT NULL THEN "
         "greatest(next_due_at, $3 + least(interval '1 hour', interval '1 second' * "
         "power(2, least(failures, 12)))) END, status = CASE WHEN $4 > 0 AND failures + 1 "
         ">= $4 THEN 'failed' ELSE status END, updated_at = now() WHERE id = $1",
         4,
         {INT8OID, TEXTOID, TIMESTAMPTZOID, INT4OID}},
    [Q_CHART_IMAGE] = {"SELECT image FROM tinyactors.charts WHERE name = $1 AND version = $2",
                       2,
                       {TEXTOID, INT4OID}},
    /* the store links' hellos: a router loads them when a send leaves this database */
    [Q_PEERS] = {"SELECT id, reach FROM tinyactors.peers ORDER BY hello_at DESC, id", 0, {0}},
    /* the dedup window: true (a row) if (origin, key) was not seen */
    [Q_SEEN] = {"INSERT INTO tinyactors.seen (origin, key) VALUES ($1, $2) ON CONFLICT DO NOTHING",
                2,
                {TEXTOID, TEXTOID}},
};
static SPIPlanPtr plans[Q_N];

/* run a kept plan; nulls as for SPI_execute_with_args (' ' or 'n') */
static int exec(int q, Datum *values, const char *nulls, bool read_only, long count) {
    if (!plans[q]) {
        SPIPlanPtr p = SPI_prepare(stmts[q].sql, stmts[q].count, (Oid *)stmts[q].types);
        if (!p) ereport(ERROR, (errmsg("tinyactors: cannot prepare: %s", stmts[q].sql)));
        if (SPI_keepplan(p)) ereport(ERROR, (errmsg("tinyactors: cannot keep a plan")));
        plans[q] = p;
    }
    return SPI_execute_plan(plans[q], values, nulls, read_only, count);
}

static void compile_error(ta_compile_result *result, const char *name) {
    const ta_diagnostic *diagnostic;
    uint32_t n = ta_compile_diagnostics(result, &diagnostic);
    StringInfoData message;
    initStringInfo(&message);
    for (uint32_t i = 0; i < n; i++)
        appendStringInfo(
            &message, "%s%.*s:%u:%u: %.*s", i ? "\n" : "", (int)diagnostic[i].file.length,
            diagnostic[i].file.pointer ? diagnostic[i].file.pointer : "", diagnostic[i].line,
            diagnostic[i].column, (int)diagnostic[i].message.length, diagnostic[i].message.pointer);
    ta_compile_free(result);
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("tinyactors: chart \"%s\" does not compile", name),
                    errdetail("%s", message.data)));
}

/* ---- compile -------------------------------------------------------------------------------- */

/* <invoke src="..."> (and <data>/<script> src) name charts of the definitions
 * table: "worker" or "tinyactors:worker" is the newest version of chart
 * "worker", "worker@3" a given version. Their SCXML is compiled into the
 * image of the chart that names them, so the invoked chart is fixed when the
 * invoking chart is defined (define the invoking chart again to pick up a
 * new version). A src that names no defined chart is a compile error. */
static ta_status resolve_defined(void *context, ta_string_view base, ta_string_view source,
                                 ta_source *out) {
    (void)context;
    (void)base;
    char name[NAMEDATALEN + 16];
    const char *p = source.pointer;
    uint32_t n = source.length;
    if (n > 11 && !memcmp(p, "tinyactors:", 11)) {
        p += 11;
        n -= 11;
    }
    if (!n || n >= sizeof name) return TA_ERROR_INVALID;
    memcpy(name, p, n);
    name[n] = 0;
    int32 version = 0; /* 0: the newest */
    char *at = strrchr(name, '@');
    if (at) {
        *at = 0;
        version = atoi(at + 1);
        if (version <= 0) return TA_ERROR_INVALID;
    }
    Datum a[2] = {CStringGetTextDatum(name), Int32GetDatum(version)};
    Oid t[2] = {TEXTOID, INT4OID};
    if (SPI_execute_with_args("SELECT scxml FROM tinyactors.charts WHERE name = $1 AND "
                              "(version = $2 OR $2 = 0) ORDER BY version DESC LIMIT 1",
                              2, t, a, NULL, true, 1) != SPI_OK_SELECT ||
        SPI_processed != 1)
        return TA_ERROR_INVALID;
    char *text = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
    /* the canonical name keeps one chart per (name, version) in the image */
    char *canon = psprintf("tinyactors:%s%s%s", name, version ? "@" : "",
                           version ? psprintf("%d", version) : "");
    out->name = (ta_string_view){canon, (uint32_t)strlen(canon)};
    out->text = (ta_string_view){text, (uint32_t)strlen(text)};
    return TA_OK;
}

static void *compile_alloc(void *context, size_t size, size_t align) {
    (void)align; /* MAXALIGN */
    return MemoryContextAllocExtended((MemoryContext)context, size ? size : 1,
                                      MCXT_ALLOC_HUGE | MCXT_ALLOC_NO_OOM);
}
static void compile_free(void *context, void *p, size_t size) {
    (void)context;
    (void)size;
    pfree(p);
}

Datum ta_postgres_compile(PG_FUNCTION_ARGS) {
    text *source = PG_GETARG_TEXT_PP(0);
    if (VARSIZE_ANY_EXHDR(source) > (size_t)max_chart_kb * 1024)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("tinyactors: chart is too large: %zu kB (tinyactors.max_chart_bytes)",
                        (size_t)VARSIZE_ANY_EXHDR(source) / 1024)));
    char *name = text_to_cstring(PG_GETARG_TEXT_PP(1));
    ta_source s = {{name, (uint32_t)strlen(name)},
                   {VARDATA_ANY(source), (uint32_t)VARSIZE_ANY_EXHDR(source)}};
    /* the compiler allocates in a context of its own: an error or a cancel
     * (which can also longjmp out of the resolver's SPI calls) frees it all */
    MemoryContext mcx =
        AllocSetContextCreate(CurrentMemoryContext, "tinyactors compile", ALLOCSET_DEFAULT_SIZES);
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.alloc = (ta_allocator){mcx, compile_alloc, compile_free};
    options.resolve = resolve_defined;
    ta_compile_result *result = NULL;
    ta_status cst;
    for (int attempt = 0;; attempt++) {
        /* cancel, statement_timeout, pg_terminate_backend stop a long compile:
         * it returns TA_ERROR_ABORTED and CHECK_FOR_INTERRUPTS acts. Not while
         * interrupts are held off: the flag would stay set. */
        options.abort_flag =
            INTERRUPTS_CAN_BE_PROCESSED() ? (const volatile int *)&InterruptPending : NULL;
        SPI_connect();
        cst = ta_compile_charts(&options, &s, 1, &result);
        SPI_finish();
        if (cst != TA_ERROR_ABORTED || attempt == 3) break;
        ta_compile_free(result);
        result = NULL;
        CHECK_FOR_INTERRUPTS();
        /* still here: an interrupt that does not end the statement; again */
    }
    if (cst != TA_OK) compile_error(result, name);
    ta_string_view image = ta_compile_image(result);
    bytea *out = (bytea *)palloc(VARHDRSZ + image.length);
    SET_VARSIZE(out, VARHDRSZ + image.length);
    memcpy(VARDATA(out), image.pointer, image.length);
    ta_compile_free(result);
    MemoryContextDelete(mcx);
    PG_RETURN_BYTEA_P(out);
}

/* an image view over bytes copied to 8-byte alignment */
static void *aligned_copy(const void *p, size_t n) {
    void *q = palloc(n + 8);
    memcpy(q, p, n);
    return q;
}

Datum ta_postgres_chart_datamodel(PG_FUNCTION_ARGS) {
    bytea *b = PG_GETARG_BYTEA_PP(0);
    ta_image image;
    void *bytes = aligned_copy(VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b));
    if (ta_image_open(bytes, VARSIZE_ANY_EXHDR(b), &image) != TA_OK)
        ereport(ERROR, (errmsg("tinyactors: not a chart image")));
    ta_string_view datamodel = ta_image_chart_datamodel(&image, 0);
    PG_RETURN_TEXT_P(cstring_to_text_with_len(datamodel.pointer ? datamodel.pointer : "",
                                              (int)datamodel.length));
}

/* ---- chart images (cached per backend) ---------------------------------------------------------
 * A (name, version) row is immutable while it exists, but a rolled-back
 * define, DELETE + define, or DROP/CREATE EXTENSION can put a different row
 * under the same key: entries carry the row's xmin and are checked on every
 * use (one index lookup), so a stale image is never served. */
typedef struct chart_image {
    char name[NAMEDATALEN];
    int32 version;
    char xmin[16]; /* the row version the image came from */
    void *bytes;
    size_t length;
    ta_image image;
    bool invokes; /* has charts besides the main one: sessions may invoke sessions */
} chart_image;
static chart_image cache[64];
static int cache_n, cache_next;

/* the image of a chart version; xmin: its row version, read with the actor's row */
static chart_image *image_of(const char *name, int32 version, const char *xmin) {
    if (!xmin) ereport(ERROR, (errmsg("tinyactors: no chart %s version %d", name, version)));
    for (int i = 0; i < cache_n; i++)
        if (cache[i].bytes && cache[i].version == version && !strcmp(cache[i].name, name)) {
            if (!strcmp(cache[i].xmin, xmin)) return &cache[i];
            free(cache[i].bytes); /* another row now: drop the stale image */
            cache[i].bytes = NULL;
        }
    Datum args[2] = {CStringGetTextDatum(name), Int32GetDatum(version)};
    if (exec(Q_CHART_IMAGE, args, NULL, true, 1) != SPI_OK_SELECT || SPI_processed != 1)
        ereport(ERROR, (errmsg("tinyactors: no chart %s version %d", name, version)));
    bool isnull;
    bytea *b =
        DatumGetByteaPP(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
    /* build the entry completely before it is visible in the cache */
    size_t length = VARSIZE_ANY_EXHDR(b);
    void *bytes = malloc(length + 8);
    if (!bytes) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("out of memory")));
    memcpy(bytes, VARDATA_ANY(b), length);
    ta_image image;
    if (ta_image_open(bytes, length, &image) != TA_OK) {
        free(bytes);
        ereport(ERROR,
                (errmsg("tinyactors: chart %s version %d is not a valid image", name, version)));
    }
    int slot = -1;
    for (int i = 0; i < cache_n && slot < 0; i++)
        if (!cache[i].bytes) slot = i;
    if (slot < 0) slot = cache_n < 64 ? cache_n++ : (cache_next++ % 64);
    chart_image *c = &cache[slot];
    if (c->bytes) free(c->bytes);
    c->bytes = bytes;
    c->length = length;
    c->image = image;
    snprintf(c->name, sizeof c->name, "%s", name);
    snprintf(c->xmin, sizeof c->xmin, "%s", xmin);
    c->version = version;
    ta_image_info image_info;
    image_info.size = sizeof image_info;
    ta_image_get_info(&c->image, &image_info);
    c->invokes = image_info.chart_count > 1;
    return c;
}

/* ---- the datamodel (per backend) ---------------------------------------------------------------
 */
static ta_datamodel_ecmascript *ecmascript;
static ta_datamodel_binding binding;

StaticAssertDecl(sizeof(sig_atomic_t) == sizeof(int), "InterruptPending is read as an int");

static void datamodel_open(void) {
    if (ecmascript) return;
    ta_datamodel_ecmascript_config c;
    memset(&c, 0, sizeof c);
    c.size = sizeof c;
    /* one evaluation: about 10^8 operations at most (every operation also
     * costs fuel, so tinyactors.fuel bounds a whole processing) */
    c.max_ops = 10000;
    /* cancel, statement_timeout, pg_terminate_backend stop JavaScript too: the
     * evaluation fails, the system returns, CHECK_FOR_INTERRUPTS acts */
    c.abort_flag = (const volatile int *)&InterruptPending;
    ecmascript = ta_datamodel_ecmascript_new(&c);
    if (!ecmascript)
        ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("tinyactors: no datamodel")));
    binding = ta_datamodel_ecmascript_binding(ecmascript);
}
static void datamodel_drop(void) { /* after an error: sessions may be left half-processed */
    if (ecmascript) ta_datamodel_ecmascript_free(ecmascript);
    ecmascript = NULL;
}

/* JSON text for a jsonb column: escapes jsonb cannot hold — \u0000 and lone
 * UTF-16 surrogates, both legal JSON that JavaScript produces — become
 * \ufffd (in place: the same length), so storing never fails on them */
static bool hexq(const char *p, int lo, int hi) { /* \uXXXX value in [lo, hi]? */
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9'   ? c - '0'
                : c >= 'a' && c <= 'f' ? c - 'a' + 10
                : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                       : -1;
        if (d < 0) return false;
        v = v * 16 + d;
    }
    return v >= lo && v <= hi;
}
static char *jsonb_safe(char *s) {
    if (!s) return s;
    for (char *p = s; *p; p++) {
        if (*p != '\\') continue;
        if (p[1] != 'u') { /* another escape: skip the escaped character */
            if (p[1]) p++;
            continue;
        }
        char *u = p + 2;
        if (strlen(u) < 4) break; /* not JSON */
        if (hexq(u, 0xD800, 0xDBFF) && u[4] == '\\' && u[5] == 'u' && strlen(u + 6) >= 4 &&
            hexq(u + 6, 0xDC00, 0xDFFF)) {
            p = u + 9; /* a surrogate pair: fine, skip both halves */
            continue;
        }
        if (hexq(u, 0, 0) || hexq(u, 0xD800, 0xDFFF)) memcpy(u, "fffd", 4);
        p = u + 3;
    }
    return s;
}

/* How deep JSON text nests ([ and { outside strings). Event data deeper than
 * MAX_NESTING is refused at the boundary: JavaScript could not store it back
 * (and a stored _event that cannot be loaded would brick the actor). */
#define MAX_NESTING 100
static int json_depth(const char *s) {
    int d = 0, max = 0;
    bool str = false;
    for (; *s; s++) {
        if (str) {
            if (*s == '\\' && s[1])
                s++;
            else if (*s == '"')
                str = false;
        } else if (*s == '"') {
            str = true;
        } else if (*s == '[' || *s == '{') {
            if (++d > max) max = d;
        } else if (*s == ']' || *s == '}') {
            d--;
        }
    }
    return max;
}
static void check_nesting(const char *json) {
    if (json && json_depth(json) > MAX_NESTING)
        ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("tinyactors: event data nested deeper than %d levels", MAX_NESTING)));
}

/* event data as a neutral wire value (docs/protocol.md, "Values"): JSON plus
 * {"$bytes"}, {"$ref"}, {"$"}; palloc'd, or NULL (no data, or not representable) */
static char *payload_wire(ta_payload payload) {
    if (!payload) return NULL;
    const ta_datamodel_vtable *vtable = ta_datamodel_ecmascript_vtable();
    char small[1024];
    size_t n = 0;
    ta_status status = vtable->to_wire(ecmascript, payload, small, sizeof small, &n);
    if (status == TA_OK) return jsonb_safe(pnstrdup(small, n));
    if (status != TA_ERROR_NO_MEMORY) {
        ereport(WARNING, (errmsg("tinyactors: event data that is not a neutral value (a cycle?) "
                                 "was sent as null")));
        return NULL;
    }
    char *big = palloc(n + 1);
    if (vtable->to_wire(ecmascript, payload, big, n, &n) != TA_OK) return NULL;
    big[n] = 0;
    return jsonb_safe(big);
}

static ta_payload payload_from_wire(const char *text) {
    ta_payload payload = 0;
    check_nesting(text);
    if (ta_datamodel_ecmascript_vtable()->from_wire(
            ecmascript, (ta_string_view){text, (uint32_t)strlen(text)}, &payload) != TA_OK)
        payload = ta_datamodel_ecmascript_payload_from_json(
            ecmascript, (ta_string_view){text, (uint32_t)strlen(text)});
    return payload;
}

static char *payload_json(ta_payload payload) { /* palloc'd, or NULL */
    if (!payload) return NULL;
    char small[1024];
    size_t n = ta_datamodel_ecmascript_payload_to_json(ecmascript, payload, small, sizeof small);
    if (n < sizeof small) return jsonb_safe(pstrdup(small));
    char *big = palloc(n + 1);
    ta_datamodel_ecmascript_payload_to_json(ecmascript, payload, big, n + 1);
    return jsonb_safe(big);
}

/* ---- one actor's system
 * -------------------------------------------------------------------------- */
typedef struct run {
    chart_image *chart_image;
    ta_system *system;
    void *memory;
    ta_system_config config;
    ta_io_processor_descriptor
        *io_processors; /* [0] our base, then tinyactors.processors (any number) */
    char **types;
    int io_processor_count;
    uint32_t actor; /* handle */
    int64 id;       /* session */
    bool done, failed;
    int64 block;  /* first session id of the block reserved for invoked sessions (0: none) */
    char *output; /* donedata JSON */
    char *final_state;
    int64 *targets; /* actors that got events, in order */
    int target_count, target_capacity;
    int64 *seen; /* open-addressing set over targets (0 = empty; ids are > 0) */
    int seen_capacity;
    MemoryContext memory_context; /* where targets live (NULL: the current context) */
    int sends;                    /* effects of this processing, counted for idempotency keys */
    int64 events;                 /* events delivered in this call */
    int64 step; /* this processing's number for the actor (its steps column + 1) */
    int logs;   /* <log> lines of this processing (tinyactors.log_limit) */
} run;

#define LOG_LINE_MAX 1000 /* bytes of one <log> message in the server log */

static bool seen_insert(int64 *set, int capacity, int64 id) { /* false if present */
    uint64 h = (uint64)id * 0x9E3779B97F4A7C15ull;
    for (int i = (int)(h >> 40) & (capacity - 1);; i = (i + 1) & (capacity - 1)) {
        if (set[i] == id) return false;
        if (!set[i]) {
            set[i] = id;
            return true;
        }
    }
}

/* remember an actor that got events (once; an actor may send to any number) */
static void add_target(run *r, int64 id) {
    MemoryContext memory_context = r->memory_context ? r->memory_context : CurrentMemoryContext;
    if (2 * (r->target_count + 1) > r->seen_capacity) { /* keep the set at most half full */
        int capacity = r->seen_capacity ? r->seen_capacity * 2 : 16;
        int64 *set = MemoryContextAllocZero(memory_context, sizeof(int64) * (size_t)capacity);
        for (int i = 0; i < r->target_count; i++)
            seen_insert(set, capacity, r->targets[i]);
        if (r->seen) pfree(r->seen);
        r->seen = set;
        r->seen_capacity = capacity;
    }
    if (!seen_insert(r->seen, r->seen_capacity, id)) return;
    if (r->target_count == r->target_capacity) {
        r->target_capacity = r->target_capacity ? r->target_capacity * 2 : 8;
        r->targets = r->targets ? repalloc(r->targets, sizeof(int64) * (size_t)r->target_capacity)
                                : MemoryContextAlloc(memory_context,
                                                     sizeof(int64) * (size_t)r->target_capacity);
    }
    r->targets[r->target_count++] = id;
}

/* the I/O processors: [0] our base (SCXML between actors), then tinyactors.processors
 * (`types`: their text[], read with the actor's row; isnull: none) */
static void load_processors(run *r, Datum types, bool isnull) {
    Datum *values = NULL;
    bool *nulls;
    int n = 0;
    if (!isnull)
        deconstruct_array(DatumGetArrayTypeP(types), TEXTOID, -1, false, TYPALIGN_INT, &values,
                          &nulls, &n);
    r->io_processors = palloc0(sizeof(ta_io_processor_descriptor) * (size_t)(n + 1));
    r->types = palloc0(sizeof(char *) * (size_t)(n + 1));
    r->io_processors[0].type =
        (ta_string_view){"http://www.w3.org/TR/scxml/#SCXMLEventProcessor", 48};
    r->io_processors[0].location = (ta_string_view){base(), (uint32_t)strlen(base())};
    r->io_processors[0].flags = TA_IO_PROCESSOR_SCXML_REMOTE | TA_IO_PROCESSOR_SCXML_SHARED;
    r->io_processor_count = 1;
    for (int i = 0; i < n; i++) {
        char *t = TextDatumGetCString(values[i]); /* a primary key: never NULL */
        r->types[r->io_processor_count] =
            t; /* palloc'd in the procedure's context: lives for the call */
        r->io_processors[r->io_processor_count].type = (ta_string_view){t, (uint32_t)strlen(t)};
        r->io_processors[r->io_processor_count].location = (ta_string_view){"", 0};
        r->io_processor_count++;
    }
}

static void system_open(run *r, chart_image *ci, uint64_t first_serial) {
    datamodel_open();
    ta_datamodel_ecmascript_set_memory_limit(ecmascript, (size_t)js_memory_kb *
                                                             1024); /* this session's setting */
    r->chart_image = ci;
    memset(&r->config, 0, sizeof r->config);
    r->config.size = sizeof r->config;
    r->config.max_actors = 8;
    r->config.max_events = 128;
    r->config.max_timers = 32;
    r->config.string_bytes = 16384;
    r->config.ticks_per_second = 1000; /* epoch ms */
    r->config.quantum = 100000;
    r->config.first_serial = first_serial;
    r->config.io_processors = r->io_processors;
    r->config.io_processor_count = (uint32_t)r->io_processor_count;
    size_t need;
    if (ta_system_layout(&ci->image, &r->config, &need) != TA_OK)
        ereport(ERROR, (errmsg("tinyactors: cannot lay out a system for %s", ci->name)));
    r->memory = palloc(need);
    if (ta_system_init(r->memory, need, &ci->image, &r->config, &binding, 1, &r->system) != TA_OK)
        ereport(ERROR, (errmsg("tinyactors: cannot start a system for %s", ci->name)));
    ta_system_advance_time(r->system, to_ms(clock_now()));
}

/* the system ran out of a pool: move it into a bigger region */
static void system_grow(run *r) {
    ta_system_config config = r->config;
    config.max_actors *= 2;
    config.max_events *= 2;
    config.max_timers *= 2;
    config.string_bytes *= 2;
    config.instance_bytes = 0;
    size_t need;
    ta_system *new_system;
    if (ta_system_layout(&r->chart_image->image, &config, &need) != TA_OK)
        ereport(ERROR, (errmsg("tinyactors: cannot grow")));
    if (need > (size_t)max_system_kb * 1024)
        ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                        errmsg("tinyactors: actor " INT64_FORMAT " is too large: its system needs "
                               "%zu kB (tinyactors.max_system_bytes)",
                               r->id, need / 1024),
                        errhint("Endless <invoke>s or queued events? The transaction is rolled "
                                "back.")));
    void *memory = palloc(need);
    if (ta_system_relocate(r->system, memory, need, &config, &new_system) != TA_OK)
        ereport(ERROR, (errmsg("tinyactors: cannot grow")));
    r->system = new_system;
    r->memory = memory;
    r->config = config;
}

/* Invoked sessions get ids from tinyactors.session_seq (from 2^62: never an
 * actor id), a block of SESSION_BLOCK per processing of a chart that can
 * invoke; the block's first id is r->block. */
#define SESSION_BLOCK ((int64)1 << 20)
static void reserve_sessions(run *r) {
    if (!r->chart_image->invokes) return;
    bool isnull;
    if (exec(Q_SESSION_BLOCK, NULL, NULL, false, 1) != SPI_OK_SELECT || SPI_processed != 1)
        ereport(ERROR, (errmsg("tinyactors: cannot reserve session ids")));
    r->block =
        DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
    if (ta_system_set_next_serial(r->system, (uint64_t)r->block - 1) != TA_OK)
        ereport(ERROR, (errmsg("tinyactors: session ids out of order")));
}

/* the session ids of the actor's invoked sessions, as a bigint[] literal (NULL: none) */
static char *tree_sessions(run *r) {
    StringInfoData b;
    uint32_t cursor = 0, h[64], n;
    int k = 0;
    initStringInfo(&b);
    appendStringInfoChar(&b, '{');
    while ((n = ta_system_actors(r->system, &cursor, h, 64)) > 0)
        for (uint32_t i = 0; i < n; i++) {
            ta_actor_info actor_info;
            actor_info.size = sizeof actor_info;
            if (h[i] == r->actor || ta_actor_info_get(r->system, h[i], &actor_info) != TA_OK)
                continue;
            appendStringInfo(&b, "%s" UINT64_FORMAT, k++ ? "," : "", actor_info.serial);
        }
    appendStringInfoChar(&b, '}');
    return k ? b.data : NULL;
}

static void insert_inbox(int64 actor, const char *event, const char *data, const char *origin,
                         const char *sendid, int64 session, const char *origintype) {
    Datum a[7] = {Int64GetDatum(actor),
                  CStringGetTextDatum(event),
                  data ? CStringGetTextDatum(data) : (Datum)0,
                  origin ? CStringGetTextDatum(origin) : (Datum)0,
                  CStringGetTextDatum(sendid ? sendid : ""),
                  Int64GetDatum(session),
                  CStringGetTextDatum(origintype ? origintype : "")};
    char nulls[7] = {' ', ' ', data ? ' ' : 'n', origin ? ' ' : 'n', ' ', session ? ' ' : 'n', ' '};
    exec(Q_INBOX_INSERT, a, nulls, false, 0);
}

/* the active actor whose session tree holds invoked session `id` (0: none) */
static int64 tree_of(int64 id) {
    Datum a[1] = {Int64GetDatum(id)};
    bool isnull;
    if (exec(Q_TREE_OF, a, NULL, true, 1) != SPI_OK_SELECT || SPI_processed != 1) return 0;
    return DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
}

static bool actor_active(int64 id) {
    Datum a[1] = {Int64GetDatum(id)};
    return exec(Q_ACTIVE, a, NULL, true, 1) == SPI_OK_SELECT && SPI_processed == 1;
}

static ta_string_view str(const char *s) {
    return (ta_string_view){s ? s : "", s ? (uint32_t)strlen(s) : 0};
}
static char *c_string(ta_string_view s) {
    return pnstrdup(s.pointer ? s.pointer : "", s.length);
}
static Datum textdatum(ta_string_view s) {
    return PointerGetDatum(cstring_to_text_with_len(s.pointer ? s.pointer : "", (int)s.length));
}
static void host_check(ta_status status) {
    if (status != TA_OK) ereport(ERROR, (errmsg("tinyactors: router failed (%d)", status)));
}
static void router_free(void *arg) {
    ta_host_free((ta_host *)arg);
}

/* Transaction-local routing metadata, never actor segments. The context callback
 * also runs on ERROR/rollback, including errors from user table triggers. */
static ta_host *router_open(void) {
    ta_host_config config = {0};
    config.size = sizeof config;
    config.system_id = system_id();
    ta_host *host = NULL;
    host_check(ta_host_new(&config, &host));
    MemoryContextCallback *callback = palloc(sizeof *callback);
    callback->func = router_free;
    callback->arg = host;
    MemoryContextRegisterResetCallback(CurrentMemoryContext, callback);
    return host;
}

/* The peers table into the router: only when the router asks (TA_ROUTE_PEERS),
 * or for an envelope from a peer (`from`: its handle in *handle). */
static void router_peers(ta_host *host, const char *from, uint32_t *handle) {
    if (handle) *handle = 0;
    exec(Q_PEERS, NULL, NULL, true, 0);
    for (uint64 i = 0; i < SPI_processed; i++) {
        bool nil;
        HeapTuple row = SPI_tuptable->vals[i];
        char *id = SPI_getvalue(row, SPI_tuptable->tupdesc, 1);
        Datum arr = SPI_getbinval(row, SPI_tuptable->tupdesc, 2, &nil);
        Datum *values;
        bool *nulls;
        int n;
        deconstruct_array(DatumGetArrayTypeP(arr), TEXTOID, -1, false, TYPALIGN_INT, &values,
                          &nulls, &n);
        ta_string_view *reach = palloc(sizeof *reach * (size_t)n);
        uint32_t count = 0;
        for (int k = 0; k < n; k++)
            if (!nulls[k]) reach[count++] = str(TextDatumGetCString(values[k]));
        ta_peer_config peer_config = {0};
        peer_config.size = sizeof peer_config;
        peer_config.id = str(id);
        peer_config.flags = TA_PEER_TRUSTED; /* SQL grant_peer authenticates the store link */
        peer_config.reach = reach;
        peer_config.reach_count = count;
        uint32_t peer;
        host_check(ta_host_peer_add(host, &peer_config, &peer));
        if (from && !strcmp(from, id)) *handle = peer;
    }
    if (from && !*handle) {
        ta_peer_config peer_config = {0};
        peer_config.size = sizeof peer_config;
        peer_config.id = str(from);
        peer_config.flags = TA_PEER_TRUSTED;
        host_check(ta_host_peer_add(host, &peer_config, handle));
    }
}

/* Answer only storage requests. The host chooses lookup vs dedup vs delivery.
 * Return the root row that owns the session; no row locks are taken here. */
static int64 route_storage(ta_route_request *request, run *r) {
    int64 root = 0;
    if (request->action == TA_ROUTE_LOOKUP) {
        int64 session = request->session <= INT64_MAX ? (int64)request->session : 0;
        if (request->name.length) {
            char *name = palloc(request->name.length);
            uint32_t length = ta_address_name_decode(request->name, name, request->name.length);
            Datum a[] = {textdatum((ta_string_view){name, length})};
            bool nil;
            if (exec(Q_BY_NAME, a, NULL, true, 1) == SPI_OK_SELECT && SPI_processed)
                session = DatumGetInt64(
                    SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &nil));
        }
        if (session) {
            if (r && ta_system_actor_by_session(r->system, (uint64_t)session))
                root = r->id;
            else if (actor_active(session))
                root = session;
            else
                root = tree_of(session);
        }
        host_check(ta_host_route_step(request, root ? (uint64_t)session : 0));
    }
    if (request->action == TA_ROUTE_DEDUPLICATE) {
        Datum a[] = {textdatum(request->origin), textdatum(request->key)};
        exec(Q_SEEN, a, NULL, false, 0);
        host_check(ta_host_route_step(request, SPI_processed != 0));
    }
    return root;
}

/* route the effects of a run; returns how many events were injected back
 * (error.communication for sends nobody can take) */
static int drain_effects(run *r) {
    const ta_effect *effect;
    uint32_t n = ta_system_effects(r->system, &effect);
    int injected = 0;
    ta_host *host = NULL;
    bool peers = false; /* loaded into host */
    for (uint32_t i = 0; i < n; i++) {
        const ta_effect *effect_view = &effect[i];
        /* the sending session: the actor, or a session it invoked */
        int64 from = effect_view->session ? (int64)effect_view->session : r->id;
        switch (effect_view->kind) {
        case TA_EFFECT_LOG: { /* "tinyactors <id>: [label: ]message", capped per processing */
            if (log_level < 0) break;
            if (++r->logs > log_limit) break; /* counted: one note at the end */
            int ml = effect_view->event.length > LOG_LINE_MAX ? LOG_LINE_MAX
                                                              : (int)effect_view->event.length;
            const char *more = effect_view->event.length > LOG_LINE_MAX ? " [...]" : "";
            if (effect_view->auxiliary.length)
                ereport(
                    log_level,
                    (errmsg("tinyactors " INT64_FORMAT ": %.*s: %.*s%s", r->id,
                            (int)(effect_view->auxiliary.length > 200
                                      ? 200
                                      : effect_view->auxiliary.length),
                            effect_view->auxiliary.pointer, ml,
                            effect_view->event.pointer ? effect_view->event.pointer : "", more)));
            else
                ereport(
                    log_level,
                    (errmsg("tinyactors " INT64_FORMAT ": %.*s%s", r->id, ml,
                            effect_view->event.pointer ? effect_view->event.pointer : "", more)));
            break;
        }
        case TA_EFFECT_DONE:
            r->done = true;
            r->output = payload_json(effect_view->payload);
            r->final_state =
                pnstrdup(effect_view->auxiliary.pointer ? effect_view->auxiliary.pointer : "",
                         effect_view->auxiliary.length);
            break;
        case TA_EFFECT_ERROR: /* a session faulted: the actor fails; an invoked one just ends */
            if (from == r->id)
                r->failed = true;
            else
                ereport(WARNING, (errmsg("tinyactors " INT64_FORMAT
                                         ": invoked session " INT64_FORMAT " faulted",
                                         r->id, from)));
            break;
        case TA_EFFECT_REQUEST: { /* <invoke srcexpr>: a defined chart compiled into the image */
            char key[NAMEDATALEN + 32];
            const char *p = effect_view->auxiliary.pointer ? effect_view->auxiliary.pointer : "";
            uint32_t pl = effect_view->auxiliary.length;
            if (pl > 11 && !memcmp(p, "tinyactors:", 11)) {
                p += 11;
                pl -= 11;
            }
            snprintf(key, sizeof key, "tinyactors:%.*s", (int)pl, p);
            uint32_t c = ta_image_chart_by_name(&r->chart_image->image,
                                                (ta_string_view){key, (uint32_t)strlen(key)});
            if (c == TA_NIL)
                ta_system_resume(r->system, effect_view->token, TA_ERROR_INVALID,
                                 0); /* error.execution */
            else
                ta_system_resume(r->system, effect_view->token, TA_OK, (ta_payload)c + 1);
            injected++;
            break;
        }
        case TA_EFFECT_SEND: {
            char *event = pnstrdup(effect_view->event.pointer ? effect_view->event.pointer : "",
                                   effect_view->event.length);
            char *data = payload_wire(effect_view->payload);
            if (!host) host = router_open();
            ta_host_message message;
            message.size = sizeof message;
            host_check(ta_host_message_init(host, effect_view, (uint64_t)r->step,
                                            (uint32_t)r->sends++, &message));
            ta_envelope *envelope = &message.envelope;
            char *sendid = c_string(message.sendid);
            char *key = c_string(message.key);
            if (effect_view->io_processor == 0) {
                ta_route_request request;
                request.size = sizeof request;
                host_check(ta_host_route_begin(host, 0, envelope,
                                               TA_ROUTE_DURABLE | (peers ? 0 : TA_ROUTE_ASK_PEERS),
                                               &request));
                if (request.action == TA_ROUTE_PEERS) { /* to another system: through which peer? */
                    router_peers(host, NULL, NULL);
                    peers = true;
                    host_check(ta_host_route_begin(host, 0, envelope, TA_ROUTE_DURABLE, &request));
                }
                int64 root = route_storage(&request, r);
                if (request.action == TA_ROUTE_LOCAL && root != r->id) {
                    insert_inbox(root, event, data, c_string(envelope->origin), sendid,
                                 (uint64_t)root != request.session ? (int64)request.session : 0,
                                 c_string(envelope->origintype));
                    add_target(r, root);
                } else if (request.action == TA_ROUTE_LOCAL || request.action == TA_ROUTE_REFUSED) {
                    ta_event_in event_in;
                    ta_host_error_event(envelope->sendid, &event_in);
                    if (request.action == TA_ROUTE_LOCAL) {
                        event_in.flags = 0;
                        event_in.name = envelope->name;
                        event_in.origin = envelope->origin;
                        event_in.origintype = envelope->origintype;
                        event_in.data = effect_view->payload;
                        if (event_in.data)
                            ta_datamodel_ecmascript_vtable()->retain(ecmascript, event_in.data);
                    }
                    event_in.target = ta_system_actor_by_session(
                        r->system,
                        request.action == TA_ROUTE_LOCAL ? request.session : (uint64_t)from);
                    if (event_in.target) {
                        host_check(ta_system_send(r->system, &event_in));
                        injected++;
                    }
                } else if (request.action == TA_ROUTE_PEER) {
                    envelope->hops = request.hops;
                    size_t length = ta_host_envelope_json(envelope, str(data), NULL, 0);
                    char *json = palloc(length + 1);
                    ta_host_envelope_json(envelope, str(data), json, length + 1);
                    Datum a[10] = {
                        Int64GetDatum(r->id),        textdatum(envelope->target),
                        textdatum(envelope->name),   data ? CStringGetTextDatum(data) : (Datum)0,
                        CStringGetTextDatum(sendid), Int64GetDatum(from),
                        textdatum(request.system),   CStringGetTextDatum(json),
                        CStringGetTextDatum(key),    textdatum(request.peer_id)};
                    char nulls[10] = {
                        ' ', ' ', ' ', data ? ' ' : 'n', ' ', from != r->id ? ' ' : 'n', ' ',
                        ' ', ' ', ' '};
                    exec(Q_OUTBOX_PEER, a, nulls, false, 0);
                }
            } else { /* an I/O processor of the application: the outbox */
                const char *type = effect_view->io_processor < (uint32_t)r->io_processor_count
                                       ? r->types[effect_view->io_processor]
                                       : "unknown";
                char *target =
                    pnstrdup(effect_view->target.pointer ? effect_view->target.pointer : "",
                             effect_view->target.length);
                Datum a[8] = {Int64GetDatum(r->id),
                              CStringGetTextDatum(type),
                              CStringGetTextDatum(target),
                              CStringGetTextDatum(event),
                              data ? CStringGetTextDatum(data) : (Datum)0,
                              CStringGetTextDatum(sendid),
                              Int64GetDatum(from),
                              CStringGetTextDatum(key)};
                char nulls[8] = {
                    ' ', ' ', ' ', ' ', data ? ' ' : 'n', ' ', from != r->id ? ' ' : 'n', ' '};
                exec(Q_OUTBOX_INSERT, a, nulls, false, 0);
            }
            break;
        }
        default:
            break;
        }
    }
    ta_system_effects_ack(r->system, n);
    return injected;
}

/* run to a stable point within the fuel budget */
static void run_actor(run *r) {
    uint64 used = 0;
    for (int round = 0; round < 100000; round++) {
        CHECK_FOR_INTERRUPTS();
        ta_run s = ta_system_run(r->system, 100000);
        CHECK_FOR_INTERRUPTS(); /* JavaScript stopped for a cancel: act on it now */
        used += s.used;
        int injected = drain_effects(r);
        if (s.status == TA_RUN_EXHAUSTED && used <= (uint64)fuel_per_actor) {
            system_grow(r);
            continue;
        }
        if (s.status == TA_RUN_FAULT)
            ereport(ERROR, (errmsg("tinyactors: actor " INT64_FORMAT " faulted", r->id)));
        if (s.status == TA_RUN_IDLE && !injected) {
            if (r->logs > log_limit && log_level >= 0)
                ereport(log_level, (errmsg("tinyactors " INT64_FORMAT ": %d more log lines "
                                           "suppressed (tinyactors.log_limit)",
                                           r->id, r->logs - log_limit)));
            return;
        }
        if (used > (uint64)fuel_per_actor) /* also a system that keeps growing */
            ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                            errmsg("tinyactors: actor " INT64_FORMAT " used more than %d fuel",
                                   r->id, fuel_per_actor),
                            errhint("An eventless loop? Raise tinyactors.fuel if the work is real; "
                                    "this transaction is rolled back.")));
    }
}

/* one element of a text[] literal: quoted, with " and \ escaped */
static void append_elem(StringInfo b, const char *p, size_t n) {
    appendStringInfoChar(b, '"');
    for (size_t i = 0; i < n; i++) {
        if (p[i] == '"' || p[i] == '\\') appendStringInfoChar(b, '\\');
        appendStringInfoChar(b, p[i]);
    }
    appendStringInfoChar(b, '"');
}

/* configuration (atomic states) and datamodel as text, for the row (datamodel
 * NULL with tinyactors.store_datamodel = off) */
static void describe(run *r, char **config, char **datamodel) {
    ta_actor_info actor_info;
    actor_info.size = sizeof actor_info;
    StringInfoData c, d;
    initStringInfo(&c);
    initStringInfo(&d);
    appendStringInfoChar(&c, '{');
    appendStringInfoChar(&d, '{');
    if (ta_actor_info_get(r->system, r->actor, &actor_info) == TA_OK) {
        uint32_t small[256], *states = small;
        uint32_t n = ta_actor_config(r->system, r->actor, states, 256), k = 0;
        if (n > 256) { /* a big parallel chart: ask again with room for all */
            states = palloc(sizeof(uint32_t) * n);
            n = ta_actor_config(r->system, r->actor, states, n);
        }
        for (uint32_t i = 0; i < n; i++) {
            ta_state_info state_info;
            if (ta_image_state_info(&r->chart_image->image, actor_info.chart, states[i],
                                    &state_info) != TA_OK ||
                (state_info.kind != TA_STATE_ATOMIC && state_info.kind != TA_STATE_FINAL))
                continue;
            if (k++) appendStringInfoChar(&c, ',');
            append_elem(&c, state_info.id.pointer, state_info.id.length);
        }
        uint32_t nd =
            store_datamodel ? ta_image_data_count(&r->chart_image->image, actor_info.chart) : 0;
        for (uint32_t i = 0, k2 = 0; i < nd; i++) {
            ta_data_info data_info;
            if (ta_image_data(&r->chart_image->image, actor_info.chart, i, &data_info) != TA_OK)
                continue;
            char buffer[8192];
            size_t length = ta_datamodel_ecmascript_eval_json(
                ecmascript, actor_info.datamodel_instance, data_info.id, buffer, sizeof buffer);
            char *value = buffer;
            if (length == (size_t)-1 || length == 0) {
                value = "null";
            } else if (length >= sizeof buffer) {
                value = palloc(length + 1);
                ta_datamodel_ecmascript_eval_json(ecmascript, actor_info.datamodel_instance,
                                                  data_info.id, value, length + 1);
            }
            if (value != buffer && strcmp(value, "null"))
                jsonb_safe(value);
            else if (value == buffer)
                jsonb_safe(buffer);
            appendStringInfo(&d, "%s\"%.*s\":%s", k2++ ? "," : "", (int)data_info.id.length,
                             data_info.id.pointer, value);
        }
    }
    appendStringInfoChar(&c, '}');
    appendStringInfoChar(&d, '}');
    *config = c.data;
    *datamodel = store_datamodel ? d.data : NULL;
}

/* write the actor back: exported state + what it looks like now */
static void persist(run *r, bool insert, const char *name, const char *chart, int32 version) {
    char *config = NULL, *datamodel = NULL;
    bytea *state = NULL;
    TimestampTz due = 0;
    bool has_due = false;
    char *sessions = NULL;
    if (!r->done && !r->failed) {
        describe(r, &config, &datamodel);
        sessions = tree_sessions(r); /* before the export takes them out */
        if (r->block) {              /* invoked sessions stayed in their block of ids */
            ta_system_stats stats;
            stats.size = sizeof stats;
            ta_system_stats_get(r->system, &stats);
            if ((int64)stats.spawned >= r->block + SESSION_BLOCK)
                ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                                errmsg("tinyactors: actor " INT64_FORMAT
                                       " invoked more than %lld sessions in one processing",
                                       r->id, (long long)SESSION_BLOCK)));
        }
        ta_time nd = ta_system_next_deadline(r->system);
        if (nd != UINT64_MAX) {
            due = from_ms(nd);
            has_due = true;
        }
        size_t length = 0;
        ta_status states = ta_actor_export(r->system, r->actor, NULL, 0, &length);
        if (states == TA_ERROR_NO_MEMORY && length) {
            state = (bytea *)palloc(VARHDRSZ + length);
            states = ta_actor_export(r->system, r->actor, VARDATA(state), length, &length);
            SET_VARSIZE(state, VARHDRSZ + length);
        }
        if (states == TA_OK &&
            (uint64)length + (datamodel ? strlen(datamodel) : 0) > (uint64)max_state_kb * 1024)
            ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                            errmsg("tinyactors: actor " INT64_FORMAT " is too large to store: "
                                   "%zu kB of state (tinyactors.max_state_bytes)",
                                   r->id, (length + (datamodel ? strlen(datamodel) : 0)) / 1024),
                            errhint("The transaction is rolled back; keep big data in tables of "
                                    "your own and its keys in the datamodel.")));
        if (states != TA_OK)
            ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                            errmsg("tinyactors: actor " INT64_FORMAT " cannot be stored: %s", r->id,
                                   states != TA_ERROR_UNSUPPORTED
                                       ? "a session of it is not at a stable point"
                                   : ecmascript && ta_datamodel_ecmascript_last_error(ecmascript)[0]
                                       ? ta_datamodel_ecmascript_last_error(
                                             ecmascript) /* names the variable */
                                       : "its datamodel holds values that cannot be stored"),
                            errdetail("Actors are stored between events at stable points, with the "
                                      "sessions they invoked.")));
    } else {
        StringInfoData c;
        initStringInfo(&c);
        appendStringInfoChar(&c, '{');
        append_elem(&c, r->final_state ? r->final_state : "",
                    r->final_state ? strlen(r->final_state) : 0);
        appendStringInfoChar(&c, '}');
        config = c.data;
    }
    const char *status = r->failed ? "failed" : r->done ? "done" : "active";
    bool active = !r->done && !r->failed;
    Datum a[14] = {Int64GetDatum(r->id),
                   CStringGetTextDatum(status),
                   CStringGetTextDatum(config),
                   datamodel ? CStringGetTextDatum(datamodel) : (Datum)0,
                   r->output ? CStringGetTextDatum(r->output) : (Datum)0,
                   has_due ? TimestampTzGetDatum(due) : (Datum)0,
                   state ? PointerGetDatum(state) : (Datum)0,
                   Int64GetDatum(r->events),
                   sessions ? CStringGetTextDatum(sessions) : (Datum)0,
                   CStringGetTextDatum(chart ? chart : ""),
                   Int32GetDatum(version),
                   name ? CStringGetTextDatum(name) : (Datum)0,
                   BoolGetDatum(active),
                   Int64GetDatum(r->step)};
    char nulls[14] = {' ',
                      ' ',
                      ' ',
                      datamodel ? ' ' : 'n',
                      r->output ? ' ' : 'n',
                      has_due ? ' ' : 'n',
                      state ? ' ' : 'n',
                      ' ',
                      sessions ? ' ' : 'n',
                      ' ',
                      ' ',
                      name ? ' ' : 'n',
                      ' ',
                      ' '};
    if (insert) {
        if (exec(Q_ACTOR_INSERT, a, nulls, false, 0) != SPI_OK_INSERT)
            ereport(ERROR, (errmsg("tinyactors: cannot store the new actor")));
    } else if (exec(Q_ACTOR_UPDATE, a, nulls, false, 0) != SPI_OK_UPDATE) {
        ereport(ERROR, (errmsg("tinyactors: cannot store actor " INT64_FORMAT, r->id)));
    }
}

typedef enum { LOCK_WAIT, LOCK_SKIP, LOCK_HELD } lock_mode;

typedef struct inbox_row {
    int64 id;
    uint64 row;
} inbox_row;
static int by_id(const void *a, const void *b) {
    int64 x = ((const inbox_row *)a)->id, y = ((const inbox_row *)b)->id;
    return x < y ? -1 : x > y;
}

/* Process one actor: its inbox and due timers. Returns false if it was
 * skipped (locked by another transaction, gone or not active). */
/* an event handed to the actor directly (send(): no inbox row) */
typedef struct direct_event {
    const char *name, *data; /* data: JSON or NULL */
    int64 session;           /* an invoked session of the actor (0: the actor) */
} direct_event;

static bool process_locked(int64 id, lock_mode mode, run *out_targets, const direct_event *de);

/* Each actor runs in an SPI level of its own: its system region, the rows it
 * read and everything else allocated for it are freed when it is done, so a
 * call's memory does not grow with the number of actors it processes. */
static bool process_one(int64 id, lock_mode mode, run *out_targets, const direct_event *de) {
    SPI_connect();
    bool ok = process_locked(id, mode, out_targets, de);
    SPI_finish();
    return ok;
}

static bool process_locked(int64 id, lock_mode mode, run *out_targets, const direct_event *de) {
    Datum a[1] = {Int64GetDatum(id)};
    if (exec(mode == LOCK_SKIP ? Q_LOCK_SKIP : Q_LOCK_WAIT, a, NULL, false, 1) != SPI_OK_SELECT ||
        SPI_processed != 1)
        return false;
    HeapTuple row = SPI_tuptable->vals[0];
    TupleDesc td = SPI_tuptable->tupdesc;
    char *chart = SPI_getvalue(row, td, 1);
    bool isnull;
    int32 version = DatumGetInt32(SPI_getbinval(row, td, 2, &isnull));
    Datum sd = SPI_getbinval(row, td, 3, &isnull);
    char *status = SPI_getvalue(row, td, 4);
    bool stnull;
    int64 steps = DatumGetInt64(SPI_getbinval(row, td, 5, &stnull));
    char *chart_xmin = SPI_getvalue(row, td, 6);
    run r;
    memset(&r, 0, sizeof r);
    bool pnull;
    Datum procs = SPI_getbinval(row, td, 7, &pnull);
    if (strcmp(status, "active") || isnull) {
        exec(Q_INBOX_DROP, a, NULL, false, 0);
        return false;
    }
    bytea *state = DatumGetByteaPP(sd);
    void *blob = aligned_copy(VARDATA_ANY(state), VARSIZE_ANY_EXHDR(state));
    size_t blob_length = VARSIZE_ANY_EXHDR(state);
    load_processors(&r, procs, pnull);
    /* its events, oldest first, at most tinyactors.inbox_batch of them */
    Datum ta[2] = {Int64GetDatum(id), Int32GetDatum(inbox_batch)};
    if (exec(Q_INBOX_TAKE, ta, NULL, false, 0) != SPI_OK_DELETE_RETURNING)
        ereport(ERROR, (errmsg("tinyactors: cannot read the inbox of " INT64_FORMAT, id)));
    uint64 event_count = SPI_processed;
    SPITupleTable *events = SPI_tuptable;
    if (de && event_count == (uint64)inbox_batch) {
        /* a backlog: the direct event goes behind it (order is delivery order) */
        insert_inbox(id, de->name, de->data, NULL, NULL, de->session, NULL);
        de = NULL;
    }
    r.id = id;
    r.step = (stnull ? 0 : steps) + 1;
    PG_TRY();
    {
        chart_image *ci = image_of(chart, version, chart_xmin);
        system_open(&r, ci, 0);
        reserve_sessions(&r);
        ta_status states;
        while ((states = ta_system_adopt(r.system, blob, blob_length, &r.actor)) ==
               TA_ERROR_NO_MEMORY)
            system_grow(&r);
        if (states != TA_OK)
            ereport(ERROR, (errmsg("tinyactors: cannot load actor " INT64_FORMAT " (status %d)", id,
                                   states)));
        /* inbox order is delivery order: rows sorted by id (RETURNING has no
         * order; a busy actor's inbox can hold many thousands) */
        inbox_row *order = palloc(sizeof(inbox_row) * (event_count + 1));
        for (uint64 i = 0; i < event_count; i++) {
            bool idnull;
            order[i].id =
                DatumGetInt64(SPI_getbinval(events->vals[i], events->tupdesc, 1, &idnull));
            order[i].row = i;
        }
        qsort(order, event_count, sizeof *order, by_id);
        /* deferred events first (they are older), then the direct one */
        for (uint64 k = 0; k < event_count + (de ? 1 : 0); k++) {
            const char *name, *data, *origin = NULL, *sendid = NULL, *kind = "event",
                                     *origintype = NULL;
            int64 session = 0;
            if (k < event_count) {
                HeapTuple event_row = events->vals[order[k].row];
                bool snull;
                name = SPI_getvalue(event_row, events->tupdesc, 2);
                data = SPI_getvalue(event_row, events->tupdesc, 3);
                origin = SPI_getvalue(event_row, events->tupdesc, 4);
                sendid = SPI_getvalue(event_row, events->tupdesc, 5);
                kind = SPI_getvalue(event_row, events->tupdesc, 6);
                Datum sv = SPI_getbinval(event_row, events->tupdesc, 7, &snull);
                session = snull ? 0 : DatumGetInt64(sv);
                origintype = SPI_getvalue(event_row, events->tupdesc, 8);
            } else {
                name = de->name;
                data = de->data;
                session = de->session;
            }
            ta_event_in event;
            memset(&event, 0, sizeof event);
            event.size = sizeof event;
            /* for one of its invoked sessions: dropped if that one ended meanwhile
             * (as for an actor that ended after the event was queued) */
            event.target =
                session ? ta_system_actor_by_session(r.system, (uint64_t)session) : r.actor;
            if (!event.target) continue;
            event.name = (ta_string_view){name, (uint32_t)strlen(name)};
            if (origin) {
                event.origin = (ta_string_view){origin, (uint32_t)strlen(origin)};
                event.origintype =
                    origintype
                        ? str(origintype)
                        : (ta_string_view){"http://www.w3.org/TR/scxml/#SCXMLEventProcessor", 47};
            }
            if (sendid) event.sendid = (ta_string_view){sendid, (uint32_t)strlen(sendid)};
            if (!strcmp(kind, "error.communication")) event.flags = TA_EVENT_IN_ERROR_COMMUNICATION;
            event.data = data ? payload_from_wire(data) : 0;
            while ((states = ta_system_send(r.system, &event)) == TA_ERROR_NO_MEMORY)
                system_grow(&r);
            if (states != TA_OK) {
                if (event.data) ta_datamodel_ecmascript_vtable()->release(ecmascript, event.data);
                ereport(ERROR,
                        (errmsg("tinyactors: cannot deliver %s to " INT64_FORMAT, name, id)));
            }
            r.events++;
            CHECK_FOR_INTERRUPTS();
        }
        run_actor(&r);
        persist(&r, false, NULL, chart, version);
        ta_system_deinit(r.system);
    }
    PG_CATCH();
    {
        datamodel_drop();
        PG_RE_THROW();
    }
    PG_END_TRY();
    for (int i = 0; out_targets && i < r.target_count; i++)
        add_target(out_targets, r.targets[i]);
    return true;
}

/* Process an actor in a subtransaction of its own: if it fails, its changes
 * roll back (its inbox is restored), the failure is recorded on its row
 * (failures, last_error, retry_at with backoff; failed after
 * tinyactors.max_failures) and the caller goes on. Cancels and shutdowns are
 * not failures: they propagate. */
static bool process_isolated(int64 id, lock_mode mode, run *out_targets) {
    MemoryContext memory_context = CurrentMemoryContext;
    ResourceOwner owner = CurrentResourceOwner;
    bool ok = false;
    BeginInternalSubTransaction(NULL);
    MemoryContextSwitchTo(memory_context);
    PG_TRY();
    {
        ok = process_one(id, mode, out_targets, NULL);
        ReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(memory_context);
        CurrentResourceOwner = owner;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(memory_context);
        ErrorData *ed = CopyErrorData();
        FlushErrorState();
        RollbackAndReleaseCurrentSubTransaction();
        MemoryContextSwitchTo(memory_context);
        CurrentResourceOwner = owner;
        if (ed->sqlerrcode == ERRCODE_QUERY_CANCELED || ed->sqlerrcode == ERRCODE_ADMIN_SHUTDOWN ||
            ed->sqlerrcode == ERRCODE_OUT_OF_MEMORY)
            ReThrowError(ed);
        Datum a[4] = {Int64GetDatum(id), CStringGetTextDatum(ed->message ? ed->message : "error"),
                      TimestampTzGetDatum(clock_now()), Int32GetDatum(max_failures)};
        exec(Q_FAILURE, a, NULL, false, 0);
        ereport(WARNING, (errmsg("tinyactors: actor " INT64_FORMAT " failed: %s", id,
                                 ed->message ? ed->message : "error"),
                          errdetail("Recorded in tinyactors.actors (failures, last_error, "
                                    "retry_at); its events stay queued for a retry.")));
        FreeErrorData(ed);
        ok = false;
    }
    PG_END_TRY();
    return ok;
}

/* process `first` (with `mode`), then the actors it sent events to, breadth
 * first. Only `first` can fail the caller (it is the one asked for; with
 * isolate_first, as in run(), not even that): the others are processed on
 * their own (process_isolated). */
static int cascade(int64 first, lock_mode mode, const direct_event *de, bool isolate_first) {
    run q;
    memset(&q, 0, sizeof q);
    q.memory_context = CurrentMemoryContext; /* outlives the actors' SPI levels */
    int done = 0, at = 0;
    if (isolate_first ? process_isolated(first, mode, &q) : process_one(first, mode, &q, de))
        done++;
    while (at < q.target_count && done <= cascade_limit) {
        int64 next = q.targets[at++];
        if (process_isolated(next, LOCK_SKIP, &q)) done++;
    }
    return done;
}

/* ---- SQL entry points
 * ------------------------------------------------------------------------------ */

/* Re-entry: while an actor is processed, SQL runs (inserts into the inbox
 * and outbox); a trigger on those tables that calls back into the extension
 * would process actors inside another actor's processing, sharing (and on
 * error freeing) the backend's datamodel. Refused with a clean error. */
static bool in_actor = false;

static Datum guarded(Datum (*function)(FunctionCallInfo), FunctionCallInfo fcinfo) {
    if (in_actor)
        ereport(ERROR,
                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                 errmsg("tinyactors: cannot be called while an actor is being processed"),
                 errhint("A trigger on a tinyactors table calls back into tinyactors? Queue the "
                         "work instead (an outbox worker, or tinyactors.run() later).")));
    Datum r = (Datum)0;
    in_actor = true;
    PG_TRY();
    {
        r = function(fcinfo);
    }
    PG_FINALLY();
    {
        in_actor = false;
    }
    PG_END_TRY();
    return r;
}

static Datum process_impl(FunctionCallInfo fcinfo) {
    int64 id = PG_GETARG_INT64(0);
    SPI_connect();
    int n = cascade(id, LOCK_WAIT, NULL, false);
    SPI_finish();
    PG_RETURN_INT32(n);
}

/* deliver(actor, event, data): send()'s path. The event goes straight to the
 * actor (no inbox row: an insert and a delete saved per event, and no dead
 * inbox entries piling up under a busy actor); events it deferred earlier
 * are delivered first. Waits for the actor's lock. false: no such actor. */
static Datum deliver_impl(FunctionCallInfo fcinfo) {
    if (PG_ARGISNULL(0) || PG_ARGISNULL(1))
        ereport(ERROR, (errmsg("tinyactors.deliver: actor and event are required")));
    int64 id = PG_GETARG_INT64(0);
    direct_event de = {text_to_cstring(PG_GETARG_TEXT_PP(1)),
                       PG_ARGISNULL(2)
                           ? NULL
                           : DatumGetCString(DirectFunctionCall1(jsonb_out, PG_GETARG_DATUM(2))),
                       0};
    SPI_connect();
    Datum a[1] = {Int64GetDatum(id)};
    bool exists = exec(Q_EXISTS, a, NULL, true, 1) == SPI_OK_SELECT && SPI_processed == 1;
    int64 actor = id;
    if (!exists) { /* a session an actor invoked: through that actor */
        int64 root = tree_of(id);
        if (root) {
            actor = root;
            de.session = id;
            exists = true;
        }
    }
    if (exists) cascade(actor, LOCK_WAIT, &de, false);
    SPI_finish();
    PG_RETURN_BOOL(exists);
}

static int64 queued_cursor = 0;    /* run(): where the scan of queued actors goes on */
static TimestampTz last_prune = 0; /* run(): outbox and dedup window pruned at most once a minute */

static void prune(void) {
    TimestampTz now = GetCurrentTimestamp();
    if (last_prune && now - last_prune < 60 * USECS_PER_SEC) return;
    last_prune = now;
    SPI_execute(
        "WITH o AS (DELETE FROM tinyactors.outbox WHERE id IN (SELECT id FROM "
        "tinyactors.outbox WHERE delivered_at < now() - "
        "current_setting('tinyactors.outbox_retention')::interval LIMIT 10000) RETURNING "
        "1), f AS (DELETE FROM tinyactors.outbox WHERE id IN (SELECT id FROM "
        "tinyactors.outbox WHERE error IS NOT NULL AND created_at < now() - "
        "current_setting('tinyactors.outbox_retention')::interval LIMIT 10000) RETURNING "
        "1), s AS (DELETE FROM tinyactors.seen WHERE ctid IN (SELECT ctid FROM "
        "tinyactors.seen WHERE at < now() - "
        "current_setting('tinyactors.dedup_ttl')::interval LIMIT 10000) RETURNING 1) "
        "SELECT (SELECT count(*) FROM o), (SELECT count(*) FROM f), (SELECT count(*) FROM s)",
        false, 0);
}

static Datum run_impl(FunctionCallInfo fcinfo) {
    int32 max = PG_GETARG_INT32(0);
    if (max <= 0) PG_RETURN_INT32(0);
    SPI_connect();
    /* Work, not population: (1) actors whose earliest timer is due, oldest
     * first, from the actors_due index; (2) actors with deferred events,
     * oldest event first, from the inbox's primary key. Both take only rows
     * nobody else holds (SKIP LOCKED), so any number of schedulers share. */
    int64 *ids = palloc(sizeof(int64) * ((size_t)max + 1));
    uint64 n = 0;
    TimestampTz now = clock_now();
    Datum a[2] = {TimestampTzGetDatum(now), Int32GetDatum(max)};
    if (exec(Q_DUE, a, NULL, false, 0) != SPI_OK_SELECT)
        ereport(ERROR, (errmsg("tinyactors: cannot find due timers")));
    for (uint64 i = 0; i < SPI_processed; i++) {
        bool isnull;
        ids[n++] =
            DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull));
    }
    /* queued events: distinct actors from where the last call stopped, then
     * from the start (every queued actor gets its turn) */
    int64 start = queued_cursor;
    for (int pass = 0; pass < 2 && n < (uint64)max; pass++) {
        Datum b[3] = {Int32GetDatum(max - (int32)n), TimestampTzGetDatum(now),
                      Int64GetDatum(pass ? 0 : start)};
        if (exec(Q_QUEUED, b, NULL, false, 0) != SPI_OK_SELECT)
            ereport(ERROR, (errmsg("tinyactors: cannot find queued events")));
        uint64 got = SPI_processed;
        for (uint64 i = 0; i < got && n < (uint64)max; i++) {
            bool isnull;
            int64 id = DatumGetInt64(
                SPI_getbinval(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1, &isnull));
            bool dup = false;
            for (uint64 k = 0; k < n && !dup; k++)
                dup = ids[k] == id;
            if (!dup) ids[n++] = id;
            queued_cursor = id;
        }
        if (pass == 0 && start == 0) break; /* that was already from the beginning */
    }
    int done = 0;
    for (uint64 i = 0; i < n; i++)
        done += cascade(ids[i], LOCK_HELD, NULL, true);
    prune();
    SPI_finish();
    PG_RETURN_INT32(done);
}

static Datum spawn_impl(FunctionCallInfo fcinfo) {
    if (PG_ARGISNULL(0)) ereport(ERROR, (errmsg("tinyactors.spawn: chart is required")));
    char *chart = text_to_cstring(PG_GETARG_TEXT_PP(0));
    char *input = PG_ARGISNULL(1)
                      ? NULL
                      : DatumGetCString(DirectFunctionCall1(jsonb_out, PG_GETARG_DATUM(1)));
    char *name = PG_ARGISNULL(2) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(2));
    check_nesting(input);
    SPI_connect();
    Datum a[1] = {CStringGetTextDatum(chart)};
    if (exec(Q_SPAWN_INFO, a, NULL, false, 1) != SPI_OK_SELECT)
        ereport(ERROR, (errmsg("tinyactors: cannot read charts")));
    bool vnull, snull;
    Datum vd = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &vnull);
    int64 serial =
        DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &snull));
    if (vnull)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("tinyactors: no chart \"%s\"", chart),
                 errhint("Define it first: SELECT tinyactors.define('%s', $scxml$...$scxml$);",
                         chart)));
    int32 version = DatumGetInt32(vd);
    char *chart_xmin = SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 3);
    run r;
    memset(&r, 0, sizeof r);
    bool pnull;
    Datum procs = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 4, &pnull);
    load_processors(&r, procs, pnull);
    PG_TRY();
    {
        chart_image *ci = image_of(chart, version, chart_xmin);
        system_open(&r, ci, (uint64_t)serial - 1); /* the session id is the sequence value */
        uint32_t c =
            ta_image_chart_by_name(&ci->image, (ta_string_view){chart, (uint32_t)strlen(chart)});
        if (c == TA_NIL) c = 0;
        ta_payload params = input
                                ? ta_datamodel_ecmascript_payload_from_json(
                                      ecmascript, (ta_string_view){input, (uint32_t)strlen(input)})
                                : 0;
        ta_string_view nm = name ? (ta_string_view){name, (uint32_t)strlen(name)}
                                 : ta_image_chart_name(&ci->image, c);
        if (ta_system_spawn(r.system, c, nm, params, &r.actor) != TA_OK)
            ereport(ERROR, (errmsg("tinyactors: cannot start an actor of %s", chart)));
        ta_actor_info actor_info;
        actor_info.size = sizeof actor_info;
        ta_actor_info_get(r.system, r.actor, &actor_info);
        r.id = (int64)actor_info.serial;
        r.step = 1;
        reserve_sessions(&r); /* the sessions its first macrostep may invoke */
        run_actor(&r);
        persist(&r, true, name, chart, version);
        ta_system_deinit(r.system);
    }
    PG_CATCH();
    {
        datamodel_drop();
        PG_RE_THROW();
    }
    PG_END_TRY();
    /* the initial macrostep may have sent events */
    for (int i = 0; i < r.target_count && i < cascade_limit; i++)
        cascade(r.targets[i], LOCK_SKIP, NULL, true);
    SPI_finish();
    PG_RETURN_INT64(r.id);
}

Datum ta_postgres_now(PG_FUNCTION_ARGS) {
    PG_RETURN_TIMESTAMPTZ(clock_now());
}

/* tinyactors.system_id(): this database's system id (the setting, or derived) */
Datum ta_postgres_system_id(PG_FUNCTION_ARGS) {
    ta_string_view id = system_id();
    PG_RETURN_TEXT_P(cstring_to_text_with_len(id.pointer, (int)id.length));
}

/* tinyactors.address(text) -> (system, kind, session, name): the protocol's
 * address grammar (ta_address, docs/protocol.md "Addresses"); all NULL if the text
 * is not an address; session NULL above 2^63-1 (no session of a database) */
Datum ta_postgres_address(PG_FUNCTION_ARGS) {
    TupleDesc td;
    if (get_call_result_type(fcinfo, NULL, &td) != TYPEFUNC_COMPOSITE)
        ereport(ERROR, (errmsg("tinyactors.address: a composite result is expected")));
    td = BlessTupleDesc(td);
    Datum v[4] = {0};
    bool nulls[4] = {true, true, true, true};
    text *t = PG_GETARG_TEXT_PP(0);
    ta_address address;
    if (ta_address_parse((ta_string_view){VARDATA_ANY(t), (uint32_t)VARSIZE_ANY_EXHDR(t)},
                         &address) == TA_OK) {
        v[0] = PointerGetDatum(
            cstring_to_text_with_len(address.system.pointer, (int)address.system.length));
        nulls[0] = false;
        v[1] = CStringGetTextDatum(address.kind == TA_ADDRESS_SESSION ? "session" : "name");
        nulls[1] = false;
        if (address.kind == TA_ADDRESS_SESSION && address.session <= INT64_MAX) {
            v[2] = Int64GetDatum((int64)address.session);
            nulls[2] = false;
        } else if (address.kind == TA_ADDRESS_NAME) {
            char dec[256];
            uint32_t dl = ta_address_name_decode(address.name, dec, sizeof dec);
            if (dl) {
                v[3] = PointerGetDatum(cstring_to_text_with_len(dec, (int)dl));
                nulls[3] = false;
            }
        }
    }
    PG_RETURN_DATUM(HeapTupleGetDatum(heap_form_tuple(td, v, nulls)));
}

Datum ta_postgres_process(PG_FUNCTION_ARGS) {
    return guarded(process_impl, fcinfo);
}
Datum ta_postgres_deliver(PG_FUNCTION_ARGS) {
    return guarded(deliver_impl, fcinfo);
}
Datum ta_postgres_run(PG_FUNCTION_ARGS) {
    return guarded(run_impl, fcinfo);
}
Datum ta_postgres_spawn(PG_FUNCTION_ARGS) {
    return guarded(spawn_impl, fcinfo);
}

/* tinyactors.data_ok(jsonb): event data within the nesting limit (deliver(envelope)
 * answers 'malformed' otherwise) */
Datum ta_postgres_data_ok(PG_FUNCTION_ARGS) {
    char *t = DatumGetCString(DirectFunctionCall1(jsonb_out, PG_GETARG_DATUM(0)));
    PG_RETURN_BOOL(json_depth(t) <= MAX_NESTING);
}

static JsonbValue *json_field(Jsonb *j, const char *name) {
    return getKeyJsonValueFromContainer(&j->root, name, (int)strlen(name), NULL);
}
static bool json_text(Jsonb *j, const char *key, ta_string_view *s) {
    JsonbValue *v = json_field(j, key);
    *s = (ta_string_view){"", 0};
    if (!v || v->type == jbvNull) return true;
    if (v->type != jbvString) return false;
    *s = (ta_string_view){v->val.string.val, (uint32_t)v->val.string.len};
    return true;
}

/* JSONB is the transport codec. Addresses, policy, hops and dedup decisions
 * belong to the host; this adapter only converts JSON types to C fields. */
static Datum envelope_impl(FunctionCallInfo fcinfo) {
    if (PG_ARGISNULL(0)) PG_RETURN_TEXT_P(cstring_to_text("malformed"));
    Jsonb *j = PG_GETARG_JSONB_P(0);
    ta_envelope envelope = {0};
    envelope.size = sizeof envelope;
    if (!JB_ROOT_IS_OBJECT(j) || !json_text(j, "target", &envelope.target) ||
        !json_text(j, "name", &envelope.name) || !json_text(j, "origin", &envelope.origin) ||
        !json_text(j, "sendid", &envelope.sendid) ||
        !json_text(j, "origintype", &envelope.origintype) ||
        !json_text(j, "msgid", &envelope.msgid) ||
        !json_text(j, "dedup", &envelope.deduplication_key))
        PG_RETURN_TEXT_P(cstring_to_text("malformed"));
    JsonbValue *hops = json_field(j, "hops");
    if (hops && hops->type != jbvNull) {
        if (hops->type != jbvNumeric) PG_RETURN_TEXT_P(cstring_to_text("malformed"));
        char *n =
            DatumGetCString(DirectFunctionCall1(numeric_out, NumericGetDatum(hops->val.numeric)));
        uint64 value = 0;
        for (char *p = n; *p; p++) {
            if (*p < '0' || *p > '9' || value > UINT32_MAX / 10u)
                PG_RETURN_TEXT_P(cstring_to_text("malformed"));
            value = value * 10 + (unsigned)(*p - '0');
        }
        if (value > UINT32_MAX) PG_RETURN_TEXT_P(cstring_to_text("malformed"));
        envelope.hops = (uint32_t)value;
    }
    JsonbValue *v = json_field(j, "data");
    char *data =
        v ? DatumGetCString(DirectFunctionCall1(jsonb_out, JsonbPGetDatum(JsonbValueToJsonb(v))))
          : NULL;
    if (data && json_depth(data) > MAX_NESTING) PG_RETURN_TEXT_P(cstring_to_text("malformed"));
    SPI_connect();
    uint32_t peer;
    ta_host *host = router_open();
    router_peers(host, PG_ARGISNULL(1) ? NULL : text_to_cstring(PG_GETARG_TEXT_PP(1)), &peer);
    ta_route_request request;
    request.size = sizeof request;
    host_check(ta_host_route_begin(host, peer, &envelope,
                                   TA_ROUTE_DEDUPLICATE_HOST | TA_ROUTE_LOCAL_ONLY, &request));
    int64 root = route_storage(&request, NULL);
    if (request.action == TA_ROUTE_LOCAL) {
        insert_inbox(root, c_string(envelope.name), data,
                     envelope.origin.length ? c_string(envelope.origin) : NULL,
                     c_string(envelope.sendid),
                     (uint64_t)root != request.session ? (int64)request.session : 0,
                     c_string(envelope.origintype));
        cascade(root, LOCK_WAIT, NULL, false);
    }
    ta_string_view reason = ta_host_route_reason(request.reason);
    text *result =
        cstring_to_text(request.action == TA_ROUTE_DUPLICATE ? "duplicate" : c_string(reason));
    /* SPI_finish frees the host and its context, not the result. */
    text *out = (text *)SPI_palloc(VARSIZE(result));
    memcpy(out, result, VARSIZE(result));
    SPI_finish();
    PG_RETURN_TEXT_P(out);
}

Datum ta_postgres_envelope(PG_FUNCTION_ARGS) {
    return guarded(envelope_impl, fcinfo);
}

/* hello changed durable peer metadata: apply the router's next-hop choices to
 * pending rows. SQL owns the update/locks, not route precedence. */
Datum ta_postgres_repeer(PG_FUNCTION_ARGS) {
    SPI_connect();
    ta_host *host = router_open();
    router_peers(host, NULL, NULL);
    SPI_execute("SELECT DISTINCT system FROM tinyactors.outbox WHERE delivered_at IS NULL AND "
                "error IS NULL AND peer IS NOT NULL",
                true, 0);
    uint64 n = SPI_processed;
    char **systems = palloc(sizeof *systems * n);
    for (uint64 i = 0; i < n; i++)
        systems[i] = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 1);
    for (uint64 i = 0; i < n; i++) {
        ta_envelope envelope = {0};
        envelope.size = sizeof envelope;
        envelope.target = str(psprintf("tinyactors://%s/@pending", systems[i]));
        envelope.name = str("pending");
        ta_route_request request;
        request.size = sizeof request;
        host_check(ta_host_route_begin(host, 0, &envelope, TA_ROUTE_DURABLE, &request));
        if (request.action != TA_ROUTE_PEER) continue;
        Oid types[] = {TEXTOID, TEXTOID};
        Datum a[] = {CStringGetTextDatum(systems[i]), textdatum(request.peer_id)};
        SPI_execute_with_args("UPDATE tinyactors.outbox SET peer=$2 WHERE system=$1 AND peer<>$2 "
                              "AND delivered_at IS NULL AND error IS NULL",
                              2, types, a, NULL, false, 0);
    }
    SPI_finish();
    PG_RETURN_VOID();
}

static Datum error_impl(FunctionCallInfo fcinfo) {
    char address[TA_ADDRESS_SESSION_MAX];
    ta_envelope envelope = {0};
    envelope.size = sizeof envelope;
    if (!PG_ARGISNULL(0))
        envelope.target = str(text_to_cstring(PG_GETARG_TEXT_PP(0)));
    else if (!PG_ARGISNULL(1)) {
        uint32_t length =
            ta_address_format(system_id(), (uint64_t)PG_GETARG_INT64(1), address, sizeof address);
        envelope.target = (ta_string_view){address, length};
    }
    ta_event_in event;
    ta_host_error_event(PG_ARGISNULL(2) ? str(NULL) : str(text_to_cstring(PG_GETARG_TEXT_PP(2))),
                        &event);
    envelope.name = event.name;
    SPI_connect();
    ta_host *host = router_open(); /* errors go to local sessions only: no peers */
    ta_route_request request;
    request.size = sizeof request;
    host_check(ta_host_error_route(host, envelope.target, &request));
    int64 root = route_storage(&request, NULL);
    if (root) {
        Oid types[] = {INT8OID, TEXTOID, TEXTOID, JSONBOID, INT8OID};
        Datum a[] = {Int64GetDatum(root), textdatum(event.name), textdatum(event.sendid),
                     PG_ARGISNULL(3) ? (Datum)0 : PG_GETARG_DATUM(3),
                     Int64GetDatum((int64)request.session)};
        char nulls[] = {' ', ' ', ' ', PG_ARGISNULL(3) ? 'n' : ' ', ' '};
        SPI_execute_with_args("INSERT INTO tinyactors.inbox(actor,event,sendid,data,kind,session) "
                              "VALUES ($1,$2,nullif($3,''),$4,$2,CASE WHEN $5<>$1 THEN $5 END)",
                              5, types, a, nulls, false, 0);
        cascade(root, LOCK_WAIT, NULL, false);
    }
    SPI_finish();
    PG_RETURN_BOOL(root != 0);
}
Datum ta_postgres_error(PG_FUNCTION_ARGS) {
    return guarded(error_impl, fcinfo);
}
