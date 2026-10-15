/*
 * The package's C layer over staticjs (docs/api-map.md), compiled with
 * staticjs's amalgamation into staticjs.wasm (mise run build). JavaScript
 * reads no struct of staticjs's: every export takes and returns numbers,
 * values cross as handles, and the few records the page reads are this
 * file's own arrays of words, in the order written beside each.
 *
 * Handles. A handle is an index into one stack of sjs_value slots, which
 * own what they hold. A call takes a mark (sjs_js_mark) and releases to it
 * at its end (sjs_js_release_to), so a conversion, a native's arguments and
 * a result's walk leave nothing behind. A value kept past a call is a row
 * (sjs_js_keep), retained until dropped.
 *
 * Text in: JavaScript asks for room (sjs_js_scratch), writes UTF-8 there,
 * and passes the length. Text out: a call leaves a view in `text`, read
 * with sjs_js_text_ptr and sjs_js_text_len, valid until the next call.
 *
 * Errors: a call that fails returns SJS_JS_NONE (or a negative status) and
 * fills `error`, read with sjs_js_error.
 */
#include "staticjs.h"

#define JS __attribute__((visibility("default")))
#define SJS_JS_NONE 0xFFFFFFFFu

/* A native's call into JavaScript: the result's handle, or -1 for a fault
 * whose message sjs_js_native_fault set. */
__attribute__((import_module("staticjs_js"), import_name("native"))) extern int32_t staticjs_js_native(uint32_t self, uint32_t base, uint32_t count);
/* A native's function value was freed: its slot may be reused. */
__attribute__((import_module("staticjs_js"), import_name("release"))) extern void staticjs_js_release(uint32_t self);

static void copy(uint8_t *to, const uint8_t *from, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) to[i] = from[i];
}

/* ------------------------------------------------------------------------
 * Growable arrays from the module's allocator
 */

typedef struct {
    uint8_t *ptr;
    uint32_t length;
    uint32_t capacity;
} bytes;

static bool reserve(bytes *b, uint32_t need) {
    if (need <= b->capacity) return true;
    uint32_t capacity = b->capacity < 256u ? 256u : b->capacity;
    while (capacity < need) capacity *= 2u;
    uint8_t *grown = sjs_wasm_alloc(capacity, 8);
    if (grown == NULL) return false;
    if (b->ptr != NULL) {
        copy(grown, b->ptr, b->length);
        sjs_wasm_free(b->ptr, b->capacity, 8);
    }
    b->ptr = grown;
    b->capacity = capacity;
    return true;
}

static bytes scratch;      /* text and records in */
static bytes text_out;     /* a writer's output */
static sjs_str text;       /* the last text out: a view */
static sjs_str text2;      /* a second one: a regular expression's flags */

JS uint8_t *sjs_js_scratch(uint32_t n) {
    return reserve(&scratch, n) ? scratch.ptr : NULL;
}
JS const char *sjs_js_text_ptr(void) { return text.ptr; }
JS uint32_t sjs_js_text_len(void) { return text.length; }
JS const char *sjs_js_text2_ptr(void) { return text2.ptr; }
JS uint32_t sjs_js_text2_len(void) { return text2.length; }

/* ------------------------------------------------------------------------
 * Errors: one record, words in this order: code, offset, line, column,
 * message ptr, message length, subject ptr, subject length.
 */

static uint32_t error_words[8];
static uint8_t error_subject[64];

static void set_error(const sjs_error *e) {
    uint32_t n = e->subject.length < sizeof error_subject ? e->subject.length : (uint32_t)sizeof error_subject;
    copy(error_subject, (const uint8_t *)e->subject.ptr, n);
    uint32_t words[8] = { (uint32_t)e->code, e->at.offset, e->at.line, e->at.column,
                          (uint32_t)(uintptr_t)e->message.ptr, e->message.length, (uint32_t)(uintptr_t)error_subject, n };
    copy((uint8_t *)error_words, (const uint8_t *)words, sizeof words);
}

/* An error of the API (not of a program): code is the sjs_result. */
static void set_failure(sjs_result r, const char *message) {
    uint32_t length = 0;
    while (message[length] != '\0') length++;
    sjs_error e = { (sjs_error_code)0, { 0, 0, 0 }, { message, length }, { "", 0 } };
    set_error(&e);
    error_words[0] = 0x80000000u | (uint32_t)(-(int32_t)r);
}

