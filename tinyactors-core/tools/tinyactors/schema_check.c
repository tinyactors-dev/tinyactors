/* schema_check.c — a message's CBOR tokens checked against a generated
 * schema's tables (schema.h). One pass in document order with an explicit
 * stack of the values open, as deep as a decoded CBOR item can nest: no
 * recursion, no allocation but the path of a refusal, written into the
 * input's arena. The first value the schema does not allow is the answer:
 * the generator made every oneOf's branches exclusive by their selectors, so
 * a value is checked against one branch, and the first refusal found is the
 * one a reader of the document in order would find first. */
#include "base/memory.h"
#include "schema.h"
#include "schema_run.h"

enum { SCHEMA_INVALID_PARAMS = -32602 };
enum { SCHEMA_FRAMES = TA_CBOR_DEPTH_MAX + 1 };

/* A value being checked: its node and token, and the children not yet
 * walked (a map's pairs, an array's items). */
typedef struct schema_frame {
    uint32_t node, at;
    uint32_t next;     /* the next child's token: an item, or a member's key */
    uint32_t left;     /* children not yet walked */
    uint32_t item;     /* the next item's index */
    uint32_t key;      /* the key token whose value this is; TA_NIL: an item, or the root */
    uint32_t position; /* its index in its parent array */
    uint32_t pad;
} schema_frame;

typedef struct schema_walk {
    const tool_schema_input *input;
    tool_schema_error *error;
    uint32_t depth;
    uint32_t pad;
    schema_frame frames[SCHEMA_FRAMES];
} schema_walk;

/* What is refused, and which member of the current value it is about
 * (empty: the value itself). */
typedef struct schema_refusal {
    const char *reason;
    const char *message;
    ta_string_view member;
} schema_refusal;

/* ---- the path of a refusal ------------------------------------------------------- */

/* Text appended to `pointer`, or only counted when it is NULL. */
typedef struct schema_path {
    char *pointer;
    uint32_t length;
} schema_path;

static void schema_path_text(schema_path *path, ta_string_view text) {
    if (path->pointer && text.length) ta_memcpy(path->pointer + path->length, text.pointer, text.length);
    path->length += text.length;
}

static void schema_path_member(schema_path *path, ta_string_view name) {
    if (path->length) schema_path_text(path, (ta_string_view){".", 1});
    schema_path_text(path, name);
}

static void schema_path_index(schema_path *path, uint32_t index) {
    char digits[10];
    uint32_t count = 0;
    do {
        digits[sizeof digits - 1 - count++] = (char)('0' + index % 10);
        index /= 10;
    } while (index);
    schema_path_text(path, (ta_string_view){"[", 1});
    schema_path_text(path, (ta_string_view){digits + sizeof digits - count, count});
    schema_path_text(path, (ta_string_view){"]", 1});
}

static void schema_path_write(const schema_walk *walk, ta_string_view member, schema_path *path) {
    const ta_cbor_document *document = walk->input->document;
    schema_path_text(path, walk->input->root);
    for (uint32_t i = 1; i < walk->depth; i++) {
        const schema_frame *frame = &walk->frames[i];
        if (frame->key != TA_NIL) schema_path_member(path, ta_cbor_string(document, frame->key));
        else schema_path_index(path, frame->position);
    }
    if (member.length) schema_path_member(path, member);
}

static ta_status schema_refuse(schema_walk *walk, const schema_refusal *refusal) {
    schema_path measure = {0, 0};
    schema_path_write(walk, refusal->member, &measure);
    schema_path written = {0, 0};
    if (measure.length) {
        written.pointer = (char *)ta_growable_arena_alloc(walk->input->arena, measure.length, 1);
        if (!written.pointer) return TA_ERROR_NO_MEMORY;
        schema_path_write(walk, refusal->member, &written);
    }
    walk->error->code = SCHEMA_INVALID_PARAMS;
    walk->error->reason = ta_string(refusal->reason);
    walk->error->message = ta_string(refusal->message);
    walk->error->path = (ta_string_view){written.pointer, written.length};
    return TA_ERROR_INVALID;
}

