/* datamodel_staticjs.c — a "staticjs" datamodel over the staticjs library, a
 * spike for comparison with src/datamodel_json (branch work).
 *
 * Each chart's expressions compile once into staticjs programs whose globals
 * are, in order: the chart's variables (its <data> ids and its <foreach>
 * items and indexes), _event, _sessionid, _name, _ioprocessors, In, and the
 * functions registered for the chart. A session holds one staticjs value per
 * variable. Values never change once shared, so a write at a path rebuilds
 * the containers along it. Freestanding; it allocates through its config's
 * allocator, staticjs's context included.
 */
#include <tinyactors/cbor.h>
#include <tinyactors/datamodel_staticjs.h>
#include <tinyactors/image.h>
#include <tinyactors/table.h>

#include "base/memory.h"

#define NIL 0xFFFFFFFFu
enum { G_EVENT, G_SESSIONID, G_NAME, G_IOPROCESSORS, G_IN, G_FIXED }; /* after the variables */

/* ---- memory ------------------------------------------------------------------------------ */

typedef struct smemory {
    ta_allocator allocator;
    size_t bytes;
} smemory;

static void *salloc(smemory *m, size_t size) {
    if (!size) size = 1;
    size_t *p = (size_t *)m->allocator.alloc(m->allocator.context, size + 16, 16);
    if (!p) return 0;
    *p = size;
    m->bytes += size + 16;
    return (uint8_t *)p + 16;
}

static void sfree(smemory *m, void *q) {
    if (!q) return;
    size_t *p = (size_t *)(void *)((uint8_t *)q - 16);
    m->bytes -= *p + 16;
    m->allocator.free(m->allocator.context, p, *p + 16);
}

/* staticjs's allocator and the tables', counted with the rest */
static void *sjs_allocate(void *self, size_t size, size_t alignment) {
    smemory *m = (smemory *)self;
    void *p = m->allocator.alloc(m->allocator.context, size, alignment);
    if (p) m->bytes += size;
    return p;
}

static void sjs_free_block(void *self, const sjs_allocator_free_params *params) {
    smemory *m = (smemory *)self;
    m->bytes -= params->size;
    m->allocator.free(m->allocator.context, params->ptr, params->size);
}

static const sjs_allocator_vtable staticjs_heap = {sjs_allocate, sjs_free_block};

static void *table_allocate(void *self, size_t size, size_t alignment) { return sjs_allocate(self, size, alignment); }

static void table_free(void *self, void *p, size_t size) {
    smemory *m = (smemory *)self;
    m->bytes -= size;
    m->allocator.free(m->allocator.context, p, size);
}

/* ---- the datamodel's records ---------------------------------------------------------- */

typedef struct ssegment {
    sjs_str key;          /* .name or ["literal"]; empty with program set: [expression] */
    sjs_program *program; /* the index's expression */
} ssegment;

typedef struct sexpression {
    sjs_program *program; /* an expression's */
    uint32_t root;        /* a path's root variable; NIL: not a path */
    uint32_t segment_count;
    ssegment *segments;
    sjs_str error;        /* why it cannot be evaluated; empty: it can */
} sexpression;

typedef struct sfunction {
    sjs_str name, chart;
    sjs_value value; /* the native function value */
} sfunction;

typedef struct schart {
    const ta_image *image;
    uint32_t chart, segment;
    struct schart *next;
    uint32_t variable_count, global_count;
    sjs_str *globals;           /* names, in the globals' order */
    sjs_value *function_values; /* borrowed: the datamodel's */
    uint32_t function_count;
    uint32_t *data_slot;        /* per <data>: its variable */
    uint32_t data_count;
    sexpression *expressions;
    uint32_t expression_count;
} schart;

typedef struct ssession {
    schart *chart;
    sjs_value *variables;
    sjs_value event, sessionid, name, ioprocessors, init, kept;
    uint32_t bound; /* _event is bound */
    uint32_t pending_fuel;
} ssession;

typedef struct spayload {
    sjs_value value;
    uint32_t references, pad;
} spayload;

typedef struct sloop {
    sjs_value array;
} sloop;

struct ta_datamodel_staticjs {
    smemory memory;
    ta_allocator table_allocator;
    sjs_context *context;
    sjs_vm *vm;
    sfunction *functions;
    uint32_t function_count, function_capacity;
    sjs_value in_function;
    ta_table sessions, payloads, loops;
    schart *charts;
    /* the evaluation in flight, for In() */
    ta_datamodel_environment *environment;
    schart *current_chart;
    sjs_value *globals; /* scratch, per evaluation */
    uint32_t globals_capacity;
    uint8_t *out;       /* the last output, valid until the next call */
    uint32_t out_capacity;
    char message[256];
    uint32_t message_length, reason;
};

static int view_equal(sjs_str a, sjs_str b) { return a.length == b.length && ta_memcmp(a.ptr, b.ptr, a.length) == 0; }
static sjs_str view_of(ta_string_view v) { return (sjs_str){v.pointer, v.length}; }

static sjs_str copy_text(smemory *m, sjs_str text) {
    char *p = (char *)salloc(m, text.length + 1);
    if (!p) return (sjs_str){0, 0};
    if (text.length) ta_memcpy(p, text.ptr, text.length);
    return (sjs_str){p, text.length};
}

static void set_message(ta_datamodel_staticjs *datamodel, sjs_str first, sjs_str second) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < first.length && n < sizeof datamodel->message; i++) datamodel->message[n++] = first.ptr[i];
    for (uint32_t i = 0; i < second.length && n < sizeof datamodel->message; i++) datamodel->message[n++] = second.ptr[i];
    datamodel->message_length = n;
    datamodel->reason = TA_DATAMODEL_FAILURE_EXCEPTION;
}

/* ---- In() ------------------------------------------------------------------------------- */

static sjs_result in_call(void *self, sjs_native_call *call) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)self;
    if (call->count != 1 || call->arguments[0].kind != SJS_STRING || !datamodel->environment || !datamodel->current_chart) {
        call->fault = SJS_STR("In() takes a state's id, during a step");
        return SJS_ERR_PROGRAM;
    }
    sjs_str id = sjs_string_text(call->arguments[0]);
    schart *c = datamodel->current_chart;
    uint32_t state = ta_image_state_by_name(c->image, c->chart, (ta_string_view){id.ptr, id.length});
    call->result = sjs_boolean(state != NIL && ta_datamodel_environment_in(datamodel->environment, state));
    return SJS_OK;
}

static const sjs_native_vtable in_vtable = {in_call, 0};

/* ---- writing values as bytes and text --------------------------------------------------- */

static int out_reserve(ta_datamodel_staticjs *datamodel, uint32_t capacity) {
    if (capacity <= datamodel->out_capacity) return 1;
    uint32_t size = datamodel->out_capacity ? datamodel->out_capacity : 256;
    while (size < capacity) size *= 2;
    uint8_t *p = (uint8_t *)salloc(&datamodel->memory, size);
    if (!p) return 0;
    sfree(&datamodel->memory, datamodel->out);
    datamodel->out = p;
    datamodel->out_capacity = size;
    return 1;
}

/* value written in format into the output room: 1 and its bytes, or 0 */
static int write_value(ta_datamodel_staticjs *datamodel, sjs_value value, sjs_format format, sjs_bytes *out) {
    sjs_writer *writer;
    sjs_error error;
    sjs_writer_config config = {datamodel->context, value, SJS_STR("<function>"), format, &error};
    if (sjs_writer_init(&writer, &config) != SJS_OK) return 0;
    if (!out_reserve(datamodel, 256)) return sjs_writer_destroy(writer), 0;
    sjs_buffer buffer = {datamodel->out, datamodel->out_capacity, 0};
    sjs_result r;
    while ((r = sjs_writer_write(writer, &buffer)) == SJS_NEED_SPACE) {
        uint32_t used = buffer.length;
        if (!out_reserve(datamodel, datamodel->out_capacity * 2)) break;
        buffer = (sjs_buffer){datamodel->out, datamodel->out_capacity, used};
    }
    sjs_writer_destroy(writer);
    if (r != SJS_OK) return 0;
    *out = (sjs_bytes){datamodel->out, buffer.length};
    return 1;
}

static int read_cbor(ta_datamodel_staticjs *datamodel, ta_bytes bytes, sjs_value *out) {
    sjs_read_params params = {SJS_FORMAT_CBOR, {(const uint8_t *)bytes.pointer, (uint32_t)bytes.length}, 0, 0};
    return sjs_value_read(datamodel->context, &params, out) == SJS_OK;
}

