/* datamodel_test.c — ECMAScript-like test datamodel. See datamodel_test.h. */
#include "datamodel_test.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum { V_UNDEF, V_NULL, V_BOOL, V_NUM, V_STR, V_ARR, V_OBJ } value_kind;

typedef struct test_value {
    value_kind kind;
    int boolean;
    double number;
    const char *text;
    size_t length;
    struct test_value **items; /* arrays: elements; objects: values */
    const char **keys;         /* objects */
    size_t count, capacity;
} test_value;

typedef struct test_variable {
    const char *name;
    test_value *value;
    int read_only;
} test_variable;

typedef struct test_chart {
    const ta_image *image;
    uint32_t chart, binding;
} test_chart;

typedef struct test_instance {
    int live;
    test_chart *chart_context;
    test_variable *variables;
    size_t variable_count, capacity;
} test_instance;

struct datamodel_test {
    void **blocks;
    size_t block_count, block_capacity;
    test_instance **instances;
    size_t instance_count, instance_capacity;
    test_value **payloads;
    size_t payload_count, payload_capacity;
    test_value ***iterations;
    size_t *iteration_counts;
    size_t iteration_count, iteration_capacity;
};

/* ---- arena ------------------------------------------------------------------------- */

static void *A(datamodel_test *datamodel, size_t size) {
    if (datamodel->block_count == datamodel->block_capacity) {
        datamodel->block_capacity = datamodel->block_capacity ? datamodel->block_capacity * 2 : 256;
        datamodel->blocks =
            (void **)realloc(datamodel->blocks, sizeof(void *) * datamodel->block_capacity);
    }
    void *p = calloc(1, size ? size : 1);
    datamodel->blocks[datamodel->block_count++] = p;
    return p;
}

static char *Astr(datamodel_test *datamodel, const char *s, size_t length) {
    char *p = (char *)A(datamodel, length + 1);
    memcpy(p, s, length);
    p[length] = 0;
    return p;
}

static test_value *mk(datamodel_test *datamodel, value_kind k) {
    test_value *value = (test_value *)A(datamodel, sizeof *value);
    value->kind = k;
    return value;
}
static test_value *mknum(datamodel_test *datamodel, double n) {
    test_value *value = mk(datamodel, V_NUM);
    value->number = n;
    return value;
}
static test_value *mkbool(datamodel_test *datamodel, int b) {
    test_value *value = mk(datamodel, V_BOOL);
    value->boolean = b != 0;
    return value;
}
static test_value *mkstr(datamodel_test *datamodel, const char *s, size_t length) {
    test_value *value = mk(datamodel, V_STR);
    value->text = Astr(datamodel, s ? s : "", s ? length : 0);
    value->length = s ? length : 0;
    return value;
}
static void push_item(datamodel_test *datamodel, test_value *a, const char *key, test_value *x) {
    if (a->count == a->capacity) {
        size_t nc = a->capacity ? a->capacity * 2 : 4;
        test_value **ni = (test_value **)A(datamodel, sizeof(test_value *) * nc);
        const char **nk = (const char **)A(datamodel, sizeof(char *) * nc);
        if (a->count) {
            memcpy(ni, a->items, sizeof(test_value *) * a->count);
            if (a->keys) memcpy(nk, a->keys, sizeof(char *) * a->count);
        }
        a->items = ni;
        a->keys = nk;
        a->capacity = nc;
    }
    a->keys[a->count] = key;
    a->items[a->count++] = x;
}
static test_value **obj_slot(datamodel_test *datamodel, test_value *o, const char *key, size_t klen,
                             int create) {
    for (size_t i = 0; i < o->count; i++)
        if (strlen(o->keys[i]) == klen && memcmp(o->keys[i], key, klen) == 0) return &o->items[i];
    if (!create) return NULL;
    push_item(datamodel, o, Astr(datamodel, key, klen), mk(datamodel, V_UNDEF));
    return &o->items[o->count - 1];
}

/* ---- conversions -------------------------------------------------------------------- */

static const char *tostr(datamodel_test *datamodel, const test_value *value) {
    char buffer[64];
    switch (value->kind) {
    case V_UNDEF:
        return "undefined";
    case V_NULL:
        return "null";
    case V_BOOL:
        return value->boolean ? "true" : "false";
    case V_NUM:
        if (isnan(value->number)) return "NaN";
        if (value->number == floor(value->number) && fabs(value->number) < 1e15)
            snprintf(buffer, sizeof buffer, "%lld", (long long)value->number);
        else
            snprintf(buffer, sizeof buffer, "%.15g", value->number);
        return Astr(datamodel, buffer, strlen(buffer));
    case V_STR:
        return value->text;
    case V_ARR: {
        size_t total = 1;
        const char **parts = (const char **)A(datamodel, sizeof(char *) * (value->count + 1));
        for (size_t i = 0; i < value->count; i++) {
            parts[i] = tostr(datamodel, value->items[i]);
            total += strlen(parts[i]) + 1;
        }
        char *out = (char *)A(datamodel, total);
        for (size_t i = 0; i < value->count; i++) {
            if (i) strcat(out, ",");
            strcat(out, parts[i]);
        }
        return out;
    }
    case V_OBJ:
        return "[object Object]";
    }
    return "";
}