JS const uint32_t *sjs_js_error(void) { return error_words; }

/* ------------------------------------------------------------------------
 * Contexts: a context, its one evaluator, the globals of its run
 */

typedef struct {
    sjs_context *context;
    sjs_vm *vm;
    sjs_value *globals;    /* the run's globals or arguments, retained */
    uint32_t global_count;
} ctx;

static void drop_globals(ctx *c) {
    for (uint32_t i = 0; i < c->global_count; i++) sjs_release(c->context, c->globals[i]);
    if (c->globals != NULL) sjs_wasm_free(c->globals, c->global_count * sizeof(sjs_value), 8);
    c->globals = NULL;
    c->global_count = 0;
}

/* The limits are the scratch's first eleven words, in sjs_limits' order as
 * staticjs.h declares it. */
JS ctx *sjs_js_context_new(void) {
    const uint32_t *w = (const uint32_t *)(const void *)scratch.ptr;
    if (w == NULL) return NULL;
    sjs_limits limits = { w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10] };
    ctx *c = sjs_wasm_alloc(sizeof *c, 8);
    if (c == NULL) return NULL;
    *c = (ctx){ 0 };
    sjs_context_config config = { sjs_wasm_allocator(), limits };
    sjs_result r = sjs_context_init(&c->context, &config);
    if (r == SJS_OK) r = sjs_vm_init(&c->vm, &(sjs_vm_config){ .context = c->context });
    if (r != SJS_OK) {
        if (c->context != NULL) sjs_context_destroy(c->context);
        sjs_wasm_free(c, sizeof *c, 8);
        set_failure(r, "the context could not be made");
        return NULL;
    }
    return c;
}

JS void sjs_js_context_free(ctx *c) {
    sjs_vm_destroy(c->vm);
    drop_globals(c);
    sjs_context_destroy(c->context);
    sjs_wasm_free(c, sizeof *c, 8);
}

JS double sjs_js_live_bytes(const ctx *c) { return (double)sjs_context_live_bytes(c->context); }

/* The clock in whole seconds (rounded toward negative infinity) and the
 * nanoseconds past them, each exact in a double. */
JS int32_t sjs_js_set_clock(ctx *c, double seconds, double nanoseconds) {
    sjs_epoch now = { (int64_t)seconds, (uint32_t)nanoseconds };
    return sjs_vm_set_clock(c->vm, &now);
}

JS int32_t sjs_js_advance_clock(ctx *c, double nanoseconds) {
    return sjs_vm_advance_clock(c->vm, (uint64_t)nanoseconds);
}

JS int32_t sjs_js_set_seed(ctx *c, double seed) {
    return sjs_vm_set_seed(c->vm, (int64_t)seed);
}

/* ------------------------------------------------------------------------
 * Handles: slots owning values, released to a mark
 */

static sjs_value *slots;
static uint32_t slot_count;
static uint32_t slot_capacity;

/* Push a value the slot takes over; SJS_JS_NONE when there is no room. */
static uint32_t push(sjs_value v) {
    if (slot_count == slot_capacity) {
        uint32_t capacity = slot_capacity == 0 ? 256u : slot_capacity * 2u;
        sjs_value *grown = sjs_wasm_alloc(capacity * sizeof *grown, 8);
        if (grown == NULL) {
            set_failure(SJS_ERR_NOMEM, "out of memory");
            return SJS_JS_NONE;
        }
        for (uint32_t i = 0; i < slot_count; i++) grown[i] = slots[i];
        if (slots != NULL) sjs_wasm_free(slots, slot_capacity * sizeof *slots, 8);
        slots = grown;
        slot_capacity = capacity;
    }
    slots[slot_count] = v;
    return slot_count++;
}

/* Push a borrowed value: retained, so the slot owns a reference too. */
static uint32_t push_borrowed(sjs_value v) {
    uint32_t h = push(v);
    if (h != SJS_JS_NONE) sjs_retain(v);
    return h;
}

JS uint32_t sjs_js_mark(void) { return slot_count; }

JS void sjs_js_release_to(const ctx *c, uint32_t mark) {
    while (slot_count > mark) sjs_release(c->context, slots[--slot_count]);
}

/* ------------------------------------------------------------------------
 * Rows: values kept past a call, each with a generation
 */

