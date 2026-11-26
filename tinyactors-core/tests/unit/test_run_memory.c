/* tinyactors run's memory: the server run in this process (tool_main) over
 * a platform whose allocator refuses after a budget, for every budget until
 * the session succeeds. Every refusal is a status: the server answers the
 * message it could not serve with -32004 and goes on, or, at startup or for
 * the line buffer, exits 3; it never crashes, and frees what it took (the
 * sanitizer build checks that nothing leaks). The server takes few blocks
 * (its arenas' chunks, the host's, the tables'), so every budget is tried.
 * The session is the worked example's, with a batch, a forwarded request,
 * an image added and refusals; batches held and acked, a send failed,
 * services invoked, done, failed and cancelled, the lookups; orders swept,
 * hibernated (with no batch open and into a held batch), woken for an
 * event and for a migration to a second version (with its wakes, its end
 * and the retired segment); the host snapshotted (answered, and written to
 * a file the platform keeps in memory) and restored in place from the file
 * and from the answer; and the end of input with a batch held, fed a few
 * bytes at a time. A second session runs with telemetry (session_telemetry).
 * The server's method table is held to the schema's run.* methods too. */
#include <stdlib.h>
#include <string.h>

#include "../harness/harness.h"
#include "schema_run.h"
#include "serve.h"

#ifndef TA_TEST_TRANSCRIPTS
#define TA_TEST_TRANSCRIPTS "tests/run"
#endif

static const char session[] =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"run.spawn\",\"params\":{\"chart\":\"order\",\"name\":\"order-"
    "17\",\"params\":{\"total\":4200}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"run.send\",\"params\":{\"target\":1,\"event\":\"checkout\",\""
    "data\":{\"card\":\"tok_visa\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"run.turn\",\"params\":{\"hold\":true}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"actor.state\",\"params\":{\"session\":1}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"run.send\",\"params\":{\"target\":1,\"event\":\"charge.ok\","
    "\"sendid\":\"pay\",\"origintype\":\"payments\",\"data\":{\"charge\":\"ch_81\"}}}\n"
    "[{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"run.turn\"},{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"actor"
    ".state\",\"params\":{\"session\":1}},{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"run.advanceTime\",\"para"
    "ms\":{\"by\":\"30s\"}}]\n"
    "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"run.spawn\",\"params\":{\"chart\":\"refund\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"run.addImage\",\"params\":{\"sources\":[{\"name\":\"renewal."
    "scxml\",\"text\":\"<scxml xmlns='http://www.w3.org/2005/07/scxml' name='renewal' datamodel='ecmascript'>"
    "<state id='charging'><invoke type='payment' src='psp:renewals' id='payment'/><transition event='done.inv"
    "oke.payment' target='renewed'/></state><final id='renewed'/></scxml>\"}]}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"run.spawn\",\"params\":{\"chart\":\"renewal\",\"name\":\"ren"
    "ewal-1\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"run.turn\"}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"run.send\",\"params\":{\"target\":\"tinyactors://tinyactors-"
    "run/@renewal-1\",\"invocation\":4294967297,\"done\":true,\"data\":{\"receipt\":\"rcpt_9\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"run.spawn\",\"params\":{\"chart\":\"order\",\"name\":\"order"
    "-18\",\"params\":{\"total\":990}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"run.sendMany\",\"params\":{\"events\":[{\"target\":2,\"event"
    "\":\"checkout\",\"data\":{\"card\":\"tok_expired\"}}]}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"run.turn\",\"params\":{\"hold\":true}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"run.fail\",\"params\":{\"target\":\"tinyactors://tinyactors-"
    "run/@order-18\",\"sendid\":\"pay\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"run.resolve\",\"params\":{\"address\":\"tinyactors://tinyact"
    "ors-run/@order-18\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"run.ack\"}\n"
    "[{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"run.chart\",\"params\":{\"name\":\"renewal\"}},{\"jsonrpc\""
    ":\"2.0\",\"id\":21,\"method\":\"run.actorByName\",\"params\":{\"name\":\"renewal-1\"}},{\"jsonrpc\":\"2."
    "0\",\"id\":22,\"method\":\"run.terminate\",\"params\":{\"target\":2}},{\"jsonrpc\":\"2.0\",\"id\":23,\"m"
    "ethod\":\"run.turn\"}]\n"
    "{\"jsonrpc\":\"2.0\",\"id\":28,\"method\":\"run.spawn\",\"params\":{\"chart\":\"renewal\",\"name\":\"ren"
    "ewal-2\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":29,\"method\":\"run.turn\"}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"run.fail\",\"params\":{\"target\":\"tinyactors://tinyactors-"
    "run/@renewal-2\",\"invocation\":4294967298}}\n"
    "[{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"run.spawn\",\"params\":{\"chart\":\"renewal\",\"name\":\"re"
    "newal-3\"}},{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"run.turn\"}]\n"
    "[{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"run.terminate\",\"params\":{\"target\":\"tinyactors://tinya"
    "ctors-run/@renewal-3\"}},{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"run.turn\"}]\n"
    "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"run.schema\"}\n"
    "not json\n";

