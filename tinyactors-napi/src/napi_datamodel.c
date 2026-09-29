#include "napi_datamodel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "napi_system.h"
#include "values.h"

/* ------------------------------------------------------------------------
 * Expression records (TA_EXPRESSION_HOST values), one per value (ADR 0030)
 */

typedef struct tna_expr_record tna_expr_record;

/*
 * The records keyed by one object, one per kind. The WeakMap entry for the
 * object holds an external naming the cell, and each record in it holds the
 * cell too; the cell goes with the last of them. The external's finalizer
 * may run after the system is gone, so a cell is plain malloc memory.
 */
typedef struct {
    uint32_t references;
    tna_expr_record *records[3]; /* by tna_expr_kind */
} tna_intern_cell;

struct tna_expr_record {
    tna_values *values; /* env, allocator, torn_down */
    tna_js_datamodel *dm;
    napi_ref ref;
    uint32_t references;
    tna_expr_kind kind;
    /* Where the record is found again: an object's cell, or the key of the
     * primitive a literal holds, in dm->primitives. Neither when it is not. */
    tna_intern_cell *cell;
    uint8_t *key;
    uint32_t key_len;
    uint64_t hash;
};

static void tna_cell_unref(tna_intern_cell *cell) {
    if (--cell->references == 0) {
        free(cell);
    }
}

static void tna_cell_finalize(napi_env env, void *data, void *hint) {
    (void)env;
    (void)hint;
    tna_cell_unref(data);
}

static uint64_t tna_hash_bytes(const uint8_t *bytes, uint32_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t at = 0; at < len; at += 1) {
        hash ^= bytes[at];
        hash *= 1099511628211ull;
    }
    return hash;
}

static bool tna_same_key(const tna_expr_record *record, uint64_t hash, const uint8_t *key, uint32_t len) {
    return record->hash == hash && record->key_len == len && memcmp(record->key, key, len) == 0;
}

static tna_expr_record *tna_primitive_find(const tna_intern_table *table, uint64_t hash, const uint8_t *key,
                                           uint32_t len) {
    if (table->capacity == 0) {
        return NULL;
    }
    uint32_t mask = table->capacity - 1u;
    for (uint32_t at = (uint32_t)hash & mask;; at = (at + 1u) & mask) {
        tna_expr_record *record = table->slots[at].record;
        if (record == NULL) {
            return NULL;
        }
        if (tna_same_key(record, hash, key, len)) {
            return record;
        }
    }
}

static void tna_primitive_place(tna_intern_table *table, tna_expr_record *record) {
    uint32_t mask = table->capacity - 1u;
    uint32_t at = (uint32_t)record->hash & mask;
    while (table->slots[at].record != NULL) {
        at = (at + 1u) & mask;
    }
    table->slots[at] = (tna_intern_slot){ .hash = record->hash, .record = record };
    table->count += 1u;
}

/* Adds `record` under its key; false when the table cannot grow, and the
 * record then stays out of it. */
static bool tna_primitive_add(tna_intern_table *table, ta_allocator allocator, tna_expr_record *record) {
    if ((table->count + 1u) * 2u > table->capacity) {
        uint32_t capacity = table->capacity == 0 ? 16u : table->capacity * 2u;
        if (capacity < table->capacity) {
            return false;
        }
        tna_intern_slot *slots = tah_alloc(allocator, (size_t)capacity * sizeof *slots, _Alignof(tna_intern_slot));
        if (slots == NULL) {
            return false;
        }
        for (uint32_t at = 0; at < capacity; at += 1) {
            slots[at] = (tna_intern_slot){ 0 };
        }
        tna_intern_table grown = { .slots = slots, .capacity = capacity };
        for (uint32_t at = 0; at < table->capacity; at += 1) {
            if (table->slots[at].record != NULL) {
                tna_primitive_place(&grown, table->slots[at].record);
            }
        }
        if (table->slots != NULL) {
            tah_free(allocator, &(ta_allocator_free_params){ .ptr = table->slots,
                                                            .size = (size_t)table->capacity * sizeof *table->slots,
                                                            .alignment = _Alignof(tna_intern_slot) });
        }
        *table = grown;
    }
    tna_primitive_place(table, record);
    return true;
}

/* Takes `record` out, shifting back the slots its probe chain covered. */
static void tna_primitive_remove(tna_intern_table *table, const tna_expr_record *record) {
    if (table->capacity == 0) {
        return;
    }
    uint32_t mask = table->capacity - 1u;
    uint32_t at = (uint32_t)record->hash & mask;
    while (table->slots[at].record != record) {
        if (table->slots[at].record == NULL) {
            return;
        }
        at = (at + 1u) & mask;
    }
    table->slots[at] = (tna_intern_slot){ 0 };
    table->count -= 1u;
    for (uint32_t next = (at + 1u) & mask; table->slots[next].record != NULL; next = (next + 1u) & mask) {
        uint32_t home = (uint32_t)table->slots[next].hash & mask;
        /* The entry at `next` may move to the hole when its home is not in (at, next]. */
        bool stays = at <= next ? (home > at && home <= next) : (home > at || home <= next);
        if (!stays) {
            table->slots[at] = table->slots[next];
            table->slots[next] = (tna_intern_slot){ 0 };
            at = next;
        }
    }
}

static ta_result tna_expr_retain(void *self, void *value, ta_allocator allocator, void **out) {
    (void)self;
    (void)allocator;
    ((tna_expr_record *)value)->references += 1u;
    *out = value;
    return TA_OK;
}

static void tna_expr_release(void *self, void *value, ta_allocator allocator) {
    (void)self;
    (void)allocator;
    tna_expr_record *record = value;
    if (--record->references != 0) {
        return;
    }
    tna_values *values = record->values;
    if (record->cell != NULL) {
        if (record->cell->records[record->kind] == record) {
            record->cell->records[record->kind] = NULL;
        }
        tna_cell_unref(record->cell);
    }
    if (record->key != NULL) {
        tna_primitive_remove(&record->dm->primitives, record);
        tah_free(values->allocator,
                 &(ta_allocator_free_params){ .ptr = record->key, .size = record->key_len, .alignment = 1 });
    }
    if (record->ref != NULL && !*values->torn_down) {
        napi_delete_reference(values->env, record->ref);
    }
    tah_free(values->allocator,
             &(ta_allocator_free_params){ .ptr = record, .size = sizeof *record, .alignment = _Alignof(tna_expr_record) });
}

static const ta_expression_host_vtable tna_expr_vtable = { tna_expr_retain, tna_expr_release };

static const tna_expr_record *tna_record_of(const ta_expression *expression) {
    if (expression == NULL || expression->kind != TA_EXPRESSION_HOST || expression->as.host.v != &tna_expr_vtable) {
        return NULL;
    }
    return expression->as.host.value;
}

/*
 * The key of a primitive, as Object.is compares it: a tag, then the value
 * (a number's bits, one NaN for all; a string's UTF-16; a bigint's sign and
 * words). Allocated from the system's allocator. False for a symbol, which
 * is not interned, and when memory runs out.
 */
static bool tna_primitive_key(napi_env env, napi_value value, napi_valuetype type, tna_values *values,
                              uint8_t **key, uint32_t *len) {
    uint8_t head[1 + sizeof(double)];
    size_t size = 1;
    size_t extra = 0;
    int sign = 0;
    switch (type) {
    case napi_undefined:
        head[0] = 0;
        break;
    case napi_null:
        head[0] = 1;
        break;
    case napi_boolean: {
        bool flag = false;
        if (napi_get_value_bool(env, value, &flag) != napi_ok) {
            return false;
        }
        head[0] = flag ? 3 : 2;
        break;
    }
    case napi_number: {
        double number = 0;
        if (napi_get_value_double(env, value, &number) != napi_ok) {
            return false;
        }
        if (number != number) {
            number = __builtin_nan("");
        }
        head[0] = 4;
        memcpy(head + 1, &number, sizeof number);
        size += sizeof number;
        break;
    }
    case napi_string:
        head[0] = 5;
        if (napi_get_value_string_utf16(env, value, NULL, 0, &extra) != napi_ok) {
            return false;
        }
        extra *= sizeof(char16_t);
        break;
    case napi_bigint:
        head[0] = 6;
        if (napi_get_value_bigint_words(env, value, NULL, &extra, NULL) != napi_ok) {
            return false;
        }
        extra = 1 + extra * sizeof(uint64_t);
        break;
    default:
        return false;
    }
    size_t total = size + extra;
    if (total > UINT32_MAX) {
        return false;
    }
    uint8_t *bytes = tah_alloc(values->allocator, total, 1);
    if (bytes == NULL) {
        return false;
    }
    memcpy(bytes, head, size);
    bool ok = true;
    if (type == napi_string) {
        size_t units = extra / sizeof(char16_t);
        char16_t *text = tah_alloc(values->allocator, (units + 1u) * sizeof(char16_t), _Alignof(char16_t));
        size_t copied = 0;
        ok = text != NULL && napi_get_value_string_utf16(env, value, text, units + 1u, &copied) == napi_ok &&
             copied == units;
        if (ok) {
            memcpy(bytes + size, text, extra);
        }
        if (text != NULL) {
            tah_free(values->allocator, &(ta_allocator_free_params){ .ptr = text, .size = (units + 1u) * sizeof(char16_t),
                                                                    .alignment = _Alignof(char16_t) });
        }
    } else if (type == napi_bigint) {
        size_t count = (extra - 1u) / sizeof(uint64_t);
        uint64_t *words = count == 0 ? NULL : tah_alloc(values->allocator, count * sizeof(uint64_t), _Alignof(uint64_t));
        ok = (count == 0 || words != NULL) &&
             napi_get_value_bigint_words(env, value, &sign, &count, words) == napi_ok;
        if (ok) {
            bytes[size] = sign != 0 ? 1 : 0;
            if (count != 0) {
                memcpy(bytes + size + 1, words, count * sizeof(uint64_t));
            }
        }
        if (words != NULL) {
            tah_free(values->allocator, &(ta_allocator_free_params){ .ptr = words, .size = count * sizeof(uint64_t),
                                                                    .alignment = _Alignof(uint64_t) });
        }
    }
    if (!ok) {
        tah_free(values->allocator, &(ta_allocator_free_params){ .ptr = bytes, .size = total, .alignment = 1 });
        return false;
    }
    *key = bytes;
    *len = (uint32_t)total;
    return true;
}

/* The system's WeakMap of objects to cells, made on first use. */
static bool tna_objects_map(tna_js_datamodel *dm, napi_value *out) {
    napi_env env = dm->env;
    if (dm->objects != NULL) {
        return napi_get_reference_value(env, dm->objects, out) == napi_ok && *out != NULL;
    }
    napi_value global = NULL;
    napi_value constructor = NULL;
    return napi_get_global(env, &global) == napi_ok &&
           napi_get_named_property(env, global, "WeakMap", &constructor) == napi_ok &&
           napi_new_instance(env, constructor, 0, NULL, out) == napi_ok &&
           napi_create_reference(env, *out, 1, &dm->objects) == napi_ok;
}

static bool tna_map_call(napi_env env, napi_value map, const char *method, size_t argc, napi_value *argv,
                         napi_value *out) {
    napi_value function = NULL;
    napi_value result = NULL;
    return napi_get_named_property(env, map, method, &function) == napi_ok &&
           napi_call_function(env, map, function, argc, argv, out != NULL ? out : &result) == napi_ok;
}

/* The cell of object `key`, made (and entered in the WeakMap) when it has none. */
static tna_intern_cell *tna_cell_of(tna_js_datamodel *dm, napi_value key) {
    napi_env env = dm->env;
    napi_value map = NULL;
    napi_value found = NULL;
    napi_valuetype type = napi_undefined;
    if (!tna_objects_map(dm, &map) || !tna_map_call(env, map, "get", 1, &key, &found) ||
        napi_typeof(env, found, &type) != napi_ok) {
        return NULL;
    }
    void *data = NULL;
    if (type == napi_external) {
        return napi_get_value_external(env, found, &data) == napi_ok ? data : NULL;
    }
    tna_intern_cell *cell = malloc(sizeof *cell);
    if (cell == NULL) {
        return NULL;
    }
    *cell = (tna_intern_cell){ .references = 1u };
    napi_value external = NULL;
    if (napi_create_external(env, cell, tna_cell_finalize, NULL, &external) != napi_ok) {
        free(cell);
        return NULL;
    }
    /* From here the external owns the cell's first reference. */
    napi_value argv[2] = { key, external };
    return tna_map_call(env, map, "set", 2, argv, NULL) ? cell : NULL;
}

/*
 * Where the record for `value` of `kind` is kept: an object's cell, or a
 * primitive's key. A place with neither (a symbol, a non-object location, a
 * failed lookup) means the record is not shared.
 */
typedef struct {
    tna_intern_cell *cell;
    uint8_t *key;
    uint32_t key_len;
    uint64_t hash;
} tna_intern_place;