/* ---- values ---------------------------------------------------------------------- */

static const ta_cbor_token *schema_token(const schema_walk *walk, uint32_t at) {
    return &walk->input->document->tokens[at];
}

static int schema_text_is(ta_string_view text, ta_string_view other) {
    return text.length == other.length && ta_memcmp(text.pointer, other.pointer, text.length) == 0;
}

/* A double, given by its bits, as an integer of the schema's form: 1 when
 * it is a finite integral value within -2^64 … 2^64-1. `integral` says
 * whether it is integral at all. */
typedef struct schema_float {
    uint64_t bits;
    int integral;
    int exact;
    tool_schema_integer integer;
} schema_float;

static void schema_float_read(schema_float *number) {
    uint64_t bits = number->bits;
    uint32_t exponent = (uint32_t)((bits >> 52) & 0x7FFu);
    uint64_t mantissa = bits & ((UINT64_C(1) << 52) - 1);
    int negative = (int)(bits >> 63);
    uint64_t magnitude = 0;
    number->integral = number->exact = 0;
    if (exponent == 0x7FFu) return; /* NaN, an infinity */
    if (exponent == 0) {
        if (mantissa) return; /* a subnormal: below 1 */
        number->integral = number->exact = 1;
    } else {
        uint64_t significand = mantissa | (UINT64_C(1) << 52);
        int32_t shift = (int32_t)exponent - 1075; /* value = significand * 2^shift */
        if (shift >= 0) {
            number->integral = 1;
            if (shift > 11) return; /* past 2^64 */
            magnitude = significand << shift;
        } else {
            if (shift < -52) return; /* below 1 */
            if (significand & ((UINT64_C(1) << -shift) - 1)) return;
            number->integral = 1;
            magnitude = significand >> -shift;
        }
        number->exact = 1;
    }
    if (negative && magnitude) number->integer = (tool_schema_integer){magnitude - 1, 1, 0};
    else number->integer = (tool_schema_integer){magnitude, 0, 0};
}

static double schema_float_value(uint64_t bits) {
    double value;
    ta_memcpy(&value, &bits, sizeof value);
    return value;
}

static double schema_integer_value(tool_schema_integer integer) {
    return integer.negative ? -1.0 - (double)integer.value : (double)integer.value;
}

/* The token's types: what a value of its kind is in JSON Schema's terms
 * (none for bytes, a tag or undefined, which JSON has not). */
static uint32_t schema_type_of(const ta_cbor_token *token) {
    switch (token->kind) {
    case TA_CBOR_UINT:
    case TA_CBOR_NEGINT: return TOOL_SCHEMA_INTEGER | TOOL_SCHEMA_NUMBER;
    case TA_CBOR_FLOAT: {
        schema_float number = {token->value, 0, 0, {0, 0, 0}};
        schema_float_read(&number);
        return TOOL_SCHEMA_NUMBER | (number.integral ? (uint32_t)TOOL_SCHEMA_INTEGER : 0u);
    }
    case TA_CBOR_TEXT: return TOOL_SCHEMA_STRING;
    case TA_CBOR_ARRAY: return TOOL_SCHEMA_ARRAY;
    case TA_CBOR_MAP: return TOOL_SCHEMA_OBJECT;
    case TA_CBOR_SIMPLE:
        if (token->value == TA_CBOR_FALSE || token->value == TA_CBOR_TRUE) return TOOL_SCHEMA_BOOLEAN;
        return token->value == TA_CBOR_NULL ? (uint32_t)TOOL_SCHEMA_NULL : 0u;
    default: return 0;
    }
}

/* The token as an integer: 1, or 0 when it is none (or a float that is not
 * one exactly). */