/* ---- charts: names and expressions --------------------------------------------------- */

static int is_identifier_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$'; }
static int is_identifier(char c) { return is_identifier_start(c) || (c >= '0' && c <= '9'); }

static uint32_t global_of(const schart *c, sjs_str name) {
    for (uint32_t i = 0; i < c->global_count; i++)
        if (view_equal(c->globals[i], name)) return i;
    return NIL;
}

static sjs_program *compile_expression(ta_datamodel_staticjs *datamodel, schart *c, sjs_str source, sjs_str *error_out) {
    sjs_program *program = 0;
    sjs_error error;
    sjs_compile_params params;
    ta_memset(&params, 0, sizeof params);
    params.compiler.context = datamodel->context;
    params.compiler.goal = SJS_GOAL_EXPRESSION;
    params.compiler.globals = c->globals;
    params.compiler.global_count = c->global_count;
    params.source = source;
    params.error = &error;
    if (sjs_compile(&params, &program) != SJS_OK) {
        *error_out = error.message.length ? error.message : SJS_STR("the expression does not compile");
        return 0;
    }
    return program;
}

/* A location: a variable, then .name or [expression] steps. */
static void parse_path(ta_datamodel_staticjs *datamodel, schart *c, sjs_str source, sexpression *x) {
    uint32_t i = 0, n = source.length;
    const char *s = source.ptr;
    while (i < n && (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r')) i++;
    uint32_t start = i;
    if (i >= n || !is_identifier_start(s[i])) {
        x->error = SJS_STR("a location is a variable and its steps");
        return;
    }
    while (i < n && is_identifier(s[i])) i++;
    sjs_str root = {s + start, i - start};
    uint32_t slot = global_of(c, root);
    if (slot == NIL || slot >= c->variable_count) {
        x->error = slot == NIL ? SJS_STR("no variable of that name") : SJS_STR("only a variable can be written");
        return;
    }
    x->root = slot;
    ssegment steps[32];
    uint32_t count = 0;
    while (i < n) {
        if (s[i] == ' ' || s[i] == '\n' || s[i] == '\t' || s[i] == '\r') {
            i++;
            continue;
        }
        if (count == 32) {
            x->error = SJS_STR("a location too long");
            return;
        }
        if (s[i] == '.') {
            i++;
            uint32_t k = i;
            while (i < n && is_identifier(s[i])) i++;
            if (i == k) {
                x->error = SJS_STR("a name after '.'");
                return;
            }
            steps[count++] = (ssegment){copy_text(&datamodel->memory, (sjs_str){s + k, i - k}), 0};
        } else if (s[i] == '[') {
            uint32_t depth = 1, k = ++i;
            char quote = 0;
            while (i < n && depth) {
                char d = s[i];
                if (quote) {
                    if (d == '\\') i++;
                    else if (d == quote) quote = 0;
                } else if (d == '\'' || d == '"') quote = d;
                else if (d == '[') depth++;
                else if (d == ']') depth--;
                i++;
            }
            if (depth) {
                x->error = SJS_STR("']' expected");
                return;
            }
            sjs_str error;
            sjs_program *program = compile_expression(datamodel, c, (sjs_str){s + k, i - 1 - k}, &error);
            if (!program) {
                x->error = error;
                return;
            }
            steps[count++] = (ssegment){(sjs_str){0, 0}, program};
        } else {
            x->error = SJS_STR("a location is a variable and its steps");
            return;
        }
    }
    x->segment_count = count;
    if (count) {
        x->segments = (ssegment *)salloc(&datamodel->memory, sizeof(ssegment) * count);
        if (x->segments) ta_memcpy(x->segments, steps, sizeof(ssegment) * count);
    }
}

static ta_status s_attach(void *d, const ta_datamodel_chart *chart, void **context) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)d;
    schart *c = (schart *)salloc(&datamodel->memory, sizeof *c);
    if (!c) return TA_ERROR_NO_MEMORY;
    ta_memset(c, 0, sizeof *c);
    c->image = chart->image;
    c->chart = chart->chart;
    c->segment = chart->segment;
    c->data_count = ta_image_data_count(chart->image, chart->chart);
    c->expression_count = ta_image_expression_count(chart->image, chart->chart);
    ta_string_view chart_name = ta_image_chart_name(chart->image, chart->chart);
    uint32_t most = c->data_count + c->expression_count + G_FIXED + datamodel->function_count;
    c->globals = (sjs_str *)salloc(&datamodel->memory, sizeof(sjs_str) * most);
    c->data_slot = (uint32_t *)salloc(&datamodel->memory, sizeof(uint32_t) * (c->data_count + 1));
    c->function_values = (sjs_value *)salloc(&datamodel->memory, sizeof(sjs_value) * (datamodel->function_count + 1));
    c->expressions = (sexpression *)salloc(&datamodel->memory, sizeof(sexpression) * (c->expression_count + 1));
    if (!c->globals || !c->data_slot || !c->function_values || !c->expressions) return TA_ERROR_NO_MEMORY;
    /* the variables: <data> ids, then <foreach> items and indexes */
    for (uint32_t i = 0; i < c->data_count; i++) {
        ta_data_info info;
        ta_image_data(chart->image, (ta_chart_item){chart->chart, i}, &info);
        uint32_t slot = global_of(c, view_of(info.id));
        if (slot == NIL) {
            slot = c->global_count;
            c->globals[c->global_count++] = view_of(info.id);
        }
        c->data_slot[i] = slot;
    }
    for (uint32_t x = 0; x < c->expression_count; x++) {
        ta_expression_info info;
        if (ta_image_expression(chart->image, (ta_chart_item){chart->chart, x}, &info) != TA_OK || info.kind != TA_EXPRESSION_ITEM) continue;
        uint32_t k = 0;
        while (k < info.source.length && is_identifier(info.source.pointer[k])) k++;
        sjs_str root = {info.source.pointer, k};
        if (k && global_of(c, root) == NIL) c->globals[c->global_count++] = root;
    }
    c->variable_count = c->global_count;
    static const sjs_str fixed[G_FIXED] = {SJS_STR("_event"), SJS_STR("_sessionid"), SJS_STR("_name"), SJS_STR("_ioprocessors"), SJS_STR("In")};
    for (uint32_t i = 0; i < G_FIXED; i++) c->globals[c->global_count++] = fixed[i];
    /* the functions: the chart's own, else those for every chart */
    for (uint32_t i = 0; i < datamodel->function_count; i++) {
        sfunction *f = &datamodel->functions[i];
        if (f->chart.length && !view_equal(f->chart, view_of(chart_name))) continue;
        uint32_t at = global_of(c, f->name);
        if (at != NIL && at >= c->variable_count + G_FIXED) {
            if (f->chart.length) c->function_values[at - c->variable_count - G_FIXED] = f->value;
            continue;
        }
        if (at != NIL) continue; /* a variable or a fixed name wins */
        c->function_values[c->function_count++] = f->value;
        c->globals[c->global_count++] = f->name;
    }
    for (uint32_t x = 0; x < c->expression_count; x++) {
        sexpression *e = &c->expressions[x];
        ta_memset(e, 0, sizeof *e);
        e->root = NIL;
        ta_expression_info info;
        if (ta_image_expression(chart->image, (ta_chart_item){chart->chart, x}, &info) != TA_OK) {
            e->error = SJS_STR("no such expression");
            continue;
        }
        sjs_str source = view_of(info.source);
        if (info.kind == TA_EXPRESSION_SCRIPT || info.kind == TA_EXPRESSION_GLOBAL_SCRIPT) {
            e->error = SJS_STR("the staticjs datamodel has no <script>");
            continue;
        }
        if (info.kind == TA_EXPRESSION_LOCATION || info.kind == TA_EXPRESSION_ITEM) {
            parse_path(datamodel, c, source, e);
            if (!e->error.length) { /* a path is read too (namelist, a trace's value) */
                sjs_str ignored;
                e->program = compile_expression(datamodel, c, source, &ignored);
            }
            continue;
        }
        sjs_str error;
        e->program = compile_expression(datamodel, c, source, &error);
        if (!e->program && info.kind == TA_EXPRESSION_CONTENT) {
            /* content that is no expression is its text, whitespace normalized */
            char *text = (char *)salloc(&datamodel->memory, source.length + 1);
            uint32_t n = 0;
            int space = 0;
            for (uint32_t at = 0; text && at < source.length; at++) {
                char character = source.ptr[at];
                if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
                    space = n > 0;
                    continue;
                }
                if (space) text[n++] = ' ';
                space = 0;
                text[n++] = character;
            }
            if (text && (!n || text[0] != '<')) {
                /* a string literal of it, compiled */
                char *literal = (char *)salloc(&datamodel->memory, (size_t)n * 2 + 3);
                uint32_t m = 0;
                literal[m++] = '"';
                for (uint32_t at = 0; at < n; at++) {
                    if (text[at] == '"' || text[at] == '\\') literal[m++] = '\\';
                    literal[m++] = text[at];
                }
                literal[m++] = '"';
                e->program = compile_expression(datamodel, c, (sjs_str){literal, m}, &error);
                sfree(&datamodel->memory, literal);
            }
            sfree(&datamodel->memory, text);
        }
        if (!e->program) e->error = error.length ? error : SJS_STR("the expression does not compile");
    }
    c->next = datamodel->charts;
    datamodel->charts = c;
    *context = c;
    return TA_OK;
}