static tna_expr_record *tna_interned(tna_js_datamodel *dm, napi_value value, tna_expr_kind kind,
                                     tna_intern_place *place) {
    napi_env env = dm->env;
    *place = (tna_intern_place){ 0 };
    napi_value key = value;
    if (kind == TNA_EXPR_LITERAL && napi_get_named_property(env, value, "value", &key) != napi_ok) {
        napi_value error = NULL;
        napi_get_and_clear_last_exception(env, &error);
        return NULL;
    }
    napi_valuetype type = napi_undefined;
    if (napi_typeof(env, key, &type) != napi_ok) {
        return NULL;
    }
    if (type == napi_object || type == napi_function) {
        place->cell = tna_cell_of(dm, key);
        if (place->cell == NULL) {
            napi_value error = NULL;
            napi_get_and_clear_last_exception(env, &error);
            return NULL;
        }
        return place->cell->records[kind];
    }
    if (kind != TNA_EXPR_LITERAL ||
        !tna_primitive_key(env, key, type, &dm->wrapper->values, &place->key, &place->key_len)) {
        return NULL;
    }
    place->hash = tna_hash_bytes(place->key, place->key_len);
    return tna_primitive_find(&dm->primitives, place->hash, place->key, place->key_len);
}

static void tna_place_drop(tna_values *values, tna_intern_place *place) {
    if (place->key != NULL) {
        tah_free(values->allocator, &(ta_allocator_free_params){ .ptr = place->key, .size = place->key_len, .alignment = 1 });
        place->key = NULL;
    }
}

bool tna_expression_make(tna_js_datamodel *dm, napi_value value, tna_expr_kind kind, ta_expression *out) {
    tna_values *values = &dm->wrapper->values;
    tna_intern_place place;
    tna_expr_record *record = tna_interned(dm, value, kind, &place);
    if (record != NULL) {
        tna_place_drop(values, &place);
        record->references += 1u;
    } else {
        record = tah_alloc(values->allocator, sizeof *record, _Alignof(tna_expr_record));
        if (record == NULL) {
            tna_place_drop(values, &place);
            return false;
        }
        *record = (tna_expr_record){ .values = values, .dm = dm, .references = 1u, .kind = kind };
        if (napi_create_reference(dm->env, value, 1, &record->ref) != napi_ok) {
            tna_place_drop(values, &place);
            tah_free(values->allocator, &(ta_allocator_free_params){ .ptr = record, .size = sizeof *record,
                                                                    .alignment = _Alignof(tna_expr_record) });
            return false;
        }
        if (place.cell != NULL) {
            place.cell->references += 1u;
            place.cell->records[kind] = record;
            record->cell = place.cell;
        } else if (place.key != NULL) {
            record->key = place.key;
            record->key_len = place.key_len;
            record->hash = place.hash;
            if (!tna_primitive_add(&dm->primitives, values->allocator, record)) {
                /* Not shared, then: the record works, core just never matches it. */
                record->key = NULL;
                tah_free(values->allocator, &(ta_allocator_free_params){ .ptr = place.key, .size = place.key_len,
                                                                        .alignment = 1 });
            }
        }
    }
    *out = (ta_expression){ .kind = TA_EXPRESSION_HOST,
                            .as.host = { .self = NULL, .v = &tna_expr_vtable, .value = record } };
    return true;
}

static const char *const tna_expr_kind_names[] = { "function", "literal", "location" };

/*
 * A host expression as source text (napi ADR 0031): lib's print(kind,
 * value) answers the text, or an object whose `message` says why not,
 * which is kept in the printer and reported as the codec's
 * `unrepresentable`. A literal's value is the one its box holds.
 */
static ta_result tna_print_expression(void *self, const ta_expression_host *host, ta_arena *arena, ta_str *out) {
    tna_printer *printer = self;
    napi_env env = printer->dm->env;
    if (host->v != &tna_expr_vtable) {
        snprintf(printer->message, sizeof printer->message, "not a JavaScript expression");
        return TA_ERR_INVALID;
    }
    const tna_expr_record *record = host->value;
    napi_value argv[2] = { NULL, NULL };
    napi_value global = NULL;
    napi_value text = NULL;
    napi_valuetype type = napi_undefined;
    size_t length = 0;
    bool ok = napi_create_string_utf8(env, tna_expr_kind_names[record->kind], NAPI_AUTO_LENGTH, &argv[0]) == napi_ok &&
              napi_get_reference_value(env, record->ref, &argv[1]) == napi_ok &&
              (record->kind != TNA_EXPR_LITERAL || napi_get_named_property(env, argv[1], "value", &argv[1]) == napi_ok) &&
              napi_get_global(env, &global) == napi_ok &&
              napi_call_function(env, global, printer->print, 2, argv, &text) == napi_ok &&
              napi_typeof(env, text, &type) == napi_ok;
    if (!ok) {
        napi_value error = NULL;
        napi_get_and_clear_last_exception(env, &error);
        snprintf(printer->message, sizeof printer->message, "the expression could not be printed");
        return TA_ERR_INVALID;
    }
    if (type != napi_string) {
        napi_value message = NULL;
        if (type != napi_object || napi_get_named_property(env, text, "message", &message) != napi_ok ||
            napi_get_value_string_utf8(env, message, printer->message, sizeof printer->message, &length) != napi_ok) {
            snprintf(printer->message, sizeof printer->message, "the expression could not be printed");
        }
        return TA_ERR_INVALID;
    }
    if (napi_get_value_string_utf8(env, text, NULL, 0, &length) != napi_ok || length > UINT32_MAX - 1u) {
        return TA_ERR_INVALID;
    }
    char *bytes = ta_arena_alloc(arena, length + 1u, 1);
    if (bytes == NULL) {
        return TA_ERR_NOMEM;
    }
    if (napi_get_value_string_utf8(env, text, bytes, length + 1u, &length) != napi_ok) {
        return TA_ERR_INVALID;
    }
    *out = (ta_str){ bytes, (uint32_t)length };
    return TA_OK;
}

static const ta_chart_expression_printer_vtable tna_printer_vtable = { tna_print_expression };

ta_chart_expression_printer tna_printer_adapter(tna_printer *printer) {
    printer->message[0] = '\0';
    return (ta_chart_expression_printer){ .self = printer, .v = &tna_printer_vtable };
}

void tna_expression_release(ta_expression *expression) {
    if (tna_record_of(expression) != NULL) {
        tna_expr_release(NULL, expression->as.host.value, (ta_allocator){ 0 });
    }
    *expression = (ta_expression){ .kind = TA_EXPRESSION_NONE };
}

napi_value tna_expression_value(napi_env env, const ta_expression *expression) {
    const tna_expr_record *record = tna_record_of(expression);
    napi_value value = NULL;
    if (record == NULL || napi_get_reference_value(env, record->ref, &value) != napi_ok) {
        return NULL;
    }
    return value;
}

bool tna_expression_is_literal(napi_env env, napi_value value) {
    tna_env *data = tna_env_get(env);
    napi_valuetype type = napi_undefined;
    napi_value brand = NULL;
    bool has = false;
    if (data == NULL || data->literal_brand == NULL || napi_typeof(env, value, &type) != napi_ok ||
        type != napi_object || napi_get_reference_value(env, data->literal_brand, &brand) != napi_ok ||
        napi_has_property(env, value, brand, &has) != napi_ok) {
        return false;
    }
    return has;
}

/* The shared data model behind an adapter (tna_dm_adapter): every
 * operation's `self`. */
static tna_js_datamodel *tna_dm_of(void *self) {
    return ((tna_dm_adapter *)self)->dm;
}

/* ------------------------------------------------------------------------
 * Source text: `In(state)`, data names, and calls of registered functions.
 */

typedef enum {
    TNA_SOURCE_OTHER = 0,
    TNA_SOURCE_IN,
    TNA_SOURCE_NAME,
    TNA_SOURCE_CALL,
    TNA_SOURCE_JSON,
    TNA_SOURCE_NEW
} tna_source_kind;

static bool tna_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static ta_str tna_trim(ta_str text) {
    while (text.len > 0 && tna_space(text.ptr[0])) {
        text.ptr += 1;
        text.len -= 1u;
    }
    while (text.len > 0 && tna_space(text.ptr[text.len - 1u])) {
        text.len -= 1u;
    }
    return text;
}

static bool tna_identifier(ta_str text) {
    if (text.len == 0) {
        return false;
    }
    for (uint32_t at = 0; at < text.len; at += 1) {
        char c = text.ptr[at];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
        if (!letter && !(at > 0 && c >= '0' && c <= '9')) {
            return false;
        }
    }
    return true;
}

/*
 * Whether source text reads as a JSON value: it starts as a number, a
 * string, an array or an object does, or is true, false or null. The
 * builder writes a plain data value this way (napi ADR 0029), and the data
 * model parses it once, when the chart is defined; text that starts so and
 * does not parse is refused then.
 */
static bool tna_json_start(ta_str text) {
    if (text.len == 0) {
        return false;
    }
    char c = text.ptr[0];
    if (c == '-' || (c >= '0' && c <= '9') || c == '"' || c == '[' || c == '{') {
        return true;
    }
    return (text.len == 4 && (memcmp(text.ptr, "true", 4) == 0 || memcmp(text.ptr, "null", 4) == 0)) ||
           (text.len == 5 && memcmp(text.ptr, "false", 5) == 0);
}

/* `name()`, spaces allowed around the parentheses: the name, or empty. */
static ta_str tna_call_name(ta_str text) {
    if (text.len < 3 || text.ptr[text.len - 1u] != ')') {
        return (ta_str){ NULL, 0 };
    }
    ta_str inside = tna_trim((ta_str){ text.ptr, text.len - 1u });
    if (inside.len < 2 || inside.ptr[inside.len - 1u] != '(') {
        return (ta_str){ NULL, 0 };
    }
    ta_str name = tna_trim((ta_str){ inside.ptr, inside.len - 1u });
    return tna_identifier(name) ? name : (ta_str){ NULL, 0 };
}

/* Whether source text reads as `new Name(...)`: `new`, a space, and a
 * closing parenthesis at the end (napi ADR 0031); lib/values.js parses the
 * rest when the chart is defined. */
static bool tna_new_start(ta_str text) {
    return text.len > 5 && memcmp(text.ptr, "new", 3) == 0 && tna_space(text.ptr[3]) && text.ptr[text.len - 1u] == ')';
}

/*
 * Classifies source text; for In(state), `argument` is the state ID, for a
 * data name the name, for a call the function's name, and for JSON and
 * `new Name(...)` the whole text.
 */
static tna_source_kind tna_source(ta_str source, ta_str *argument) {
    ta_str text = tna_trim(source);
    if (tna_json_start(text)) {
        *argument = text;
        return TNA_SOURCE_JSON;
    }
    if (tna_new_start(text)) {
        *argument = text;
        return TNA_SOURCE_NEW;
    }
    if (tna_identifier(text)) {
        *argument = text;
        return TNA_SOURCE_NAME;
    }
    ta_str called = tna_call_name(text);
    if (called.len != 0 && !(called.len == 2 && memcmp(called.ptr, "In", 2) == 0)) {
        *argument = called;
        return TNA_SOURCE_CALL;
    }
    if (text.len < 4 || memcmp(text.ptr, "In(", 3) != 0 || text.ptr[text.len - 1u] != ')') {
        return TNA_SOURCE_OTHER;
    }
    ta_str inner = tna_trim((ta_str){ text.ptr + 3, text.len - 4u });
    if (inner.len >= 2 && (inner.ptr[0] == '\'' || inner.ptr[0] == '"') && inner.ptr[inner.len - 1u] == inner.ptr[0]) {
        inner = (ta_str){ inner.ptr + 1, inner.len - 2u };
    }
    if (inner.len == 0) {
        return TNA_SOURCE_OTHER;
    }
    *argument = inner;
    return TNA_SOURCE_IN;
}

/* ------------------------------------------------------------------------
 * Definitions and instances
 */

typedef struct {
    tna_js_datamodel *dm;
    const ta_expression *expressions;
    /*
     * A registered data model's compiled expressions, by expression ID: a
     * host expression each (ADR 0019). NULL for the built-in model, which
     * reads `expressions` as they are.
     */
    ta_expression *compiled;
    uint32_t count;
} tna_dm_shared;

typedef struct {
    ta_value root; /* the data object, or absent before init */
} tna_dm_instance;

static const char tna_source_refused[] =
    "expected a function or source text: In(), a data name, name(), new Name() or JSON";
static const char tna_call_unknown[] = "no function is registered under this name; register it with registerFunction()";
static const char tna_call_location[] = "a location cannot be a call; use a data name or { get, set }";
static const char tna_json_role[] = "a JSON value is a value; it cannot be a location or a script";
static const char tna_new_role[] = "new Name() is a value; it cannot be a location or a script";

