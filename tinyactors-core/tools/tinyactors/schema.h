/* schema.h — the protocol's JSON Schema as data, and the checker that walks
 * it (schema_check.c).
 *
 * tools/tinyactors/schema/generate.rb turns a JSON Schema (2020-12) into the
 * tables below: one node per schema object, with its types, members, values,
 * bounds, items and oneOf branches; its $refs resolved to node numbers. The
 * checker reads a message's CBOR tokens (a ta_cbor_document, as the
 * command's JSON reader makes it) against a node, with an explicit stack,
 * and refuses the first value the schema does not allow with JSON-RPC's
 * -32602, a reason as core's (missing, type, unknown, range) and the value's
 * path ("params.events[2].data"). What it supports is what the generator
 * accepts: a schema with any other keyword does not generate.
 *
 * The run schema (run.schema.json) is generated as tool_schema_run, with
 * its methods and definitions as enums, in schema_run.h; the build writes
 * both into build/<variant>/generated/tools/tinyactors/. */
#ifndef TINYACTORS_TOOL_SCHEMA_H
#define TINYACTORS_TOOL_SCHEMA_H

#include <tinyactors/arena.h>
#include <tinyactors/cbor.h>

/* A node's types, as bits; none: any value. */
typedef enum tool_schema_type {
    TOOL_SCHEMA_OBJECT = 1u << 0,
    TOOL_SCHEMA_ARRAY = 1u << 1,
    TOOL_SCHEMA_STRING = 1u << 2,
    TOOL_SCHEMA_INTEGER = 1u << 3,
    TOOL_SCHEMA_NUMBER = 1u << 4,
    TOOL_SCHEMA_BOOLEAN = 1u << 5,
    TOOL_SCHEMA_NULL = 1u << 6
} tool_schema_type;

/* Which of a node's bounds apply, and whether it takes only its members. */
typedef enum tool_schema_check {
    TOOL_SCHEMA_MINIMUM = 1u << 0,
    TOOL_SCHEMA_MAXIMUM = 1u << 1,
    TOOL_SCHEMA_MIN_LENGTH = 1u << 2,
    TOOL_SCHEMA_MAX_LENGTH = 1u << 3,
    TOOL_SCHEMA_MIN_ITEMS = 1u << 4,
    TOOL_SCHEMA_MAX_ITEMS = 1u << 5,
    TOOL_SCHEMA_CLOSED = 1u << 6 /* additionalProperties: false */
} tool_schema_check;

/* The patterns the checker knows, each a function of schema_check.c (the
 * command carries no regular-expression engine). */
typedef enum tool_schema_pattern {
    TOOL_SCHEMA_PATTERN_NONE = 0,
    TOOL_SCHEMA_PATTERN_BASE64URL = 1, /* ^[A-Za-z0-9_-]*$ */
    TOOL_SCHEMA_PATTERN_DURATION = 2,  /* ^[ \t]*([0-9]+(\.[0-9]*)?|\.[0-9]+)(ms|s)[ \t]*$ */
    TOOL_SCHEMA_PATTERN_HEX = 3        /* ^[0-9a-f]*$ */
} tool_schema_pattern;

/* An integer of the schema, as CBOR has one: `value`, or with `negative`
 * the integer -1 - value, so -2^64 … 2^64-1. */
typedef struct tool_schema_integer {
    uint64_t value;
    uint32_t negative;
    uint32_t pad;
} tool_schema_integer;

typedef enum tool_schema_value_kind {
    TOOL_SCHEMA_VALUE_TEXT = 1,
    TOOL_SCHEMA_VALUE_INTEGER = 2,
    TOOL_SCHEMA_VALUE_BOOLEAN = 3 /* integer.value: 0 or 1 */
} tool_schema_value_kind;

/* A value of an enum or a const. */
typedef struct tool_schema_value {
    uint32_t kind; /* tool_schema_value_kind */
    uint32_t pad;
    tool_schema_integer integer;
    ta_string_view text;
} tool_schema_value;