static int schema_integer_of(const ta_cbor_token *token, tool_schema_integer *out) {
    if (token->kind == TA_CBOR_UINT || token->kind == TA_CBOR_NEGINT) {
        *out = (tool_schema_integer){token->value, token->kind == TA_CBOR_NEGINT, 0};
        return 1;
    }
    if (token->kind != TA_CBOR_FLOAT) return 0;
    schema_float number = {token->value, 0, 0, {0, 0, 0}};
    schema_float_read(&number);
    *out = number.integer;
    return number.exact;
}

static int schema_integer_compare(tool_schema_integer first, tool_schema_integer second) {
    if (first.negative != second.negative) return first.negative ? -1 : 1;
    if (first.value == second.value) return 0;
    int below = first.value < second.value;
    if (first.negative) below = !below; /* -1 - value: a larger value is a smaller integer */
    return below ? -1 : 1;
}

/* A number token against a bound: below 0, 0 or above 0. */
static int schema_number_compare(const ta_cbor_token *token, tool_schema_integer bound) {
    tool_schema_integer integer;
    if (schema_integer_of(token, &integer)) return schema_integer_compare(integer, bound);
    double value = schema_float_value(token->value), limit = schema_integer_value(bound);
    return value < limit ? -1 : value > limit ? 1 : 0;
}

static int schema_value_matches(const schema_walk *walk, uint32_t at, const tool_schema_value *value) {
    const ta_cbor_token *token = schema_token(walk, at);
    tool_schema_integer integer;
    switch (value->kind) {
    case TOOL_SCHEMA_VALUE_TEXT:
        return token->kind == TA_CBOR_TEXT && schema_text_is(ta_cbor_string(walk->input->document, at), value->text);
    case TOOL_SCHEMA_VALUE_INTEGER:
        return schema_integer_of(token, &integer) && schema_integer_compare(integer, value->integer) == 0;
    case TOOL_SCHEMA_VALUE_BOOLEAN:
        return token->kind == TA_CBOR_SIMPLE &&
               token->value == (value->integer.value ? (uint64_t)TA_CBOR_TRUE : (uint64_t)TA_CBOR_FALSE);
    default: return 0;
    }
}

/* ---- patterns -------------------------------------------------------------------- */

static int schema_is_digit(char c) { return c >= '0' && c <= '9'; }

static int schema_is_base64url(ta_string_view text) {
    for (uint32_t i = 0; i < text.length; i++) {
        char c = text.pointer[i];
        int letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        if (!letter && !schema_is_digit(c) && c != '_' && c != '-') return 0;
    }
    return 1;
}

static int schema_is_hex(ta_string_view text) {
    for (uint32_t i = 0; i < text.length; i++) {
        char c = text.pointer[i];
        if (!schema_is_digit(c) && !(c >= 'a' && c <= 'f')) return 0;
    }
    return 1;
}

/* ^[ \t]*([0-9]+(\.[0-9]*)?|\.[0-9]+)(ms|s)[ \t]*$ */
static int schema_is_duration(ta_string_view text) {
    uint32_t i = 0, integer = 0, fraction = 0;
    while (i < text.length && (text.pointer[i] == ' ' || text.pointer[i] == '\t'))
        i++;
    while (i < text.length && schema_is_digit(text.pointer[i]))
        i++, integer++;
    if (i < text.length && text.pointer[i] == '.') {
        i++;
        while (i < text.length && schema_is_digit(text.pointer[i]))
            i++, fraction++;
    }
    if (!integer && !fraction) return 0;
    if (i + 1 < text.length && text.pointer[i] == 'm' && text.pointer[i + 1] == 's') i += 2;
    else if (i < text.length && text.pointer[i] == 's') i += 1;
    else return 0;
    while (i < text.length && (text.pointer[i] == ' ' || text.pointer[i] == '\t'))
        i++;
    return i == text.length;
}

static int schema_pattern_matches(uint32_t pattern, ta_string_view text) {
    switch (pattern) {
    case TOOL_SCHEMA_PATTERN_BASE64URL: return schema_is_base64url(text);
    case TOOL_SCHEMA_PATTERN_DURATION: return schema_is_duration(text);
    case TOOL_SCHEMA_PATTERN_HEX: return schema_is_hex(text);
    default: return 1;
    }
}