static void s_detach(void *d, void *context) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)d;
    schart *c = (schart *)context;
    if (!c) return;
    for (schart **at = &datamodel->charts; *at; at = &(*at)->next)
        if (*at == c) {
            *at = c->next;
            break;
        }
    for (uint32_t x = 0; x < c->expression_count; x++) {
        sexpression *e = &c->expressions[x];
        if (e->program) sjs_program_release(e->program);
        for (uint32_t i = 0; i < e->segment_count; i++) {
            if (e->segments[i].program) sjs_program_release(e->segments[i].program);
            if (e->segments[i].key.length) sfree(&datamodel->memory, (void *)e->segments[i].key.ptr);
        }
        sfree(&datamodel->memory, e->segments);
    }
    sfree(&datamodel->memory, c->expressions);
    sfree(&datamodel->memory, c->globals);
    sfree(&datamodel->memory, c->data_slot);
    sfree(&datamodel->memory, c->function_values);
    sfree(&datamodel->memory, c);
}

/* ---- evaluation ------------------------------------------------------------------------ */

static ssession *session_of(const ta_datamodel_call *call) {
    return (ssession *)ta_table_get(&((ta_datamodel_staticjs *)call->datamodel)->sessions, call->instance);
}

typedef struct sevaluation {
    ta_datamodel_staticjs *datamodel;
    ssession *session;
    ta_datamodel_environment *environment;
} sevaluation;

static void charge(sevaluation *e, uint64_t fuel) {
    uint64_t total = fuel + e->session->pending_fuel;
    if (e->environment) {
        e->session->pending_fuel = 0;
        if (total) ta_datamodel_environment_charge(e->environment, total > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)total);
    } else
        e->session->pending_fuel = total > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)total;
}

static int fail(sevaluation *e, sjs_str why) {
    set_message(e->datamodel, why, (sjs_str){0, 0});
    return 0;
}

/* program evaluated over the session's globals: 1 and an owned value, or 0 */
static int run(sevaluation *e, sjs_program *program, sjs_value *out) {
    ta_datamodel_staticjs *datamodel = e->datamodel;
    schart *c = e->session->chart;
    if (datamodel->globals_capacity < c->global_count) {
        sjs_value *g = (sjs_value *)salloc(&datamodel->memory, sizeof(sjs_value) * c->global_count);
        if (!g) return fail(e, SJS_STR("out of memory"));
        sfree(&datamodel->memory, datamodel->globals);
        datamodel->globals = g;
        datamodel->globals_capacity = c->global_count;
    }
    sjs_value *g = datamodel->globals;
    ssession *s = e->session;
    for (uint32_t i = 0; i < c->variable_count; i++) g[i] = s->variables[i];
    g[c->variable_count + G_EVENT] = s->bound ? s->event : sjs_undefined();
    g[c->variable_count + G_SESSIONID] = s->sessionid;
    g[c->variable_count + G_NAME] = s->name;
    g[c->variable_count + G_IOPROCESSORS] = s->ioprocessors;
    g[c->variable_count + G_IN] = datamodel->in_function;
    for (uint32_t i = 0; i < c->function_count; i++) g[c->variable_count + G_FIXED + i] = c->function_values[i];
    datamodel->environment = e->environment;
    datamodel->current_chart = c;
    sjs_run_params params;
    ta_memset(&params, 0, sizeof params);
    params.program = program;
    params.globals = g;
    params.global_count = c->global_count;
    if (sjs_vm_start(datamodel->vm, &params) != SJS_OK) return fail(e, SJS_STR("the evaluation could not start"));
    sjs_run status;
    while ((status = sjs_vm_run(datamodel->vm, 1u << 20)) == SJS_RUN_PAUSED) {
    }
    charge(e, sjs_vm_fuel_used(datamodel->vm));
    datamodel->environment = 0;
    if (status == SJS_RUN_DONE) return sjs_vm_take_result(datamodel->vm, out) == SJS_OK || fail(e, SJS_STR("no result"));
    sjs_error error;
    sjs_vm_error(datamodel->vm, &error);
    set_message(datamodel, error.message, error.subject.length ? error.subject : (sjs_str){0, 0});
    sjs_vm_reset(datamodel->vm);
    return 0;
}

static int evaluate(sevaluation *e, ta_expression x, sjs_value *out) {
    schart *c = e->session->chart;
    if (x >= c->expression_count) return fail(e, SJS_STR("no such expression"));
    sexpression *expression = &c->expressions[x];
    if (expression->error.length || !expression->program)
        return fail(e, expression->error.length ? expression->error : SJS_STR("not an expression"));
    return run(e, expression->program, out);
}

static int truthy(sjs_value v) {
    switch (v.kind) {
    case SJS_UNDEFINED:
    case SJS_NULL: return 0;
    case SJS_BOOLEAN: return v.boolean != 0;
    case SJS_NUMBER: return v.as.number == v.as.number && v.as.number != 0;
    case SJS_STRING: return sjs_string_text(v).length != 0;
    default: return 1;
    }
}

/* ---- writing a path: the containers along it are made again ---------------------------- */

/* container with `key` (a string or a number) set to child (owned): an owned new container */
static int with_child(sevaluation *e, sjs_value container, sjs_value key, sjs_value child, sjs_value *out) {
    sjs_context *context = e->datamodel->context;
    uint32_t n = sjs_length(container);
    if (container.kind == SJS_ARRAY && key.kind == SJS_NUMBER) {
        double index = key.as.number;
        if (index < 0 || index > n || index != (double)(uint32_t)index) return sjs_release(context, child), fail(e, SJS_STR("a position past the array's end"));
        uint32_t at = (uint32_t)index;
        if (sjs_array_new(context, n + 1, out) != SJS_OK) return sjs_release(context, child), fail(e, SJS_STR("out of memory"));
        for (uint32_t i = 0; i < n; i++) {
            sjs_value item = i == at ? child : sjs_array_at(container, i);
            if (i != at) sjs_retain(item);
            sjs_array_append(context, out, item);
        }
        if (at == n) sjs_array_append(context, out, child);
        return 1;
    }
    if (container.kind == SJS_OBJECT && key.kind == SJS_STRING) {
        if (sjs_object_new(context, n + 1, out) != SJS_OK) return sjs_release(context, child), fail(e, SJS_STR("out of memory"));
        for (uint32_t i = 0; i < n; i++) {
            sjs_property property;
            sjs_object_at(container, i, &property);
            sjs_retain(property.value);
            sjs_object_put(context, out, &property);
        }
        sjs_property property = {sjs_string_text(key), child};
        sjs_object_put(context, out, &property);
        return 1;
    }
    sjs_release(context, child);
    return fail(e, SJS_STR("a step into a value that is neither an object nor an array"));
}

static int step_key(sevaluation *e, const ssegment *segment, sjs_value *key) {
    if (!segment->program) return sjs_string_new(e->datamodel->context, segment->key, key) == SJS_OK || fail(e, SJS_STR("out of memory"));
    return run(e, segment->program, key);
}