/* Then the host snapshotted, answered and to a file, and restored from the
 * file; then (session_restore) from the answer, and a turn. */
static const char session_snapshot[] =
    "{\"jsonrpc\":\"2.0\",\"id\":60,\"method\":\"run.snapshot\"}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":61,\"method\":\"run.snapshot\",\"params\":{\"path\":\"shop.snapshot\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":62,\"method\":\"run.restore\",\"params\":{\"path\":\"shop.snapshot\"}}\n";
static const char restore_begin[] = "{\"jsonrpc\":\"2.0\",\"id\":63,\"method\":\"run.restore\",\"params\":{\"snapshot\":\"";
static const char restore_end[] = "\"}}\n{\"jsonrpc\":\"2.0\",\"id\":64,\"method\":\"run.turn\"}\n";
static const char snapshot_answered[] = "\"id\":60,\"result\":{\"snapshot\":\"";

/* Then orders hibernated, woken, and migrated to a second version; the
 * input ends with a batch held. */
static const char session_hibernation[] =
    "[{\"jsonrpc\":\"2.0\",\"id\":40,\"method\":\"run.spawn\",\"params\":{\"chart\":\"order\",\"name\":\"order"
    "-20\",\"params\":{\"total\":500}}},{\"jsonrpc\":\"2.0\",\"id\":41,\"method\":\"run.spawn\",\"params\":{\"c"
    "hart\":\"order\",\"name\":\"order-21\",\"params\":{\"total\":600}}},{\"jsonrpc\":\"2.0\",\"id\":42,\"meth"
    "od\":\"run.turn\"}]\n"
    "{\"jsonrpc\":\"2.0\",\"id\":43,\"method\":\"run.sweep\",\"params\":{\"maxScan\":1024,\"limit\":64}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":44,\"method\":\"run.sweep\",\"params\":{\"maxScan\":1024,\"limit\":64}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":45,\"method\":\"run.hibernate\",\"params\":{\"session\":3}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":46,\"method\":\"run.send\",\"params\":{\"target\":3,\"event\":\"checkout\","
    "\"data\":{\"card\":\"tok_visa\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":47,\"method\":\"run.turn\"}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":48,\"method\":\"run.wake\",\"params\":{\"session\":3,\"blob\":\"VEFIQgUAAAAAAAAA"
    "SAAAAAQAAAA5KacD1X0pOI5i5ccDAAAAAAAAACcAAAAAAAAAAAAAAAYAAAABAAAAAAAAAP____________________8BAAAFAAAAAAA"
    "AAAAAAAAAAQAAAAAAAAABAAAA_____wIAAAAAAAAAAAAAABy8Cc9jAgEKdG90YWwBDGNoYXJnZQkDBQQIAgIF6AcEAQgBAgXoBw\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":49,\"method\":\"run.turn\",\"params\":{\"hold\":true}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":50,\"method\":\"run.hibernate\",\"params\":{\"session\":4}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":51,\"method\":\"run.ack\"}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":52,\"method\":\"run.addImage\",\"params\":{\"sources\":[{\"name\":\"order-v2."
    "scxml\",\"text\":\"<scxml xmlns='http://www.w3.org/2005/07/scxml' name='order' datamodel='ecmascript'>"
    "<datamodel><data id='total' expr='0'/><data id='currency' expr='\\\"EUR\\\"'/></datamodel><state id='op"
    "en'><transition event='checkout' target='paying'/></state><state id='paying'><onentry><send type='paymen"
    "ts' target='charges' event='charge' id='pay'><param name='amount' expr='total'/><param name='currency' e"
    "xpr='currency'/></send></onentry><transition event='charge.ok' target='paid'/></state><final id='paid'/>"
    "</scxml>\"}]}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":53,\"method\":\"run.migrate\",\"params\":{\"chart\":\"order\",\"wake\":true,"
    "\"timeout\":\"1s\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":54,\"method\":\"run.turn\"}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":55,\"method\":\"run.wake\",\"params\":{\"session\":4,\"blob\":\"VEFIQgUAAAAAAAAA"
    "SAAAAAQAAAA5KacD1X0pOI5i5ccEAAAAAAAAACcAAAAAAAAAAAAAAAYAAAABAAAAAAAAAP____________________8CAAABAAAAAAA"
    "AAAAAAAAAAQAAAAAAAAABAAAA_____wIAAAAAAAAAAAAAABxMDa-rAgEKdG90YWwBDGNoYXJnZQkDBQQIAgIFsAkEAQgBAgWwCQ\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":56,\"method\":\"run.turn\"}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":25,\"method\":\"run.spawn\",\"params\":{\"chart\":\"order\",\"name\":\"order"
    "-19\",\"params\":{\"total\":150}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":26,\"method\":\"run.send\",\"params\":{\"target\":\"tinyactors://tinyactors-"
    "run/@order-19\",\"event\":\"checkout\",\"data\":{\"card\":\"tok_visa\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":27,\"method\":\"run.turn\",\"params\":{\"hold\":true}}\n";