/* Code points, as JSON Schema counts a string's length. */
static uint64_t schema_code_points(ta_string_view text) {
    uint64_t count = 0;
    for (uint32_t i = 0; i < text.length; i++)
        count += ((unsigned char)text.pointer[i] & 0xC0u) != 0x80u;
    return count;
}

/* ---- one value against one node -------------------------------------------------- */

static ta_status schema_value_check(schema_walk *walk, const tool_schema_node *node, uint32_t at) {
    const ta_cbor_token *token = schema_token(walk, at);
    uint32_t type = schema_type_of(token);
    if (node->types && !(node->types & type))
        return schema_refuse(walk, &(schema_refusal){"type", "has a type the schema does not allow", {0, 0}});
    if (node->value_count) {
        int found = 0;
        for (uint32_t i = 0; i < node->value_count && !found; i++)
            found = schema_value_matches(walk, at, &walk->input->schema->values[node->values + i]);
        if (!found)
            return schema_refuse(walk, &(schema_refusal){"range", "is not one of the values the schema allows", {0, 0}});
    }
    if (type & TOOL_SCHEMA_NUMBER) {
        if ((node->checks & TOOL_SCHEMA_MINIMUM) && schema_number_compare(token, node->minimum) < 0)
            return schema_refuse(walk, &(schema_refusal){"range", "is below the schema's minimum", {0, 0}});
        if ((node->checks & TOOL_SCHEMA_MAXIMUM) && schema_number_compare(token, node->maximum) > 0)
            return schema_refuse(walk, &(schema_refusal){"range", "is above the schema's maximum", {0, 0}});
    }
    if (type & TOOL_SCHEMA_STRING) {
        ta_string_view text = ta_cbor_string(walk->input->document, at);
        if (node->checks & (TOOL_SCHEMA_MIN_LENGTH | TOOL_SCHEMA_MAX_LENGTH)) {
            uint64_t length = schema_code_points(text);
            if (((node->checks & TOOL_SCHEMA_MIN_LENGTH) && length < node->min_length) ||
                ((node->checks & TOOL_SCHEMA_MAX_LENGTH) && length > node->max_length))
                return schema_refuse(walk, &(schema_refusal){"range", "has a length the schema does not allow", {0, 0}});
        }
        if (!schema_pattern_matches(node->pattern, text))
            return schema_refuse(walk, &(schema_refusal){"range", "is not in the form the schema gives", {0, 0}});
    }
    if (type & TOOL_SCHEMA_ARRAY) {
        if (((node->checks & TOOL_SCHEMA_MIN_ITEMS) && token->count < node->min_items) ||
            ((node->checks & TOOL_SCHEMA_MAX_ITEMS) && token->count > node->max_items))
            return schema_refuse(walk, &(schema_refusal){"range", "has a number of items the schema does not allow", {0, 0}});
    }
    return TA_OK;
}

/* The member `name` of the map at `at`: its value's token, or TA_NIL. */
static uint32_t schema_member(const schema_walk *walk, uint32_t at, ta_string_view name) {
    if (schema_token(walk, at)->kind != TA_CBOR_MAP) return TA_NIL;
    return ta_cbor_member(walk->input->document, at, name);
}

