/*
 * JSON.stringify. A leaf (docs/builtins.md): its input is one value, and it
 * finishes in one C call. It does not recurse: the containers open while a
 * value is written are an explicit stack, as everything in staticjs keeps
 * one.
 *
 * Where JavaScript calls a function, staticjs faults: a replacer function, a
 * toJSON method. Where JavaScript would write a string staticjs cannot hold
 * (a lone surrogate), it faults too.
 */
#include "vm.h"

static sjs_value json_arg(const sjs_leaf_call *call, uint32_t i) {
    return i < call->count ? call->args[i] : sjs_undefined();
}

static bool json_callable(sjs_value v) {
    return v.kind == SJS_FUNCTION && v.internal == SJS_INTERNAL_NONE;
}

/* ------------------------------------------------------------------------
 * JSON.stringify
 */

/* A container being written: borrowed from the value written. */
typedef struct {
    sjs_value value;
    uint32_t index;     /* the next item, or the next key of the list */
    uint32_t written;   /* items written */
} json_frame;

typedef struct {
    sjs_vm *vm;
    sjs_buf out;
    sjs_buf frames;     /* json_frame */
    sjs_array *keys;    /* an array replacer's keys, a set of strings, owned; or NULL */
    uint64_t visited;   /* fuel: a unit per value and per key looked up */
    uint64_t omitted;   /* members left out, which count toward the output's limit */
    uint32_t gap_length;
    char gap[32];       /* at most 10 UTF-16 code units: 30 bytes */
} json_writer;

/* The output, the members left out included, is at most max_string_bytes,
 * so that the work of one call is bounded by its limits. */
static sjs_result json_put(json_writer *w, sjs_str s) {
    sjs_context *context = w->vm->context;
    if ((uint64_t)w->out.length + s.length + w->omitted > context->limits.max_string_bytes) return sjs_vm_fail(w->vm, SJS_ERR_LIMIT);
    sjs_result e = sjs_buf_append(&context->counting, &w->out, (sjs_bytes){ (const uint8_t *)s.ptr, s.length });
    return e == SJS_OK ? SJS_OK : sjs_vm_fail(w->vm, e);
}

static sjs_result json_omit(json_writer *w) {
    w->visited++;
    w->omitted++;
    return json_put(w, SJS_STR(""));
}

/* QuoteJSONString, with the writer's escapes. */
static sjs_result json_quote(json_writer *w, sjs_str s) {
    if (json_put(w, SJS_STR("\"")) != SJS_OK) return SJS_ERR_PROGRAM;
    uint32_t start = 0;
    for (uint32_t i = 0; i < s.length; i++) {
        uint8_t c = (uint8_t)s.ptr[i];
        if (c >= 0x20u && c != '"' && c != '\\') continue;
        char escape[8];
        uint32_t n = sjs_json_escape(c, escape);
        if (json_put(w, (sjs_str){ s.ptr + start, i - start }) != SJS_OK) return SJS_ERR_PROGRAM;
        if (json_put(w, (sjs_str){ escape, n }) != SJS_OK) return SJS_ERR_PROGRAM;
        start = i + 1u;
    }
    if (json_put(w, (sjs_str){ s.ptr + start, s.length - start }) != SJS_OK) return SJS_ERR_PROGRAM;
    return json_put(w, SJS_STR("\""));
}