/* Telemetry, on tests/run/telemetry's chart with logs on (telemetry_config):
 * spans and log records flushed by a turn, a watch forwarded, run.traces and
 * run.metrics refused while a batch is held and taken, run.post answered
 * with the client's id, as a notification and with a message core cannot
 * read, the old host's spans ended and written before a restore, and the
 * end of input's trace.end. */
static const char telemetry_config[] =
    "{\"system\": {\"ticksPerSecond\": 1000}, \"ioProcessors\": {\"payments\": {}}, "
    "\"telemetry\": {\"flushInterval\": 5000, \"resource\": {\"service.name\": \"orders\"}}}";
static const char session_telemetry[] =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"run.spawn\",\"params\":{\"chart\":\"order\",\"name\":\"order-1\",\"p"
    "arams\":{\"total\":4200}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"run.send\",\"params\":{\"target\":1,\"event\":\"checkout\",\"data\":"
    "{\"card\":\"tok_visa\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"run.turn\",\"params\":{\"hold\":true}}\n"
    "[{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"run.traces\"},{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"run.metri"
    "cs\"},{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"run.post\",\"params\":{\"message\":\"omJpZAdmbWV0aG9kaXRyYWNl"
    "LmVuZA\"}}]\n"
    "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"run.send\",\"params\":{\"target\":1,\"event\":\"charge.ok\",\"sendid\""
    ":\"pay\",\"origintype\":\"payments\",\"data\":{\"charge\":\"ch_81\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"run.turn\"}\n"
    "[{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"run.advanceTime\",\"params\":{\"by\":\"5s\"}},{\"jsonrpc\":\"2.0\","
    "\"id\":10,\"method\":\"run.turn\"}]\n"
    "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"trace.watch\",\"params\":{\"rules\":[{\"chart\":\"order\",\"detail\""
    ":\"decisions\"}],\"seconds\":60}}\n"
    "[{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"run.spawn\",\"params\":{\"chart\":\"order\",\"name\":\"order-2\",\"p"
    "arams\":{\"total\":990}}},{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"run.send\",\"params\":{\"target\":2,\"even"
    "t\":\"checkout\",\"data\":{\"card\":\"tok_expired\"}}},{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"run.turn\"}]\n"
    "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"run.traces\"}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"run.metrics\"}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"run.post\",\"params\":{\"message\":\"o2JpZGVjdHgtMmZtZXRob2RtYWN0b3Iu"
    "Y29udGV4dGZwYXJhbXOhZ3Nlc3Npb24C\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"run.post\",\"params\":{\"message\":\"omZtZXRob2RqYWN0b3Iuc2VuZGZwYXJh"
    "bXOjZ3Nlc3Npb24CZWV2ZW50bmNvdXBvbi5hcHBsaWVkZGRhdGGhZ3ZvdWNoZXJEC63A3g\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"run.post\",\"params\":{\"message\":\"gW1hY3Rvci5jb250ZXh0\"}}\n"
    "[{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"run.advanceTime\",\"params\":{\"by\":\"30s\"}},{\"jsonrpc\":\"2.0\""
    ",\"id\":21,\"method\":\"run.turn\"}]\n"
    "[{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"run.spawn\",\"params\":{\"chart\":\"order\",\"name\":\"order-3\",\"p"
    "arams\":{\"total\":150}}},{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"run.turn\"}]\n"
    "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"run.snapshot\",\"params\":{\"path\":\"shop.snapshot\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":25,\"method\":\"run.restore\",\"params\":{\"path\":\"shop.snapshot\"}}\n"
    "[{\"jsonrpc\":\"2.0\",\"id\":26,\"method\":\"run.send\",\"params\":{\"target\":3,\"event\":\"checkout\",\"data\""
    ":{\"card\":\"tok_visa\"}}},{\"jsonrpc\":\"2.0\",\"id\":27,\"method\":\"run.turn\"}]\n";