static const char *const tna_role_names[] = { "value", "condition", "location", "script" };

static void tna_release_compiled(ta_expression *compiled, uint32_t count, ta_allocator allocator) {
    if (compiled == NULL) {
        return;
    }
    for (uint32_t index = 0; index < count; index += 1) {
        tna_expression_release(&compiled[index]);
    }
    tah_free(allocator, &(ta_allocator_free_params){ .ptr = compiled,
                                                    .size = (size_t)count * sizeof(ta_expression),
                                                    .alignment = _Alignof(ta_expression) });
}

/* A number property of `object`, when it has one. */
static bool tna_number_property(napi_env env, napi_value object, const char *name, uint32_t *out) {
    napi_value value = NULL;
    napi_valuetype type = napi_undefined;
    double number = 0;
    if (napi_get_named_property(env, object, name, &value) != napi_ok || napi_typeof(env, value, &type) != napi_ok ||
        type != napi_number || napi_get_value_double(env, value, &number) != napi_ok || !(number >= 0) ||
        number > 4294967295.0) {
        return false;
    }
    *out = (uint32_t)number;
    return true;
}

/*
 * The refusal of expression `index`: the thrown value's message, and the
 * span of the source it names when it has numeric `offset` and `length`
 * (ADR 0019 section 1). The message is kept in `dm->rejection`, which core
 * copies before it next calls the data model.
 */
static void tna_compile_refused(tna_js_datamodel *dm, const ta_datamodel_prepare_definition_params *params,
                                uint32_t index, const char *fallback) {
    napi_env env = dm->env;
    napi_value error = NULL;
    bool pending = false;
    size_t length = 0;
    uint32_t offset = 0;
    uint32_t span = 0;
    bool located = false;
    dm->rejection[0] = '\0';
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending &&
        napi_get_and_clear_last_exception(env, &error) == napi_ok && error != NULL) {
        napi_valuetype type = napi_undefined;
        napi_value message = NULL;
        if (napi_typeof(env, error, &type) == napi_ok && type == napi_object) {
            located = tna_number_property(env, error, "offset", &offset) && tna_number_property(env, error, "length", &span);
            if (napi_get_named_property(env, error, "message", &message) != napi_ok) {
                message = NULL;
            }
        } else {
            message = error;
        }
        napi_value text = NULL;
        if (message != NULL && napi_coerce_to_string(env, message, &text) == napi_ok) {
            napi_get_value_string_utf8(env, text, dm->rejection, dm->rejection_bytes, &length);
        } else {
            napi_get_and_clear_last_exception(env, &error);
        }
    }
    if (length == 0) {
        length = strlen(fallback);
        if (length >= dm->rejection_bytes) {
            length = dm->rejection_bytes - 1u;
        }
        memcpy(dm->rejection, fallback, length);
        dm->rejection[length] = '\0';
    }
    if (params->rejected != NULL) {
        *params->rejected = (ta_expression_rejection){
            .expression = index,
            .offset = located ? offset : 0,
            .length = located ? span : 0,
            .message = { dm->rejection, (uint32_t)length },
        };
    }
}

/*
 * A registered data model compiles every source expression with the user's
 * compiler (ADR 0019): compile(source, role) returns a function for a value,
 * a condition or a script, and a { get, set } location for a location. lib/
 * normalizes a data name into a location before the result gets here.
 */
static ta_result tna_compile_all(tna_dm_adapter *adapter, const ta_datamodel_prepare_definition_params *params,
                                 ta_expression **out) {
    tna_js_datamodel *dm = adapter->dm;
    napi_env env = dm->env;
    *out = NULL;
    if (params->expression_count == 0) {
        return TA_OK;
    }
    if (!dm->js_usable) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_expression *compiled = tah_alloc(params->allocator, (size_t)params->expression_count * sizeof(ta_expression),
                                        _Alignof(ta_expression));
    if (compiled == NULL) {
        return TA_ERR_NOMEM;
    }
    for (uint32_t index = 0; index < params->expression_count; index += 1) {
        compiled[index] = (ta_expression){ .kind = TA_EXPRESSION_NONE };
    }
    napi_handle_scope scope = NULL;
    if (napi_open_handle_scope(env, &scope) != napi_ok) {
        tna_release_compiled(compiled, params->expression_count, params->allocator);
        return TA_ERR_NOMEM;
    }
    napi_value compile = NULL;
    napi_value undefined = NULL;
    ta_result result = napi_get_reference_value(env, adapter->compile, &compile) == napi_ok && compile != NULL &&
                               napi_get_undefined(env, &undefined) == napi_ok
                           ? TA_OK
                           : TA_ERR_NOMEM;
    for (uint32_t index = 0; result == TA_OK && index < params->expression_count; index += 1) {
        const ta_expression *expression = &params->expressions[index];
        if (expression->kind != TA_EXPRESSION_SOURCE) {
            continue;
        }
        ta_expr_role role = params->roles != NULL ? params->roles[index] : TA_EXPR_ROLE_VALUE;
        napi_value argv[2] = { NULL, NULL };
        napi_value made = NULL;
        napi_valuetype type = napi_undefined;
        if (napi_create_string_utf8(env, expression->as.source.ptr, expression->as.source.len, &argv[0]) != napi_ok ||
            napi_create_string_utf8(env, tna_role_names[role <= TA_EXPR_ROLE_SCRIPT ? role : 0], NAPI_AUTO_LENGTH,
                                    &argv[1]) != napi_ok) {
            result = TA_ERR_NOMEM;
        } else if (napi_call_function(env, undefined, compile, 2, argv, &made) != napi_ok) {
            tna_compile_refused(dm, params, index, "the compiler refused this expression");
            result = TA_ERR_INVALID;
        } else if (napi_typeof(env, made, &type) != napi_ok ||
                   (role == TA_EXPR_ROLE_LOCATION ? type != napi_object : type != napi_function)) {
            tna_compile_refused(dm, params, index,
                                role == TA_EXPR_ROLE_LOCATION ? "compile() must return a { get, set } location here"
                                                              : "compile() must return a function here");
            result = TA_ERR_INVALID;
        } else if (!tna_expression_make(dm, made, role == TA_EXPR_ROLE_LOCATION ? TNA_EXPR_LOCATION : TNA_EXPR_FUNCTION,
                                        &compiled[index])) {
            result = TA_ERR_NOMEM;
        }
    }
    napi_close_handle_scope(env, scope);
    if (result != TA_OK) {
        tna_release_compiled(compiled, params->expression_count, params->allocator);
        return result;
    }
    *out = compiled;
    return TA_OK;
}

static void tna_reject(const ta_datamodel_prepare_definition_params *params, uint32_t index, const char *message,
                       size_t length) {
    if (params->rejected != NULL) {
        *params->rejected = (ta_expression_rejection){ .expression = index, .message = { message, (uint32_t)length } };
    }
}

/*
 * The function registered under `name`, or NULL (napi ADR 0029). The table
 * is a plain object; own properties only, so nothing on its prototype is a
 * function a chart can call.
 */
static napi_value tna_registered_function(tna_js_datamodel *dm, ta_str name) {
    napi_env env = dm->env;
    napi_value table = NULL;
    napi_value key = NULL;
    napi_value run = NULL;
    bool has = false;
    if (dm->functions == NULL || napi_get_reference_value(env, dm->functions, &table) != napi_ok || table == NULL ||
        napi_create_string_utf8(env, name.ptr, name.len, &key) != napi_ok ||
        napi_has_own_property(env, table, key, &has) != napi_ok || !has ||
        napi_get_property(env, table, key, &run) != napi_ok) {
        return NULL;
    }
    return run;
}

/*
 * JSON text parsed once, boxed as a literal() is ({ value }), or NULL when
 * it does not parse (the exception is cleared).
 */
static napi_value tna_parsed_literal(tna_js_datamodel *dm, ta_str text) {
    napi_env env = dm->env;
    napi_value parse = NULL;
    napi_value global = NULL;
    napi_value source = NULL;
    napi_value value = NULL;
    napi_value box = NULL;
    if (napi_get_reference_value(env, dm->json_parse, &parse) != napi_ok || napi_get_global(env, &global) != napi_ok ||
        napi_create_string_utf8(env, text.ptr, text.len, &source) != napi_ok) {
        return NULL;
    }
    if (napi_call_function(env, global, parse, 1, &source, &value) != napi_ok) {
        bool pending = false;
        napi_value ignored = NULL;
        if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
            napi_get_and_clear_last_exception(env, &ignored);
        }
        return NULL;
    }
    if (napi_create_object(env, &box) != napi_ok || napi_set_named_property(env, box, "value", value) != napi_ok) {
        return NULL;
    }
    return box;
}

/*
 * `new Name(...)` as a function that builds its value at each evaluation
 * (lib/values.js, napi ADR 0031), or NULL with the refusal's message in
 * `dm->rejection` (the exception is cleared).
 */
static napi_value tna_construction(tna_js_datamodel *dm, ta_str text) {
    napi_env env = dm->env;
    tna_env *data = tna_env_get(env);
    napi_value construct = NULL;
    napi_value global = NULL;
    napi_value argv[2] = { NULL, NULL };
    napi_value run = NULL;
    dm->rejection[0] = '\0';
    if (data == NULL || data->construct == NULL || napi_get_reference_value(env, data->construct, &construct) != napi_ok ||
        napi_get_global(env, &global) != napi_ok || napi_create_string_utf8(env, text.ptr, text.len, &argv[0]) != napi_ok ||
        dm->classes == NULL || napi_get_reference_value(env, dm->classes, &argv[1]) != napi_ok) {
        return NULL;
    }
    if (napi_call_function(env, global, construct, 2, argv, &run) == napi_ok) {
        return run;
    }
    napi_value error = NULL;
    napi_value message = NULL;
    size_t length = 0;
    if (napi_get_and_clear_last_exception(env, &error) == napi_ok && error != NULL &&
        napi_get_named_property(env, error, "message", &message) == napi_ok &&
        napi_get_value_string_utf8(env, message, dm->rejection, dm->rejection_bytes, &length) != napi_ok) {
        dm->rejection[0] = '\0';
    }
    return NULL;
}

/*
 * The built-in model's check of a definition's expressions: source text in
 * its forms and its own host expressions. Each call `name()` is bound here
 * to the function registered under the name, and each JSON value parsed
 * into a literal, as host expressions in `compiled` (allocated on the first
 * one), which the data model then runs as it runs a builder function or a
 * literal().
 */
static ta_result tna_bind_builtin(tna_js_datamodel *dm, const ta_datamodel_prepare_definition_params *params,
                                  ta_expression **out) {
    *out = NULL;
    ta_expression *compiled = NULL;
    napi_handle_scope scope = NULL;
    ta_result result = TA_OK;
    for (uint32_t index = 0; result == TA_OK && index < params->expression_count; index += 1) {
        const ta_expression *expression = &params->expressions[index];
        ta_str argument = { NULL, 0 };
        if (expression->kind == TA_EXPRESSION_HOST) {
            if (tna_record_of(expression) == NULL) {
                tna_reject(params, index, tna_source_refused, sizeof tna_source_refused - 1u);
                result = TA_ERR_UNSUPPORTED;
            }
            continue;
        }
        if (expression->kind != TA_EXPRESSION_SOURCE) {
            continue;
        }
        tna_source_kind kind = tna_source(expression->as.source, &argument);
        if (kind == TNA_SOURCE_OTHER) {
            tna_reject(params, index, tna_source_refused, sizeof tna_source_refused - 1u);
            result = TA_ERR_INVALID;
            continue;
        }
        if (kind != TNA_SOURCE_CALL && kind != TNA_SOURCE_JSON && kind != TNA_SOURCE_NEW) {
            continue;
        }
        ta_expr_role role = params->roles != NULL ? params->roles[index] : TA_EXPR_ROLE_VALUE;
        if (kind == TNA_SOURCE_CALL && role == TA_EXPR_ROLE_LOCATION) {
            tna_reject(params, index, tna_call_location, sizeof tna_call_location - 1u);
            result = TA_ERR_INVALID;
            continue;
        }
        if (kind == TNA_SOURCE_JSON && (role == TA_EXPR_ROLE_LOCATION || role == TA_EXPR_ROLE_SCRIPT)) {
            tna_reject(params, index, tna_json_role, sizeof tna_json_role - 1u);
            result = TA_ERR_INVALID;
            continue;
        }
        if (kind == TNA_SOURCE_NEW && (role == TA_EXPR_ROLE_LOCATION || role == TA_EXPR_ROLE_SCRIPT)) {
            tna_reject(params, index, tna_new_role, sizeof tna_new_role - 1u);
            result = TA_ERR_INVALID;
            continue;
        }
        if (!dm->js_usable) {
            result = TA_ERR_UNSUPPORTED;
            continue;
        }
        if (scope == NULL && napi_open_handle_scope(dm->env, &scope) != napi_ok) {
            scope = NULL;
            result = TA_ERR_NOMEM;
            continue;
        }
        napi_value run = NULL;
        bool literal = false;
        if (kind == TNA_SOURCE_JSON) {
            run = tna_parsed_literal(dm, argument);
            literal = run != NULL;
            if (run == NULL && argument.ptr[0] != '[' && argument.ptr[0] != '{') {
                tna_reject(params, index, tna_source_refused, sizeof tna_source_refused - 1u);
                result = TA_ERR_INVALID;
                continue;
            }
        }
        if (run != NULL) {
            /* JSON, parsed once into a literal. */
        } else if (kind == TNA_SOURCE_NEW || kind == TNA_SOURCE_JSON) {
            /* An array or object with `new Name(...)` inside builds as
             * `new Name(...)` does (napi ADR 0031). */
            run = tna_construction(dm, argument);
            if (run == NULL) {
                if (dm->rejection[0] != '\0') {
                    tna_reject(params, index, dm->rejection, strlen(dm->rejection));
                    result = TA_ERR_INVALID;
                } else {
                    result = TA_ERR_NOMEM;
                }
                continue;
            }
        } else if ((run = tna_registered_function(dm, argument)) == NULL) {
            tna_reject(params, index, tna_call_unknown, sizeof tna_call_unknown - 1u);
            result = TA_ERR_INVALID;
            continue;
        }
        if (compiled == NULL) {
            compiled = tah_alloc(params->allocator, (size_t)params->expression_count * sizeof(ta_expression),
                                 _Alignof(ta_expression));
            if (compiled == NULL) {
                result = TA_ERR_NOMEM;
                continue;
            }
            for (uint32_t at = 0; at < params->expression_count; at += 1) {
                compiled[at] = (ta_expression){ .kind = TA_EXPRESSION_NONE };
            }
        }
        if (!tna_expression_make(dm, run, literal ? TNA_EXPR_LITERAL : TNA_EXPR_FUNCTION, &compiled[index])) {
            result = TA_ERR_NOMEM;
        }
    }
    if (scope != NULL) {
        napi_close_handle_scope(dm->env, scope);
    }
    if (result != TA_OK) {
        tna_release_compiled(compiled, params->expression_count, params->allocator);
        return result;
    }
    *out = compiled;
    return TA_OK;
}