typedef struct {
    sjs_value value;
    const ctx *owner;
    double generation;     /* 0: free */
} row;

static row *rows;
static uint32_t row_capacity;
static double generations;    /* one counter for the instance */

/* Keep the value at handle `h` (retained) as a row: its index; its
 * generation by sjs_js_row_generation. */
JS uint32_t sjs_js_keep(const ctx *c, uint32_t h) {
    uint32_t i = 0;
    while (i < row_capacity && rows[i].generation != 0) i++;
    if (i == row_capacity) {
        uint32_t capacity = row_capacity == 0 ? 64u : row_capacity * 2u;
        row *grown = sjs_wasm_alloc(capacity * sizeof *grown, 8);
        if (grown == NULL) {
            set_failure(SJS_ERR_NOMEM, "out of memory");
            return SJS_JS_NONE;
        }
        for (uint32_t k = 0; k < capacity; k++) grown[k] = k < row_capacity ? rows[k] : (row){ { 0 }, NULL, 0 };
        if (rows != NULL) sjs_wasm_free(rows, row_capacity * sizeof *rows, 8);
        rows = grown;
        row_capacity = capacity;
    }
    sjs_retain(slots[h]);
    rows[i] = (row){ slots[h], c, ++generations };
    return i;
}

JS double sjs_js_row_generation(uint32_t i) { return i < row_capacity ? rows[i].generation : 0; }

/* Drop a row, when its generation is still `generation`: a finalizer that
 * runs late finds a row reused, or its context gone, and does nothing. */
JS void sjs_js_drop(uint32_t i, double generation) {
    if (i >= row_capacity || rows[i].generation != generation || generation == 0) return;
    sjs_release(rows[i].owner->context, rows[i].value);
    rows[i] = (row){ { 0 }, NULL, 0 };
}

/* Drop every row of a context, before it is freed. */
JS void sjs_js_drop_rows(const ctx *c) {
    for (uint32_t i = 0; i < row_capacity; i++) {
        if (rows[i].generation != 0 && rows[i].owner == c) {
            sjs_release(c->context, rows[i].value);
            rows[i] = (row){ { 0 }, NULL, 0 };
        }
    }
}

/* The row's value on the stack. */
JS uint32_t sjs_js_row(uint32_t i) {
    return push_borrowed(rows[i].value);
}

/* ------------------------------------------------------------------------
 * Building values: each returns the new value's handle, or SJS_JS_NONE
 */

static uint32_t made(sjs_result r, sjs_value v, const char *what) {
    if (r != SJS_OK) {
        set_failure(r, what);
        return SJS_JS_NONE;
    }
    return push(v);
}

JS uint32_t sjs_js_undefined(void) { return push(sjs_undefined()); }
JS uint32_t sjs_js_null(void) { return push(sjs_null()); }
JS uint32_t sjs_js_boolean(int32_t b) { return push(sjs_boolean(b != 0)); }
JS uint32_t sjs_js_number(double d) { return push(sjs_number(d)); }

/* A string of the scratch's first `length` bytes, UTF-8. */
JS uint32_t sjs_js_string(const ctx *c, uint32_t length) {
    sjs_value v;
    sjs_result r = sjs_string_new(c->context, (sjs_str){ (const char *)scratch.ptr, length }, &v);
    return made(r, v, "a string past the context's limits");
}

/* A BigInt of the scratch's decimal text. */
JS uint32_t sjs_js_bigint(const ctx *c, uint32_t length) {
    sjs_value v;
    sjs_result r = sjs_bigint_new(c->context, (sjs_str){ (const char *)scratch.ptr, length }, &v);
    return made(r, v, "a BigInt past the context's limits");
}

/* An array (kind 0), an object (1), a set (2) or a map (3) with room for n. */
JS uint32_t sjs_js_container(const ctx *c, uint32_t kind, uint32_t n) {
    sjs_value v;
    sjs_result r = kind == 0 ? sjs_array_new(c->context, n, &v)
                 : kind == 1 ? sjs_object_new(c->context, n, &v)
                 : kind == 2 ? sjs_set_new(c->context, n, &v)
                             : sjs_map_new(c->context, n, &v);
    return made(r, v, "a value past the context's limits");
}

/* Append the item at handle `item` to the array or the set at `container`;
 * the container takes it over, and its slot becomes undefined. */