/* ---- a platform in memory ------------------------------------------------------------ */

typedef struct memory_platform {
    int telemetry; /* session_telemetry, in tests/run/telemetry with telemetry_config */
    long budget;   /* allocations left; negative: no limit */
    long made, live;
    size_t at;  /* of the session's parts: what receive gave so far */
    char *output;
    size_t output_length;
    char *file; /* the one file the session writes, shop.snapshot */
    size_t file_length;
    char *restore; /* session_restore's lines, made from the output when reached */
    size_t restore_length;
} memory_platform;

static void *memory_alloc(void *context, size_t size, size_t align) {
    (void)align;
    memory_platform *platform = (memory_platform *)context;
    if (platform->budget == 0) return NULL;
    if (platform->budget > 0) platform->budget--;
    platform->made++;
    platform->live++;
    return malloc(size);
}

static void memory_free(void *context, void *pointer, size_t size) {
    (void)size;
    memory_platform *platform = (memory_platform *)context;
    if (pointer) platform->live--;
    free(pointer);
}

static int is_written_file(ta_string_view path) {
    return path.length == strlen("shop.snapshot") && memcmp(path.pointer, "shop.snapshot", path.length) == 0;
}

static ta_status memory_read(void *context, const tool_read *request, ta_bytes *out) {
    memory_platform *platform = (memory_platform *)context;
    if (is_written_file(request->path)) {
        if (!platform->file) return TA_ERROR_INVALID;
        void *bytes = ta_growable_arena_alloc(request->arena, platform->file_length + 1, 8);
        if (!bytes) return TA_ERROR_NO_MEMORY;
        memcpy(bytes, platform->file, platform->file_length);
        *out = (ta_bytes){bytes, platform->file_length};
        return TA_OK;
    }
    if (platform->telemetry && request->path.length == strlen("config.json") &&
        memcmp(request->path.pointer, "config.json", request->path.length) == 0) {
        void *bytes = ta_growable_arena_alloc(request->arena, sizeof telemetry_config, 8);
        if (!bytes) return TA_ERROR_NO_MEMORY;
        memcpy(bytes, telemetry_config, sizeof telemetry_config - 1);
        *out = (ta_bytes){bytes, sizeof telemetry_config - 1};
        return TA_OK;
    }
    char path[1024];
    snprintf(path, sizeof path, "%s/%s/%.*s", TA_TEST_TRANSCRIPTS, platform->telemetry ? "telemetry" : "order",
             (int)request->path.length, request->path.pointer);
    FILE *file = fopen(path, "rb");
    if (!file) return TA_ERROR_INVALID;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    void *bytes = ta_growable_arena_alloc(request->arena, (size_t)length + 1, 8);
    size_t read = bytes ? fread(bytes, 1, (size_t)length, file) : 0;
    fclose(file);
    if (!bytes) return TA_ERROR_NO_MEMORY;
    *out = (ta_bytes){bytes, read};
    return TA_OK;
}