/* The oneOf branch the value at `at` selects, into *node. */
static ta_status schema_choose(schema_walk *walk, const tool_schema_node *node, uint32_t at, uint32_t *chosen) {
    const tool_schema_choice *choices = &walk->input->schema->choices[node->choices];
    uint32_t type = schema_type_of(schema_token(walk, at));
    const tool_schema_choice *keyed = 0;
    int typed = 0, present = 0;
    for (uint32_t i = 0; i < node->choice_count; i++) {
        const tool_schema_choice *choice = &choices[i];
        if (!(choice->types & type)) continue;
        typed = 1;
        if (!choice->key.length) return *chosen = choice->node, TA_OK;
        uint32_t member = schema_member(walk, at, choice->key);
        if (!keyed) keyed = choice;
        if (member == TA_NIL) continue;
        present = 1;
        if (!choice->keyed_by_value) return *chosen = choice->node, TA_OK;
        if (schema_token(walk, member)->kind == TA_CBOR_TEXT &&
            schema_text_is(ta_cbor_string(walk->input->document, member), choice->value))
            return *chosen = choice->node, TA_OK;
    }
    if (!typed || !keyed)
        return schema_refuse(walk, &(schema_refusal){"type", "has a type the schema does not allow", {0, 0}});
    if (keyed->keyed_by_value && present)
        return schema_refuse(walk, &(schema_refusal){"range", "names no form the schema has", keyed->key});
    if (keyed->keyed_by_value)
        return schema_refuse(walk, &(schema_refusal){"missing", "is missing", keyed->key});
    return schema_refuse(walk, &(schema_refusal){"missing", "has none of the members that tell its forms apart", {0, 0}});
}

/* An object's members: each one the node takes (when it takes only its
 * own), the required ones present, and no two that exclude each other. */
static ta_status schema_members_check(schema_walk *walk, const tool_schema_node *node, uint32_t at) {
    const tool_schema *schema = walk->input->schema;
    const ta_cbor_document *document = walk->input->document;
    uint32_t key = at + 1;
    for (uint32_t pair = 0; pair < schema_token(walk, at)->count; pair++) {
        if (schema_token(walk, key)->kind != TA_CBOR_TEXT)
            return schema_refuse(walk, &(schema_refusal){"type", "has a key that is not text", {0, 0}});
        ta_string_view name = ta_cbor_string(document, key);
        if (node->checks & TOOL_SCHEMA_CLOSED) {
            int known = 0;
            for (uint32_t i = 0; i < node->member_count && !known; i++)
                known = schema_text_is(name, schema->members[node->members + i].name);
            if (!known) return schema_refuse(walk, &(schema_refusal){"unknown", "is not a member the schema takes", name});
        }
        key = ta_cbor_next(document, ta_cbor_next(document, key));
    }
    for (uint32_t i = 0; i < node->member_count; i++) {
        const tool_schema_member *member = &schema->members[node->members + i];
        if (member->required && schema_member(walk, at, member->name) == TA_NIL)
            return schema_refuse(walk, &(schema_refusal){"missing", "is missing", member->name});
    }
    if (node->exclusive != TA_NIL) {
        const tool_schema_exclusive *pair = &schema->exclusives[node->exclusive];
        if (schema_member(walk, at, pair->first) != TA_NIL && schema_member(walk, at, pair->second) != TA_NIL)
            return schema_refuse(walk, &(schema_refusal){"range", "cannot come with the member it excludes", pair->second});
    }
    return TA_OK;
}

/* Push the value at `at` (reached through `key`, or as item `position`) and
 * check it against `node`, its oneOf branches resolved. */
typedef struct schema_entry {
    uint32_t node, at;
    uint32_t key, position;
} schema_entry;

static ta_status schema_enter(schema_walk *walk, schema_entry entry) {
    if (walk->depth == SCHEMA_FRAMES) return TA_ERROR_LIMIT;
    schema_frame *frame = &walk->frames[walk->depth++];
    *frame = (schema_frame){entry.node, entry.at, 0, 0, 0, entry.key, entry.position, 0};
    const tool_schema *schema = walk->input->schema;
    uint32_t index = entry.node;
    for (;;) { /* the generator refuses a cycle of $refs: each branch is another node */
        const tool_schema_node *node = &schema->nodes[index];
        ta_status status = schema_value_check(walk, node, entry.at);
        if (status != TA_OK) return status;
        if (!node->choice_count) break;
        status = schema_choose(walk, node, entry.at, &index);
        if (status != TA_OK) return status;
    }
    const tool_schema_node *node = &schema->nodes[index];
    const ta_cbor_token *token = schema_token(walk, entry.at);
    frame->node = index;
    if (token->kind == TA_CBOR_MAP) {
        ta_status status = schema_members_check(walk, node, entry.at);
        if (status != TA_OK) return status;
        if (node->member_count) frame->next = entry.at + 1, frame->left = token->count;
    } else if (token->kind == TA_CBOR_ARRAY && node->items != TA_NIL) {
        frame->next = entry.at + 1, frame->left = token->count;
    }
    return TA_OK;
}