JS int32_t sjs_js_append(const ctx *c, uint32_t container, uint32_t item) {
    sjs_value *to = &slots[container];
    sjs_result r = to->kind == SJS_SET ? sjs_set_add(c->context, to, slots[item]) : sjs_array_append(c->context, to, slots[item]);
    if (r == SJS_OK) slots[item] = sjs_undefined();
    else set_failure(r, to->kind == SJS_SET ? "a Set's element in staticjs is a primitive" : "an array past the context's limits");
    return r;
}

/* Put the value at the top slot into the object at `object`, under the
 * key that is the scratch's first `key_length` bytes. */
JS int32_t sjs_js_object_put(const ctx *c, uint32_t object, uint32_t key_length) {
    uint32_t item = slot_count - 1u;
    sjs_property p = { { (const char *)scratch.ptr, key_length }, slots[item] };
    sjs_result r = sjs_object_put(c->context, &slots[object], &p);
    if (r == SJS_OK) slots[item] = sjs_undefined();
    else set_failure(r, r == SJS_ERR_INVALID ? "staticjs has no object with this key" : "an object past the context's limits");
    return r;
}

/* Set the key at the slot below the top and the value at the top into the
 * map at `map`. */
JS int32_t sjs_js_map_set(const ctx *c, uint32_t map) {
    sjs_map_entry e = { slots[slot_count - 2u], slots[slot_count - 1u] };
    sjs_result r = sjs_map_set(c->context, &slots[map], &e);
    if (r == SJS_OK) {
        slots[slot_count - 2u] = sjs_undefined();
        slots[slot_count - 1u] = sjs_undefined();
    } else {
        set_failure(r, r == SJS_ERR_INVALID ? "a Map's key in staticjs is a primitive" : "a Map past the context's limits");
    }
    return r;
}

/* A Date or a Temporal value from the scratch's doubles: its sjs_time_type,
 * then for a plain type year, month, day, hour, minute, second,
 * millisecond, microsecond, nanosecond; for an Instant seconds and
 * nanoseconds; for a Duration its ten fields; for a Date its time value. */
JS uint32_t sjs_js_time(const ctx *c) {
    const double *f = (const double *)(const void *)scratch.ptr;
    sjs_time t = { .type = (sjs_time_type)f[0] };
    switch (t.type) {
    case SJS_TIME_INSTANT:
        t.instant = (sjs_epoch){ (int64_t)f[1], (uint32_t)f[2] };
        break;
    case SJS_TIME_DURATION:
        for (uint32_t i = 0; i < 10u; i++) t.duration[i] = f[1u + i];
        break;
    case SJS_TIME_DATE:
        t.time = f[1];
        break;
    default:
        t.date_time = (sjs_date_time){ (int32_t)f[1], (uint8_t)f[2], (uint8_t)f[3], (uint8_t)f[4], (uint8_t)f[5],
                                       (uint8_t)f[6], (uint16_t)f[7], (uint16_t)f[8], (uint16_t)f[9] };
        break;
    }
    sjs_value v;
    sjs_result r = sjs_time_new(c->context, &t, &v);
    return made(r, v, "JavaScript refuses these fields for this Temporal type or Date");
}

/* A value read from the scratch's first `length` bytes as JSON (format 1)
 * or CBOR (2). */
JS uint32_t sjs_js_read(const ctx *c, uint32_t format, uint32_t length) {
    sjs_value v;
    sjs_error e = { 0 };
    sjs_read_params params = { (sjs_format)format, { scratch.ptr, length }, &e, NULL };
    sjs_result r = sjs_value_read(c->context, &params, &v);
    if (r == SJS_ERR_PROGRAM) {
        set_error(&e);
        return SJS_JS_NONE;
    }
    return made(r, v, "the bytes are past the context's limits");
}

/* ------------------------------------------------------------------------
 * Natives: a function value whose calls go to JavaScript
 */

static uint8_t fault_text[64];
static uint32_t fault_length;

/* The fault a native gives, as the scratch's first `length` bytes, cut to
 * 64 between characters. */
JS void sjs_js_native_fault(uint32_t length) {
    uint32_t n = length < sizeof fault_text ? length : (uint32_t)sizeof fault_text;
    while (n > 0 && n < length && (scratch.ptr[n] & 0xC0u) == 0x80u) n--;
    copy(fault_text, scratch.ptr, n);
    fault_length = n;
}