static ta_status memory_write(void *context, const tool_write *request) {
    memory_platform *platform = (memory_platform *)context;
    if (is_written_file(request->path)) { /* replaced whole */
        char *file = (char *)malloc(request->bytes.length + 1);
        if (!file) return TA_ERROR_INVALID;
        memcpy(file, request->bytes.pointer, request->bytes.length);
        free(platform->file);
        platform->file = file;
        platform->file_length = request->bytes.length;
        return TA_OK;
    }
    if (request->stream != TOOL_STANDARD_OUTPUT || request->path.length) return TA_OK; /* complaints */
    char *grown = (char *)realloc(platform->output, platform->output_length + request->bytes.length + 1);
    if (!grown) return TA_ERROR_INVALID;
    memcpy(grown + platform->output_length, request->bytes.pointer, request->bytes.length);
    platform->output = grown;
    platform->output_length += request->bytes.length;
    platform->output[platform->output_length] = 0;
    return TA_OK;
}

/* session_restore's lines: run.restore of the snapshot the server
 * answered to id 60 (none when it was refused), and a turn. */
static void restore_make(memory_platform *platform) {
    const char *found = platform->output ? strstr(platform->output, snapshot_answered) : NULL;
    const char *start = found ? found + strlen(snapshot_answered) : "";
    size_t length = strcspn(start, "\"");
    platform->restore_length = strlen(restore_begin) + length + strlen(restore_end);
    platform->restore = (char *)malloc(platform->restore_length + 1);
    if (!platform->restore) abort();
    snprintf(platform->restore, platform->restore_length + 1, "%s%.*s%s", restore_begin, (int)length, start,
             restore_end);
}

typedef struct session_part {
    const char *text;
    size_t length;
} session_part;

/* The session's parts, read one after the other; session_restore is made
 * from the output when it is reached. */
enum { SESSION_PARTS = 4, SESSION_RESTORE = 2 };

static session_part part_of(memory_platform *platform, size_t part) {
    if (platform->telemetry)
        return part ? (session_part){"", 0} : (session_part){session_telemetry, sizeof session_telemetry - 1};
    switch (part) {
    case 0: return (session_part){session, sizeof session - 1};
    case 1: return (session_part){session_snapshot, sizeof session_snapshot - 1};
    case SESSION_RESTORE:
        if (!platform->restore) restore_make(platform);
        return (session_part){platform->restore, platform->restore_length};
    default: return (session_part){session_hibernation, sizeof session_hibernation - 1};
    }
}

/* A few bytes at a time, so lines arrive split. */
static ta_status memory_receive(void *context, ta_buffer *room) {
    memory_platform *platform = (memory_platform *)context;
    size_t part = 0, at = platform->at;
    while (part < SESSION_PARTS && at >= part_of(platform, part).length)
        at -= part_of(platform, part++).length;
    size_t left = part < SESSION_PARTS ? part_of(platform, part).length - at : 0;
    size_t take = left < 97 ? left : 97;
    if (take > room->capacity) take = room->capacity;
    if (take) memcpy(room->pointer, part_of(platform, part).text + at, take);
    platform->at += take;
    room->length = take;
    return TA_OK;
}

typedef struct outcome {
    int exit;
    long made, live;
    char *output;
} outcome;

/* Which session, and the allocations it may make (negative: no limit). */
typedef struct serving {
    int telemetry;
    long budget;
} serving;