static ta_result tna_dm_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    tna_dm_adapter *adapter = self;
    tna_js_datamodel *dm = adapter->dm;
    ta_expression *compiled = NULL;
    ta_result result = adapter->compile != NULL ? tna_compile_all(adapter, params, &compiled)
                                                : tna_bind_builtin(dm, params, &compiled);
    if (result != TA_OK) {
        return result;
    }
    tna_dm_shared *shared = tah_alloc(params->allocator, sizeof *shared, _Alignof(tna_dm_shared));
    if (shared == NULL) {
        tna_release_compiled(compiled, params->expression_count, params->allocator);
        return TA_ERR_NOMEM;
    }
    *shared = (tna_dm_shared){
        .dm = dm, .expressions = params->expressions, .compiled = compiled, .count = params->expression_count
    };
    *params->shared = shared;
    return TA_OK;
}

static void tna_dm_destroy_definition(void *self, void *shared, ta_allocator allocator) {
    (void)self;
    if (shared != NULL) {
        tna_dm_shared *data = shared;
        tna_release_compiled(data->compiled, data->count, allocator);
        tah_free(allocator,
                 &(ta_allocator_free_params){ .ptr = shared, .size = sizeof(tna_dm_shared), .alignment = _Alignof(tna_dm_shared) });
    }
}

static uint32_t tna_dm_instance_size(void *self, void *shared) {
    (void)self;
    (void)shared;
    return (uint32_t)sizeof(tna_dm_instance);
}

static uint32_t tna_dm_instance_alignment(void *self, void *shared) {
    (void)self;
    (void)shared;
    return (uint32_t)_Alignof(tna_dm_instance);
}

/* ------------------------------------------------------------------------
 * One callback: a handle scope, the call in progress, and faults.
 */

typedef struct {
    tna_js_datamodel *dm;
    tna_dm_instance *instance;
    ta_fault *fault;
    napi_handle_scope scope;
    const ta_dm_host *saved_host;
    void *saved_instance;
    napi_ref saved_event;
    bool saved_text_failed;
} tna_frame;

static void tna_fault(tna_frame *frame, const char *message) {
    if (frame->fault == NULL) {
        return;
    }
    *frame->fault = (ta_fault){ .code = TA_ERR_INVALID, .message = { message, (uint32_t)strlen(message) } };
    napi_env env = frame->dm->env;
    bool pending = false;
    napi_value error = NULL;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending &&
        napi_get_and_clear_last_exception(env, &error) == napi_ok && error != NULL) {
        ta_value data;
        if (tna_value_make(&frame->dm->wrapper->values, error, &data)) {
            frame->fault->data = data;
        }
    }
}

static ta_op_status tna_fail(tna_frame *frame, const char *message) {
    tna_fault(frame, message);
    return TA_OP_ERROR;
}

static bool tna_begin(tna_frame *frame, tna_js_datamodel *dm, void *instance, const ta_dm_host *host, ta_fault *fault) {
    *frame = (tna_frame){ .dm = dm, .instance = instance, .fault = fault };
    if (!dm->js_usable) {
        tna_fault(frame, "javascript: no JavaScript can run while the system closes");
        return false;
    }
    if (napi_open_handle_scope(dm->env, &frame->scope) != napi_ok) {
        tna_fault(frame, "javascript: cannot open a handle scope");
        return false;
    }
    frame->saved_host = dm->host;
    frame->saved_instance = dm->instance;
    frame->saved_event = dm->event;
    frame->saved_text_failed = dm->text_failed;
    dm->host = host;
    dm->instance = instance;
    dm->event = NULL;
    dm->text_failed = false;
    return true;
}

static void tna_end(tna_frame *frame) {
    tna_js_datamodel *dm = frame->dm;
    if (dm->event != NULL) {
        napi_delete_reference(dm->env, dm->event);
    }
    dm->host = frame->saved_host;
    dm->instance = frame->saved_instance;
    dm->event = frame->saved_event;
    dm->text_failed = frame->saved_text_failed;
    napi_close_handle_scope(dm->env, frame->scope);
}

static bool tna_root(tna_frame *frame, napi_value *out) {
    return frame->instance != NULL && tna_value_get(frame->dm->env, &frame->instance->root, out);
}

static bool tna_context(tna_frame *frame, napi_value *out) {
    return napi_get_reference_value(frame->dm->env, frame->dm->context, out) == napi_ok && *out != NULL;
}

/*
 * Calls into JavaScript. A call during which core could not render a system
 * variable fails, even when the chart's code caught the getter's exception.
 */
static bool tna_js_call(tna_frame *frame, napi_value self, napi_value function, size_t argc, const napi_value *argv,
                        napi_value *result) {
    napi_value ignored = NULL;
    return napi_call_function(frame->dm->env, self, function, argc, argv, result == NULL ? &ignored : result) ==
               napi_ok &&
           !frame->dm->text_failed;
}

/* Calls `function` with `this` = the root and (args..., context). */
static bool tna_invoke(tna_frame *frame, napi_value function, size_t argc, const napi_value *args, napi_value *result) {
    napi_value argv[3];
    napi_value root = NULL;
    if (argc > 2 || !tna_root(frame, &root) || !tna_context(frame, &argv[argc])) {
        return false;
    }
    for (size_t at = 0; at < argc; at += 1) {
        argv[at] = args[at];
    }
    return tna_js_call(frame, root, function, argc + 1u, argv, result);
}

/* Expression `id` as the data model runs it: compiled when it was (ADR 0019). */
static const ta_expression *tna_expression(const tna_dm_shared *shared, ta_expr_id id) {
    if (shared == NULL || id >= shared->count) {
        return NULL;
    }
    if (shared->compiled != NULL && shared->compiled[id].kind == TA_EXPRESSION_HOST) {
        return &shared->compiled[id];
    }
    return &shared->expressions[id];
}

static bool tna_named_property(tna_frame *frame, ta_str name, napi_value *key) {
    return napi_create_string_utf8(frame->dm->env, name.ptr, name.len, key) == napi_ok;
}

/* root[name] */
static bool tna_read_name(tna_frame *frame, ta_str name, napi_value *out) {
    napi_value root = NULL;
    napi_value key = NULL;
    return tna_root(frame, &root) && tna_named_property(frame, name, &key) &&
           napi_get_property(frame->dm->env, root, key, out) == napi_ok;
}

/* In(state): false for a state the chart does not have (SCXML B.1). */
static bool tna_in_state(const ta_dm_host *host, ta_str id) {
    ta_state_id state = 0;
    return ta_dm_host_find_state(host, id, &state) == TA_OK && ta_dm_host_in_state(host, state);
}

/*
 * Evaluates expression `id` as a value. A function is called; a literal is
 * its value; a location is read; a data name reads the root; In(state) is a
 * boolean.
 */
static bool tna_evaluate(tna_frame *frame, const tna_dm_shared *shared, ta_expr_id id, napi_value *out) {
    napi_env env = frame->dm->env;
    const ta_expression *expression = tna_expression(shared, id);
    if (expression == NULL) {
        return false;
    }
    if (expression->kind == TA_EXPRESSION_SOURCE) {
        ta_str argument = { NULL, 0 };
        switch (tna_source(expression->as.source, &argument)) {
        case TNA_SOURCE_NAME:
            return tna_read_name(frame, argument, out);
        case TNA_SOURCE_IN:
            return napi_get_boolean(env, tna_in_state(frame->dm->host, argument), out) == napi_ok;
        default:
            return false;
        }
    }
    const tna_expr_record *record = tna_record_of(expression);
    napi_value value = tna_expression_value(env, expression);
    if (record == NULL || value == NULL) {
        return false;
    }
    switch (record->kind) {
    case TNA_EXPR_FUNCTION:
        return tna_invoke(frame, value, 0, NULL, out);
    case TNA_EXPR_LITERAL:
        return napi_get_named_property(env, value, "value", out) == napi_ok;
    case TNA_EXPR_LOCATION: {
        napi_value get = NULL;
        return napi_get_named_property(env, value, "get", &get) == napi_ok && tna_invoke(frame, get, 0, NULL, out);
    }
    }
    return false;
}

/* ------------------------------------------------------------------------
 * The vtable
 */

static ta_result tna_dm_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    tna_js_datamodel *dm = tna_dm_of(self);
    tna_dm_instance *instance = params->instance;
    *instance = (tna_dm_instance){ .root = { NULL, NULL } };
    if (params->binding != NULL && params->binding->v != NULL) {
        instance->root = *params->binding;
        ta_value_retain(&instance->root);
        return TA_OK;
    }
    if (!dm->js_usable) {
        return TA_ERR_UNSUPPORTED;
    }
    napi_handle_scope scope = NULL;
    napi_value root = NULL;
    if (napi_open_handle_scope(dm->env, &scope) != napi_ok) {
        return TA_ERR_NOMEM;
    }
    bool made = napi_create_object(dm->env, &root) == napi_ok && tna_value_make(&dm->wrapper->values, root, &instance->root);
    napi_close_handle_scope(dm->env, scope);
    return made ? TA_OK : TA_ERR_NOMEM;
}

static void tna_dm_destroy_instance(void *self, void *shared, void *instance) {
    (void)self;
    (void)shared;
    tna_dm_instance *data = instance;
    ta_value_release(&data->root);
    data->root = (ta_value){ NULL, NULL };
}