static int child_of(sevaluation *e, sjs_value container, sjs_value key, sjs_value *out) {
    if (container.kind == SJS_ARRAY && key.kind == SJS_NUMBER) {
        double index = key.as.number;
        if (index < 0 || index >= sjs_length(container) || index != (double)(uint32_t)index) return fail(e, SJS_STR("no item at that position"));
        *out = sjs_array_at(container, (uint32_t)index);
        return 1;
    }
    if (container.kind == SJS_OBJECT && key.kind == SJS_STRING) {
        if (sjs_object_get(container, sjs_string_text(key), out) != SJS_OK) return fail(e, SJS_STR("no such key"));
        return 1;
    }
    return fail(e, SJS_STR("a step into a value that is neither an object nor an array"));
}

/* value (owned) written at the location: 1, or 0 with value released */
static int write_path(sevaluation *e, ta_expression location, sjs_value value) {
    sjs_context *context = e->datamodel->context;
    schart *c = e->session->chart;
    if (location >= c->expression_count) return sjs_release(context, value), fail(e, SJS_STR("no such location"));
    sexpression *x = &c->expressions[location];
    if (x->error.length || x->root == NIL) return sjs_release(context, value), fail(e, x->error.length ? x->error : SJS_STR("not a location"));
    sjs_value chain[33], keys[32];
    uint32_t n = x->segment_count;
    chain[0] = e->session->variables[x->root];
    for (uint32_t i = 0; i < n; i++) {
        if (!step_key(e, &x->segments[i], &keys[i])) {
            for (uint32_t k = 0; k < i; k++) sjs_release(context, keys[k]);
            return sjs_release(context, value), 0;
        }
        if (i + 1 < n && !child_of(e, chain[i], keys[i], &chain[i + 1])) {
            for (uint32_t k = 0; k <= i; k++) sjs_release(context, keys[k]);
            return sjs_release(context, value), 0;
        }
    }
    sjs_value made = value;
    int ok = 1;
    for (uint32_t i = n; i-- > 0;) {
        sjs_value next;
        if (!with_child(e, chain[i], keys[i], made, &next)) {
            ok = 0;
            break;
        }
        made = next;
    }
    for (uint32_t k = 0; k < n; k++) sjs_release(context, keys[k]);
    if (!ok) return 0;
    sjs_release(context, e->session->variables[x->root]);
    e->session->variables[x->root] = made;
    return 1;
}

/* ---- payloads -------------------------------------------------------------------------- */

static ta_payload payload_new(ta_datamodel_staticjs *datamodel, sjs_value value) {
    uint64_t handle;
    spayload *row = (spayload *)ta_table_add(&datamodel->payloads, &handle);
    if (!row) return sjs_release(datamodel->context, value), 0;
    row->value = value;
    row->references = 1;
    return handle;
}

static void s_retain(void *d, ta_payload p) {
    spayload *row = (spayload *)ta_table_get(&((ta_datamodel_staticjs *)d)->payloads, p);
    if (row) row->references++;
}

static void s_release(void *d, ta_payload p) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)d;
    spayload *row = (spayload *)ta_table_get(&datamodel->payloads, p);
    if (!row || --row->references) return;
    sjs_release(datamodel->context, row->value);
    ta_table_remove(&datamodel->payloads, p);
}

/* ---- the vtable ------------------------------------------------------------------------ */

static int make_text(ta_datamodel_staticjs *datamodel, ta_string_view text, sjs_value *out) {
    return sjs_string_new(datamodel->context, (sjs_str){text.pointer, text.length}, out) == SJS_OK;
}

static int system_values(ta_datamodel_staticjs *datamodel, ssession *s, const ta_datamodel_system_variables *v) {
    if (!make_text(datamodel, v->sessionid, &s->sessionid) || !make_text(datamodel, v->name, &s->name)) return 0;
    if (sjs_object_new(datamodel->context, v->io_processor_count * 2, &s->ioprocessors) != SJS_OK) return 0;
    for (uint32_t i = 0; i < v->io_processor_count; i++) {
        ta_string_view type = v->io_processors[i].type;
        for (int k = 0; k < 2; k++) {
            sjs_str key = view_of(type);
            if (k == 1) {
                if (type.length > 20 && !ta_memcmp(type.pointer + type.length - 20, "#SCXMLEventProcessor", 20)) key = SJS_STR("scxml");
                else if (type.length > 24 && !ta_memcmp(type.pointer + type.length - 24, "#BasicHTTPEventProcessor", 24)) key = SJS_STR("basichttp");
                else continue;
            }
            sjs_value entry, location;
            if (sjs_object_new(datamodel->context, 1, &entry) != SJS_OK || !make_text(datamodel, v->io_processors[i].location, &location)) return 0;
            sjs_property l = {SJS_STR("location"), location};
            sjs_object_put(datamodel->context, &entry, &l);
            sjs_property p = {key, entry};
            sjs_object_put(datamodel->context, &s->ioprocessors, &p);
        }
    }
    return 1;
}

static ta_status session_new(ta_datamodel_staticjs *datamodel, const ta_datamodel_start *start, uint64_t *out, ssession **made) {
    schart *c = (schart *)start->chart_context;
    ssession *s = (ssession *)ta_table_add(&datamodel->sessions, out);
    if (!s) return TA_ERROR_NO_MEMORY;
    s->chart = c;
    s->event = s->init = s->kept = sjs_undefined();
    s->variables = (sjs_value *)salloc(&datamodel->memory, sizeof(sjs_value) * (c->variable_count + 1));
    if (!s->variables) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < c->variable_count; i++) s->variables[i] = sjs_undefined();
    if (!system_values(datamodel, s, start->system_variables)) return TA_ERROR_NO_MEMORY;
    *made = s;
    return TA_OK;
}

static ta_status s_create(void *d, const ta_datamodel_start *start, uint64_t *out) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)d;
    ssession *s;
    ta_status status = session_new(datamodel, start, out, &s);
    if (status != TA_OK) return status;
    if (start->init) {
        spayload *row = (spayload *)ta_table_get(&datamodel->payloads, start->init);
        if (row && row->value.kind == SJS_OBJECT) {
            sjs_retain(row->value);
            s->init = row->value;
        }
        s_release(d, start->init);
    }
    return TA_OK;
}

static void session_release(ta_datamodel_staticjs *datamodel, ssession *s) {
    sjs_context *context = datamodel->context;
    for (uint32_t i = 0; i < s->chart->variable_count; i++) sjs_release(context, s->variables[i]);
    sfree(&datamodel->memory, s->variables);
    sjs_release(context, s->event);
    sjs_release(context, s->sessionid);
    sjs_release(context, s->name);
    sjs_release(context, s->ioprocessors);
    sjs_release(context, s->init);
    sjs_release(context, s->kept);
}

static void s_destroy(const ta_datamodel_call *call) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)call->datamodel;
    ssession *s = session_of(call);
    if (!s) return;
    session_release(datamodel, s);
    ta_table_remove(&datamodel->sessions, call->instance);
}

#define BEGIN(call)                                                                                           \
    ssession *s = session_of(call);                                                                           \
    if (!s) return TA_ERROR_INVALID;                                                                          \
    sevaluation e = {(ta_datamodel_staticjs *)(call)->datamodel, s, (call)->environment};                    \
    (void)e

static ta_status s_init_data(const ta_datamodel_call *call, uint32_t declaration) {
    BEGIN(call);
    schart *c = s->chart;
    ta_data_info info;
    if (declaration >= c->data_count || ta_image_data(c->image, (ta_chart_item){c->chart, declaration}, &info) != TA_OK) return TA_ERROR_EXECUTION;
    uint32_t slot = c->data_slot[declaration];
    sjs_value value;
    if (s->init.kind == SJS_OBJECT && sjs_object_get(s->init, view_of(info.id), &value) == SJS_OK) {
        sjs_retain(value);
    } else if (info.expression == TA_NIL) {
        return TA_OK;
    } else if (!evaluate(&e, info.expression, &value)) {
        return TA_ERROR_EXECUTION;
    }
    sjs_release(e.datamodel->context, s->variables[slot]);
    s->variables[slot] = value;
    return TA_OK;
}