static sjs_result native_call(void *self, sjs_native_call *call) {
    uint32_t mark = slot_count;
    for (uint32_t i = 0; i < call->count; i++) {
        if (push_borrowed(call->arguments[i]) == SJS_JS_NONE) {
            while (slot_count > mark) sjs_release(call->context, slots[--slot_count]);
            return SJS_ERR_NOMEM;
        }
    }
    int32_t result = staticjs_js_native((uint32_t)(uintptr_t)self, mark, call->count);
    sjs_result r = SJS_OK;
    if (result < 0) {
        call->fault = (sjs_str){ (const char *)fault_text, fault_length };
        r = SJS_ERR_PROGRAM;
    } else {
        call->result = slots[result];
        slots[result] = sjs_undefined();   /* the program takes it over */
    }
    while (slot_count > mark) sjs_release(call->context, slots[--slot_count]);
    return r;
}

static void native_release(void *self) {
    staticjs_js_release((uint32_t)(uintptr_t)self);
}

static const sjs_native_vtable native_vtable = { native_call, native_release };

/* A native function whose calls go to the import with `self`. */
JS uint32_t sjs_js_native(const ctx *c, uint32_t self) {
    sjs_value v;
    sjs_result r = sjs_function_native(c->context, (sjs_native){ (void *)(uintptr_t)self, &native_vtable }, &v);
    return made(r, v, "out of memory");
}

/* ------------------------------------------------------------------------
 * Reading values
 */

JS uint32_t sjs_js_kind(uint32_t h) { return slots[h].kind; }
JS double sjs_js_number_of(uint32_t h) { return slots[h].as.number; }
JS uint32_t sjs_js_boolean_of(uint32_t h) { return slots[h].boolean; }

/* Elements of an array or a set, properties of an object, entries of a
 * map, what an iterator has left. */
JS uint32_t sjs_js_length(uint32_t h) {
    sjs_value v = slots[h];
    return v.kind == SJS_ITERATOR ? sjs_iterator_count(v) : v.kind == SJS_MAP ? sjs_map_size(v) : sjs_length(v);
}

/* A string's text, into `text`: its length. */
JS uint32_t sjs_js_string_of(uint32_t h) {
    text = sjs_string_text(slots[h]);
    return text.length;
}

/* A BigInt's decimal text, into `text`. */
JS int32_t sjs_js_bigint_of(const ctx *c, uint32_t h) {
    uint32_t bound = sjs_bigint_text_bound(slots[h]);
    if (!reserve(&text_out, bound)) return SJS_ERR_NOMEM;
    sjs_buffer b = { text_out.ptr, bound, 0 };
    sjs_result r = sjs_bigint_text(c->context, slots[h], &b);
    text = (sjs_str){ (const char *)text_out.ptr, b.length };
    return r;
}

/* Item `i` of an array, a set or an iterator: its handle. An entries
 * iterator's item pushes its key, then its value, and gives the key's
 * handle with the high bit set. */
JS uint32_t sjs_js_item(uint32_t h, uint32_t i) {
    sjs_value v = slots[h];
    if (v.kind == SJS_ARRAY) return push_borrowed(sjs_array_at(v, i));
    if (v.kind == SJS_SET) return push_borrowed(sjs_set_at(v, i));
    sjs_iterated item;
    if (sjs_iterator_at(v, i, &item) != SJS_OK) return SJS_JS_NONE;
    if (!item.entry) return push_borrowed(item.value);
    uint32_t key = push_borrowed(item.key);
    if (key == SJS_JS_NONE || push_borrowed(item.value) == SJS_JS_NONE) return SJS_JS_NONE;
    return key | 0x80000000u;
}

/* Property `i` of an object: its value's handle; its key into `text`. */
JS uint32_t sjs_js_property(uint32_t h, uint32_t i) {
    sjs_property p;
    if (sjs_object_at(slots[h], i, &p) != SJS_OK) return SJS_JS_NONE;
    text = p.key;
    return push_borrowed(p.value);
}