static double tonum(const test_value *value) {
    switch (value->kind) {
    case V_NUM:
        return value->number;
    case V_BOOL:
        return value->boolean;
    case V_NULL:
        return 0;
    case V_STR: {
        if (!value->length) return 0;
        char *end;
        double d = strtod(value->text, &end);
        while (*end == ' ')
            end++;
        return *end ? NAN : d;
    }
    default:
        return NAN;
    }
}

static int truthy(const test_value *value) {
    switch (value->kind) {
    case V_UNDEF:
    case V_NULL:
        return 0;
    case V_BOOL:
        return value->boolean;
    case V_NUM:
        return value->number != 0 && !isnan(value->number);
    case V_STR:
        return value->length > 0;
    default:
        return 1;
    }
}

static int strict_equal(const test_value *a, const test_value *b) {
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case V_UNDEF:
    case V_NULL:
        return 1;
    case V_BOOL:
        return a->boolean == b->boolean;
    case V_NUM:
        return a->number == b->number;
    case V_STR:
        return a->length == b->length && memcmp(a->text, b->text, a->length) == 0;
    default:
        return a == b;
    }
}

static int loose_equal(const test_value *a, const test_value *b) {
    if (a->kind == b->kind) return strict_equal(a, b);
    if ((a->kind == V_UNDEF || a->kind == V_NULL) && (b->kind == V_UNDEF || b->kind == V_NULL))
        return 1;
    if (a->kind == V_UNDEF || a->kind == V_NULL || b->kind == V_UNDEF || b->kind == V_NULL)
        return 0;
    if (a->kind == V_ARR || a->kind == V_OBJ || b->kind == V_ARR || b->kind == V_OBJ) return 0;
    return tonum(a) == tonum(b);
}

/* ---- instance variables ------------------------------------------------------------- */

static test_variable *find_var(test_instance *instance, const char *name, size_t length) {
    for (size_t i = 0; i < instance->variable_count; i++)
        if (strlen(instance->variables[i].name) == length &&
            memcmp(instance->variables[i].name, name, length) == 0)
            return &instance->variables[i];
    return NULL;
}

static test_variable *declare(datamodel_test *datamodel, test_instance *instance, const char *name,
                              size_t length) {
    test_variable *variable = find_var(instance, name, length);
    if (variable) return variable;
    if (instance->variable_count == instance->capacity) {
        instance->capacity = instance->capacity ? instance->capacity * 2 : 16;
        test_variable *nv =
            (test_variable *)A(datamodel, sizeof(test_variable) * instance->capacity);
        if (instance->variable_count)
            memcpy(nv, instance->variables, sizeof(test_variable) * instance->variable_count);
        instance->variables = nv;
    }
    variable = &instance->variables[instance->variable_count++];
    variable->name = Astr(datamodel, name, length);
    variable->value = mk(datamodel, V_UNDEF);
    variable->read_only = 0;
    return variable;
}

/* ---- parser / evaluator -------------------------------------------------------------- */

typedef struct test_parser {
    datamodel_test *datamodel;
    test_instance *instance;
    ta_datamodel_environment *environment;
    const char *text;
    size_t i, number;
    int error;
} test_parser;

static void skip_whitespace(test_parser *parser) {
    while (parser->i < parser->number &&
           (parser->text[parser->i] == ' ' || parser->text[parser->i] == '\t' ||
            parser->text[parser->i] == '\n' || parser->text[parser->i] == '\r'))
        parser->i++;
}
static int peek(test_parser *parser, const char *t) {
    skip_whitespace(parser);
    size_t l = strlen(t);
    return parser->number - parser->i >= l && memcmp(parser->text + parser->i, t, l) == 0;
}
static int eat(test_parser *parser, const char *t) {
    if (!peek(parser, t)) return 0;
    parser->i += strlen(t);
    return 1;
}
static int isid0(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$';
}
static int isid(char c) {
    return isid0(c) || (c >= '0' && c <= '9');
}
static int ident(test_parser *parser, const char **s, size_t *length) {
    skip_whitespace(parser);
    if (parser->i >= parser->number || !isid0(parser->text[parser->i])) return 0;
    size_t b = parser->i;
    while (parser->i < parser->number && isid(parser->text[parser->i]))
        parser->i++;
    *s = parser->text + b;
    *length = parser->i - b;
    return 1;
}
static int keyword(test_parser *parser, const char *kw) {
    skip_whitespace(parser);
    size_t l = strlen(kw), save = parser->i;
    if (parser->number - parser->i >= l && memcmp(parser->text + parser->i, kw, l) == 0 &&
        (parser->i + l == parser->number || !isid(parser->text[parser->i + l]))) {
        parser->i += l;
        return 1;
    }
    parser->i = save;
    return 0;
}
static test_value *fail(test_parser *parser) {
    parser->error = 1;
    return mk(parser->datamodel, V_UNDEF);
}

static test_value *expression(test_parser *p, int event);

static test_value *string_lit(test_parser *parser) {
    char q = parser->text[parser->i++];
    size_t capacity = 16, length = 0;
    char *buffer = (char *)malloc(capacity);
    while (parser->i < parser->number && parser->text[parser->i] != q) {
        char c = parser->text[parser->i++];
        if (c == '\\' && parser->i < parser->number) {
            c = parser->text[parser->i++];
            if (c == 'n')
                c = '\n';
            else if (c == 't')
                c = '\t';
        }
        if (length + 1 >= capacity) buffer = (char *)realloc(buffer, capacity *= 2);
        buffer[length++] = c;
    }
    if (parser->i >= parser->number) {
        free(buffer);
        return fail(parser);
    }
    parser->i++;
    test_value *value = mkstr(parser->datamodel, buffer, length);
    free(buffer);
    return value;
}