static outcome serve(serving how) {
    static const char *const words[] = {"run", "--config", "config.json", "order.scxml"};
    memory_platform state;
    memset(&state, 0, sizeof state);
    state.telemetry = how.telemetry;
    state.budget = how.budget;
    tool_platform platform;
    memset(&platform, 0, sizeof platform);
    platform.context = &state;
    platform.allocator = (ta_allocator){&state, memory_alloc, memory_free};
    platform.read = memory_read;
    platform.write = memory_write;
    platform.receive = memory_receive;
    platform.version = ta_string("");
    int exit = tool_main(&platform, (tool_command_line){4, words});
    free(state.file);
    free(state.restore);
    return (outcome){exit, state.made, state.live, state.output};
}

static void test_every_refusal_is_a_status(void) {
    outcome whole = serve((serving){0, -1});
    CHECK_EQUAL(whole.exit, 0);
    CHECK_EQUAL(whole.live, 0);
    CHECK(whole.output && strstr(whole.output, "\"name\":\"order-17\",\"final\":\"paid\""));
    CHECK(whole.output && strstr(whole.output, "\"id\":3,\"result\":{\"status\":\"held\""));
    CHECK(whole.output && strstr(whole.output, "\"id\":4,\"error\":{\"code\":-32002"));
    CHECK(whole.output && strstr(whole.output, "\"name\":\"renewal-1\",\"final\":\"renewed\""));
    CHECK(whole.output && strstr(whole.output, "\"method\":\"effect.uninvoke\""));
    CHECK(whole.output && strstr(whole.output, "\"id\":24,\"result\":{\"schema\":{"));
    CHECK(whole.output && strstr(whole.output, "\"id\":27,\"result\":{\"status\":\"held\""));
    CHECK(whole.output && strstr(whole.output, "\"id\":44,\"result\":{\"sessions\":[3,4,4294967298]}"));
    CHECK(whole.output && strstr(whole.output, "\"id\":45,\"result\":{\"blob\":\""));
    CHECK(whole.output && strstr(whole.output, "\"id\":48,\"result\":{}"));
    CHECK(whole.output && strstr(whole.output, "\"id\":50,\"result\":{\"blob\":\""));
    CHECK(whole.output && strstr(whole.output, "\"method\":\"effect.wake\",\"params\":{\"batch\":11,\"index\":1,"
                                               "\"session\":4,\"migration\":1}"));
    CHECK(whole.output && strstr(whole.output, "\"id\":55,\"result\":{}"));
    CHECK(whole.output && strstr(whole.output, "\"migration\":1,\"migrated\":2,\"failed\":[],\"pending\":[]"));
    CHECK(whole.output && strstr(whole.output, "\"method\":\"effect.segmentFreed\""));
    CHECK(whole.output && strstr(whole.output, "\"id\":60,\"result\":{\"snapshot\":\""));
    CHECK(whole.output && strstr(whole.output, "\"id\":61,\"result\":{\"bytes\":"));
    CHECK(whole.output && strstr(whole.output, "\"id\":62,\"result\":{\"segments\":[0,1]"));
    CHECK(whole.output && strstr(whole.output, "\"id\":63,\"result\":{\"segments\":[0,1]"));
    CHECK(whole.output && strstr(whole.output, "\"id\":64,\"result\":{\"status\":\"idle\""));
    long tried = 0, answered_memory = 0;
    for (long budget = 0; budget < whole.made; budget++) {
        outcome refused = serve((serving){0, budget});
        tried++;
        CHECK(refused.exit == 0 || refused.exit == 3);
        CHECK_EQUAL(refused.live, 0); /* everything taken was given back */
        if (refused.output && strstr(refused.output, "-32004")) answered_memory++;
        free(refused.output);
    }
    fprintf(stderr, "  %ld allocations; %ld budgets tried, %ld answered -32004 and went on\n", whole.made, tried,
            answered_memory);
    CHECK(answered_memory > 0);
    free(whole.output);
}