/* Entry `i` of a map: its key's handle, its value's the next one. */
JS uint32_t sjs_js_entry(uint32_t h, uint32_t i) {
    sjs_map_entry e;
    if (sjs_map_at(slots[h], i, &e) != SJS_OK) return SJS_JS_NONE;
    uint32_t key = push_borrowed(e.key);
    if (key == SJS_JS_NONE || push_borrowed(e.value) == SJS_JS_NONE) return SJS_JS_NONE;
    return key;
}

/* A Date's or a Temporal value's fields, as sjs_js_time takes them: a
 * pointer to the doubles. */
static double time_words[11];
JS const double *sjs_js_time_of(uint32_t h) {
    sjs_time t;
    if (sjs_time_read(slots[h], &t) != SJS_OK) return NULL;
    time_words[0] = (double)t.type;
    switch (t.type) {
    case SJS_TIME_INSTANT:
        time_words[1] = (double)t.instant.seconds;
        time_words[2] = (double)t.instant.nanoseconds;
        break;
    case SJS_TIME_DURATION:
        for (uint32_t i = 0; i < 10u; i++) time_words[1u + i] = t.duration[i];
        break;
    case SJS_TIME_DATE:
        time_words[1] = t.time;
        break;
    default: {
        const sjs_date_time *d = &t.date_time;
        double f[9] = { d->year, d->month, d->day, d->hour, d->minute, d->second, d->millisecond, d->microsecond, d->nanosecond };
        for (uint32_t i = 0; i < 9u; i++) time_words[1u + i] = f[i];
        break;
    }
    }
    return time_words;
}

/* A regular expression's source into `text`, its flags into `text2`. */
static sjs_regexp_text regexp;
JS int32_t sjs_js_regexp_of(uint32_t h) {
    sjs_result r = sjs_regexp_read(slots[h], &regexp);
    if (r != SJS_OK) return r;
    text = regexp.source;
    text2 = (sjs_str){ regexp.flags, regexp.flag_count };
    return SJS_OK;
}

/* The value at `h` written in `format` (0 source, 1 JSON, 2 CBOR) into
 * `text`, at most the context's max_string_bytes: SJS_OK, SJS_ERR_LIMIT
 * past it, SJS_ERR_PROGRAM (the error says why) for no text. */
JS int32_t sjs_js_write(const ctx *c, uint32_t h, uint32_t format) {
    sjs_writer *w;
    sjs_error e = { 0 };
    sjs_writer_config config = { c->context, slots[h], SJS_STR("<function>"), (sjs_format)format, &e };
    sjs_result r = sjs_writer_init(&w, &config);
    if (r != SJS_OK) return r;
    text_out.length = 0;
    uint32_t cap = 16u * 1024u * 1024u;
    do {
        if (text_out.length >= cap || !reserve(&text_out, text_out.length + 4096u)) {
            r = text_out.length >= cap ? SJS_ERR_LIMIT : SJS_ERR_NOMEM;
            break;
        }
        sjs_buffer b = { text_out.ptr + text_out.length, 4096u, 0 };
        r = sjs_writer_write(w, &b);
        text_out.length += b.length;
    } while (r == SJS_NEED_SPACE);
    sjs_writer_destroy(w);
    if (r == SJS_ERR_PROGRAM) set_error(&e);
    text = (sjs_str){ (const char *)text_out.ptr, text_out.length };
    return r;
}

/* ------------------------------------------------------------------------
 * Compiling: begin, names of globals one by one, then the source
 */

static bytes names;        /* the globals' names, each as its length and its bytes */
static uint32_t name_count;
static bytes module_names; /* the modules' names, the same way */
static sjs_value *module_values;   /* each module's exports, retained for the compile */
static uint32_t module_count;
static uint32_t module_capacity;

static void drop_modules(const ctx *c) {
    for (uint32_t i = 0; i < module_count; i++) sjs_release(c->context, module_values[i]);
    module_count = 0;
    module_names.length = 0;
}

JS void sjs_js_compile_begin(const ctx *c) {
    names.length = 0;
    name_count = 0;
    drop_modules(c);
}

/* A module a program may import: the scratch's first `length` bytes name
 * it, and the value at handle `exports` is its exports (retained until the
 * compile ends). */