static ta_status s_bind_event(const ta_datamodel_call *call, const ta_event_view *view) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)call->datamodel;
    ssession *s = session_of(call);
    if (!s) return TA_ERROR_INVALID;
    sjs_context *context = datamodel->context;
    static const sjs_str types[] = {SJS_STR("platform"), SJS_STR("internal"), SJS_STR("external")};
    sjs_value event;
    if (sjs_object_new(context, 8, &event) != SJS_OK) return TA_ERROR_NO_MEMORY;
    struct {
        sjs_str key;
        ta_string_view text;
    } fields[] = {{SJS_STR("name"), view->name},         {SJS_STR("sendid"), view->sendid},
                  {SJS_STR("origin"), view->origin},     {SJS_STR("origintype"), view->origintype},
                  {SJS_STR("invokeid"), view->invokeid}, {SJS_STR("raw"), view->raw}};
    for (uint32_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        sjs_value text = sjs_undefined(); /* absent: an undefined member, as the ECMAScript data model has */
        if ((!i || fields[i].text.length) && !make_text(datamodel, fields[i].text, &text))
            return sjs_release(context, event), TA_ERROR_NO_MEMORY;
        sjs_property p = {fields[i].key, text};
        sjs_object_put(context, &event, &p);
        if (!i) {
            sjs_value type;
            sjs_string_new(context, types[view->type % 3], &type);
            sjs_property t = {SJS_STR("type"), type};
            sjs_object_put(context, &event, &t);
        }
    }
    spayload *row = view->data ? (spayload *)ta_table_get(&datamodel->payloads, view->data) : 0;
    sjs_value data = sjs_undefined();
    if (row) {
        sjs_retain(row->value);
        data = row->value;
    }
    sjs_property data_property = {SJS_STR("data"), data};
    sjs_object_put(context, &event, &data_property);
    sjs_release(context, s->event);
    s->event = event;
    s->bound = 1;
    sjs_release(context, s->kept); /* the first event: no migrate asked */
    s->kept = sjs_undefined();
    return TA_OK;
}

static ta_status s_condition(const ta_datamodel_call *call, ta_expression x, int *out) {
    BEGIN(call);
    sjs_value v;
    if (!evaluate(&e, x, &v)) return TA_ERROR_EXECUTION;
    *out = truthy(v);
    sjs_release(e.datamodel->context, v);
    return TA_OK;
}

static ta_status text_of(sevaluation *e, sjs_value v, ta_string_view *out) {
    ta_datamodel_staticjs *datamodel = e->datamodel;
    if (v.kind == SJS_STRING) {
        sjs_str t = sjs_string_text(v);
        if (!out_reserve(datamodel, t.length + 1)) return TA_ERROR_NO_MEMORY;
        if (t.length) ta_memcpy(datamodel->out, t.ptr, t.length);
        *out = (ta_string_view){(const char *)datamodel->out, t.length};
        return TA_OK;
    }
    sjs_bytes b;
    if (!write_value(datamodel, v, SJS_FORMAT_SOURCE, &b)) return TA_ERROR_EXECUTION;
    *out = (ta_string_view){(const char *)b.ptr, b.length};
    return TA_OK;
}

static ta_status s_string(const ta_datamodel_call *call, ta_expression x, ta_string_view *out) {
    BEGIN(call);
    sjs_value v;
    if (!evaluate(&e, x, &v)) return TA_ERROR_EXECUTION;
    ta_status status = text_of(&e, v, out);
    sjs_release(e.datamodel->context, v);
    return status;
}

static ta_status s_assign(const ta_datamodel_call *call, ta_expression location, ta_expression x) {
    BEGIN(call);
    sjs_value v;
    if (!evaluate(&e, x, &v)) return TA_ERROR_EXECUTION;
    return write_path(&e, location, v) ? TA_OK : TA_ERROR_EXECUTION;
}

static ta_status s_assign_string(const ta_datamodel_call *call, ta_expression location, ta_string_view value) {
    BEGIN(call);
    sjs_value v;
    if (!make_text(e.datamodel, value, &v)) return TA_ERROR_NO_MEMORY;
    return write_path(&e, location, v) ? TA_OK : TA_ERROR_EXECUTION;
}

static ta_status s_script(const ta_datamodel_call *call, ta_expression x) {
    BEGIN(call);
    sjs_value v;
    if (evaluate(&e, x, &v)) sjs_release(e.datamodel->context, v);
    return TA_ERROR_EXECUTION;
}

static ta_status s_log(const ta_datamodel_call *call, ta_expression x, ta_string_view *out) { return s_string(call, x, out); }

static ta_status s_foreach_begin(const ta_datamodel_call *call, ta_expression array, ta_datamodel_loop *loop) {
    BEGIN(call);
    sjs_value v;
    if (!evaluate(&e, array, &v)) return TA_ERROR_EXECUTION;
    if (v.kind != SJS_ARRAY) {
        sjs_release(e.datamodel->context, v);
        set_message(e.datamodel, SJS_STR("<foreach> needs an array"), (sjs_str){0, 0});
        return TA_ERROR_EXECUTION;
    }
    sloop *row = (sloop *)ta_table_add(&e.datamodel->loops, &loop->iter);
    if (!row) return sjs_release(e.datamodel->context, v), TA_ERROR_NO_MEMORY;
    row->array = v; /* immutable: the shallow copy SCXML asks for */
    loop->n = sjs_length(v);
    return TA_OK;
}

static ta_status s_foreach_item(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    BEGIN(call);
    sloop *row = (sloop *)ta_table_get(&e.datamodel->loops, loop->iter);
    if (!row || loop->i >= sjs_length(row->array)) return TA_ERROR_EXECUTION;
    sjs_value item = sjs_array_at(row->array, loop->i);
    sjs_retain(item);
    if (!write_path(&e, loop->item, item)) return TA_ERROR_EXECUTION;
    if (loop->index != TA_NIL && !write_path(&e, loop->index, sjs_number(loop->i))) return TA_ERROR_EXECUTION;
    return TA_OK;
}

static void s_foreach_end(const ta_datamodel_call *call, const ta_datamodel_loop *loop) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)call->datamodel;
    sloop *row = (sloop *)ta_table_get(&datamodel->loops, loop->iter);
    if (!row) return;
    sjs_release(datamodel->context, row->array);
    ta_table_remove(&datamodel->loops, loop->iter);
}

static sjs_str location_name(schart *c, ta_expression x) {
    sexpression *e = &c->expressions[x];
    if (e->root == NIL) return (sjs_str){0, 0};
    if (e->segment_count && e->segments[e->segment_count - 1].key.length) return e->segments[e->segment_count - 1].key;
    return c->globals[e->root];
}

static ta_status s_capture(const ta_datamodel_call *call, const ta_datamodel_capture *spec, ta_payload *out) {
    BEGIN(call);
    sjs_context *context = e.datamodel->context;
    schart *c = s->chart;
    sjs_value v;
    if (spec->content != TA_NIL) {
        if (!evaluate(&e, spec->content, &v)) return TA_ERROR_EXECUTION;
        *out = payload_new(e.datamodel, v);
        return *out ? TA_OK : TA_ERROR_NO_MEMORY;
    }
    sjs_value object;
    if (sjs_object_new(context, spec->namelist_count + spec->param_count, &object) != SJS_OK) return TA_ERROR_NO_MEMORY;
    for (uint32_t i = 0; i < spec->namelist_count + spec->param_count; i++) {
        sjs_str key;
        if (i < spec->namelist_count) {
            ta_expression x = spec->namelist[i];
            if (x >= c->expression_count || !evaluate(&e, x, &v)) return sjs_release(context, object), TA_ERROR_EXECUTION;
            key = location_name(c, x);
        } else {
            uint32_t k = i - spec->namelist_count;
            ta_string_view name = ta_image_string(c->image, spec->params[2 * k]);
            key = view_of(name);
            if (!evaluate(&e, spec->params[2 * k + 1], &v)) return sjs_release(context, object), TA_ERROR_EXECUTION;
        }
        sjs_property p = {key, v}; /* a later name replaces an earlier one */
        sjs_object_put(context, &object, &p);
    }
    *out = payload_new(e.datamodel, object);
    return *out ? TA_OK : TA_ERROR_NO_MEMORY;
}

static ta_status s_custom(const ta_datamodel_call *call, uint32_t definition) {
    (void)call;
    (void)definition;
    return TA_ERROR_UNSUPPORTED;
}

/* {vars: {name: value}, event} as one staticjs value */
static int state_of(ta_datamodel_staticjs *datamodel, ssession *s, sjs_value *out) {
    sjs_context *context = datamodel->context;
    sjs_value vars, state;
    if (sjs_object_new(context, s->chart->variable_count, &vars) != SJS_OK) return 0;
    for (uint32_t i = 0; i < s->chart->variable_count; i++) {
        sjs_retain(s->variables[i]);
        sjs_property p = {s->chart->globals[i], s->variables[i]};
        sjs_object_put(context, &vars, &p);
    }
    if (sjs_object_new(context, 2, &state) != SJS_OK) return sjs_release(context, vars), 0;
    sjs_property v = {SJS_STR("vars"), vars};
    sjs_object_put(context, &state, &v);
    sjs_value event = s->bound ? s->event : sjs_null();
    sjs_retain(event);
    sjs_property event_property = {SJS_STR("event"), event};
    sjs_object_put(context, &state, &event_property);
    *out = state;
    return 1;
}