/* A line break and `depth` gaps, when there is a gap. */
static sjs_result json_indent(json_writer *w, uint32_t depth) {
    if (w->gap_length == 0) return SJS_OK;
    if (json_put(w, SJS_STR("\n")) != SJS_OK) return SJS_ERR_PROGRAM;
    for (uint32_t i = 0; i < depth; i++) {
        if (json_put(w, (sjs_str){ w->gap, w->gap_length }) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    return SJS_OK;
}

/* Left out of an object, null in an array, undefined at the top. */
static bool json_omitted(sjs_value v) {
    return v.kind == SJS_UNDEFINED || v.kind == SJS_FUNCTION;
}

/* A map, a set or an iterator: an object with no own enumerable property,
 * `{}`. A key of an array replacer that JavaScript would find on its
 * prototype (a method, or size) faults. */
static sjs_result json_empty(json_writer *w, sjs_value v) {
    if (w->keys != NULL) {
        uint8_t flag = v.kind == SJS_MAP ? SJS_NAME_ON_MAP : v.kind == SJS_SET ? SJS_NAME_ON_SET : SJS_NAME_ON_ITERATOR;
        for (uint32_t i = 0; i < w->keys->length; i++) {
            const sjs_string *key = SJS_AS_STRING(w->keys->items[i]);
            const sjs_name *name = sjs_name_of(key);
            if ((name != NULL && (name->flags & flag) != 0) || (v.kind != SJS_ITERATOR && sjs_str_equal(sjs_string_view(key), SJS_STR("size")))) {
                sjs_vm_fault(w->vm, "JSON.stringify would read this key of its list from a prototype");
                sjs_error_subject(&w->vm->error, sjs_string_view(key));
                return SJS_ERR_PROGRAM;
            }
        }
        w->visited += w->keys->length;
    }
    return json_put(w, SJS_STR("{}"));
}

/* A value: written whole, or its container opened. */
static sjs_result json_value_out(json_writer *w, sjs_value v) {
    w->visited++;
    switch (v.kind) {
    case SJS_NULL:
        return json_put(w, SJS_STR("null"));
    case SJS_BOOLEAN:
        return json_put(w, v.boolean ? SJS_STR("true") : SJS_STR("false"));
    case SJS_NUMBER: {
        if (!sjs_number_is_finite(v.as.number)) return json_put(w, SJS_STR("null"));
        char digits[32];
        return json_put(w, (sjs_str){ digits, sjs_number_format(v.as.number, digits) });
    }
    case SJS_STRING:
        return json_quote(w, sjs_string_view(SJS_AS_STRING(v)));
    case SJS_BIGINT:
        return sjs_vm_fault(w->vm, "JSON.stringify of a BigInt throws a TypeError in JavaScript");
    case SJS_OBJECT: {
        const sjs_entry *e = sjs_object_find(SJS_AS_OBJECT(v), SJS_STR("toJSON"));
        if (e != NULL && json_callable(e->value)) {
            return sjs_vm_fault(w->vm, "JSON.stringify calls an object's toJSON method, which staticjs does not");
        }
        break;
    }
    case SJS_ARRAY:
        break;
    default:
        return json_empty(w, v);
    }
    if (json_put(w, v.kind == SJS_ARRAY ? SJS_STR("[") : SJS_STR("{")) != SJS_OK) return SJS_ERR_PROGRAM;
    json_frame f = { v, 0, 0 };
    sjs_result e = sjs_buf_append(&w->vm->context->counting, &w->frames, (sjs_bytes){ (const uint8_t *)&f, sizeof f });
    return e == SJS_OK ? SJS_OK : sjs_vm_fail(w->vm, e);
}

/* The container on top is done: its end, on a line of its own when it has
 * items and there is a gap. */
static sjs_result json_close_out(json_writer *w, json_frame *f) {
    uint32_t depth = SJS_BUF_COUNT(w->frames, json_frame);
    sjs_str end = f->value.kind == SJS_ARRAY ? SJS_STR("]") : SJS_STR("}");
    if (f->written != 0 && json_indent(w, depth - 1u) != SJS_OK) return SJS_ERR_PROGRAM;
    w->frames.length -= (uint32_t)sizeof(json_frame);
    return json_put(w, end);
}

/* The next item of the container on top. */
static sjs_result json_step(json_writer *w) {
    uint32_t depth = SJS_BUF_COUNT(w->frames, json_frame);
    json_frame *f = &SJS_BUF_AT(w->frames, json_frame, depth - 1u);
    if (f->value.kind == SJS_ARRAY) {
        const sjs_array *a = SJS_AS_ARRAY(f->value);
        if (f->index == a->length) return json_close_out(w, f);
        sjs_value item = a->items[f->index++];
        if (f->written++ != 0 && json_put(w, SJS_STR(",")) != SJS_OK) return SJS_ERR_PROGRAM;
        if (json_indent(w, depth) != SJS_OK) return SJS_ERR_PROGRAM;
        if (json_omitted(item)) {
            w->visited++;
            return json_put(w, SJS_STR("null"));
        }
        return json_value_out(w, item);
    }
    const sjs_object *o = SJS_AS_OBJECT(f->value);
    const sjs_entry *e;
    if (w->keys != NULL) {
        if (f->index == w->keys->length) return json_close_out(w, f);
        const sjs_string *key = SJS_AS_STRING(w->keys->items[f->index++]);
        e = sjs_object_find(o, sjs_string_view(key));
        if (e == NULL) {
            const sjs_name *name = sjs_name_of(key);
            if (name != NULL && (name->flags & SJS_NAME_OBJECT) != 0) {
                sjs_vm_fault(w->vm, "JSON.stringify would read this key of its list from a prototype");
                sjs_error_subject(&w->vm->error, sjs_string_view(key));
                return SJS_ERR_PROGRAM;
            }
            return json_omit(w);
        }
    } else {
        if (f->index == o->count) return json_close_out(w, f);
        e = &o->entries[f->index++];
    }
    if (json_omitted(e->value)) return json_omit(w);
    if (f->written++ != 0 && json_put(w, SJS_STR(",")) != SJS_OK) return SJS_ERR_PROGRAM;
    if (json_indent(w, depth) != SJS_OK || json_quote(w, sjs_string_view(e->key)) != SJS_OK) return SJS_ERR_PROGRAM;
    if (json_put(w, w->gap_length != 0 ? SJS_STR(": ") : SJS_STR(":")) != SJS_OK) return SJS_ERR_PROGRAM;
    return json_value_out(w, e->value);
}

/* The gap from `space`: a number of spaces, clamped to 10, or a string's
 * first 10 UTF-16 code units, which must not end inside a pair. Anything
 * else gives none, as in JavaScript. A number between 0 and 1 faults: the
 * specification gives no gap, and V8 (Node) breaks lines all the same. */
static sjs_result json_gap(json_writer *w, sjs_value space) {
    if (space.kind == SJS_NUMBER) {
        double d = space.as.number;
        if (d > 0 && d < 1) {
            /* The specification gives no gap; V8 breaks lines with an empty one. */
            return sjs_vm_fault(w->vm, "JSON.stringify with a space between 0 and 1, which engines write differently");
        }
        uint32_t n = d >= 10.0 ? 10u : d >= 1.0 ? (uint32_t)d : 0u;
        for (uint32_t i = 0; i < n; i++) w->gap[i] = ' ';
        w->gap_length = n;
        return SJS_OK;
    }
    if (space.kind != SJS_STRING) return SJS_OK;
    sjs_str s = sjs_string_view(SJS_AS_STRING(space));
    uint32_t units = 0;
    uint32_t i = 0;
    while (i < s.length && units < 10u) {
        uint32_t next = i;
        int32_t point = sjs_utf8_next(s, &next);
        units += point >= 0x10000 ? 2u : 1u;
        if (units > 10u) {
            return sjs_vm_fault(w->vm, "JSON.stringify's space would end inside a surrogate pair, which a staticjs string cannot hold");
        }
        i = next;
    }
    sjs_copy(w->gap, s.ptr, i);
    w->gap_length = i;
    return SJS_OK;
}

/* An array replacer's keys: its strings and numbers' strings, each once, in
 * order; anything else in it is skipped, as in JavaScript. */
static sjs_result json_keys(json_writer *w, sjs_value replacer) {
    sjs_context *context = w->vm->context;
    const sjs_array *a = SJS_AS_ARRAY(replacer);
    sjs_value set;
    sjs_result e = sjs_set_new(context, 0, &set);
    if (e != SJS_OK) return sjs_vm_fail(w->vm, e);
    w->keys = SJS_AS_ARRAY(set);
    w->visited += a->length;
    for (uint32_t i = 0; i < a->length; i++) {
        sjs_value item = a->items[i];
        if (item.kind != SJS_STRING && item.kind != SJS_NUMBER) continue;
        sjs_string *key;
        e = sjs_to_key(context, item, &key);
        if (e == SJS_OK) {
            e = sjs_set_insert(context, &w->keys, sjs_heap_value(SJS_STRING, &key->h));
            if (e != SJS_OK) sjs_unref(context, sjs_heap_value(SJS_STRING, &key->h));
        }
        if (e != SJS_OK) return sjs_vm_fail(w->vm, e);
    }
    return SJS_OK;
}

static sjs_result json_write(json_writer *w, sjs_leaf_call *call) {
    sjs_value value = json_arg(call, 0);
    sjs_value replacer = json_arg(call, 1);
    if (json_callable(replacer)) {
        return sjs_vm_fault(w->vm, "JSON.stringify with a replacer function calls it back for every value, which staticjs does not");
    }
    if (replacer.kind == SJS_ARRAY && json_keys(w, replacer) != SJS_OK) return SJS_ERR_PROGRAM;
    if (json_gap(w, json_arg(call, 2)) != SJS_OK) return SJS_ERR_PROGRAM;
    if (json_omitted(value)) return SJS_OK;
    if (json_value_out(w, value) != SJS_OK) return SJS_ERR_PROGRAM;
    while (w->frames.length != 0) {
        if (json_step(w) != SJS_OK) return SJS_ERR_PROGRAM;
    }
    sjs_string *s;
    sjs_result e = sjs_string_make(w->vm->context, (sjs_str){ (const char *)w->out.ptr, w->out.length }, &s);
    if (e != SJS_OK) return sjs_vm_fail(w->vm, e);
    call->result = sjs_heap_value(SJS_STRING, &s->h);
    return SJS_OK;
}

/* JSON.stringify(value, replacer, space): a fuel unit per 16 bytes written,
 * per value visited and per key of an array replacer looked up. */
sjs_result sjs_json_stringify(sjs_vm *vm, sjs_leaf_call *call) {
    json_writer w = { 0 };
    w.vm = vm;
    sjs_result result = json_write(&w, call);
    call->cost = w.visited + w.out.length / 16u;
    sjs_buf_free(&vm->context->counting, &w.out);
    sjs_buf_free(&vm->context->counting, &w.frames);
    if (w.keys != NULL) sjs_unref(vm->context, sjs_heap_value(SJS_SET, &w.keys->h));
    return result;
}