JS int32_t sjs_js_compile_module(uint32_t length, uint32_t exports) {
    if (module_count == module_capacity) {
        uint32_t capacity = module_capacity == 0 ? 8u : module_capacity * 2u;
        sjs_value *grown = sjs_wasm_alloc(capacity * sizeof *grown, 8);
        if (grown == NULL) return SJS_ERR_NOMEM;
        for (uint32_t i = 0; i < module_count; i++) grown[i] = module_values[i];
        if (module_values != NULL) sjs_wasm_free(module_values, module_capacity * sizeof *module_values, 8);
        module_values = grown;
        module_capacity = capacity;
    }
    if (!reserve(&module_names, module_names.length + 4u + length)) return SJS_ERR_NOMEM;
    copy(module_names.ptr + module_names.length, (const uint8_t *)&length, 4u);
    copy(module_names.ptr + module_names.length + 4u, scratch.ptr, length);
    module_names.length += 4u + length;
    sjs_retain(slots[exports]);
    module_values[module_count++] = slots[exports];
    return SJS_OK;
}

/* The modules adapter: a name among those given for this compile. */
static sjs_result find_module(void *self, sjs_str name, sjs_value *exports) {
    (void)self;
    uint32_t at = 0;
    for (uint32_t i = 0; i < module_count; i++) {
        uint32_t n;
        copy((uint8_t *)&n, module_names.ptr + at, 4u);
        const uint8_t *text = module_names.ptr + at + 4u;
        bool same = n == name.length;
        for (uint32_t k = 0; same && k < n; k++) same = text[k] == (uint8_t)name.ptr[k];
        if (same) {
            *exports = module_values[i];
            return SJS_OK;
        }
        at += 4u + n;
    }
    return SJS_ERR_NOT_FOUND;
}

static const sjs_modules_vtable modules_vtable = { find_module };

/* A global's name: the scratch's first `length` bytes. */
JS int32_t sjs_js_compile_global(uint32_t length) {
    if (!reserve(&names, names.length + 4u + length)) return SJS_ERR_NOMEM;
    copy(names.ptr + names.length, (const uint8_t *)&length, 4u);
    copy(names.ptr + names.length + 4u, scratch.ptr, length);
    names.length += 4u + length;
    name_count++;
    return SJS_OK;
}

/* Compile the scratch's first `length` bytes as an expression (goal 0) or
 * a module (1): the program, or NULL with the error set. */
JS sjs_program *sjs_js_compile(const ctx *c, uint32_t goal, uint32_t length) {
    sjs_str *globals = name_count == 0 ? NULL : sjs_wasm_alloc(name_count * sizeof *globals, 8);
    if (name_count != 0 && globals == NULL) {
        set_failure(SJS_ERR_NOMEM, "out of memory");
        return NULL;
    }
    uint32_t at = 0;
    for (uint32_t i = 0; i < name_count; i++) {
        uint32_t n;
        copy((uint8_t *)&n, names.ptr + at, 4u);
        globals[i] = (sjs_str){ (const char *)names.ptr + at + 4u, n };
        at += 4u + n;
    }
    sjs_program *program = NULL;
    sjs_error e = { 0 };
    uint8_t subject[64];
    sjs_compile_params params = {
        .compiler = { .context = c->context, .goal = goal == 1u ? SJS_GOAL_MODULE : SJS_GOAL_EXPRESSION,
                      .globals = globals, .global_count = name_count,
                      .modules = { NULL, module_count == 0 ? NULL : &modules_vtable } },
        .source = { (const char *)scratch.ptr, length },
        .error = &e,
        .subject = &(sjs_buffer){ subject, sizeof subject, 0 },
    };
    sjs_result r = sjs_compile(&params, &program);
    if (globals != NULL) sjs_wasm_free(globals, name_count * sizeof *globals, 8);
    drop_modules(c);   /* the program retains what it imports */
    if (r == SJS_ERR_PROGRAM) set_error(&e);
    else if (r != SJS_OK) set_failure(r, r == SJS_ERR_LIMIT ? "the source is past the context's limits" : "out of memory");
    return r == SJS_OK ? program : NULL;
}

JS void sjs_js_program_release(sjs_program *p) { sjs_program_release(p); }
JS uint32_t sjs_js_function_count(const sjs_program *p) { return sjs_program_function_count(p); }

/* Function `i` of a program, as words: offset, line, column, params; its
 * shape into `text`. */