static test_value *member(test_parser *parser, test_value *base, const char *key, size_t klen,
                          int event) {
    if (!event) return base;
    if (base->kind == V_UNDEF || base->kind == V_NULL) return fail(parser);
    if (klen == 6 && memcmp(key, "length", 6) == 0) {
        if (base->kind == V_STR) return mknum(parser->datamodel, (double)base->length);
        if (base->kind == V_ARR) return mknum(parser->datamodel, (double)base->count);
    }
    if (base->kind == V_OBJ) {
        test_value **s = obj_slot(parser->datamodel, base, key, klen, 0);
        return s ? *s : mk(parser->datamodel, V_UNDEF);
    }
    if (base->kind == V_ARR) {
        char temporary[32];
        size_t l = klen < sizeof temporary - 1 ? klen : sizeof temporary - 1;
        memcpy(temporary, key, l);
        temporary[l] = 0;
        char *end;
        long index = strtol(temporary, &end, 10);
        if (*end == 0 && index >= 0 && (size_t)index < base->count) return base->items[index];
    }
    return mk(parser->datamodel, V_UNDEF);
}

static test_value *call_in(test_parser *parser, int event) {
    if (!eat(parser, "(")) return fail(parser);
    test_value *a = expression(parser, event);
    if (!eat(parser, ")")) return fail(parser);
    if (!event) return a;
    if (a->kind != V_STR || !parser->environment) return mkbool(parser->datamodel, 0);
    ta_string_view id = {a->text, (uint32_t)a->length};
    uint32_t state = ta_image_state_by_name(parser->instance->chart_context->image,
                                            parser->instance->chart_context->chart, id);
    return mkbool(parser->datamodel,
                  state != TA_NIL && ta_datamodel_environment_in(parser->environment, state));
}

static test_value *primary(test_parser *parser, int event) {
    skip_whitespace(parser);
    if (parser->i >= parser->number) return fail(parser);
    char c = parser->text[parser->i];
    if (c == '(') {
        parser->i++;
        test_value *v = expression(parser, event);
        if (!eat(parser, ")")) return fail(parser);
        return v;
    }
    if (c == '\'' || c == '"') return string_lit(parser);
    if ((c >= '0' && c <= '9') ||
        (c == '.' && parser->i + 1 < parser->number && parser->text[parser->i + 1] >= '0' &&
         parser->text[parser->i + 1] <= '9')) {
        char *end;
        char temporary[64];
        size_t l = parser->number - parser->i < sizeof temporary - 1 ? parser->number - parser->i
                                                                     : sizeof temporary - 1;
        memcpy(temporary, parser->text + parser->i, l);
        temporary[l] = 0;
        double d = strtod(temporary, &end);
        parser->i += (size_t)(end - temporary);
        return mknum(parser->datamodel, d);
    }
    if (c == '[') {
        parser->i++;
        test_value *a = mk(parser->datamodel, V_ARR);
        if (!eat(parser, "]")) {
            do
                push_item(parser->datamodel, a, NULL, expression(parser, event));
            while (eat(parser, ","));
            if (!eat(parser, "]")) return fail(parser);
        }
        return a;
    }
    if (c == '{') {
        parser->i++;
        test_value *o = mk(parser->datamodel, V_OBJ);
        if (!eat(parser, "}")) {
            do {
                const char *k;
                size_t kl;
                skip_whitespace(parser);
                test_value *ks = NULL;
                if (parser->i < parser->number &&
                    (parser->text[parser->i] == '\'' || parser->text[parser->i] == '"')) {
                    ks = string_lit(parser);
                    k = ks->text;
                    kl = ks->length;
                } else if (!ident(parser, &k, &kl)) {
                    return fail(parser);
                }
                if (!eat(parser, ":")) return fail(parser);
                test_value *x = expression(parser, event);
                *obj_slot(parser->datamodel, o, k, kl, 1) = x;
            } while (eat(parser, ","));
            if (!eat(parser, "}")) return fail(parser);
        }
        return o;
    }
    if (keyword(parser, "true")) return mkbool(parser->datamodel, 1);
    if (keyword(parser, "false")) return mkbool(parser->datamodel, 0);
    if (keyword(parser, "null")) return mk(parser->datamodel, V_NULL);
    if (keyword(parser, "undefined")) return mk(parser->datamodel, V_UNDEF);
    if (keyword(parser, "In")) return call_in(parser, event);
    const char *id;
    size_t length;
    if (!ident(parser, &id, &length)) return fail(parser);
    if (!event) return mk(parser->datamodel, V_UNDEF);
    test_variable *v = find_var(parser->instance, id, length);
    if (!v) return fail(parser); /* ReferenceError */
    return v->value;
}

