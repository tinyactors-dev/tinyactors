/* cli/rpc.h — JSON-RPC 2.0, one message per line (NDJSON), for the command's
 * run server (docs/cli.md, "Run"). The command's own: in no released
 * library. Freestanding: no C library, no recursion, no allocation but the
 * allocator and arenas a caller gives.
 *
 * Three pieces:
 *   - lines: bytes as standard input gives them, cut into lines, each
 *     without its "\n" or "\r\n"; blank lines skipped; a line past a limit
 *     dropped up to its newline and reported once;
 *   - messages: a line read as JSON (cli/json.h) into CBOR tokens and its
 *     envelope checked as JSON-RPC 2.0 and core's posted messages have it
 *     (docs/requests.md, "The envelope"): a request, a notification or a
 *     batch;
 *   - writing: the message a forwarded request becomes for ta_host_post,
 *     a response and a notification as CBOR, and one CBOR item as a JSON
 *     line. */
#ifndef TINYACTORS_CLI_RPC_H
#define TINYACTORS_CLI_RPC_H

#include <tinyactors/arena.h>
#include <tinyactors/cbor.h>

#include "base/output.h"

/* JSON-RPC's codes, and those core's posted messages add (docs/requests.md),
 * with -32007 the command's. */
typedef enum ta_rpc_code {
    TA_RPC_PARSE_ERROR = -32700,
    TA_RPC_INVALID_REQUEST = -32600,
    TA_RPC_METHOD_NOT_FOUND = -32601,
    TA_RPC_INVALID_PARAMS = -32602,
    TA_RPC_INTERNAL_ERROR = -32603,
    TA_RPC_STALE = -32001,
    TA_RPC_STATE = -32002,
    TA_RPC_LIMIT = -32003,
    TA_RPC_MEMORY = -32004,
    TA_RPC_INVALID = -32005,
    TA_RPC_MALFORMED = -32006,
    TA_RPC_UNSUPPORTED = -32007
} ta_rpc_code;

typedef enum ta_rpc_limits {
    /* a line whose block grew past this gives the block back once it is read */
    TA_RPC_LINES_KEEP = 1 << 20
} ta_rpc_limits;

/* ---- lines ------------------------------------------------------------------- */

typedef struct ta_rpc_lines {
    ta_output pending; /* bytes not yet handed out as lines, over the caller's allocator */
    size_t consumed;   /* of `pending`: bytes already handed out */
    size_t scanned;    /* of `pending`: bytes searched for a newline */
    size_t limit;      /* the longest line taken (--max-message) */
    uint32_t skipping; /* past the limit: bytes dropped up to the next newline */
    uint32_t ended;    /* the end of input was fed: the unended last line is whole */
    uint32_t shrink;   /* a line past TA_RPC_LINES_KEEP was handed out: give the block back */
    uint32_t pad;
} ta_rpc_lines;

typedef struct ta_rpc_lines_config {
    const ta_allocator *allocator; /* borrowed until ta_rpc_lines_free */
    size_t limit;
} ta_rpc_lines_config;

void ta_rpc_lines_init(ta_rpc_lines *lines, const ta_rpc_lines_config *config);
/* Bytes as they came: TA_OK, or TA_ERROR_NO_MEMORY (nothing taken). An
 * empty chunk is the end of input: the unended last line becomes whole.
 * Every line handed out before ends here. */
ta_status ta_rpc_lines_feed(ta_rpc_lines *lines, ta_bytes chunk);

/* A line: its text without "\r\n" or "\n", a view valid until the next
 * feed; or, with `too_long`, a line past the limit, reported once when the
 * limit is passed (its text empty; the rest of it is dropped as it comes). */
typedef struct ta_rpc_line {
    ta_string_view text;
    uint32_t too_long;
    uint32_t pad;
} ta_rpc_line;
/* The next line: 1; 0 when none is whole yet. Blank lines (nothing, or
 * only spaces and tabs) are skipped. */
int ta_rpc_lines_next(ta_rpc_lines *lines, ta_rpc_line *out);
void ta_rpc_lines_free(ta_rpc_lines *lines);

/* ---- messages ------------------------------------------------------------------ */

typedef enum ta_rpc_kind {
    TA_RPC_REQUEST = 0,      /* with an id: answered */
    TA_RPC_NOTIFICATION = 1, /* without one: applied, not answered */
    TA_RPC_BATCH = 2         /* an array of them: ta_rpc_element reads each */
} ta_rpc_kind;