/* The telemetry session: whole, then with every budget. */
static void test_telemetry_refusals(void) {
    outcome whole = serve((serving){1, -1});
    CHECK_EQUAL(whole.exit, 0);
    CHECK_EQUAL(whole.live, 0);
    const char *output = whole.output ? whole.output : "";
    CHECK(strstr(output, "\"id\":4,\"error\":{\"code\":-32002"));
    CHECK(strstr(output, "\"id\":5,\"error\":{\"code\":-32002"));
    CHECK(strstr(output, "\"id\":6,\"error\":{\"code\":-32002"));
    CHECK(strstr(output, "\"method\":\"effect.logs\",\"params\":{\"batch\":3,\"index\":1,\"otlp\":\""));
    CHECK(strstr(output, "\"method\":\"effect.traces\",\"params\":{\"batch\":3,\"index\":0,\"spans\":[{"));
    CHECK(strstr(output, "\"id\":11,\"result\":{\"id\":1}"));
    CHECK(strstr(output, "\"id\":15,\"result\":{\"spans\":[{"));
    CHECK(strstr(output, "\"id\":16,\"result\":{\"otlp\":\""));
    /* {"id": "ctx-2", "result": {"variables": {"total": 990, "charge": null, "coupon": undefined}}} */
    CHECK(strstr(output, "\"id\":17,\"result\":{\"reply\":\"omJpZGVjdHgtMmZyZXN1bHShaXZhcmlhYmxlc6NldG90YWwZA95mY2hh"
                         "cmdl9mZjb3Vwb273\"}"));
    CHECK(strstr(output, "\"id\":18,\"result\":{}"));
    CHECK(strstr(output, "\"id\":19,\"result\":{\"reply\":\"omJpZPZlZXJyb3Kj"));
    /* the old host's spans before the restore's response, the new one's at the end of input */
    const char *ended = strstr(output, "\"id\":24,\"result\":{\"bytes\":");
    const char *before = ended ? strstr(ended, "\"method\":\"effect.traces\"") : NULL;
    const char *restored = strstr(output, "\"id\":25,\"result\":{\"segments\":[0]");
    CHECK(before && restored && before < restored);
    const char *turned = strstr(output, "\"id\":27,\"result\":{\"status\":\"idle\"");
    CHECK(turned && strstr(turned, "\"method\":\"effect.traces\""));
    long answered_memory = 0;
    for (long budget = 0; budget < whole.made; budget++) {
        outcome refused = serve((serving){1, budget});
        CHECK(refused.exit == 0 || refused.exit == 3);
        CHECK_EQUAL(refused.live, 0);
        if (refused.output && strstr(refused.output, "-32004")) answered_memory++;
        free(refused.output);
    }
    fprintf(stderr, "  telemetry: %ld allocations, %ld budgets answered -32004 and went on\n", whole.made,
            answered_memory);
    CHECK(answered_memory > 0);
    free(whole.output);
}

static int is_served(tool_method_list served, ta_string_view name) {
    for (uint32_t i = 0; i < served.count; i++)
        if (served.methods[i].name.length == name.length &&
            memcmp(served.methods[i].name.pointer, name.pointer, name.length) == 0)
            return 1;
    return 0;
}

/* Both ways: every method the server answers is a run.* method of the
 * schema, and every run.* method of the schema is the server's. */
static void test_methods_are_the_schemas(void) {
    tool_method_list served = tool_serve_methods();
    CHECK(served.count > 0);
    for (uint32_t i = 0; i < served.count; i++) {
        ta_string_view name = served.methods[i].name;
        int found = tool_schema_method_find(&tool_schema_run, name) < tool_schema_run.method_count;
        if (!found) fprintf(stderr, "  the server has %.*s, the schema has not\n", (int)name.length, name.pointer);
        CHECK(found);
        CHECK(name.length > 4 && memcmp(name.pointer, "run.", 4) == 0);
    }
    uint32_t described = 0;
    for (uint32_t i = 0; i < tool_schema_run.method_count; i++) {
        ta_string_view name = tool_schema_run.methods[i].name;
        if (name.length < 4 || memcmp(name.pointer, "run.", 4) != 0) continue;
        described++;
        int known = is_served(served, name);
        if (!known) fprintf(stderr, "  the schema has %.*s, the server has not\n", (int)name.length, name.pointer);
        CHECK(known);
    }
    CHECK_EQUAL(described, served.count);
}

int main(void) {
    RUN(test_methods_are_the_schemas);
    RUN(test_every_refusal_is_a_status);
    RUN(test_telemetry_refusals);
    TEST_MAIN_END();
}