static ta_status s_serialize(const ta_datamodel_call *call, ta_bytes *out) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)call->datamodel;
    ssession *s = session_of(call);
    sjs_value state;
    if (!s || !state_of(datamodel, s, &state)) return TA_ERROR_NO_MEMORY;
    sjs_bytes b;
    int ok = write_value(datamodel, state, SJS_FORMAT_CBOR, &b);
    sjs_release(datamodel->context, state);
    if (!ok) return TA_ERROR_UNSUPPORTED;
    *out = (ta_bytes){b.ptr, b.length};
    return TA_OK;
}

static void fill_from(ta_datamodel_staticjs *datamodel, ssession *s, sjs_value vars) {
    for (uint32_t i = 0; i < s->chart->variable_count; i++) {
        sjs_value v;
        if (vars.kind == SJS_OBJECT && sjs_object_get(vars, s->chart->globals[i], &v) == SJS_OK) {
            sjs_retain(v);
            sjs_release(datamodel->context, s->variables[i]);
            s->variables[i] = v;
        }
    }
}

static ta_status s_deserialize(void *d, const ta_datamodel_start *start, uint64_t *out) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)d;
    sjs_value state, vars, event;
    if (!read_cbor(datamodel, start->state, &state)) return TA_ERROR_INVALID;
    ssession *s;
    ta_status status = session_new(datamodel, start, out, &s);
    if (status != TA_OK) return sjs_release(datamodel->context, state), status;
    if (sjs_object_get(state, SJS_STR("vars"), &vars) == SJS_OK) {
        fill_from(datamodel, s, vars);
        sjs_retain(vars);
        s->kept = vars;
    }
    if (sjs_object_get(state, SJS_STR("event"), &event) == SJS_OK && event.kind == SJS_OBJECT) {
        sjs_retain(event);
        s->event = event;
        s->bound = 1;
    }
    sjs_release(datamodel->context, state);
    return TA_OK;
}

static ta_status s_migrate(const ta_datamodel_call *call, ta_payload *bindings) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)call->datamodel;
    ssession *s = session_of(call);
    *bindings = 0;
    if (!s || s->kept.kind != SJS_OBJECT) return TA_OK;
    *bindings = payload_new(datamodel, s->kept);
    s->kept = sjs_undefined();
    return *bindings ? TA_OK : TA_ERROR_NO_MEMORY;
}

static ta_status s_to_wire(void *d, ta_payload p, ta_bytes *out) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)d;
    spayload *row = (spayload *)ta_table_get(&datamodel->payloads, p);
    sjs_bytes b;
    if (!row || !write_value(datamodel, row->value, SJS_FORMAT_CBOR, &b)) return TA_ERROR_INVALID;
    *out = (ta_bytes){b.ptr, b.length};
    return TA_OK;
}

static ta_status s_from_wire(void *d, ta_bytes cbor, ta_payload *out) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)d;
    sjs_value v;
    if (!read_cbor(datamodel, cbor, &v)) return TA_ERROR_INVALID;
    *out = payload_new(datamodel, v);
    return *out ? TA_OK : TA_ERROR_NO_MEMORY;
}

static ta_status s_variable(const ta_datamodel_call *call, ta_string_view name, ta_payload *out) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)call->datamodel;
    ssession *s = session_of(call);
    *out = 0;
    if (!s) return TA_ERROR_INVALID;
    uint32_t slot = global_of(s->chart, view_of(name));
    if (slot == NIL || slot >= s->chart->variable_count) return TA_ERROR_INVALID;
    if (s->variables[slot].kind == SJS_UNDEFINED) return TA_OK;
    sjs_retain(s->variables[slot]);
    *out = payload_new(datamodel, s->variables[slot]);
    return *out ? TA_OK : TA_ERROR_NO_MEMORY;
}

static ta_status s_failure(const ta_datamodel_call *call, ta_datamodel_failure *out) {
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)call->datamodel;
    out->reason = datamodel->reason;
    out->message = (ta_string_view){datamodel->message, datamodel->message_length};
    return TA_OK;
}

static const ta_datamodel_vtable staticjs_vtable = {
    sizeof(ta_datamodel_vtable),
    TA_DATAMODEL_EVENT_DETACHABLE,
    "staticjs",
    s_attach,
    s_detach,
    s_create,
    s_destroy,
    s_init_data,
    s_bind_event,
    0,
    s_condition,
    s_string,
    s_assign,
    s_assign_string,
    s_script,
    s_log,
    s_foreach_begin,
    s_foreach_item,
    s_foreach_end,
    s_capture,
    s_retain,
    s_release,
    s_custom,
    s_serialize,
    s_deserialize,
    s_migrate,
    s_to_wire,
    s_from_wire,
    s_to_wire,
    s_from_wire,
    s_variable,
    s_failure,
};

/* ---- snapshots: the tables' namespaces and each row, as CBOR ------------------------- */

typedef struct sbytes {
    smemory *memory;
    uint8_t *p;
    uint32_t n, capacity;
    int failed;
} sbytes;

static void put(sbytes *b, const void *p, uint32_t n) {
    if (b->failed || !n) return;
    if (b->n + n > b->capacity) {
        uint32_t capacity = b->capacity ? b->capacity * 2 : 1024;
        while (capacity < b->n + n) capacity *= 2;
        uint8_t *q = (uint8_t *)salloc(b->memory, capacity);
        if (!q) {
            b->failed = 1;
            return;
        }
        if (b->n) ta_memcpy(q, b->p, b->n);
        sfree(b->memory, b->p);
        b->p = q;
        b->capacity = capacity;
    }
    ta_memcpy(b->p + b->n, p, n);
    b->n += n;
}

static void put_head(sbytes *b, unsigned major, uint64_t v) {
    uint8_t h[9];
    uint32_t l;
    if (v < 24) h[0] = (uint8_t)(major << 5 | v), l = 1;
    else if (v <= 0xFF) h[0] = (uint8_t)(major << 5 | 24), h[1] = (uint8_t)v, l = 2;
    else if (v <= 0xFFFF) h[0] = (uint8_t)(major << 5 | 25), h[1] = (uint8_t)(v >> 8), h[2] = (uint8_t)v, l = 3;
    else if (v <= 0xFFFFFFFFu) {
        h[0] = (uint8_t)(major << 5 | 26);
        for (int i = 0; i < 4; i++) h[1 + i] = (uint8_t)(v >> (24 - 8 * i));
        l = 5;
    } else {
        h[0] = (uint8_t)(major << 5 | 27);
        for (int i = 0; i < 8; i++) h[1 + i] = (uint8_t)(v >> (56 - 8 * i));
        l = 9;
    }
    put(b, h, l);
}

/* a value as a byte string of its CBOR, or null when it has none */
static void put_value(ta_datamodel_staticjs *datamodel, sbytes *b, sjs_value v) {
    sjs_bytes w;
    if (v.kind == SJS_UNDEFINED || !write_value(datamodel, v, SJS_FORMAT_CBOR, &w)) {
        uint8_t null = 0xF6;
        put(b, &null, 1);
        return;
    }
    put_head(b, 2, w.length);
    put(b, w.ptr, w.length);
}

static void put_namespace(sbytes *b, const ta_table *table) {
    ta_buffer probe = {0, 0, 0};
    ta_table_snapshot(table, &probe);
    uint8_t *room = (uint8_t *)salloc(b->memory, probe.length + 1);
    if (!room) {
        b->failed = 1;
        return;
    }
    ta_buffer out = {room, probe.length, 0};
    ta_table_snapshot(table, &out);
    put_head(b, 2, out.length);
    put(b, room, (uint32_t)out.length);
    sfree(b->memory, room);
}