/* A member of an object: its name, its node, whether it is required. */
typedef struct tool_schema_member {
    ta_string_view name;
    uint32_t node;
    uint32_t required;
} tool_schema_member;

/* A branch of a oneOf and what selects it: the value's type among `types`;
 * then, when `key` is not empty, that member present, equal to the text
 * `value` when `keyed_by_value`. The generator makes the branches exclusive
 * by these alone, so the one selected is the only one that can hold. */
typedef struct tool_schema_choice {
    uint32_t node;
    uint32_t types; /* tool_schema_type bits */
    ta_string_view key;
    uint32_t keyed_by_value;
    uint32_t pad;
    ta_string_view value;
} tool_schema_choice;

/* not: {required: [first, second]}: not both. */
typedef struct tool_schema_exclusive {
    ta_string_view first, second;
} tool_schema_exclusive;

typedef struct tool_schema_node {
    uint32_t types;  /* tool_schema_type bits; 0: any */
    uint32_t checks; /* tool_schema_check bits */
    tool_schema_integer minimum, maximum;
    uint64_t min_length, max_length; /* in code points */
    uint64_t min_items, max_items;
    uint32_t pattern;               /* tool_schema_pattern */
    uint32_t items;                 /* each item's node; TA_NIL: any */
    uint32_t members, member_count; /* in tool_schema.members */
    uint32_t values, value_count;   /* enum or const, in tool_schema.values */
    uint32_t choices, choice_count; /* oneOf, in tool_schema.choices */
    uint32_t exclusive;             /* in tool_schema.exclusives; TA_NIL: none */
    uint32_t pad;
} tool_schema_node;

/* A method of the schema's `request`: its params' node and its result's
 * ($defs/<method>.result), TA_NIL when the schema has none. */
typedef struct tool_schema_entry {
    ta_string_view name;
    uint32_t params, result;
} tool_schema_entry;

/* A generated schema. */
typedef struct tool_schema {
    const tool_schema_node *nodes;
    const tool_schema_member *members;
    const tool_schema_value *values;
    const tool_schema_choice *choices;
    const tool_schema_exclusive *exclusives;
    const tool_schema_entry *methods;
    uint32_t node_count, method_count;
    ta_bytes text; /* the schema's JSON, as the file holds it; empty for a fixture's */
} tool_schema;

/* A refusal, as core's error object has it (JSON-RPC's code, a reason, a
 * path). `reason` and `message` are static; `path` lives in the input's
 * arena. */
typedef struct tool_schema_error {
    int32_t code;
    uint32_t pad;
    ta_string_view reason, path, message;
} tool_schema_error;

typedef struct tool_schema_input {
    const tool_schema *schema;
    const ta_cbor_document *document;
    uint32_t at;              /* the value's token; TA_NIL: absent, checked as an empty object */
    uint32_t pad;
    ta_string_view root;      /* the path's first word: "params", "result"; empty: none */
    ta_growable_arena *arena; /* the path of a refusal */
} tool_schema_input;

/* A method's number by name: schema->method_count when the schema has none
 * (a forwarded method core added since: not checked here). */
uint32_t tool_schema_method_find(const tool_schema *schema, ta_string_view name);

/* Check a method's params. TA_OK; TA_ERROR_INVALID with `error` set: code
 * -32602, reason missing, type, unknown or range, the path from the input's
 * root; TA_ERROR_NO_MEMORY: the arena refused the path; TA_ERROR_LIMIT: the
 * document nests deeper than a decoded one can. */
ta_status tool_schema_check_params(uint32_t method, const tool_schema_input *input, tool_schema_error *error);

/* The same against any node: a result, a notification's params, a whole
 * message. */
ta_status tool_schema_check_message(uint32_t node, const tool_schema_input *input, tool_schema_error *error);

/* The run schema's bytes, for --schema and run.schema. */
ta_bytes tool_schema_text(void);

#endif /* TINYACTORS_TOOL_SCHEMA_H */