static test_value *postfix(test_parser *parser, int event) {
    test_value *value = primary(parser, event);
    for (;;) {
        if (parser->error) return value;
        if (peek(parser, ".") && !peek(parser, "..")) {
            parser->i++;
            const char *k;
            size_t kl;
            if (!ident(parser, &k, &kl)) return fail(parser);
            value = member(parser, value, k, kl, event);
        } else if (eat(parser, "[")) {
            test_value *ix = expression(parser, event);
            if (!eat(parser, "]")) return fail(parser);
            if (event) {
                const char *ks = tostr(parser->datamodel, ix);
                value = member(parser, value, ks, strlen(ks), event);
            }
        } else {
            return value;
        }
    }
}

static test_value *unary(test_parser *parser, int event) {
    if (eat(parser, "!")) {
        test_value *value = unary(parser, event);
        return event ? mkbool(parser->datamodel, !truthy(value)) : value;
    }
    if (peek(parser, "-") && !peek(parser, "--")) {
        parser->i++;
        test_value *value = unary(parser, event);
        return event ? mknum(parser->datamodel, -tonum(value)) : value;
    }
    if (keyword(parser, "typeof")) {
        size_t save_error = (size_t)parser->error;
        test_value *value = unary(parser, event);
        if (!event) return value;
        if (parser->error && !save_error) { /* typeof undeclared -> "undefined" */
            parser->error = 0;
            return mkstr(parser->datamodel, "undefined", 9);
        }
        const char *t = value->kind == V_UNDEF  ? "undefined"
                        : value->kind == V_BOOL ? "boolean"
                        : value->kind == V_NUM  ? "number"
                        : value->kind == V_STR  ? "string"
                                                : "object";
        return mkstr(parser->datamodel, t, strlen(t));
    }
    return postfix(parser, event);
}

static test_value *mul(test_parser *parser, int event) {
    test_value *a = unary(parser, event);
    for (;;) {
        int operator_text = peek(parser, "*")   ? '*'
                            : peek(parser, "/") ? '/'
                                                : (peek(parser, "%") ? '%' : 0);
        if (!operator_text || parser->error) return a;
        parser->i++;
        test_value *b = unary(parser, event);
        if (event) {
            double x = tonum(a), y = tonum(b);
            a = mknum(parser->datamodel, operator_text == '*'   ? x * y
                                         : operator_text == '/' ? x / y
                                                                : fmod(x, y));
        }
    }
}

static test_value *add(test_parser *parser, int event) {
    test_value *a = mul(parser, event);
    for (;;) {
        int operator_text = peek(parser, "+") && !peek(parser, "++")
                                ? '+'
                                : (peek(parser, "-") && !peek(parser, "--") ? '-' : 0);
        if (!operator_text || parser->error) return a;
        parser->i++;
        test_value *b = mul(parser, event);
        if (!event) continue;
        if (operator_text == '+' && (a->kind == V_STR || b->kind == V_STR)) {
            const char *x = tostr(parser->datamodel, a), *y = tostr(parser->datamodel, b);
            size_t lx = strlen(x), ly = strlen(y);
            char *s = (char *)A(parser->datamodel, lx + ly + 1);
            memcpy(s, x, lx);
            memcpy(s + lx, y, ly);
            test_value *value = mk(parser->datamodel, V_STR);
            value->text = s;
            value->length = lx + ly;
            a = value;
        } else {
            a = mknum(parser->datamodel,
                      operator_text == '+' ? tonum(a) + tonum(b) : tonum(a) - tonum(b));
        }
    }
}

static test_value *relational(test_parser *parser, int event) {
    test_value *a = add(parser, event);
    for (;;) {
        const char *operator_text = peek(parser, "<=")   ? "<="
                                    : peek(parser, ">=") ? ">="
                                    : peek(parser, "<")  ? "<"
                                    : peek(parser, ">")  ? ">"
                                                         : NULL;
        if (!operator_text || parser->error) return a;
        parser->i += strlen(operator_text);
        test_value *b = add(parser, event);
        if (!event) continue;
        int r;
        if (a->kind == V_STR && b->kind == V_STR) {
            int c = strcmp(a->text, b->text);
            r = operator_text[0] == '<' ? (operator_text[1] ? c <= 0 : c < 0)
                                        : (operator_text[1] ? c >= 0 : c > 0);
        } else {
            double x = tonum(a), y = tonum(b);
            r = operator_text[0] == '<' ? (operator_text[1] ? x <= y : x < y)
                                        : (operator_text[1] ? x >= y : x > y);
        }
        a = mkbool(parser->datamodel, r);
    }
}

static test_value *equality(test_parser *parser, int event) {
    test_value *a = relational(parser, event);
    for (;;) {
        const char *operator_text = peek(parser, "===")   ? "==="
                                    : peek(parser, "!==") ? "!=="
                                    : peek(parser, "==")  ? "=="
                                    : peek(parser, "!=")  ? "!="
                                                          : NULL;
        if (!operator_text || parser->error) return a;
        parser->i += strlen(operator_text);
        test_value *b = relational(parser, event);
        if (!event) continue;
        int r = strlen(operator_text) == 3 ? strict_equal(a, b) : loose_equal(a, b);
        a = mkbool(parser->datamodel, operator_text[0] == '!' ? !r : r);
    }
}

static test_value *and_(test_parser *parser, int event) {
    test_value *a = equality(parser, event);
    while (!parser->error && eat(parser, "&&")) {
        int go = event && truthy(a);
        test_value *b = equality(parser, go);
        if (go) a = b;
    }
    return a;
}