ta_status ta_datamodel_staticjs_snapshot(ta_datamodel_staticjs *datamodel, ta_buffer *out) {
    sbytes b = {&datamodel->memory, 0, 0, 0, 0};
    put_head(&b, 4, 6);
    put_namespace(&b, &datamodel->sessions);
    put_namespace(&b, &datamodel->payloads);
    put_namespace(&b, &datamodel->loops);
    uint32_t count = 0;
    for (uint32_t slot = 0; slot < datamodel->sessions.capacity; slot++) count += ta_table_at(&datamodel->sessions, slot) != 0;
    put_head(&b, 4, count);
    for (uint32_t slot = 0; slot < datamodel->sessions.capacity; slot++) {
        ssession *s = (ssession *)ta_table_at(&datamodel->sessions, slot);
        if (!s) continue;
        sjs_value state;
        put_head(&b, 4, 10);
        put_head(&b, 0, ta_table_handle(&datamodel->sessions, slot));
        put_head(&b, 0, s->chart->segment);
        put_head(&b, 0, s->chart->chart);
        if (state_of(datamodel, s, &state)) {
            put_value(datamodel, &b, state);
            sjs_release(datamodel->context, state);
        } else
            b.failed = 1;
        put_value(datamodel, &b, s->init);
        put_value(datamodel, &b, s->kept);
        put_value(datamodel, &b, s->sessionid);
        put_value(datamodel, &b, s->name);
        put_value(datamodel, &b, s->ioprocessors);
        put_head(&b, 0, s->pending_fuel);
    }
    count = 0;
    for (uint32_t slot = 0; slot < datamodel->payloads.capacity; slot++) count += ta_table_at(&datamodel->payloads, slot) != 0;
    put_head(&b, 4, count);
    for (uint32_t slot = 0; slot < datamodel->payloads.capacity; slot++) {
        spayload *p = (spayload *)ta_table_at(&datamodel->payloads, slot);
        if (!p) continue;
        put_head(&b, 4, 3);
        put_head(&b, 0, ta_table_handle(&datamodel->payloads, slot));
        put_head(&b, 0, p->references);
        put_value(datamodel, &b, p->value);
    }
    count = 0;
    for (uint32_t slot = 0; slot < datamodel->loops.capacity; slot++) count += ta_table_at(&datamodel->loops, slot) != 0;
    put_head(&b, 4, count);
    for (uint32_t slot = 0; slot < datamodel->loops.capacity; slot++) {
        sloop *l = (sloop *)ta_table_at(&datamodel->loops, slot);
        if (!l) continue;
        put_head(&b, 4, 2);
        put_head(&b, 0, ta_table_handle(&datamodel->loops, slot));
        put_value(datamodel, &b, l->array);
    }
    ta_status status = b.failed ? TA_ERROR_NO_MEMORY : b.n > out->capacity ? TA_ERROR_NO_MEMORY : TA_OK;
    if (status == TA_OK) ta_memcpy(out->pointer, b.p, b.n);
    out->length = b.n;
    sfree(&datamodel->memory, b.p);
    return status;
}

typedef struct sread {
    const uint8_t *p;
    uint32_t n;
    int bad;
} sread;

static uint64_t read_head(sread *r, unsigned want) {
    if (r->bad || !r->n) return r->bad = 1, 0;
    unsigned major = r->p[0] >> 5, info = r->p[0] & 31;
    uint32_t extra = info < 24 ? 0 : info == 24 ? 1 : info == 25 ? 2 : info == 26 ? 4 : info == 27 ? 8 : 99;
    if (major != want || extra == 99 || 1 + extra > r->n) return r->bad = 1, 0;
    uint64_t v = info < 24 ? info : 0;
    for (uint32_t i = 0; i < extra; i++) v = (v << 8) | r->p[1 + i];
    r->p += 1 + extra;
    r->n -= 1 + extra;
    return v;
}

/* a byte string: its bytes; null: none */
static ta_bytes read_bytes(sread *r) {
    if (!r->bad && r->n && r->p[0] == 0xF6) {
        r->p++, r->n--;
        return (ta_bytes){0, 0};
    }
    uint64_t length = read_head(r, 2);
    if (r->bad || length > r->n) return r->bad = 1, (ta_bytes){0, 0};
    ta_bytes out = {r->p, (size_t)length};
    r->p += length;
    r->n -= (uint32_t)length;
    return out;
}

static sjs_value read_value(ta_datamodel_staticjs *datamodel, sread *r) {
    ta_bytes b = read_bytes(r);
    sjs_value v = sjs_undefined();
    if (b.length && !read_cbor(datamodel, b, &v)) r->bad = 1;
    return v;
}

ta_status ta_datamodel_staticjs_restore(ta_datamodel_staticjs *datamodel, ta_bytes state) {
    sread r = {(const uint8_t *)state.pointer, (uint32_t)state.length, 0};
    if (read_head(&r, 4) != 6) return TA_ERROR_INVALID;
    ta_table *tables[] = {&datamodel->sessions, &datamodel->payloads, &datamodel->loops};
    for (int t = 0; t < 3; t++) {
        ta_bytes ns = read_bytes(&r);
        if (r.bad || ta_table_restore(tables[t], ns) != TA_OK) return TA_ERROR_INVALID;
    }
    uint64_t count = read_head(&r, 4);
    for (uint64_t i = 0; i < count && !r.bad; i++) {
        read_head(&r, 4);
        uint64_t handle = read_head(&r, 0);
        uint32_t segment = (uint32_t)read_head(&r, 0), chart = (uint32_t)read_head(&r, 0);
        ssession *s = (ssession *)ta_table_get(&datamodel->sessions, handle);
        schart *c = datamodel->charts;
        while (c && (c->segment != segment || c->chart != chart)) c = c->next;
        if (!s || !c) return TA_ERROR_INVALID;
        s->chart = c;
        s->variables = (sjs_value *)salloc(&datamodel->memory, sizeof(sjs_value) * (c->variable_count + 1));
        if (!s->variables) return TA_ERROR_NO_MEMORY;
        for (uint32_t k = 0; k < c->variable_count; k++) s->variables[k] = sjs_undefined();
        sjs_value whole = read_value(datamodel, &r), vars, event;
        if (sjs_object_get(whole, SJS_STR("vars"), &vars) == SJS_OK) fill_from(datamodel, s, vars);
        s->event = sjs_undefined();
        if (sjs_object_get(whole, SJS_STR("event"), &event) == SJS_OK && event.kind == SJS_OBJECT) {
            sjs_retain(event);
            s->event = event;
            s->bound = 1;
        }
        sjs_release(datamodel->context, whole);
        s->init = read_value(datamodel, &r);
        s->kept = read_value(datamodel, &r);
        s->sessionid = read_value(datamodel, &r);
        s->name = read_value(datamodel, &r);
        s->ioprocessors = read_value(datamodel, &r);
        s->pending_fuel = (uint32_t)read_head(&r, 0);
    }
    count = read_head(&r, 4);
    for (uint64_t i = 0; i < count && !r.bad; i++) {
        read_head(&r, 4);
        spayload *p = (spayload *)ta_table_get(&datamodel->payloads, read_head(&r, 0));
        if (!p) return TA_ERROR_INVALID;
        p->references = (uint32_t)read_head(&r, 0);
        p->value = read_value(datamodel, &r);
    }
    count = read_head(&r, 4);
    for (uint64_t i = 0; i < count && !r.bad; i++) {
        read_head(&r, 4);
        sloop *l = (sloop *)ta_table_get(&datamodel->loops, read_head(&r, 0));
        if (!l) return TA_ERROR_INVALID;
        l->array = read_value(datamodel, &r);
    }
    return r.bad ? TA_ERROR_INVALID : TA_OK;
}

/* ---- the public calls ------------------------------------------------------------------ */