static int schema_is_any(const tool_schema_node *node) {
    return !node->types && !node->checks && !node->value_count && !node->choice_count && node->items == TA_NIL &&
           !node->member_count && node->exclusive == TA_NIL && node->pattern == TOOL_SCHEMA_PATTERN_NONE;
}

static ta_status schema_walk_run(schema_walk *walk, uint32_t node, uint32_t at) {
    const tool_schema *schema = walk->input->schema;
    const ta_cbor_document *document = walk->input->document;
    ta_status status = schema_enter(walk, (schema_entry){node, at, TA_NIL, TA_NIL});
    while (status == TA_OK && walk->depth) {
        schema_frame *frame = &walk->frames[walk->depth - 1];
        if (!frame->left) {
            walk->depth--;
            continue;
        }
        frame->left--;
        const tool_schema_node *parent = &schema->nodes[frame->node];
        if (schema_token(walk, frame->at)->kind == TA_CBOR_MAP) {
            uint32_t key = frame->next, value = ta_cbor_next(document, key);
            frame->next = ta_cbor_next(document, value);
            ta_string_view name = ta_cbor_string(document, key);
            for (uint32_t i = 0; i < parent->member_count; i++) {
                const tool_schema_member *member = &schema->members[parent->members + i];
                if (!schema_text_is(name, member->name)) continue;
                if (!schema_is_any(&schema->nodes[member->node]))
                    status = schema_enter(walk, (schema_entry){member->node, value, key, TA_NIL});
                break;
            }
        } else {
            uint32_t value = frame->next, position = frame->item++;
            frame->next = ta_cbor_next(document, value);
            if (!schema_is_any(&schema->nodes[parent->items]))
                status = schema_enter(walk, (schema_entry){parent->items, value, TA_NIL, position});
        }
    }
    return status;
}

/* ---- the interface ----------------------------------------------------------------- */

uint32_t tool_schema_method_find(const tool_schema *schema, ta_string_view name) {
    for (uint32_t i = 0; i < schema->method_count; i++)
        if (schema_text_is(name, schema->methods[i].name)) return i;
    return schema->method_count;
}

ta_status tool_schema_check_message(uint32_t node, const tool_schema_input *input, tool_schema_error *error) {
    if (node >= input->schema->node_count) return TA_ERROR_INVALID;
    /* absent: an empty object, as core reads absent params */
    static const unsigned char empty_bytes[] = {0xA0};
    ta_cbor_token empty_token = {TA_CBOR_MAP, 0, 0};
    ta_cbor_document empty = {{empty_bytes, sizeof empty_bytes}, &empty_token, 1, 1};
    tool_schema_input present = *input;
    if (input->at == TA_NIL) present.document = &empty, present.at = 0;
    schema_walk walk;
    walk.input = &present;
    walk.error = error;
    walk.depth = 0;
    *error = (tool_schema_error){0, 0, {0, 0}, {0, 0}, {0, 0}};
    return schema_walk_run(&walk, node, present.at);
}

ta_status tool_schema_check_params(uint32_t method, const tool_schema_input *input, tool_schema_error *error) {
    if (method >= input->schema->method_count) return TA_ERROR_INVALID;
    uint32_t node = input->schema->methods[method].params;
    if (node == TA_NIL) return TA_OK;
    return tool_schema_check_message(node, input, error);
}

ta_bytes tool_schema_text(void) { return tool_schema_run.text; }