static test_value *expression(test_parser *parser, int event) {
    test_value *a = and_(parser, event);
    while (!parser->error && eat(parser, "||")) {
        int go = event && !truthy(a);
        test_value *b = and_(parser, go);
        if (go) a = b;
    }
    return a;
}

/* Location: ident ( .key | [expr] )*  -> slot pointer (NULL on error). */
static test_value **lvalue(test_parser *parser, int declare_ok, int event) {
    const char *id;
    size_t length;
    if (!ident(parser, &id, &length)) {
        parser->error = 1;
        return NULL;
    }
    static test_value *dummy;
    test_variable *variable = event ? find_var(parser->instance, id, length) : NULL;
    if (event && !variable) {
        if (!declare_ok) {
            parser->error = 1;
            return NULL;
        }
        variable = declare(parser->datamodel, parser->instance, id, length);
    }
    if (event && variable->read_only) {
        parser->error = 1;
        return NULL;
    }
    test_value **slot = event ? &variable->value : &dummy;
    for (;;) {
        if (peek(parser, ".") && !peek(parser, "..")) {
            parser->i++;
            const char *k;
            size_t kl;
            if (!ident(parser, &k, &kl)) {
                parser->error = 1;
                return NULL;
            }
            if (event) {
                if ((*slot)->kind != V_OBJ) {
                    parser->error = 1;
                    return NULL;
                }
                slot = obj_slot(parser->datamodel, *slot, k, kl, 1);
            }
        } else if (eat(parser, "[")) {
            test_value *ix = expression(parser, event);
            if (!eat(parser, "]")) {
                parser->error = 1;
                return NULL;
            }
            if (event) {
                if ((*slot)->kind == V_ARR && ix->kind == V_NUM && ix->number >= 0) {
                    size_t i = (size_t)ix->number;
                    while ((*slot)->count <= i)
                        push_item(parser->datamodel, *slot, NULL, mk(parser->datamodel, V_UNDEF));
                    slot = &(*slot)->items[i];
                } else if ((*slot)->kind == V_OBJ) {
                    const char *ks = tostr(parser->datamodel, ix);
                    slot = obj_slot(parser->datamodel, *slot, ks, strlen(ks), 1);
                } else {
                    parser->error = 1;
                    return NULL;
                }
            }
        } else {
            return slot;
        }
    }
}

static void stmt(test_parser *parser) {
    if (keyword(parser, "var")) {
        const char *id;
        size_t length;
        if (!ident(parser, &id, &length)) {
            parser->error = 1;
            return;
        }
        test_variable *variable = declare(parser->datamodel, parser->instance, id, length);
        if (eat(parser, "=")) {
            test_value *x = expression(parser, 1);
            if (!parser->error) variable->value = x;
        }
        return;
    }
    size_t save = parser->i;
    int save_error = parser->error;
    /* try: location ( '=' expr | '++' ) */
    (void)lvalue(parser, 0, 0);
    int is_assign = !parser->error && peek(parser, "=") && !peek(parser, "==");
    int is_inc = !parser->error && peek(parser, "++");
    parser->i = save;
    parser->error = save_error;
    if (is_assign || is_inc) {
        test_value **slot = lvalue(parser, 0, 1);
        if (!slot) return;
        if (is_assign) {
            eat(parser, "=");
            test_value *x = expression(parser, 1);
            if (!parser->error) *slot = x;
        } else {
            eat(parser, "++");
            *slot = mknum(parser->datamodel, tonum(*slot) + 1);
        }
        return;
    }
    (void)expression(parser, 1);
}

static void init_p(test_parser *parser, datamodel_test *datamodel, test_instance *instance,
                   ta_datamodel_environment *environment, ta_string_view source) {
    parser->datamodel = datamodel;
    parser->instance = instance;
    parser->environment = environment;
    parser->text = source.pointer ? source.pointer : "";
    parser->i = 0;
    parser->number = source.length;
    parser->error = 0;
}

static int at_end(test_parser *parser) {
    skip_whitespace(parser);
    return parser->i == parser->number;
}

/* Evaluate a full expression string. */
static test_value *eval_source(datamodel_test *datamodel, test_instance *instance,
                               ta_datamodel_environment *environment, ta_string_view source,
                               int *error) {
    test_parser parser;
    init_p(&parser, datamodel, instance, environment, source);
    test_value *value = expression(&parser, 1);
    if (!parser.error && !at_end(&parser)) parser.error = 1;
    *error = parser.error;
    return value;
}

static ta_string_view expression_source(test_instance *instance, ta_expression expression,
                                        uint32_t *kind) {
    ta_expression_info info;
    ta_string_view none = {"", 0};
    if (ta_image_expression(instance->chart_context->image,
                            (ta_chart_item){instance->chart_context->chart, expression},
                            &info) != TA_OK)
        return none;
    if (kind) *kind = info.kind;
    return info.source;
}

/* Evaluate an expression id; CONTENT falls back to a string if it is not an expression. */
static test_value *eval_expression(datamodel_test *datamodel, test_instance *instance,
                                   ta_datamodel_environment *environment, ta_expression expression,
                                   int *error) {
    uint32_t kind = 0;
    ta_string_view source = expression_source(instance, expression, &kind);
    test_value *value = eval_source(datamodel, instance, environment, source, error);
    if (*error && kind == TA_EXPRESSION_CONTENT) {
        *error = 0;
        /* normalize whitespace like many implementations do for text content */
        return mkstr(datamodel, source.pointer, source.length);
    }
    return value;
}