/* An error as core's error object has it: JSON-RPC's code, a reason
 * (docs/requests.md), the member or param it is about ("" for the message),
 * a message for people and, for the server's own errors, the name of the
 * status a C call refused with (empty: none). Views of static text or of the
 * caller's arena. */
typedef struct ta_rpc_error {
    int32_t code; /* ta_rpc_code; 0: none */
    uint32_t pad;
    ta_string_view reason, path, message, status;
} ta_rpc_error;

/* A message read: its tokens, and where its members are. */
typedef struct ta_rpc_message {
    ta_cbor_document document;   /* the line's CBOR and tokens, in the arena */
    uint32_t kind;               /* ta_rpc_kind */
    uint32_t at;                 /* the message's token (a batch's array) */
    uint32_t method, params, id; /* token indices; TA_NIL: absent */
    uint32_t count;              /* a batch's elements */
    uint32_t pad;
    ta_rpc_error error; /* code 0: none; else the message is answered with it, id null */
} ta_rpc_message;

typedef struct ta_rpc_parsing {
    ta_string_view line;
    ta_growable_arena *arena; /* the message's CBOR and tokens, and the reader's keys */
} ta_rpc_parsing;
/* Read a line. TA_OK: `out` is the message, or out->error says why it is
 * none (-32700 malformed: not JSON, a repeated key; -32600: not a request,
 * nested too deep, an empty batch); TA_ERROR_NO_MEMORY: the arena refused. */
ta_status ta_rpc_parse(const ta_rpc_parsing *parsing, ta_rpc_message *out);

/* The element of a batch at token `at` (the batch's array's items follow it
 * in pre-order: ta_cbor_next steps from one to the next), checked as a
 * message alone: a request, a notification, or out->error. */
typedef struct ta_rpc_element {
    const ta_rpc_message *batch;
    uint32_t at;
    uint32_t pad;
} ta_rpc_element;
void ta_rpc_element_read(const ta_rpc_element *element, ta_rpc_message *out);

/* ---- writing ----------------------------------------------------------------------- */

/* CBOR bytes as a document whose tokens are in `arena`. TA_ERROR_INVALID or
 * TA_ERROR_UNSUPPORTED: not one item (ta_cbor_decode); TA_ERROR_NO_MEMORY. */
typedef struct ta_rpc_decoding {
    ta_bytes cbor;
    ta_growable_arena *arena;
} ta_rpc_decoding;
ta_status ta_rpc_decode(const ta_rpc_decoding *decoding, ta_cbor_document *out);

/* The item at token `at` of `document`, with every child, written again. */
typedef struct ta_rpc_item {
    const ta_cbor_document *document;
    uint32_t at;
    uint32_t pad;
} ta_rpc_item;
void ta_rpc_write_item(ta_cbor_writer *writer, ta_rpc_item item);

/* The message for ta_host_post: the request's members but "jsonrpc", its
 * id replaced by `id` (a notification stays without one). */
typedef struct ta_rpc_forward {
    const ta_rpc_message *message;
    uint64_t id; /* the pending row's handle */
} ta_rpc_forward;
void ta_rpc_forward_write(const ta_rpc_forward *forward, ta_cbor_writer *writer);

/* A response: {"jsonrpc": "2.0", "id", "result"} or {..., "error"}. The id
 * is the item at `id` of `ids` (TA_NIL: null). With error->code 0, the
 * result is the item at `result` of `results`; otherwise the error, its
 * "data" with "reason", "path" and, when given, "status". */
typedef struct ta_rpc_response {
    ta_rpc_item id;     /* document NULL or at TA_NIL: null */
    ta_rpc_item result; /* a map; document NULL: {} */
    const ta_rpc_error *error;
} ta_rpc_response;
void ta_rpc_response_write(const ta_rpc_response *response, ta_cbor_writer *writer);

/* A notification's envelope up to its params: {"jsonrpc": "2.0", "method":
 * <method>, "params": ...}; the caller writes the params map next. */
void ta_rpc_notification_begin(ta_cbor_writer *writer, ta_string_view method);

/* One CBOR item as one compact JSON line ending "\n", appended to `out`
 * (ta_json_write). TA_OK; TA_ERROR_INVALID and TA_ERROR_UNSUPPORTED as
 * ta_json_write; TA_ERROR_NO_MEMORY: `out` could not grow. */
ta_status ta_rpc_line_write(ta_bytes cbor, ta_output *out);

#endif /* TINYACTORS_CLI_RPC_H */