ta_status ta_datamodel_staticjs_new(const ta_datamodel_staticjs_config *config, ta_datamodel_staticjs **out) {
    smemory m = {config->allocator, 0};
    ta_datamodel_staticjs *datamodel = (ta_datamodel_staticjs *)salloc(&m, sizeof *datamodel);
    if (!datamodel) return TA_ERROR_NO_MEMORY;
    ta_memset(datamodel, 0, sizeof *datamodel);
    datamodel->memory = m;
    datamodel->table_allocator = (ta_allocator){&datamodel->memory, table_allocate, table_free};
    sjs_context_config context_config = {{&datamodel->memory, &staticjs_heap}, SJS_LIMITS_DEFAULT};
    if (sjs_context_init(&datamodel->context, &context_config) != SJS_OK) return TA_ERROR_NO_MEMORY;
    sjs_vm_config vm_config;
    ta_memset(&vm_config, 0, sizeof vm_config);
    vm_config.context = datamodel->context;
    if (sjs_vm_init(&datamodel->vm, &vm_config) != SJS_OK) return TA_ERROR_NO_MEMORY;
    if (sjs_function_native(datamodel->context, (sjs_native){datamodel, &in_vtable}, &datamodel->in_function) != SJS_OK)
        return TA_ERROR_NO_MEMORY;
    ta_table_config sessions = {sizeof sessions, sizeof(ssession), &datamodel->table_allocator};
    ta_table_config payloads = {sizeof payloads, sizeof(spayload), &datamodel->table_allocator};
    ta_table_config loops = {sizeof loops, sizeof(sloop), &datamodel->table_allocator};
    ta_table_init(&datamodel->sessions, &sessions);
    ta_table_init(&datamodel->payloads, &payloads);
    ta_table_init(&datamodel->loops, &loops);
    *out = datamodel;
    return TA_OK;
}

ta_status ta_datamodel_staticjs_register(ta_datamodel_staticjs *datamodel, const ta_staticjs_function_entry *entry) {
    if (!entry->name.length) return TA_ERROR_INVALID;
    if (datamodel->function_count == datamodel->function_capacity) {
        uint32_t capacity = datamodel->function_capacity ? datamodel->function_capacity * 2 : 16;
        sfunction *f = (sfunction *)salloc(&datamodel->memory, sizeof(sfunction) * capacity);
        if (!f) return TA_ERROR_NO_MEMORY;
        if (datamodel->function_count) ta_memcpy(f, datamodel->functions, sizeof(sfunction) * datamodel->function_count);
        sfree(&datamodel->memory, datamodel->functions);
        datamodel->functions = f;
        datamodel->function_capacity = capacity;
    }
    sfunction *f = &datamodel->functions[datamodel->function_count];
    f->name = copy_text(&datamodel->memory, view_of(entry->name));
    f->chart = entry->chart.length ? copy_text(&datamodel->memory, view_of(entry->chart)) : (sjs_str){0, 0};
    if (sjs_function_native(datamodel->context, entry->native, &f->value) != SJS_OK) return TA_ERROR_NO_MEMORY;
    datamodel->function_count++;
    return TA_OK;
}

/* ---- functions of the json datamodel's kind, as natives -------------------------------- */

typedef struct sjson_function {
    ta_datamodel_staticjs *datamodel;
    ta_json_function function;
    void *context;
    uint8_t *room; /* the result's, grown when the function asks for more */
    uint32_t room_capacity;
    uint32_t fault_length;
    char fault[256];
} sjson_function;

static sjs_result json_fault(sjson_function *f, sjs_native_call *call, ta_string_view text) {
    uint32_t length = text.length < sizeof f->fault ? text.length : (uint32_t)sizeof f->fault;
    if (length) ta_memcpy(f->fault, text.pointer, length);
    f->fault_length = length;
    call->fault = (sjs_str){f->fault, length};
    return SJS_ERR_PROGRAM;
}

static sjs_result json_function_call(void *self, sjs_native_call *call) {
    sjson_function *f = (sjson_function *)self;
    ta_datamodel_staticjs *datamodel = f->datamodel;
    sjs_value arguments;
    if (sjs_array_new(call->context, call->count, &arguments) != SJS_OK) return json_fault(f, call, ta_string("out of memory"));
    for (uint32_t i = 0; i < call->count; i++) {
        sjs_retain(call->arguments[i]);
        if (sjs_array_append(call->context, &arguments, call->arguments[i]) != SJS_OK) {
            sjs_release(call->context, arguments);
            return json_fault(f, call, ta_string("out of memory"));
        }
    }
    sjs_bytes bytes;
    int written = write_value(datamodel, arguments, SJS_FORMAT_CBOR, &bytes);
    sjs_release(call->context, arguments);
    if (!written) return json_fault(f, call, ta_string("an argument is not a value of the value table"));
    for (;;) {
        ta_buffer result = {f->room, f->room_capacity, 0};
        ta_json_call json = {sizeof json, call->count, f->context, {bytes.ptr, bytes.length}, &result, {0, 0}};
        ta_status status = f->function(&json);
        if (status == TA_ERROR_NO_MEMORY && result.length > f->room_capacity) {
            uint8_t *room = (uint8_t *)salloc(&datamodel->memory, result.length);
            if (!room) return json_fault(f, call, ta_string("out of memory"));
            sfree(&datamodel->memory, f->room);
            f->room = room;
            f->room_capacity = (uint32_t)result.length;
            continue;
        }
        if (status != TA_OK) return json_fault(f, call, json.message.length ? json.message : ta_string("the function failed"));
        sjs_value value;
        if (!read_cbor(datamodel, (ta_bytes){f->room, result.length}, &value))
            return json_fault(f, call, ta_string("the function's result is not a value of the value table"));
        call->result = value;
        return SJS_OK;
    }
}

static void json_function_release(void *self) {
    sjson_function *f = (sjson_function *)self;
    smemory *memory = &f->datamodel->memory;
    sfree(memory, f->room);
    sfree(memory, f);
}

static const sjs_native_vtable json_function_vtable = {json_function_call, json_function_release};

ta_status ta_datamodel_staticjs_register_json(ta_datamodel_staticjs *datamodel, const ta_json_function_entry *entry) {
    if (!entry->function) return TA_ERROR_INVALID;
    sjson_function *f = (sjson_function *)salloc(&datamodel->memory, sizeof *f);
    if (!f) return TA_ERROR_NO_MEMORY;
    ta_memset(f, 0, sizeof *f);
    f->datamodel = datamodel;
    f->function = entry->function;
    f->context = entry->context;
    f->room_capacity = 1024;
    f->room = (uint8_t *)salloc(&datamodel->memory, f->room_capacity);
    if (!f->room) return sfree(&datamodel->memory, f), TA_ERROR_NO_MEMORY;
    ta_staticjs_function_entry native = {sizeof native, 0, entry->name, entry->chart, {f, &json_function_vtable}};
    ta_status status = ta_datamodel_staticjs_register(datamodel, &native);
    if (status != TA_OK) json_function_release(f);
    return status;
}

ta_datamodel_binding ta_datamodel_staticjs_binding(ta_datamodel_staticjs *datamodel) {
    ta_datamodel_binding binding = {&staticjs_vtable, datamodel};
    return binding;
}

sjs_context *ta_datamodel_staticjs_context(ta_datamodel_staticjs *datamodel) { return datamodel->context; }

size_t ta_datamodel_staticjs_bytes(const ta_datamodel_staticjs *datamodel) { return datamodel->memory.bytes; }

void ta_datamodel_staticjs_free(ta_datamodel_staticjs *datamodel) {
    if (!datamodel) return;
    for (uint32_t slot = 0; slot < datamodel->sessions.capacity; slot++) {
        ssession *s = (ssession *)ta_table_at(&datamodel->sessions, slot);
        if (s) session_release(datamodel, s);
    }
    for (uint32_t slot = 0; slot < datamodel->payloads.capacity; slot++) {
        spayload *p = (spayload *)ta_table_at(&datamodel->payloads, slot);
        if (p) sjs_release(datamodel->context, p->value);
    }
    for (uint32_t slot = 0; slot < datamodel->loops.capacity; slot++) {
        sloop *l = (sloop *)ta_table_at(&datamodel->loops, slot);
        if (l) sjs_release(datamodel->context, l->array);
    }
    while (datamodel->charts) s_detach(datamodel, datamodel->charts);
    ta_table_free(&datamodel->sessions);
    ta_table_free(&datamodel->payloads);
    ta_table_free(&datamodel->loops);
    for (uint32_t i = 0; i < datamodel->function_count; i++) {
        sjs_release(datamodel->context, datamodel->functions[i].value);
        sfree(&datamodel->memory, (void *)datamodel->functions[i].name.ptr);
        if (datamodel->functions[i].chart.length) sfree(&datamodel->memory, (void *)datamodel->functions[i].chart.ptr);
    }
    sfree(&datamodel->memory, datamodel->functions);
    sjs_release(datamodel->context, datamodel->in_function);
    sfree(&datamodel->memory, datamodel->globals);
    sfree(&datamodel->memory, datamodel->out);
    sjs_vm_destroy(datamodel->vm);
    sjs_context_destroy(datamodel->context);
    smemory m = datamodel->memory;
    sfree(&m, datamodel);
}