/* ---- vtable --------------------------------------------------------------------------- */

static test_instance *I(datamodel_test *datamodel, uint64_t h) {
    return datamodel->instances[h - 1];
}

static ta_status t_attach(void *d, const ta_datamodel_chart *chart, void **context) {
    datamodel_test *datamodel = (datamodel_test *)d;
    test_chart *test_chart_record = (test_chart *)A(datamodel, sizeof *test_chart_record);
    test_chart_record->image = chart->image;
    test_chart_record->chart = chart->chart;
    test_chart_record->binding = chart->binding;
    *context = test_chart_record;
    return TA_OK;
}

static ta_status t_create(void *d, void *context, const ta_datamodel_system_variables *variables,
                          ta_payload init, uint64_t *out) {
    datamodel_test *datamodel = (datamodel_test *)d;
    test_instance *instance = (test_instance *)A(datamodel, sizeof *instance);
    instance->live = 1;
    instance->chart_context = (test_chart *)context;
    test_variable *variable = declare(datamodel, instance, "_sessionid", 10);
    variable->value = mkstr(datamodel, variables->sessionid.pointer, variables->sessionid.length);
    variable->read_only = 1;
    variable = declare(datamodel, instance, "_name", 5);
    variable->value = mkstr(datamodel, variables->name.pointer, variables->name.length);
    variable->read_only = 1;
    test_value *io = mk(datamodel, V_OBJ);
    for (uint32_t i = 0; i < variables->io_processor_count; i++) {
        test_value *e = mk(datamodel, V_OBJ);
        *obj_slot(datamodel, e, "location", 8, 1) =
            mkstr(datamodel, variables->io_processors[i].location.pointer,
                  variables->io_processors[i].location.length);
        *obj_slot(datamodel, io, variables->io_processors[i].type.pointer,
                  variables->io_processors[i].type.length, 1) = e;
    }
    variable = declare(datamodel, instance, "_ioprocessors", 13);
    variable->value = io;
    variable->read_only = 1;
    /* data ids exist from the start (late binding: undefined until entry) */
    uint32_t nd =
        ta_image_data_count(instance->chart_context->image, instance->chart_context->chart);
    for (uint32_t i = 0; i < nd; i++) {
        ta_data_info data_info;
        ta_image_data(instance->chart_context->image,
                      (ta_chart_item){instance->chart_context->chart, i}, &data_info);
        declare(datamodel, instance, data_info.id.pointer, data_info.id.length);
    }
    /* invoke params override matching <data> ids (stored as "_init") */
    if (init) {
        test_value *pv = datamodel->payloads[init - 1];
        test_variable *iv = declare(datamodel, instance, "_init", 5);
        iv->value = pv;
        iv->read_only = 1;
    }
    if (datamodel->instance_count == datamodel->instance_capacity) {
        datamodel->instance_capacity =
            datamodel->instance_capacity ? datamodel->instance_capacity * 2 : 16;
        datamodel->instances = (test_instance **)realloc(
            datamodel->instances, sizeof(test_instance *) * datamodel->instance_capacity);
    }
    datamodel->instances[datamodel->instance_count++] = instance;
    *out = datamodel->instance_count;
    return TA_OK;
}

static void t_destroy(void *d, uint64_t h) {
    I((datamodel_test *)d, h)->live = 0;
}

static ta_status t_init_data(void *d, uint64_t h, ta_datamodel_environment *environment,
                             uint32_t declaration) {
    datamodel_test *datamodel = (datamodel_test *)d;
    test_instance *instance = I(datamodel, h);
    ta_data_info data_info;
    if (ta_image_data(instance->chart_context->image,
                      (ta_chart_item){instance->chart_context->chart, declaration},
                      &data_info) != TA_OK)
        return TA_ERROR_EXECUTION;
    test_variable *variable =
        declare(datamodel, instance, data_info.id.pointer, data_info.id.length);
    test_variable *init = find_var(instance, "_init", 5);
    if (init && init->value->kind == V_OBJ) {
        test_value **s =
            obj_slot(datamodel, init->value, data_info.id.pointer, data_info.id.length, 0);
        if (s) {
            variable->value = *s;
            return TA_OK;
        }
    }
    if (data_info.expression == TA_NIL) {
        variable->value = mk(datamodel, V_UNDEF);
        return TA_OK;
    }
    int error;
    test_value *x = eval_expression(datamodel, instance, environment, data_info.expression, &error);
    if (error) {
        variable->value = mk(datamodel, V_UNDEF);
        return TA_ERROR_EXECUTION;
    }
    variable->value = x;
    return TA_OK;
}