static uint32_t function_words[4];
JS const uint32_t *sjs_js_function(const sjs_program *p, uint32_t i) {
    sjs_function_summary f;
    if (sjs_program_function(p, i, &f) != SJS_OK) return NULL;
    uint32_t w[4] = { f.at.offset, f.at.line, f.at.column, f.params };
    copy((uint8_t *)function_words, (const uint8_t *)w, sizeof w);
    text = f.shape;
    return function_words;
}

/* ------------------------------------------------------------------------
 * Running: one run per context, its globals owned by the context
 */

/* Take the top `count` slots as the run's globals (or a call's arguments),
 * retained in an array the context owns until the run ends. */
static sjs_result take_globals(ctx *c, uint32_t count) {
    drop_globals(c);
    if (count == 0) return SJS_OK;
    c->globals = sjs_wasm_alloc(count * sizeof(sjs_value), 8);
    if (c->globals == NULL) return SJS_ERR_NOMEM;
    for (uint32_t i = 0; i < count; i++) {
        sjs_value v = slots[slot_count - count + i];
        sjs_retain(v);
        c->globals[i] = v;
    }
    c->global_count = count;
    return SJS_OK;
}

/* Start `program` on the top `count` slots as its globals; a paused run is
 * abandoned, as sjs_vm_start abandons one. */
JS int32_t sjs_js_start(ctx *c, sjs_program *program, uint32_t count) {
    sjs_vm_reset(c->vm);
    sjs_result r = take_globals(c, count);
    if (r == SJS_OK) r = sjs_vm_start(c->vm, &(sjs_run_params){ .program = program, .globals = c->globals,
                                                               .global_count = c->global_count });
    if (r != SJS_OK) {
        drop_globals(c);
        set_failure(r, r == SJS_ERR_INVALID ? "the globals do not match the program's" : "the run could not start");
    }
    return r;
}

/* Call the function at handle `function` with the `count` slots above it. */
JS int32_t sjs_js_call(ctx *c, uint32_t function, uint32_t count) {
    sjs_vm_reset(c->vm);
    sjs_value f = slots[function];
    sjs_result r = take_globals(c, count);
    if (r == SJS_OK) r = sjs_vm_start(c->vm, &(sjs_run_params){ .function = f, .arguments = c->globals,
                                                               .argument_count = c->global_count });
    if (r != SJS_OK) {
        drop_globals(c);
        set_failure(r, r == SJS_ERR_INVALID ? "staticjs calls only a function a program made" : "the call could not start");
    }
    return r;
}

/* Run on `fuel` more units: 0 done, 1 paused, 2 faulted (the error set). */
JS int32_t sjs_js_step(ctx *c, double fuel) {
    sjs_run status = sjs_vm_run(c->vm, (uint64_t)fuel);
    if (status == SJS_RUN_FAULT) {
        sjs_error e;
        if (sjs_vm_error(c->vm, &e) == SJS_OK) set_error(&e);
    }
    return (int32_t)status;
}

JS double sjs_js_fuel_used(const ctx *c) { return (double)sjs_vm_fuel_used(c->vm); }

/* The result of a done run, its handle; the run's globals are let go. */
JS uint32_t sjs_js_take(ctx *c) {
    sjs_value v;
    if (sjs_vm_take_result(c->vm, &v) != SJS_OK) return SJS_JS_NONE;
    uint32_t h = push(v);
    sjs_vm_reset(c->vm);
    drop_globals(c);
    return h;
}

/* Abandon a run, or forget a faulted one's frames, and let its globals go. */
JS void sjs_js_cancel(ctx *c) {
    sjs_vm_reset(c->vm);
    drop_globals(c);
}

JS uint32_t sjs_js_frame_count(const ctx *c) { return sjs_vm_frame_count(c->vm); }

/* Frame `i`, as words: kind, offset, line, column, index, length, the
 * function's index, the program; its object and name into `text` and
 * `text2`. */
static uint32_t frame_words[8];
JS const uint32_t *sjs_js_frame(const ctx *c, uint32_t i) {
    sjs_frame f;
    if (sjs_vm_frame(c->vm, i, &f) != SJS_OK) return NULL;
    uint32_t w[8] = { (uint32_t)f.kind, f.at.offset, f.at.line, f.at.column, f.index, f.length, f.function,
                      (uint32_t)(uintptr_t)f.program };
    copy((uint8_t *)frame_words, (const uint8_t *)w, sizeof w);
    text = f.object;
    text2 = f.name;
    return frame_words;
}