static ta_op_status tna_dm_declare(void *self, const ta_datamodel_declare_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, tna_dm_of(self), params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = frame.dm->env;
    napi_value root = NULL;
    napi_value key = NULL;
    napi_value undefined = NULL;
    bool has = false;
    ta_op_status status = TA_OP_OK;
    if (!tna_root(&frame, &root) || !tna_named_property(&frame, params->name, &key) ||
        napi_has_own_property(env, root, key, &has) != napi_ok) {
        status = tna_fail(&frame, "javascript: could not declare data");
    } else if (has) {
        if (params->bound != NULL) {
            *params->bound = true;
        }
    } else if (napi_get_undefined(env, &undefined) != napi_ok || napi_set_property(env, root, key, undefined) != napi_ok) {
        status = tna_fail(&frame, "javascript: could not declare data");
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_set_named(void *self, const ta_datamodel_set_named_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, tna_dm_of(self), params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = frame.dm->env;
    napi_value root = NULL;
    napi_value key = NULL;
    napi_value value = NULL;
    ta_op_status status = TA_OP_OK;
    if (!tna_value_get(env, params->value, &value)) {
        status = tna_fail(&frame, "javascript: data value is not a JavaScript value");
    } else if (!tna_root(&frame, &root) || !tna_named_property(&frame, params->name, &key) ||
               napi_set_property(env, root, key, value) != napi_ok) {
        status = tna_fail(&frame, "javascript: could not set data");
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_eval_condition(void *self, const ta_datamodel_eval_condition_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, tna_dm_of(self), params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_value value = NULL;
    napi_value flag = NULL;
    ta_op_status status = TA_OP_OK;
    if (!tna_evaluate(&frame, params->shared, params->expr, &value) ||
        napi_coerce_to_bool(frame.dm->env, value, &flag) != napi_ok ||
        napi_get_value_bool(frame.dm->env, flag, params->out) != napi_ok) {
        status = tna_fail(&frame, "javascript: condition threw");
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_eval_value(void *self, const ta_datamodel_eval_value_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, tna_dm_of(self), params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_value value = NULL;
    ta_op_status status = TA_OP_OK;
    if (!tna_evaluate(&frame, params->shared, params->expr, &value)) {
        status = tna_fail(&frame, "javascript: expression threw");
    } else if (!tna_value_make(&frame.dm->wrapper->values, value, params->out)) {
        status = tna_fail(&frame, "javascript: cannot keep the value");
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_read_location(void *self, const ta_datamodel_read_location_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, tna_dm_of(self), params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    const ta_expression *expression = tna_expression(params->shared, params->location);
    const tna_expr_record *record = tna_record_of(expression);
    ta_str name = { NULL, 0 };
    napi_value value = NULL;
    ta_op_status status = TA_OP_OK;
    bool location = record != NULL ? record->kind == TNA_EXPR_LOCATION
                                   : expression != NULL && expression->kind == TA_EXPRESSION_SOURCE &&
                                         tna_source(expression->as.source, &name) == TNA_SOURCE_NAME;
    if (!location) {
        status = tna_fail(&frame, "javascript: not a location");
    } else if (!tna_evaluate(&frame, params->shared, params->location, &value)) {
        status = tna_fail(&frame, "javascript: location threw");
    } else if (!tna_value_make(&frame.dm->wrapper->values, value, params->out)) {
        status = tna_fail(&frame, "javascript: cannot keep the value");
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_assign(void *self, const ta_datamodel_assign_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, tna_dm_of(self), params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = frame.dm->env;
    const ta_expression *expression = tna_expression(params->shared, params->location);
    const tna_expr_record *record = tna_record_of(expression);
    napi_value value = NULL;
    ta_op_status status = TA_OP_OK;
    if (!tna_value_get(env, params->value, &value)) {
        status = tna_fail(&frame, "javascript: assigned value is not a JavaScript value");
    } else if (record != NULL && record->kind == TNA_EXPR_LOCATION) {
        napi_value location = tna_expression_value(env, expression);
        napi_value set = NULL;
        if (location == NULL || napi_get_named_property(env, location, "set", &set) != napi_ok ||
            !tna_invoke(&frame, set, 1, &value, NULL)) {
            status = tna_fail(&frame, "javascript: assign threw");
        }
    } else {
        ta_str name = { NULL, 0 };
        napi_value root = NULL;
        napi_value key = NULL;
        bool has = false;
        if (record != NULL || expression == NULL || expression->kind != TA_EXPRESSION_SOURCE ||
            tna_source(expression->as.source, &name) != TNA_SOURCE_NAME) {
            status = tna_fail(&frame, "javascript: not a location");
        } else if (!tna_root(&frame, &root) || !tna_named_property(&frame, name, &key) ||
                   napi_has_own_property(env, root, key, &has) != napi_ok) {
            status = tna_fail(&frame, "javascript: invalid location");
        } else if (!has) {
            status = tna_fail(&frame, "javascript: assignment to an undeclared data name");
        } else if (napi_set_property(env, root, key, value) != napi_ok) {
            status = tna_fail(&frame, "javascript: assign threw");
        }
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_execute_script(void *self, const ta_datamodel_execute_script_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, tna_dm_of(self), params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    const ta_expression *expression = tna_expression(params->shared, params->script);
    const tna_expr_record *record = tna_record_of(expression);
    napi_value function = tna_expression_value(frame.dm->env, expression);
    ta_op_status status = TA_OP_OK;
    if (record == NULL || record->kind != TNA_EXPR_FUNCTION || function == NULL) {
        status = tna_fail(&frame, "javascript: a script must be a function");
    } else if (!tna_invoke(&frame, function, 0, NULL, NULL)) {
        status = tna_fail(&frame, "javascript: script threw");
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_make_params(void *self, const ta_datamodel_make_params_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, tna_dm_of(self), params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = frame.dm->env;
    napi_value object = NULL;
    ta_op_status status = TA_OP_OK;
    if (napi_create_object(env, &object) != napi_ok) {
        status = tna_fail(&frame, "javascript: could not make params");
    }
    for (uint32_t at = 0; status == TA_OP_OK && at < params->count; at += 1) {
        napi_value key = NULL;
        napi_value value = NULL;
        if (!tna_value_get(env, &params->values[at].value, &value)) {
            status = tna_fail(&frame, "javascript: parameter is not a JavaScript value");
        } else if (!tna_named_property(&frame, params->values[at].name, &key) ||
                   napi_set_property(env, object, key, value) != napi_ok) {
            status = tna_fail(&frame, "javascript: could not make params");
        }
    }
    if (status == TA_OP_OK && !tna_value_make(&frame.dm->wrapper->values, object, params->out)) {
        status = tna_fail(&frame, "javascript: cannot keep the params");
    }
    tna_end(&frame);
    return status;
}

static ta_result tna_dm_inspect(void *self, const ta_datamodel_inspect_params *params) {
    (void)self;
    const tna_dm_instance *instance = params->instance;
    *params->out = instance->root;
    ta_value_retain(params->out);
    return TA_OK;
}

static ta_op_status tna_dm_value_text(void *self, const ta_datamodel_value_text_params *params) {
    tna_js_datamodel *dm = tna_dm_of(self);
    if (!tna_value_ours(params->value)) {
        *params->fault = (ta_fault){ .code = TA_ERR_UNSUPPORTED,
                                     .message = { "javascript: not a JavaScript value", 34 } };
        return TA_OP_ERROR;
    }
    tna_frame frame;
    if (!tna_begin(&frame, dm, params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    /* Only copying the text can run out of memory, and that changes nothing
     * the chart can see: core stops and asks again (core ADR 0070). */
    ta_result result = tna_value_text(&dm->wrapper->values, params->value, params->out);
    ta_op_status status = result == TA_OK      ? TA_OP_OK
                          : result == TA_ERR_NOMEM ? TA_OP_NOMEM
                                                   : tna_fail(&frame, "javascript: value has no text");
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_text_value(void *self, const ta_datamodel_text_value_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, tna_dm_of(self), params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_value text = NULL;
    ta_op_status status = TA_OP_OK;
    if (napi_create_string_utf8(frame.dm->env, params->text.ptr, params->text.len, &text) != napi_ok ||
        !tna_value_make(&frame.dm->wrapper->values, text, params->out)) {
        status = tna_fail(&frame, "javascript: could not make text");
    }
    tna_end(&frame);
    return status;
}

/* Literal content: JSON when it parses, otherwise the text (a string). */
/*
 * Content that is not JSON is a space-normalized string (SCXML B.2.2): runs
 * of XML whitespace become one space, and none leads or trails.
 * TA_OP_NOMEM when the copy cannot be allocated, which changes nothing.
 */
static ta_op_status tna_space_normalized(napi_env env, tna_values *values, ta_bytes content, napi_value *out) {
    char *normal = tah_alloc(values->allocator, content.len == 0 ? 1u : content.len, 1);
    if (normal == NULL) {
        return TA_OP_NOMEM;
    }
    size_t length = 0;
    bool space = false;
    for (uint32_t at = 0; at < content.len; at += 1) {
        char c = (char)content.ptr[at];
        if (tna_space(c)) {
            space = length != 0;
            continue;
        }
        if (space) {
            normal[length++] = ' ';
            space = false;
        }
        normal[length++] = c;
    }
    napi_status status = napi_create_string_utf8(env, normal, length, out);
    tah_free(values->allocator,
             &(ta_allocator_free_params){ .ptr = normal, .size = content.len == 0 ? 1u : content.len, .alignment = 1 });
    return status == napi_ok ? TA_OP_OK : TA_OP_ERROR;
}

static ta_op_status tna_dm_import_content(void *self, const ta_datamodel_import_content_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, tna_dm_of(self), params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = frame.dm->env;
    napi_value text = NULL;
    napi_value parse = NULL;
    napi_value global = NULL;
    napi_value value = NULL;
    ta_op_status status = TA_OP_OK;
    if (napi_create_string_utf8(env, (const char *)params->content.ptr, params->content.len, &text) != napi_ok) {
        status = tna_fail(&frame, "javascript: content is not text");
    } else {
        if (napi_get_reference_value(env, frame.dm->json_parse, &parse) != napi_ok ||
            napi_get_global(env, &global) != napi_ok ||
            napi_call_function(env, global, parse, 1, &text, &value) != napi_ok) {
            napi_value ignored = NULL;
            bool pending = false;
            if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
                napi_get_and_clear_last_exception(env, &ignored);
            }
            status = tna_space_normalized(env, &frame.dm->wrapper->values, params->content, &value);
            if (status == TA_OP_ERROR) {
                status = tna_fail(&frame, "javascript: content is not text");
            }
        }
        if (status == TA_OP_OK && !tna_value_make(&frame.dm->wrapper->values, value, params->out)) {
            status = tna_fail(&frame, "javascript: could not import content");
        }
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_foreach_snapshot(void *self, const ta_datamodel_foreach_snapshot_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, tna_dm_of(self), params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = frame.dm->env;
    napi_value array = NULL;
    napi_value from = NULL;
    napi_value global = NULL;
    napi_value snapshot = NULL;
    uint32_t length = 0;
    bool is_array = false;
    ta_op_status status = TA_OP_OK;
    if (!tna_evaluate(&frame, params->shared, params->array, &array)) {
        status = tna_fail(&frame, "javascript: foreach array threw");
    } else if (napi_is_array(env, array, &is_array) != napi_ok || !is_array) {
        status = tna_fail(&frame, "javascript: foreach array is not an array");
    } else if (napi_get_reference_value(env, frame.dm->array_from, &from) != napi_ok ||
               napi_get_global(env, &global) != napi_ok ||
               !tna_js_call(&frame, global, from, 1, &array, &snapshot) ||
               napi_get_array_length(env, snapshot, &length) != napi_ok) {
        status = tna_fail(&frame, "javascript: cannot copy the foreach array");
    } else if (!tna_value_make(&frame.dm->wrapper->values, snapshot, params->out)) {
        status = tna_fail(&frame, "javascript: cannot keep the foreach array");
    } else {
        *params->count = length;
    }
    tna_end(&frame);
    return status;
}

/*
 * A variable name, as SCXML 4.6 requires of <foreach>'s `item` and `index`:
 * an identifier, [A-Za-z_$] then [A-Za-z0-9_$]. A byte past ASCII is taken
 * as part of a Unicode identifier.
 */
static bool tna_variable_name(ta_str name) {
    if (name.len == 0) {
        return false;
    }
    for (uint32_t at = 0; at < name.len; at += 1) {
        unsigned char c = (unsigned char)name.ptr[at];
        bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80u;
        if (!letter && !(at != 0 && c >= '0' && c <= '9')) {
            return false;
        }
    }
    return true;
}

static ta_op_status tna_dm_foreach_bind(void *self, const ta_datamodel_foreach_bind_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, tna_dm_of(self), params->instance, params->host, params->fault)) {
        return TA_OP_ERROR;
    }
    /* Checked before anything is bound, so a bad name changes nothing. */
    if (!tna_variable_name(params->item) ||
        (params->index_name.ptr != NULL && !tna_variable_name(params->index_name))) {
        ta_op_status refused = tna_fail(&frame, "javascript: a <foreach> item or index is not a variable name");
        tna_end(&frame);
        return refused;
    }
    napi_env env = frame.dm->env;
    napi_value snapshot = NULL;
    napi_value item = NULL;
    napi_value root = NULL;
    napi_value key = NULL;
    ta_op_status status = TA_OP_OK;
    if (!tna_value_get(env, params->snapshot, &snapshot) || napi_get_element(env, snapshot, params->index, &item) != napi_ok ||
        !tna_root(&frame, &root) || !tna_named_property(&frame, params->item, &key) ||
        napi_set_property(env, root, key, item) != napi_ok) {
        status = tna_fail(&frame, "javascript: cannot bind the foreach item");
    } else if (params->index_name.ptr != NULL) {
        napi_value index = NULL;
        if (!tna_named_property(&frame, params->index_name, &key) ||
            napi_create_uint32(env, params->index, &index) != napi_ok || napi_set_property(env, root, key, index) != napi_ok) {
            status = tna_fail(&frame, "javascript: cannot bind the foreach index");
        }
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_value_write(void *self, const ta_datamodel_value_write_params *params);
static ta_op_status tna_dm_value_read(void *self, const ta_datamodel_value_read_params *params);

static const ta_datamodel_vtable tna_dm_vtable = {
    .name = { "javascript", 10 },
    .prepare_definition = tna_dm_prepare,
    .destroy_definition = tna_dm_destroy_definition,
    .instance_size = tna_dm_instance_size,
    .instance_alignment = tna_dm_instance_alignment,
    .init_instance = tna_dm_init_instance,
    .destroy_instance = tna_dm_destroy_instance,
    .declare = tna_dm_declare,
    .set_named = tna_dm_set_named,
    .eval_condition = tna_dm_eval_condition,
    .eval_value = tna_dm_eval_value,
    .read_location = tna_dm_read_location,
    .assign = tna_dm_assign,
    .execute_script = tna_dm_execute_script,
    .make_params = tna_dm_make_params,
    .inspect = tna_dm_inspect,
    .value_text = tna_dm_value_text,
    .text_value = tna_dm_text_value,
    .import_content = tna_dm_import_content,
    .foreach_snapshot = tna_dm_foreach_snapshot,
    .foreach_bind = tna_dm_foreach_bind,
    .value_write = tna_dm_value_write,
    .value_read = tna_dm_value_read,
};

/* ------------------------------------------------------------------------
 * A value as bytes (core ADR 0090 section 2, napi ADR 0034 section 2)
 *
 * durable=false, for offloading (core ADR 0074): a tag, then either 'S' and
 * v8.serialize's bytes, which v8.deserialize reads back as a new object, so
 * the value leaves the JavaScript heap; or 'H' and the value box itself,
 * retained, for a value serialization refuses (a function, a class instance
 * with private state) or when the system keeps every value by handle. A
 * handle keeps the object alive: it frees only core's part of the actor.
 *
 * durable=true: the built-in data model's text (ADR 0031), which
 * lib/durable.js writes in pieces from an explicit-stack walk and parses as
 * pieces arrive. Each piece crosses as a string and leaves as UTF-8, so no
 * side holds the whole text.
 */

#define TNA_OFFLOAD_SERIALIZED 'S'
#define TNA_OFFLOAD_HANDLE 'H'
#define TNA_DURABLE_PIECE 16384u

static ta_allocator tna_offload_allocator(tna_js_datamodel *dm) {
    return tah_allocator_for(&dm->wrapper->system->allocator, TAH_MEMORY_OTHER);
}

static void tna_clear_exception(napi_env env) {
    bool pending = false;
    napi_value ignored = NULL;
    if (napi_is_exception_pending(env, &pending) == napi_ok && pending) {
        napi_get_and_clear_last_exception(env, &ignored);
    }
}

static ta_op_status tna_sink_write(const ta_byte_sink *sink, const void *bytes, size_t length) {
    return sink->v->write(sink->self, (ta_bytes){ .ptr = bytes, .len = (uint32_t)length });
}

/* TA_OP_ERROR when serialization refuses the value, so the caller keeps it by handle. */
static ta_op_status tna_offload_serialize(tna_js_datamodel *dm, const ta_datamodel_value_write_params *params) {
    napi_env env = dm->env;
    napi_value value = NULL;
    napi_value serialize = NULL;
    napi_value global = NULL;
    napi_value buffer = NULL;
    void *data = NULL;
    size_t length = 0;
    if (!tna_value_get(env, params->value, &value) ||
        napi_get_reference_value(env, dm->serialize, &serialize) != napi_ok || serialize == NULL ||
        napi_get_global(env, &global) != napi_ok ||
        napi_call_function(env, global, serialize, 1, &value, &buffer) != napi_ok ||
        napi_get_buffer_info(env, buffer, &data, &length) != napi_ok) {
        tna_clear_exception(env);
        return TA_OP_ERROR;
    }
    static const uint8_t tag = TNA_OFFLOAD_SERIALIZED;
    ta_op_status status = tna_sink_write(&params->sink, &tag, 1);
    for (size_t at = 0; status == TA_OP_OK && at < length; at += TNA_DURABLE_PIECE) {
        size_t count = length - at < TNA_DURABLE_PIECE ? length - at : TNA_DURABLE_PIECE;
        status = tna_sink_write(&params->sink, (const uint8_t *)data + at, count);
    }
    return status == TA_OP_ERROR ? TA_OP_NOMEM : status;
}

static ta_op_status tna_offload_handle(tna_js_datamodel *dm, const ta_datamodel_value_write_params *params) {
    if (!dm->offloading) {
        return TA_OP_ERROR;
    }
    /* One slot at a time: an actor holds few values, and the store frees the
     * array at exactly its count. */
    if (dm->handle_count == dm->handle_capacity) {
        uint32_t capacity = dm->handle_capacity + 1u;
        ta_allocator allocator = tna_offload_allocator(dm);
        ta_value *grown = tah_alloc(allocator, (size_t)capacity * sizeof(ta_value), _Alignof(ta_value));
        if (grown == NULL) {
            return TA_OP_NOMEM;
        }
        if (dm->handle_count != 0) {
            memcpy(grown, dm->handles, (size_t)dm->handle_count * sizeof(ta_value));
        }
        if (dm->handles != NULL) {
            tah_free(allocator, &(ta_allocator_free_params){ .ptr = dm->handles,
                                                             .size = (size_t)dm->handle_capacity * sizeof(ta_value),
                                                             .alignment = _Alignof(ta_value) });
        }
        dm->handles = grown;
        dm->handle_capacity = capacity;
    }
    uint8_t bytes[1u + sizeof(ta_value)];
    bytes[0] = TNA_OFFLOAD_HANDLE;
    memcpy(bytes + 1, params->value, sizeof(ta_value));
    ta_op_status status = tna_sink_write(&params->sink, bytes, sizeof bytes);
    if (status != TA_OP_OK) {
        return status == TA_OP_ERROR ? TA_OP_NOMEM : status;
    }
    ta_value kept = *params->value;
    ta_value_retain(&kept);
    dm->handles[dm->handle_count++] = kept;
    return TA_OP_OK;
}

/* One piece of text from JavaScript, as UTF-8, into the sink. */
static ta_op_status tna_durable_piece(tna_js_datamodel *dm, const ta_byte_sink *sink, napi_value text) {
    napi_env env = dm->env;
    size_t length = 0;
    if (napi_get_value_string_utf8(env, text, NULL, 0, &length) != napi_ok) {
        return TA_OP_ERROR;
    }
    ta_allocator allocator = tna_offload_allocator(dm);
    char *utf8 = tah_alloc(allocator, length + 1u, 1);
    if (utf8 == NULL) {
        return TA_OP_NOMEM;
    }
    size_t written = 0;
    ta_op_status status = napi_get_value_string_utf8(env, text, utf8, length + 1u, &written) == napi_ok
                              ? tna_sink_write(sink, utf8, written)
                              : TA_OP_ERROR;
    tah_free(allocator, &(ta_allocator_free_params){ .ptr = utf8, .size = length + 1u, .alignment = 1 });
    return status;
}

static ta_op_status tna_durable_write(tna_js_datamodel *dm, const ta_datamodel_value_write_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, dm, NULL, NULL, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = dm->env;
    napi_value value = NULL;
    napi_value chunks = NULL;
    napi_value global = NULL;
    napi_value iterator = NULL;
    napi_value next = NULL;
    ta_op_status status = TA_OP_OK;
    if (dm->durable_chunks == NULL || !tna_value_get(env, params->value, &value) ||
        napi_get_reference_value(env, dm->durable_chunks, &chunks) != napi_ok || chunks == NULL ||
        napi_get_global(env, &global) != napi_ok ||
        napi_call_function(env, global, chunks, 1, &value, &iterator) != napi_ok ||
        napi_get_named_property(env, iterator, "next", &next) != napi_ok) {
        status = tna_fail(&frame, "javascript: the value has no durable form");
    }
    while (status == TA_OP_OK) {
        napi_value step = NULL;
        napi_value done = NULL;
        napi_value text = NULL;
        bool finished = false;
        if (napi_call_function(env, iterator, next, 0, NULL, &step) != napi_ok ||
            napi_get_named_property(env, step, "done", &done) != napi_ok ||
            napi_get_value_bool(env, done, &finished) != napi_ok) {
            status = tna_fail(&frame, "javascript: the value has no durable form");
            break;
        }
        if (finished) {
            break;
        }
        if (napi_get_named_property(env, step, "value", &text) != napi_ok) {
            status = tna_fail(&frame, "javascript: the value has no durable form");
            break;
        }
        status = tna_durable_piece(dm, &params->sink, text);
        if (status == TA_OP_ERROR) {
            status = tna_fail(&frame, "javascript: the sink failed");
        }
    }
    tna_end(&frame);
    return status;
}

static ta_op_status tna_dm_value_write(void *self, const ta_datamodel_value_write_params *params) {
    tna_js_datamodel *dm = ((tna_dm_adapter *)self)->dm;
    if (!tna_value_ours(params->value)) {
        if (params->fault != NULL) {
            *params->fault = (ta_fault){ .code = TA_ERR_INVALID,
                                         .message = { "javascript: not a value of this data model", 42 } };
        }
        return TA_OP_ERROR;
    }
    if (params->durable) {
        return tna_durable_write(dm, params);
    }
    if (!dm->by_handle && dm->serialize != NULL && dm->js_usable) {
        ta_op_status status = tna_offload_serialize(dm, params);
        if (status != TA_OP_ERROR) {
            return status;
        }
    }
    ta_op_status status = tna_offload_handle(dm, params);
    if (status == TA_OP_ERROR && params->fault != NULL) {
        *params->fault = (ta_fault){ .code = TA_ERR_UNSUPPORTED,
                                     .message = { "javascript: the value has no byte form", 38 } };
    }
    return status;
}

/*
 * How much of `bytes` ends on a whole UTF-8 character: a piece can end in
 * the middle of one, whose rest comes with the next piece.
 */
static size_t tna_utf8_whole(const uint8_t *bytes, size_t length) {
    size_t back = 0;
    while (back < 3u && back < length && (bytes[length - 1u - back] & 0xc0u) == 0x80u) {
        back += 1;
    }
    if (back == length) {
        return length;
    }
    uint8_t lead = bytes[length - 1u - back];
    size_t need = lead < 0x80u ? 1u : (lead & 0xe0u) == 0xc0u ? 2u : (lead & 0xf0u) == 0xe0u ? 3u : 4u;
    return back + 1u >= need ? length : length - 1u - back;
}

static bool tna_call_method(napi_env env, napi_value object, const char *name, size_t argc, napi_value *argv,
                            napi_value *out) {
    napi_value method = NULL;
    napi_value ignored = NULL;
    return napi_get_named_property(env, object, name, &method) == napi_ok &&
           napi_call_function(env, object, method, argc, argv, out != NULL ? out : &ignored) == napi_ok;
}

static ta_op_status tna_durable_read(tna_js_datamodel *dm, const ta_datamodel_value_read_params *params) {
    tna_frame frame;
    if (!tna_begin(&frame, dm, NULL, NULL, params->fault)) {
        return TA_OP_ERROR;
    }
    napi_env env = dm->env;
    napi_value factory = NULL;
    napi_value classes = NULL;
    napi_value global = NULL;
    napi_value reader = NULL;
    ta_allocator allocator = tna_offload_allocator(dm);
    uint8_t *piece = tah_alloc(allocator, TNA_DURABLE_PIECE + 4u, 1);
    ta_op_status status = piece == NULL ? TA_OP_NOMEM : TA_OP_OK;
    if (status == TA_OP_OK &&
        (dm->durable_reader == NULL || napi_get_reference_value(env, dm->durable_reader, &factory) != napi_ok ||
         napi_get_reference_value(env, dm->classes, &classes) != napi_ok || napi_get_global(env, &global) != napi_ok ||
         napi_call_function(env, global, factory, 1, &classes, &reader) != napi_ok)) {
        status = tna_fail(&frame, "javascript: cannot read durable values");
    }
    size_t carried = 0;
    while (status == TA_OP_OK) {
        uint32_t got = 0;
        status = params->source.v->read(params->source.self, &(ta_byte_read_params){
                                                                 .buffer = piece + carried,
                                                                 .capacity = TNA_DURABLE_PIECE,
                                                                 .got = &got });
        if (status != TA_OP_OK) {
            if (status == TA_OP_ERROR) {
                status = tna_fail(&frame, "javascript: the source failed");
            }
            break;
        }
        size_t length = carried + got;
        size_t whole = got == 0 ? length : tna_utf8_whole(piece, length);
        if (whole != 0) {
            napi_value text = NULL;
            if (napi_create_string_utf8(env, (const char *)piece, whole, &text) != napi_ok ||
                !tna_call_method(env, reader, "push", 1, &text, NULL)) {
                status = tna_fail(&frame, "javascript: the bytes are not a value in the built-in data model's text");
                break;
            }
        }
        carried = length - whole;
        memmove(piece, piece + whole, carried);
        if (got == 0) {
            napi_value value = NULL;
            if (!tna_call_method(env, reader, "end", 0, NULL, &value)) {
                status = tna_fail(&frame, "javascript: the bytes are not a value in the built-in data model's text");
            } else if (!tna_value_make(&dm->wrapper->values, value, params->out)) {
                status = TA_OP_NOMEM;
            }
            break;
        }
    }
    if (piece != NULL) {
        tah_free(allocator, &(ta_allocator_free_params){ .ptr = piece, .size = TNA_DURABLE_PIECE + 4u, .alignment = 1 });
    }
    tna_end(&frame);
    return status;
}

/* Offload's bytes, gathered: v8.deserialize and a handle both want them whole. */
static ta_op_status tna_offload_gather(tna_js_datamodel *dm, const ta_byte_source *source, uint8_t **out,
                                       size_t *length, size_t *capacity) {
    ta_allocator allocator = tna_offload_allocator(dm);
    *out = NULL;
    *length = 0;
    *capacity = 0;
    for (;;) {
        if (*capacity - *length < TNA_DURABLE_PIECE) {
            size_t grown_capacity = *capacity == 0 ? TNA_DURABLE_PIECE : *capacity * 2u;
            uint8_t *grown = tah_alloc(allocator, grown_capacity, 1);
            if (grown == NULL) {
                return TA_OP_NOMEM;
            }
            if (*length != 0) {
                memcpy(grown, *out, *length);
            }
            if (*out != NULL) {
                tah_free(allocator, &(ta_allocator_free_params){ .ptr = *out, .size = *capacity, .alignment = 1 });
            }
            *out = grown;
            *capacity = grown_capacity;
        }
        uint32_t got = 0;
        ta_op_status status = source->v->read(source->self, &(ta_byte_read_params){
                                                                .buffer = *out + *length,
                                                                .capacity = TNA_DURABLE_PIECE,
                                                                .got = &got });
        if (status != TA_OP_OK) {
            return status;
        }
        if (got == 0) {
            return TA_OP_OK;
        }
        *length += got;
    }
}

static ta_op_status tna_offload_read(tna_js_datamodel *dm, ta_bytes bytes, ta_value *out) {
    if (bytes.len < 1u) {
        return TA_OP_ERROR;
    }
    if (bytes.ptr[0] == TNA_OFFLOAD_HANDLE && bytes.len == 1u + sizeof(ta_value)) {
        ta_value kept;
        memcpy(&kept, bytes.ptr + 1, sizeof kept);
        if (!tna_value_ours(&kept)) {
            return TA_OP_ERROR;
        }
        ta_value_retain(&kept);
        *out = kept;
        return TA_OP_OK;
    }
    napi_env env = dm->env;
    napi_value buffer = NULL;
    napi_value deserialize = NULL;
    napi_value global = NULL;
    napi_value value = NULL;
    if (bytes.ptr[0] != TNA_OFFLOAD_SERIALIZED || dm->deserialize == NULL || !dm->js_usable) {
        return TA_OP_ERROR;
    }
    void *copied = NULL;
    if (napi_create_buffer_copy(env, bytes.len - 1u, bytes.ptr + 1, &copied, &buffer) != napi_ok ||
        napi_get_reference_value(env, dm->deserialize, &deserialize) != napi_ok || deserialize == NULL ||
        napi_get_global(env, &global) != napi_ok ||
        napi_call_function(env, global, deserialize, 1, &buffer, &value) != napi_ok) {
        tna_clear_exception(env);
        return TA_OP_ERROR;
    }
    return tna_value_make(&dm->wrapper->values, value, out) ? TA_OP_OK : TA_OP_NOMEM;
}

static ta_op_status tna_dm_value_read(void *self, const ta_datamodel_value_read_params *params) {
    tna_js_datamodel *dm = ((tna_dm_adapter *)self)->dm;
    if (params->durable) {
        return tna_durable_read(dm, params);
    }
    uint8_t *bytes = NULL;
    size_t length = 0;
    size_t capacity = 0;
    ta_op_status status = tna_offload_gather(dm, &params->source, &bytes, &length, &capacity);
    if (status == TA_OP_OK) {
        status = tna_offload_read(dm, (ta_bytes){ .ptr = bytes, .len = (uint32_t)length }, params->out);
        if (status == TA_OP_ERROR && params->fault != NULL) {
            *params->fault = (ta_fault){ .code = TA_ERR_INVALID,
                                         .message = { "javascript: the bytes are no offloaded value", 44 } };
        }
    }
    if (bytes != NULL) {
        tah_free(tna_offload_allocator(dm), &(ta_allocator_free_params){ .ptr = bytes, .size = capacity, .alignment = 1 });
    }
    return status;
}

bool tna_js_datamodel_set_offload(tna_js_datamodel *dm, const tna_offload_values *values) {
    napi_ref serialize = NULL;
    napi_ref deserialize = NULL;
    if (values->serialize != NULL && values->deserialize != NULL &&
        (napi_create_reference(dm->env, values->serialize, 1, &serialize) != napi_ok ||
         napi_create_reference(dm->env, values->deserialize, 1, &deserialize) != napi_ok)) {
        if (serialize != NULL) {
            napi_delete_reference(dm->env, serialize);
        }
        return false;
    }
    if (dm->serialize != NULL) {
        napi_delete_reference(dm->env, dm->serialize);
    }
    if (dm->deserialize != NULL) {
        napi_delete_reference(dm->env, dm->deserialize);
    }
    dm->serialize = serialize;
    dm->deserialize = deserialize;
    dm->by_handle = values->by_handle;
    return true;
}

/* The built-in data model's codec, for napi's own callers (napi_loads.c). */
ta_op_status tna_js_datamodel_value_write(tna_js_datamodel *dm, const ta_datamodel_value_write_params *params) {
    return tna_dm_value_write(&dm->builtin, params);
}

ta_op_status tna_js_datamodel_value_read(tna_js_datamodel *dm, const ta_datamodel_value_read_params *params) {
    return tna_dm_value_read(&dm->builtin, params);
}

bool tna_js_datamodel_set_durable(tna_js_datamodel *dm, napi_value chunks, napi_value reader) {
    napi_ref chunks_ref = NULL;
    napi_ref reader_ref = NULL;
    if (napi_create_reference(dm->env, chunks, 1, &chunks_ref) != napi_ok) {
        return false;
    }
    if (napi_create_reference(dm->env, reader, 1, &reader_ref) != napi_ok) {
        napi_delete_reference(dm->env, chunks_ref);
        return false;
    }
    if (dm->durable_chunks != NULL) {
        napi_delete_reference(dm->env, dm->durable_chunks);
    }
    if (dm->durable_reader != NULL) {
        napi_delete_reference(dm->env, dm->durable_reader);
    }
    dm->durable_chunks = chunks_ref;
    dm->durable_reader = reader_ref;
    return true;
}

void tna_js_datamodel_offload_begin(tna_js_datamodel *dm) {
    dm->offloading = true;
    dm->handles = NULL;
    dm->handle_count = 0;
    dm->handle_capacity = 0;
}

/* The array's capacity is its count, as the offload store frees it. */
tna_offload_handles tna_js_datamodel_offload_end(tna_js_datamodel *dm) {
    tna_offload_handles taken = { .handles = dm->handles, .count = dm->handle_count };
    dm->offloading = false;
    dm->handles = NULL;
    dm->handle_count = 0;
    dm->handle_capacity = 0;
    return taken;
}

void tna_js_datamodel_release_handles(tna_js_datamodel *dm, tna_offload_handles handles) {
    for (uint32_t index = 0; index < handles.count; index += 1) {
        ta_value_release(&handles.handles[index]);
    }
    if (handles.handles != NULL) {
        tah_free(tna_offload_allocator(dm),
                 &(ta_allocator_free_params){ .ptr = handles.handles, .size = (size_t)handles.count * sizeof(ta_value),
                                              .alignment = _Alignof(ta_value) });
    }
}

ta_datamodel tna_js_datamodel_adapter(tna_js_datamodel *dm) {
    return (ta_datamodel){ .self = &dm->builtin, .v = &dm->builtin.vtable };
}

static void tna_adapter_init(tna_dm_adapter *adapter, tna_js_datamodel *dm, ta_str name, char *buffer) {
    *adapter = (tna_dm_adapter){ .dm = dm, .vtable = tna_dm_vtable, .name = buffer };
    memcpy(adapter->name, name.ptr, name.len);
    adapter->name[name.len] = '\0';
    adapter->vtable.name = (ta_str){ adapter->name, name.len };
}

ta_result tna_js_datamodel_register(tna_js_datamodel *dm, ta_str name, napi_value compile) {
    if (name.len == 0 || name.len >= dm->name_bytes || dm->registered_count >= dm->registered_capacity ||
        (name.len == 10 && memcmp(name.ptr, "javascript", 10) == 0) || (name.len == 4 && memcmp(name.ptr, "null", 4) == 0)) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < dm->registered_count; index += 1) {
        const tna_dm_adapter *other = &dm->registered[index];
        if (other->vtable.name.len == name.len && memcmp(other->name, name.ptr, name.len) == 0) {
            return TA_ERR_INVALID;
        }
    }
    tna_dm_adapter *adapter = &dm->registered[dm->registered_count];
    tna_adapter_init(adapter, dm, name, dm->names + (size_t)dm->registered_count * dm->name_bytes);
    if (napi_create_reference(dm->env, compile, 1, &adapter->compile) != napi_ok) {
        return TA_ERR_NOMEM;
    }
    dm->registered_count += 1u;
    return TA_OK;
}

uint32_t tna_js_datamodel_choices(tna_js_datamodel *dm, const ta_datamodel **choices) {
    ta_datamodel *out = dm->choices;
    *choices = out;
    uint32_t count = 0;
    out[count++] = tna_js_datamodel_adapter(dm);
    out[count++] = ta_null_datamodel();
    for (uint32_t index = 0; index < dm->registered_count; index += 1) {
        out[count++] = (ta_datamodel){ .self = &dm->registered[index], .v = &dm->registered[index].vtable };
    }
    return count;
}

ta_result tna_js_datamodel_select(tna_js_datamodel *dm, ta_str name) {
    const ta_datamodel *choices = NULL;
    uint32_t count = tna_js_datamodel_choices(dm, &choices);
    for (uint32_t index = 0; index < count; index += 1) {
        ta_str candidate = choices[index].v->name;
        if (candidate.len == name.len && memcmp(candidate.ptr, name.ptr, name.len) == 0) {
            dm->selected = choices[index];
            return TA_OK;
        }
    }
    return TA_ERR_NOT_FOUND;
}

/* ------------------------------------------------------------------------
 * The evaluation context
 */

static tna_js_datamodel *tna_context_dm(napi_env env, napi_callback_info info, size_t *argc, napi_value *argv) {
    void *data = NULL;
    if (napi_get_cb_info(env, info, argc, argv, NULL, &data) != napi_ok || data == NULL) {
        return NULL;
    }
    tna_js_datamodel *dm = data;
    if (dm->host == NULL) {
        tna_throw(env, "invalid", "the evaluation context is valid only while its expression runs");
        return NULL;
    }
    return dm;
}

/*
 * System-variable text (core's view, ADR 0068): a string, or undefined when
 * the variable has no value. When core cannot render it for lack of memory,
 * it throws and marks the call failed (see `text_failed`), so the failure
 * reaches core as TA_OP_ERROR instead of reading as undefined.
 */
static bool tna_host_io_text(napi_env env, tna_js_datamodel *dm, ta_dm_text_kind kind, uint32_t io_index,
                             napi_value *out) {
    ta_str text = { 0 };
    *out = NULL;
    ta_result result = ta_dm_host_text(dm->host, &(ta_dm_text_params){ .kind = kind, .io_index = io_index, .out = &text });
    if (result == TA_OK) {
        return napi_create_string_utf8(env, text.len == 0 ? "" : text.ptr, text.len, out) == napi_ok;
    }
    if (result == TA_ERR_NOT_FOUND) {
        return napi_get_undefined(env, out) == napi_ok;
    }
    dm->text_failed = true;
    tna_throw(env, "out-of-memory", "core could not render a system variable");
    return false;
}

static bool tna_host_text(napi_env env, tna_js_datamodel *dm, ta_dm_text_kind kind, napi_value *out) {
    return tna_host_io_text(env, dm, kind, 0, out);
}

static napi_value tna_get_data(napi_env env, napi_callback_info info) {
    tna_js_datamodel *dm = tna_context_dm(env, info, NULL, NULL);
    napi_value root = NULL;
    if (dm == NULL || dm->instance == NULL || !tna_value_get(env, &((tna_dm_instance *)dm->instance)->root, &root)) {
        return NULL;
    }
    return root;
}

static const char *const tna_event_types[] = { "external", "internal", "platform" };

/* object[name] = the variable's text, unless it has no value. */
static bool tna_set_text(napi_env env, tna_js_datamodel *dm, napi_value object, const char *name, ta_dm_text_kind kind) {
    napi_value text = NULL;
    napi_valuetype type = napi_undefined;
    if (!tna_host_text(env, dm, kind, &text) || napi_typeof(env, text, &type) != napi_ok) {
        return false;
    }
    return type == napi_undefined || napi_set_named_property(env, object, name, text) == napi_ok;
}

/*
 * _event: built once per callback, on first use, and held by a reference
 * until the callback ends (each getter call has its own handle scope).
 */
static napi_value tna_get_event(napi_env env, napi_callback_info info) {
    tna_js_datamodel *dm = tna_context_dm(env, info, NULL, NULL);
    if (dm == NULL) {
        return NULL;
    }
    napi_value cached = NULL;
    if (dm->event != NULL && napi_get_reference_value(env, dm->event, &cached) == napi_ok && cached != NULL) {
        return cached;
    }
    const ta_event *event = ta_dm_host_event(dm->host);
    napi_value object = NULL;
    napi_value type = NULL;
    napi_value data = NULL;
    if (event == NULL) {
        napi_get_undefined(env, &object);
        return object;
    }
    napi_value name = NULL;
    if (napi_create_object(env, &object) != napi_ok || !tna_host_text(env, dm, TA_DM_TEXT_EVENT_NAME, &name) ||
        napi_set_named_property(env, object, "name", name) != napi_ok ||
        napi_create_string_utf8(env, tna_event_types[event->type <= TA_EVENT_PLATFORM ? event->type : 0], NAPI_AUTO_LENGTH,
                                &type) != napi_ok ||
        napi_set_named_property(env, object, "type", type) != napi_ok ||
        !tna_set_text(env, dm, object, "sendID", TA_DM_TEXT_EVENT_SENDID) ||
        !tna_set_text(env, dm, object, "origin", TA_DM_TEXT_EVENT_ORIGIN) ||
        !tna_set_text(env, dm, object, "originType", TA_DM_TEXT_EVENT_ORIGINTYPE) ||
        !tna_set_text(env, dm, object, "invocationID", TA_DM_TEXT_EVENT_INVOKEID) ||
        !tna_value_get(env, &event->data, &data) || napi_set_named_property(env, object, "data", data) != napi_ok) {
        return NULL;
    }
    napi_create_reference(env, object, 1, &dm->event);
    return object;
}

static napi_value tna_get_session_id(napi_env env, napi_callback_info info) {
    tna_js_datamodel *dm = tna_context_dm(env, info, NULL, NULL);
    napi_value text = NULL;
    return dm != NULL && tna_host_text(env, dm, TA_DM_TEXT_SESSION_ID, &text) ? text : NULL;
}

/* The logical time of the running macrostep, in milliseconds (core ADR 0089
 * section 9, napi ADR 0032 section 4): the clock expressions read. */
static napi_value tna_get_time(napi_env env, napi_callback_info info) {
    tna_js_datamodel *dm = tna_context_dm(env, info, NULL, NULL);
    napi_value time = NULL;
    if (dm == NULL || dm->host == NULL || napi_create_double(env, (double)ta_dm_host_time(dm->host), &time) != napi_ok) {
        return NULL;
    }
    return time;
}

static napi_value tna_get_name(napi_env env, napi_callback_info info) {
    tna_js_datamodel *dm = tna_context_dm(env, info, NULL, NULL);
    napi_value text = NULL;
    return dm != NULL && tna_host_text(env, dm, TA_DM_TEXT_SESSION_NAME, &text) ? text : NULL;
}

/* _ioprocessors: the SCXML Event I/O Processor, by its type URI and "scxml". */
/*
 * _ioprocessors: one {location} per processor core lists, keyed by its type
 * (SCXML 5.10, ADR 0068). The SCXML processor, index 0, is also "scxml".
 */
static napi_value tna_get_io_processors(napi_env env, napi_callback_info info) {
    tna_js_datamodel *dm = tna_context_dm(env, info, NULL, NULL);
    if (dm == NULL) {
        return NULL;
    }
    napi_value processors = NULL;
    if (napi_create_object(env, &processors) != napi_ok) {
        return NULL;
    }
    for (uint32_t index = 0;; index += 1) {
        /* Processors are listed until the type at an index has no value. */
        napi_value key = NULL;
        napi_valuetype kind = napi_undefined;
        if (!tna_host_io_text(env, dm, TA_DM_TEXT_IO_TYPE, index, &key) || napi_typeof(env, key, &kind) != napi_ok) {
            return NULL;
        }
        if (kind == napi_undefined) {
            break;
        }
        napi_value entry = NULL;
        napi_value location = NULL;
        if (napi_create_object(env, &entry) != napi_ok ||
            !tna_host_io_text(env, dm, TA_DM_TEXT_IO_LOCATION, index, &location) ||
            napi_typeof(env, location, &kind) != napi_ok) {
            return NULL;
        }
        if (kind == napi_string) {
            napi_set_named_property(env, entry, "location", location);
        }
        if (napi_set_property(env, processors, key, entry) != napi_ok) {
            return NULL;
        }
        if (index == 0) {
            napi_set_named_property(env, processors, "scxml", entry);
        }
    }
    return processors;
}

static napi_value tna_in_state_method(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1] = { NULL };
    tna_js_datamodel *dm = tna_context_dm(env, info, &argc, argv);
    if (dm == NULL) {
        return NULL;
    }
    char stack[256];
    size_t length = 0;
    napi_valuetype type = napi_undefined;
    if (argc < 1 || napi_typeof(env, argv[0], &type) != napi_ok || type != napi_string ||
        napi_get_value_string_utf8(env, argv[0], stack, sizeof stack, &length) != napi_ok) {
        return tna_throw(env, "invalid", "inState(): expected a state ID");
    }
    napi_value result = NULL;
    napi_get_boolean(env, tna_in_state(dm->host, (ta_str){ stack, (uint32_t)length }), &result);
    return result;
}

static bool tna_global_function(napi_env env, const char *object_name, const char *function_name, napi_ref *out) {
    napi_value global = NULL;
    napi_value object = NULL;
    napi_value function = NULL;
    return napi_get_global(env, &global) == napi_ok &&
           napi_get_named_property(env, global, object_name, &object) == napi_ok &&
           napi_get_named_property(env, object, function_name, &function) == napi_ok &&
           napi_create_reference(env, function, 1, out) == napi_ok;
}

bool tna_js_datamodel_init(tna_js_datamodel *dm, tna_napi_system *wrapper, napi_env env) {
    *dm = (tna_js_datamodel){ .wrapper = wrapper, .env = env, .js_usable = true };
    tna_adapter_init(&dm->builtin, dm, (ta_str){ "javascript", 10 }, dm->builtin_name);
    /* Its buffers come from the system's parameters (napi ADR 0035). */
    const tna_params *params = &wrapper->system->params;
    uint64_t message_bytes = TA_PARAM_DEFAULT_COMPILE_MESSAGE_BYTES;
    (void)ta_system_param(wrapper->system->core, TA_PARAM_COMPILE_MESSAGE_BYTES, &message_bytes);
    dm->registered_capacity = (uint32_t)params->values[TNA_PARAM_REGISTERED_DATAMODELS];
    dm->name_bytes = (uint32_t)params->values[TNA_PARAM_DATAMODEL_NAME_BYTES];
    dm->rejection_bytes = (size_t)message_bytes + 1u;
    dm->registered = calloc(dm->registered_capacity != 0 ? dm->registered_capacity : 1u, sizeof *dm->registered);
    dm->names = calloc(dm->registered_capacity != 0 ? dm->registered_capacity : 1u, dm->name_bytes);
    dm->choices = calloc(2u + dm->registered_capacity, sizeof *dm->choices);
    dm->rejection = calloc(dm->rejection_bytes, 1);
    if (dm->registered == NULL || dm->names == NULL || dm->choices == NULL || dm->rejection == NULL) {
        return false;
    }
    napi_value context = NULL;
    napi_value functions = NULL;
    napi_value classes = NULL;
    napi_property_descriptor properties[] = {
        { "data", NULL, NULL, tna_get_data, NULL, NULL, napi_enumerable, dm },
        { "event", NULL, NULL, tna_get_event, NULL, NULL, napi_enumerable, dm },
        { "sessionID", NULL, NULL, tna_get_session_id, NULL, NULL, napi_enumerable, dm },
        { "name", NULL, NULL, tna_get_name, NULL, NULL, napi_enumerable, dm },
        { "time", NULL, NULL, tna_get_time, NULL, NULL, napi_enumerable, dm },
        { "ioProcessors", NULL, NULL, tna_get_io_processors, NULL, NULL, napi_enumerable, dm },
        { "inState", NULL, tna_in_state_method, NULL, NULL, NULL, napi_enumerable, dm },
    };
    return napi_create_object(env, &context) == napi_ok &&
           napi_define_properties(env, context, sizeof properties / sizeof properties[0], properties) == napi_ok &&
           napi_object_freeze(env, context) == napi_ok && napi_create_reference(env, context, 1, &dm->context) == napi_ok &&
           tna_global_function(env, "JSON", "parse", &dm->json_parse) &&
           tna_global_function(env, "Array", "from", &dm->array_from) && napi_create_object(env, &functions) == napi_ok &&
           napi_create_reference(env, functions, 1, &dm->functions) == napi_ok &&
           napi_create_object(env, &classes) == napi_ok && napi_create_reference(env, classes, 1, &dm->classes) == napi_ok;
}

/* Whether `table` (a registered-name table) has `key` as an own property. */
static bool tna_table_has(napi_env env, napi_ref table_ref, napi_value key, bool *has) {
    napi_value table = NULL;
    *has = false;
    return table_ref != NULL && napi_get_reference_value(env, table_ref, &table) == napi_ok && table != NULL &&
           napi_has_own_property(env, table, key, has) == napi_ok;
}

ta_result tna_js_datamodel_register_name(tna_js_datamodel *dm, const tna_register_name_params *params) {
    napi_env env = dm->env;
    napi_value table = NULL;
    napi_value key = NULL;
    bool function_has = false;
    bool class_has = false;
    napi_ref target = params->constructor ? dm->classes : dm->functions;
    if (target == NULL || napi_get_reference_value(env, target, &table) != napi_ok || table == NULL ||
        napi_create_string_utf8(env, params->name.ptr, params->name.len, &key) != napi_ok ||
        !tna_table_has(env, dm->functions, key, &function_has) || !tna_table_has(env, dm->classes, key, &class_has)) {
        return TA_ERR_NOMEM;
    }
    if (function_has || class_has) {
        return TA_ERR_INVALID;
    }
    /* Defined, not set: a name such as `__proto__` is an own property too. */
    napi_property_descriptor property = { NULL, key, NULL, NULL, NULL, params->value, napi_enumerable, NULL };
    return napi_define_properties(env, table, 1, &property) == napi_ok ? TA_OK : TA_ERR_NOMEM;
}

void tna_js_datamodel_after_core(tna_js_datamodel *dm) {
    /*
     * Core has released every record, so the table is empty. A record that
     * outlived core would find no table, and its cell stays valid on its own.
     */
    if (dm->primitives.slots != NULL) {
        tah_free(dm->wrapper->values.allocator,
                 &(ta_allocator_free_params){ .ptr = dm->primitives.slots,
                                              .size = (size_t)dm->primitives.capacity * sizeof *dm->primitives.slots,
                                              .alignment = _Alignof(tna_intern_slot) });
    }
    dm->primitives = (tna_intern_table){ 0 };
}

void tna_js_datamodel_free(tna_js_datamodel *dm) {
    free(dm->registered);
    free(dm->names);
    free(dm->choices);
    free(dm->rejection);
    dm->registered = NULL;
    dm->names = NULL;
    dm->choices = NULL;
    dm->rejection = NULL;
    dm->registered_capacity = 0;
    dm->registered_count = 0;
}

void tna_js_datamodel_close(tna_js_datamodel *dm, bool refs_usable) {
    tna_js_datamodel_after_core(dm);
    napi_ref *refs[] = { &dm->context, &dm->json_parse, &dm->array_from, &dm->functions, &dm->classes, &dm->serialize, &dm->deserialize,
                         &dm->durable_chunks, &dm->durable_reader, &dm->objects };
    for (size_t at = 0; at < sizeof refs / sizeof refs[0]; at += 1) {
        if (*refs[at] != NULL && refs_usable) {
            napi_delete_reference(dm->env, *refs[at]);
        }
        *refs[at] = NULL;
    }
    /* The compilers go with the system (ADR 0019 section 3). */
    for (uint32_t index = 0; index < dm->registered_count; index += 1) {
        if (dm->registered[index].compile != NULL && refs_usable) {
            napi_delete_reference(dm->env, dm->registered[index].compile);
        }
        dm->registered[index].compile = NULL;
    }
}