static ta_status t_bind_event(void *d, uint64_t h, ta_datamodel_environment *environment,
                              const ta_event_view *view) {
    (void)environment;
    datamodel_test *datamodel = (datamodel_test *)d;
    test_instance *instance = I(datamodel, h);
    test_value *o = mk(datamodel, V_OBJ);
    static const char *const types[] = {"platform", "internal", "external"};
    *obj_slot(datamodel, o, "name", 4, 1) = mkstr(datamodel, view->name.pointer, view->name.length);
    *obj_slot(datamodel, o, "type", 4, 1) =
        mkstr(datamodel, types[view->type % 3], strlen(types[view->type % 3]));
#define FIELD(nm, f)                                                                               \
    *obj_slot(datamodel, o, nm, strlen(nm), 1) =                                                   \
        view->f.length ? mkstr(datamodel, view->f.pointer, view->f.length)                         \
                       : mk(datamodel, V_UNDEF)
    FIELD("sendid", sendid);
    FIELD("origin", origin);
    FIELD("origintype", origintype);
    FIELD("invokeid", invokeid);
#undef FIELD
    *obj_slot(datamodel, o, "data", 4, 1) =
        view->data ? datamodel->payloads[view->data - 1] : mk(datamodel, V_UNDEF);
    test_variable *variable = declare(datamodel, instance, "_event", 6);
    variable->value = o;
    variable->read_only = 1;
    return TA_OK;
}

static ta_status t_condition(void *d, uint64_t h, ta_datamodel_environment *environment,
                             ta_expression expression, int *out) {
    int error;
    test_value *value = eval_expression((datamodel_test *)d, I((datamodel_test *)d, h), environment,
                                        expression, &error);
    if (error) return TA_ERROR_EXECUTION;
    *out = truthy(value);
    return TA_OK;
}

static ta_status t_string(void *d, uint64_t h, ta_datamodel_environment *environment,
                          ta_expression expression, ta_string_view *out) {
    datamodel_test *datamodel = (datamodel_test *)d;
    int error;
    test_value *value =
        eval_expression(datamodel, I(datamodel, h), environment, expression, &error);
    if (error) return TA_ERROR_EXECUTION;
    const char *s = tostr(datamodel, value);
    out->pointer = s;
    out->length = (uint32_t)strlen(s);
    return TA_OK;
}

static ta_status assign_value(datamodel_test *datamodel, test_instance *instance,
                              ta_datamodel_environment *environment, ta_expression location,
                              test_value *x, int declare_ok) {
    test_parser parser;
    init_p(&parser, datamodel, instance, environment, expression_source(instance, location, NULL));
    test_value **slot = lvalue(&parser, declare_ok, 1);
    if (!slot || parser.error || !at_end(&parser)) return TA_ERROR_EXECUTION;
    *slot = x;
    return TA_OK;
}

static ta_status t_assign(void *d, uint64_t h, ta_datamodel_environment *environment,
                          ta_expression location, ta_expression expression) {
    datamodel_test *datamodel = (datamodel_test *)d;
    test_instance *instance = I(datamodel, h);
    int error;
    test_value *x = eval_expression(datamodel, instance, environment, expression, &error);
    if (error) return TA_ERROR_EXECUTION;
    return assign_value(datamodel, instance, environment, location, x, 0);
}

static ta_status t_assign_string(void *d, uint64_t h, ta_expression location, ta_string_view v) {
    datamodel_test *datamodel = (datamodel_test *)d;
    return assign_value(datamodel, I(datamodel, h), NULL, location,
                        mkstr(datamodel, v.pointer, v.length), 0);
}

static ta_status t_script(void *d, uint64_t h, ta_datamodel_environment *environment,
                          ta_expression expression) {
    datamodel_test *datamodel = (datamodel_test *)d;
    test_instance *instance = I(datamodel, h);
    test_parser parser;
    init_p(&parser, datamodel, instance, environment,
           expression_source(instance, expression, NULL));
    while (!parser.error && !at_end(&parser)) {
        stmt(&parser);
        if (!eat(&parser, ";") && !at_end(&parser)) parser.error = 1;
    }
    return parser.error ? TA_ERROR_EXECUTION : TA_OK;
}

static ta_status t_log(void *d, uint64_t h, ta_datamodel_environment *environment,
                       ta_expression expression, ta_string_view *out) {
    return t_string(d, h, environment, expression, out);
}

static ta_status t_foreach_begin(void *d, uint64_t h, ta_datamodel_environment *environment,
                                 ta_expression arr, uint64_t *iter, uint32_t *n) {
    datamodel_test *datamodel = (datamodel_test *)d;
    int error;
    test_value *a = eval_expression(datamodel, I(datamodel, h), environment, arr, &error);
    if (error || a->kind != V_ARR) return TA_ERROR_EXECUTION;
    if (datamodel->iteration_count == datamodel->iteration_capacity) {
        datamodel->iteration_capacity =
            datamodel->iteration_capacity ? datamodel->iteration_capacity * 2 : 8;
        datamodel->iterations = (test_value ***)realloc(
            datamodel->iterations, sizeof(test_value **) * datamodel->iteration_capacity);
        datamodel->iteration_counts = (size_t *)realloc(
            datamodel->iteration_counts, sizeof(size_t) * datamodel->iteration_capacity);
    }
    test_value **copy =
        (test_value **)A(datamodel, sizeof(test_value *) * (a->count + 1)); /* shallow copy */
    if (a->count) memcpy(copy, a->items, sizeof(test_value *) * a->count);
    datamodel->iterations[datamodel->iteration_count] = copy;
    datamodel->iteration_counts[datamodel->iteration_count] = a->count;
    *iter = ++datamodel->iteration_count;
    *n = (uint32_t)a->count;
    return TA_OK;
}

static ta_status t_foreach_item(void *d, uint64_t h, uint64_t iter, uint32_t i, ta_expression item,
                                ta_expression index) {
    datamodel_test *datamodel = (datamodel_test *)d;
    test_instance *instance = I(datamodel, h);
    if (assign_value(datamodel, instance, NULL, item, datamodel->iterations[iter - 1][i], 1) !=
        TA_OK)
        return TA_ERROR_EXECUTION;
    if (index != TA_NIL &&
        assign_value(datamodel, instance, NULL, index, mknum(datamodel, i), 1) != TA_OK)
        return TA_ERROR_EXECUTION;
    return TA_OK;
}

static ta_payload new_payload(datamodel_test *datamodel, test_value *value) {
    if (datamodel->payload_count == datamodel->payload_capacity) {
        datamodel->payload_capacity =
            datamodel->payload_capacity ? datamodel->payload_capacity * 2 : 64;
        datamodel->payloads = (test_value **)realloc(
            datamodel->payloads, sizeof(test_value *) * datamodel->payload_capacity);
    }
    datamodel->payloads[datamodel->payload_count++] = value;
    return datamodel->payload_count;
}

static ta_status t_capture(void *d, uint64_t h, ta_datamodel_environment *environment,
                           const ta_datamodel_capture *capture, ta_payload *out) {
    datamodel_test *datamodel = (datamodel_test *)d;
    test_instance *instance = I(datamodel, h);
    int error;
    if (capture->content != TA_NIL) {
        test_value *value =
            eval_expression(datamodel, instance, environment, capture->content, &error);
        if (error) return TA_ERROR_EXECUTION;
        *out = new_payload(datamodel, value);
        return TA_OK;
    }
    test_value *o = mk(datamodel, V_OBJ);
    for (uint32_t i = 0; i < capture->namelist_count; i++) {
        ta_string_view source = expression_source(instance, capture->namelist[i], NULL);
        test_value *value = eval_source(datamodel, instance, environment, source, &error);
        if (error) return TA_ERROR_EXECUTION;
        *obj_slot(datamodel, o, source.pointer, source.length, 1) = value;
    }
    for (uint32_t i = 0; i < capture->param_count; i++) {
        ta_string_view name =
            ta_image_string(instance->chart_context->image, capture->params[2 * i]);
        test_value *value =
            eval_expression(datamodel, instance, environment, capture->params[2 * i + 1], &error);
        if (error) return TA_ERROR_EXECUTION;
        *obj_slot(datamodel, o, name.pointer, name.length, 1) = value;
    }
    *out = new_payload(datamodel, o);
    return TA_OK;
}

static const ta_datamodel_vtable test_vt = {sizeof(ta_datamodel_vtable),
                                            0,
                                            "ecmascript",
                                            t_attach,
                                            NULL,
                                            t_create,
                                            t_destroy,
                                            t_init_data,
                                            t_bind_event,
                                            NULL,
                                            t_condition,
                                            t_string,
                                            t_assign,
                                            t_assign_string,
                                            t_script,
                                            t_log,
                                            t_foreach_begin,
                                            t_foreach_item,
                                            NULL,
                                            t_capture,
                                            NULL,
                                            NULL,
                                            NULL,
                                            NULL, /* serialize */
                                            NULL, /* deserialize */
                                            NULL /* migrate */,
                                            NULL /* to_wire */,
                                            NULL /* from_wire */,
                                            NULL /* payload_serialize */,
                                            NULL /* payload_deserialize */};

/* ---- public helpers ---------------------------------------------------------------------- */

datamodel_test *datamodel_test_new(void) {
    return (datamodel_test *)calloc(1, sizeof(datamodel_test));
}

void datamodel_test_free(datamodel_test *datamodel) {
    for (size_t i = 0; i < datamodel->block_count; i++)
        free(datamodel->blocks[i]);
    free(datamodel->blocks);
    free(datamodel->instances);
    free(datamodel->payloads);
    free(datamodel->iterations);
    free(datamodel->iteration_counts);
    free(datamodel);
}

ta_datamodel_binding datamodel_test_binding(datamodel_test *datamodel) {
    ta_datamodel_binding binding = {&test_vt, datamodel};
    return binding;
}

const char *datamodel_test_variable(datamodel_test *datamodel, uint64_t h, const char *name) {
    test_variable *variable = find_var(I(datamodel, h), name, strlen(name));
    return variable ? tostr(datamodel, variable->value) : "<undeclared>";
}

const char *datamodel_test_eval(datamodel_test *datamodel, uint64_t h, const char *e) {
    int error;
    ta_string_view s = {e, (uint32_t)strlen(e)};
    test_value *value = eval_source(datamodel, I(datamodel, h), NULL, s, &error);
    return error ? "<error>" : tostr(datamodel, value);
}

ta_payload datamodel_test_payload(datamodel_test *datamodel, const char *e) {
    test_instance temporary;
    memset(&temporary, 0, sizeof temporary);
    int error;
    ta_string_view s = {e, (uint32_t)strlen(e)};
    test_value *value = eval_source(datamodel, &temporary, NULL, s, &error);
    return error ? 0 : new_payload(datamodel, value);
}

const char *datamodel_test_payload_string(datamodel_test *datamodel, ta_payload payload) {
    return payload ? tostr(datamodel, datamodel->payloads[payload - 1]) : "<none>";
}

int datamodel_test_live(datamodel_test *datamodel) {
    int n = 0;
    for (size_t i = 0; i < datamodel->instance_count; i++)
        n += datamodel->instances[i]->live;
    return n;
}
