/*
 * The evaluator: a stack machine that runs one instruction, or one step of a
 * looping built-in, per unit of fuel. Its whole state is the value stack and
 * the frame stack, so it stops whenever the fuel runs out and continues on
 * the next sjs_vm_run(). Calls push frames; nothing recurses.
 */
#include "vm.h"

/* ------------------------------------------------------------------------
 * Faults and the stack
 */

static sjs_position fault_position(const sjs_vm *vm) {
    if (vm->frame_count == 0) return (sjs_position){ 0, 1, 1 };
    const vm_frame *f = &vm->frames[vm->frame_count - 1];
    if (f->kind == FRAME_LOOP) return f->at;
    return sjs_program_position(f->program, vm->op_pc);
}

sjs_result sjs_vm_fault(sjs_vm *vm, const char *message) {
    if (vm->status != VM_FAULT) {
        sjs_error_set(&vm->error, fault_position(vm), (sjs_problem){ SJS_ERROR_FAULT, message });
        vm->status = VM_FAULT;
    }
    return SJS_ERR_PROGRAM;
}

static sjs_result fault_named(sjs_vm *vm, const char *message, sjs_str subject) {
    sjs_vm_fault(vm, message);
    sjs_error_subject(&vm->error, subject);
    return SJS_ERR_PROGRAM;
}

sjs_result sjs_vm_fail(sjs_vm *vm, sjs_result r) {
    if (vm->status == VM_FAULT) return SJS_ERR_PROGRAM;
    if (r == SJS_ERR_NOMEM) {
        sjs_error_set(&vm->error, fault_position(vm), (sjs_problem){ SJS_ERROR_MEMORY, "out of memory" });
    } else if (r == SJS_ERR_LIMIT) {
        sjs_error_set(&vm->error, fault_position(vm), (sjs_problem){ SJS_ERROR_LIMIT, "a value is larger than the limits allow" });
    } else {
        sjs_error_set(&vm->error, fault_position(vm), (sjs_problem){ SJS_ERROR_FAULT, "the evaluation failed" });
    }
    vm->status = VM_FAULT;
    return SJS_ERR_PROGRAM;
}

void sjs_vm_release(sjs_vm *vm, sjs_value v) {
    if (v.internal == SJS_INTERNAL_METHOD) return;
    v.internal = SJS_INTERNAL_NONE;
    sjs_release(vm->context, v);
}

bool sjs_vm_push(sjs_vm *vm, sjs_value v) {
    if (vm->sp >= vm->stack_capacity) {
        sjs_vm_release(vm, v);
        if (vm->status != VM_FAULT) {
            sjs_error_set(&vm->error, fault_position(vm), (sjs_problem){ SJS_ERROR_LIMIT, "the evaluation needs more than max_stack values" });
            vm->status = VM_FAULT;
        }
        return false;
    }
    vm->stack[vm->sp++] = v;
    return true;
}

sjs_value sjs_vm_pop(sjs_vm *vm) {
    return vm->stack[--vm->sp];
}

static sjs_value *stack_top(sjs_vm *vm) {
    return &vm->stack[vm->sp - 1];
}

/* Release everything above `height`. */
static void cut(sjs_vm *vm, uint32_t height) {
    while (vm->sp > height) sjs_vm_release(vm, sjs_vm_pop(vm));
}

/* ------------------------------------------------------------------------
 * Properties
 */

static bool is_mutating_method(sjs_str name) {
    static const char *const names[] = { "push", "pop", "shift", "unshift", "splice", "sort", "reverse", "fill",
                                         "copyWithin", "add", "delete", "clear", "set" };
    for (uint32_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (sjs_str_equal(name, sjs_cstr(names[i]))) return true;
    }
    return false;
}

/* A character of a string, by UTF-16 index. */
typedef struct {
    const sjs_string *string;
    double index;
} char_ref;

/* The one-unit string at a UTF-16 index, or undefined past the end. */
static sjs_result string_at(sjs_vm *vm, char_ref ref, sjs_value *out) {
    const sjs_string *s = ref.string;
    double index = ref.index;
    if (!(index >= 0 && index < (double)s->utf16_length) || index != sjs_number_trunc(index)) {
        *out = sjs_undefined();
        return SJS_OK;
    }
    uint32_t want = (uint32_t)index;
    sjs_str text = sjs_string_view(s);
    uint32_t units = 0;
    uint32_t i = 0;
    while (i < text.length) {
        uint32_t start = i;
        int32_t point = sjs_utf8_next(text, &i);
        uint32_t width = point >= 0x10000 ? 2u : 1u;
        if (want < units + width) {
            if (width == 2) return sjs_vm_fault(vm, "this index is half of a character outside the BMP");
            sjs_string *c;
            sjs_result r = sjs_string_make(vm->context, (sjs_str){ text.ptr + start, i - start }, &c);
            if (r != SJS_OK) return sjs_vm_fail(vm, r);
            *out = sjs_heap_value(SJS_STRING, &c->h);
            return SJS_OK;
        }
        units += width;
    }
    *out = sjs_undefined();
    return SJS_OK;
}

sjs_result sjs_property_get(sjs_vm *vm, sjs_member member, sjs_value *out) {
    sjs_value object = member.object;
    const sjs_string *key = member.key;
    sjs_str name = sjs_string_view(key);
    bool index = (key->h.flags & SJS_STRING_INDEX) != 0;
    *out = sjs_undefined();
    switch (object.kind) {
    case SJS_UNDEFINED:
        return fault_named(vm, "cannot read a property of undefined", name);
    case SJS_NULL:
        return fault_named(vm, "cannot read a property of null", name);
    case SJS_OBJECT: {
        const sjs_entry *e = sjs_object_find(SJS_AS_OBJECT(object), name);
        if (e != NULL) {
            *out = e->value;
            sjs_retain(*out);
            return SJS_OK;
        }
        if (sjs_is_prototype_name(name)) return fault_named(vm, "staticjs objects have no prototype", name);
        return SJS_OK;
    }
    case SJS_ARRAY: {
        const sjs_array *a = SJS_AS_ARRAY(object);
        if (index) {
            if (key->index < a->length) {
                *out = a->items[key->index];
                sjs_retain(*out);
            }
            return SJS_OK;
        }
        if (sjs_str_equal(name, SJS_STR("length"))) {
            *out = sjs_number((double)a->length);
            return SJS_OK;
        }
        break;
    }
    case SJS_STRING: {
        const sjs_string *s = SJS_AS_STRING(object);
        if (index) return string_at(vm, (char_ref){ s, (double)key->index }, out);
        if (sjs_str_equal(name, SJS_STR("length"))) {
            *out = sjs_number((double)s->utf16_length);
            return SJS_OK;
        }
        break;
    }
    case SJS_SET:
        if (sjs_str_equal(name, SJS_STR("size"))) {
            *out = sjs_number((double)SJS_AS_ARRAY(object)->length);
            return SJS_OK;
        }
        break;
    default:
        return fault_named(vm, "staticjs reads properties of objects, arrays, strings and sets", name);
    }
    if (sjs_method_lookup(object.kind, name) != SJS_BUILTIN_NONE || sjs_is_prototype_name(name) || is_mutating_method(name)) {
        return fault_named(vm, "staticjs calls methods; it does not read them as values", name);
    }
    return SJS_OK;
}

/* The callee for `receiver.name(...)`. */
static sjs_result method(sjs_vm *vm, sjs_member member, sjs_value *out) {
    sjs_value receiver = member.object;
    const sjs_string *key = member.key;
    sjs_str name = sjs_string_view(key);
    switch (receiver.kind) {
    case SJS_ARRAY:
    case SJS_STRING:
    case SJS_SET: {
        sjs_builtin_id id = sjs_method_lookup(receiver.kind, name);
        if (id == SJS_BUILTIN_NONE) {
            if (is_mutating_method(name)) return fault_named(vm, "staticjs values do not change, so this method is not there", name);
            return fault_named(vm, "staticjs does not have this method", name);
        }
        sjs_value m = { 0 };
        m.kind = SJS_FUNCTION;
        m.internal = SJS_INTERNAL_METHOD;
        m.boolean = (uint32_t)id;
        *out = m;
        return SJS_OK;
    }
    case SJS_OBJECT:
    case SJS_UNDEFINED:
    case SJS_NULL:
        return sjs_property_get(vm, member, out);
    default:
        return fault_named(vm, "staticjs has methods on arrays, strings and sets only", name);
    }
}

/* ToPropertyKey of a value on the stack. */
static sjs_result key_of(sjs_vm *vm, sjs_value v, sjs_string **out) {
    sjs_result r = sjs_to_key(vm->context, v, out);
    if (r == SJS_ERR_PROGRAM) return sjs_vm_fault(vm, "a key must be a primitive");
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * Operators
 */

/* ToString of each part, joined. */
static sjs_result concat(sjs_vm *vm, sjs_values parts, sjs_value *out) {
    uint32_t count = parts.count;
    const sjs_allocator *alloc = &vm->context->counting;
    sjs_buf text = { 0 };
    sjs_result r = SJS_OK;
    for (uint32_t i = 0; r == SJS_OK && i < count; i++) {
        sjs_string *s;
        r = sjs_to_string(vm->context, parts.ptr[i], &s);
        if (r != SJS_OK) break;
        if ((uint64_t)text.length + s->length > vm->context->limits.max_string_bytes) r = SJS_ERR_LIMIT;
        else r = sjs_buf_append(alloc, &text, (sjs_bytes){ (const uint8_t *)s->bytes, s->length });
        sjs_release(vm->context, sjs_heap_value(SJS_STRING, &s->h));
    }
    sjs_string *result = NULL;
    if (r == SJS_OK) r = sjs_string_make(vm->context, (sjs_str){ (const char *)text.ptr, text.length }, &result);
    sjs_buf_free(alloc, &text);
    if (r == SJS_ERR_PROGRAM) return sjs_vm_fault(vm, "staticjs turns only primitives into strings");
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    vm->fuel_used += result->length / 64u;
    *out = sjs_heap_value(SJS_STRING, &result->h);
    return SJS_OK;
}

/* `pair` holds the left and the right operand. */
static sjs_result add(sjs_vm *vm, const sjs_value *pair, sjs_value *out) {
    sjs_value a = pair[0];
    sjs_value b = pair[1];
    if (a.kind == SJS_NUMBER && b.kind == SJS_NUMBER) {
        *out = sjs_number(a.as.number + b.as.number);
        return SJS_OK;
    }
    if ((a.kind == SJS_STRING || b.kind == SJS_STRING) && sjs_is_primitive(a) && sjs_is_primitive(b)) {
        return concat(vm, (sjs_values){ pair, 2 }, out);
    }
    return sjs_vm_fault(vm, "staticjs adds two numbers, or a string and a primitive");
}

static sjs_result in(sjs_vm *vm, const sjs_value *pair, sjs_value *out) {
    sjs_value key = pair[0];
    sjs_value object = pair[1];
    if (object.kind != SJS_OBJECT && object.kind != SJS_ARRAY) return sjs_vm_fault(vm, "in needs an object or an array");
    sjs_string *k;
    if (key_of(vm, key, &k) != SJS_OK) return SJS_ERR_PROGRAM;
    sjs_str name = sjs_string_view(k);
    sjs_result r = SJS_OK;
    if (object.kind == SJS_OBJECT && sjs_object_find(SJS_AS_OBJECT(object), name) != NULL) {
        *out = sjs_boolean(true);
    } else if (object.kind == SJS_ARRAY && (k->h.flags & SJS_STRING_INDEX) != 0) {
        *out = sjs_boolean(k->index < SJS_AS_ARRAY(object)->length);
    } else if (object.kind == SJS_ARRAY && sjs_str_equal(name, SJS_STR("length"))) {
        *out = sjs_boolean(true);
    } else if (sjs_is_prototype_name(name) || (object.kind == SJS_ARRAY && sjs_method_lookup(SJS_ARRAY, name) != SJS_BUILTIN_NONE)) {
        r = fault_named(vm, "staticjs objects have no prototype", name);
    } else {
        *out = sjs_boolean(false);
    }
    sjs_release(vm->context, sjs_heap_value(SJS_STRING, &k->h));
    return r;
}

/* The operator applied to the two values on top of the stack. */
static sjs_result apply_binary(sjs_vm *vm, uint8_t operator, sjs_value *out) {
    const sjs_value *pair = &vm->stack[vm->sp - 2];
    sjs_value a = pair[0];
    sjs_value b = pair[1];
    switch (operator) {
    case SJS_OP_ADD:
        return add(vm, pair, out);
    case SJS_OP_STRICT_EQUAL:
    case SJS_OP_STRICT_NOT_EQUAL: {
        bool equal;
        if (sjs_is_primitive(a) && sjs_is_primitive(b)) {
            equal = sjs_strict_equal_primitive(a, b);
        } else if (sjs_is_primitive(a) || sjs_is_primitive(b)) {
            equal = false;
        } else {
            return sjs_vm_fault(vm, "=== on two objects compares identity, which staticjs does not have");
        }
        *out = sjs_boolean(operator == SJS_OP_STRICT_EQUAL ? equal : !equal);
        return SJS_OK;
    }
    case SJS_OP_IN:
        return in(vm, pair, out);
    case SJS_OP_LESS:
    case SJS_OP_LESS_EQUAL:
    case SJS_OP_GREATER:
    case SJS_OP_GREATER_EQUAL: {
        bool result;
        if (a.kind == SJS_NUMBER && b.kind == SJS_NUMBER) {
            double x = a.as.number;
            double y = b.as.number;
            result = operator == SJS_OP_LESS ? x < y : operator == SJS_OP_LESS_EQUAL ? x <= y
                   : operator == SJS_OP_GREATER ? x > y : x >= y;
        } else if (a.kind == SJS_STRING && b.kind == SJS_STRING) {
            int32_t c = sjs_utf16_compare(sjs_string_view(SJS_AS_STRING(a)), sjs_string_view(SJS_AS_STRING(b)));
            result = operator == SJS_OP_LESS ? c < 0 : operator == SJS_OP_LESS_EQUAL ? c <= 0
                   : operator == SJS_OP_GREATER ? c > 0 : c >= 0;
        } else {
            return sjs_vm_fault(vm, "staticjs compares two numbers or two strings");
        }
        *out = sjs_boolean(result);
        return SJS_OK;
    }
    default: {
        if (a.kind != SJS_NUMBER || b.kind != SJS_NUMBER) return sjs_vm_fault(vm, "arithmetic in staticjs is on numbers");
        double x = a.as.number;
        double y = b.as.number;
        double z;
        switch (operator) {
        case SJS_OP_SUBTRACT:
            z = x - y;
            break;
        case SJS_OP_MULTIPLY:
            z = x * y;
            break;
        case SJS_OP_DIVIDE:
            z = x / y;
            break;
        default:
            z = sjs_number_remainder(x, y);
            break;
        }
        *out = sjs_number(z);
        return SJS_OK;
    }
    }
}

/* The operator applied to the value on top of the stack. */
static sjs_result apply_unary(sjs_vm *vm, uint8_t operator, sjs_value *out) {
    sjs_value v = vm->stack[vm->sp - 1];
    switch (operator) {
    case SJS_OP_NOT:
        *out = sjs_boolean(!sjs_truthy(v));
        return SJS_OK;
    case SJS_OP_TYPEOF: {
        sjs_string *s;
        sjs_result r = sjs_string_make(vm->context, sjs_typeof(v), &s);
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        *out = sjs_heap_value(SJS_STRING, &s->h);
        return SJS_OK;
    }
    default:
        if (v.kind != SJS_NUMBER) return sjs_vm_fault(vm, "arithmetic in staticjs is on numbers");
        *out = sjs_number(operator == SJS_OP_NEGATE ? -v.as.number : v.as.number);
        return SJS_OK;
    }
}

/* ------------------------------------------------------------------------
 * Building arrays, objects and sets
 */

static sjs_result array_append(sjs_vm *vm, sjs_value *array, sjs_value element) {
    sjs_array *a = SJS_AS_ARRAY(*array);
    sjs_result r = sjs_array_grow(vm->context, &a, 1);
    if (r != SJS_OK) {
        sjs_vm_release(vm, element);
        return sjs_vm_fail(vm, r);
    }
    a->items[a->length++] = element;
    array->as.heap = &a->h;
    return SJS_OK;
}

/* Each element a spread yields: arrays, sets, and strings by code point. */
static sjs_result spread_into(sjs_vm *vm, sjs_value *array, sjs_value source) {
    if (source.kind == SJS_ARRAY || source.kind == SJS_SET) {
        const sjs_array *s = SJS_AS_ARRAY(source);
        for (uint32_t i = 0; i < s->length; i++) {
            sjs_retain(s->items[i]);
            if (array_append(vm, array, s->items[i]) != SJS_OK) return SJS_ERR_PROGRAM;
        }
        vm->fuel_used += s->length / 8u;
        return SJS_OK;
    }
    if (source.kind == SJS_STRING) {
        sjs_str text = sjs_string_view(SJS_AS_STRING(source));
        uint32_t i = 0;
        while (i < text.length) {
            uint32_t start = i;
            sjs_utf8_next(text, &i);
            sjs_string *c;
            sjs_result r = sjs_string_make(vm->context, (sjs_str){ text.ptr + start, i - start }, &c);
            if (r != SJS_OK) return sjs_vm_fail(vm, r);
            if (array_append(vm, array, sjs_heap_value(SJS_STRING, &c->h)) != SJS_OK) return SJS_ERR_PROGRAM;
        }
        return SJS_OK;
    }
    return sjs_vm_fault(vm, "... spreads arrays, sets and strings");
}

/* Takes over the property's key and value. */
static sjs_result object_put(sjs_vm *vm, sjs_value *object, sjs_put property) {
    sjs_string *key = property.key;
    sjs_value value = property.value;
    if (sjs_str_equal(sjs_string_view(key), SJS_STR("__proto__"))) {
        sjs_release(vm->context, sjs_heap_value(SJS_STRING, &key->h));
        sjs_vm_release(vm, value);
        return sjs_vm_fault(vm, "staticjs objects have no prototype");
    }
    sjs_object *o = SJS_AS_OBJECT(*object);
    sjs_result r = sjs_object_put_key(vm->context, &o, (sjs_put){ key, value });
    object->as.heap = &o->h;
    if (r != SJS_OK) {
        sjs_release(vm->context, sjs_heap_value(SJS_STRING, &key->h));
        sjs_vm_release(vm, value);
        return sjs_vm_fail(vm, r);
    }
    return SJS_OK;
}

static sjs_result object_spread(sjs_vm *vm, sjs_value *object, sjs_value source) {
    if (source.kind == SJS_OBJECT) {
        const sjs_object *s = SJS_AS_OBJECT(source);
        for (uint32_t i = 0; i < s->count; i++) {
            sjs_retain(sjs_heap_value(SJS_STRING, &s->entries[i].key->h));
            sjs_retain(s->entries[i].value);
            if (object_put(vm, object, (sjs_put){ s->entries[i].key, s->entries[i].value }) != SJS_OK) return SJS_ERR_PROGRAM;
        }
        vm->fuel_used += s->count / 8u;
        return SJS_OK;
    }
    if (source.kind == SJS_ARRAY) {
        const sjs_array *a = SJS_AS_ARRAY(source);
        for (uint32_t i = 0; i < a->length; i++) {
            sjs_string *k;
            sjs_result r = sjs_string_from_number(vm->context, (double)i, &k);
            if (r != SJS_OK) return sjs_vm_fail(vm, r);
            sjs_retain(a->items[i]);
            if (object_put(vm, object, (sjs_put){ k, a->items[i] }) != SJS_OK) return SJS_ERR_PROGRAM;
        }
        return SJS_OK;
    }
    if (source.kind == SJS_STRING) return sjs_vm_fault(vm, "staticjs does not spread a string into an object");
    /* undefined, null, numbers, booleans, sets and functions have no own
     * enumerable properties. */
    return SJS_OK;
}

static sjs_result new_set(sjs_vm *vm, sjs_values args, sjs_value *out) {
    sjs_value source = args.count > 0 ? args.ptr[0] : sjs_undefined();
    sjs_value elements = sjs_undefined();
    if (source.kind != SJS_UNDEFINED && source.kind != SJS_NULL) {
        sjs_result r = sjs_array_new(vm->context, 4, &elements);
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        if (spread_into(vm, &elements, source) != SJS_OK) {
            sjs_release(vm->context, elements);
            return SJS_ERR_PROGRAM;
        }
    }
    uint32_t n = sjs_length(elements);
    sjs_value set = sjs_undefined();
    sjs_result r = sjs_set_new(vm->context, n, &set);
    for (uint32_t i = 0; i < n && r == SJS_OK; i++) {
        sjs_value e = sjs_array_at(elements, i);
        sjs_array *a = SJS_AS_ARRAY(set);
        sjs_retain(e);
        r = sjs_set_insert(vm->context, &a, e);
        set.as.heap = &a->h;
        if (r != SJS_OK) sjs_release(vm->context, e);
    }
    sjs_release(vm->context, elements);
    vm->fuel_used += n;
    if (r != SJS_OK) {
        if (set.kind == SJS_SET) sjs_release(vm->context, set);
        if (r == SJS_ERR_PROGRAM) return sjs_vm_fault(vm, "staticjs sets hold primitives only");
        return sjs_vm_fail(vm, r);
    }
    *out = set;
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * Calls
 */

/* Splice spread arguments into the `*argc` values on top of the stack. */
static bool expand_spread(sjs_vm *vm, uint32_t *argc) {
    uint32_t start = vm->sp - *argc;
    uint64_t total = 0;
    for (uint32_t i = start; i < vm->sp; i++) {
        sjs_value v = vm->stack[i];
        if (v.internal != SJS_INTERNAL_SPREAD) {
            total++;
            continue;
        }
        v.internal = SJS_INTERNAL_NONE;
        if (v.kind == SJS_ARRAY || v.kind == SJS_SET) total += SJS_AS_ARRAY(v)->length;
        else if (v.kind == SJS_STRING) total += SJS_AS_STRING(v)->length;
        else {
            sjs_vm_fault(vm, "... spreads arrays, sets and strings");
            return false;
        }
    }
    if (vm->sp + total > vm->stack_capacity) {
        sjs_vm_fail(vm, SJS_ERR_LIMIT);
        return false;
    }
    /* Build the arguments above the stack, then move them down. */
    sjs_value list;
    sjs_result r = sjs_array_new(vm->context, (uint32_t)total, &list);
    if (r != SJS_OK) {
        sjs_vm_fail(vm, r);
        return false;
    }
    for (uint32_t i = start; i < vm->sp; i++) {
        sjs_value v = vm->stack[i];
        if (v.internal == SJS_INTERNAL_SPREAD) {
            v.internal = SJS_INTERNAL_NONE;
            if (spread_into(vm, &list, v) != SJS_OK) {
                sjs_release(vm->context, list);
                return false;
            }
        } else {
            sjs_retain(v);
            if (array_append(vm, &list, v) != SJS_OK) {
                sjs_release(vm->context, list);
                return false;
            }
        }
    }
    cut(vm, start);
    const sjs_array *a = SJS_AS_ARRAY(list);
    for (uint32_t i = 0; i < a->length; i++) {
        sjs_retain(a->items[i]);
        vm->stack[vm->sp++] = a->items[i];
    }
    *argc = a->length;
    sjs_release(vm->context, list);
    return true;
}

/* A call on the stack: the callee's slot, its arguments, and the height the
 * call leaves the stack at when it returns. */
typedef struct {
    uint32_t callee;
    uint32_t argc;
    uint32_t bottom;
} call_site;

/* A rest parameter: the arguments from site.bottom (the parameters before
 * it) on become one array in its slot, the last parameter's. */
static bool gather_rest(sjs_vm *vm, call_site site) {
    uint32_t base = site.callee + 1u;
    uint32_t fixed = site.bottom;
    uint32_t n = site.argc > fixed ? site.argc - fixed : 0;
    sjs_array *rest;
    sjs_result r = sjs_array_make(vm->context, n, &rest);
    if (r != SJS_OK) {
        sjs_vm_fail(vm, r);
        return false;
    }
    for (uint32_t i = 0; i < n; i++) rest->items[i] = vm->stack[base + fixed + i];
    rest->length = n;
    vm->sp -= n;
    vm->fuel_used += n / 8u;
    for (uint32_t i = site.argc; i < fixed; i++) {
        if (!sjs_vm_push(vm, sjs_undefined())) {
            sjs_release(vm->context, sjs_heap_value(SJS_ARRAY, &rest->h));
            return false;
        }
    }
    return sjs_vm_push(vm, sjs_heap_value(SJS_ARRAY, &rest->h));
}

static bool enter_closure(sjs_vm *vm, call_site site) {
    uint32_t callee = site.callee;
    uint32_t argc = site.argc;
    uint32_t bottom = site.bottom;
    sjs_closure *closure = SJS_AS_CLOSURE(vm->stack[callee]);
    const sjs_function_info *fn = &closure->program->functions[closure->function];
    for (uint32_t i = 0; i < vm->frame_count; i++) {
        const vm_frame *f = &vm->frames[i];
        if (f->kind == FRAME_CODE && f->program == closure->program && f->function == closure->function) {
            sjs_vm_fault(vm, "this function is already running: staticjs functions cannot call themselves");
            return false;
        }
    }
    if (vm->frame_count >= vm->frame_capacity) {
        sjs_error_set(&vm->error, fault_position(vm), (sjs_problem){ SJS_ERROR_LIMIT, "the evaluation needs more than max_frames frames" });
        vm->status = VM_FAULT;
        return false;
    }
    uint32_t base = callee + 1;
    if ((fn->flags & SJS_FUNCTION_REST) != 0) {
        if (!gather_rest(vm, (call_site){ callee, argc, fn->params - 1u })) return false;
        argc = fn->params;
    }
    if (argc > fn->params) cut(vm, base + fn->params);
    for (uint32_t i = argc; i < fn->params; i++) {
        if (!sjs_vm_push(vm, sjs_undefined())) return false;
    }
    for (uint32_t i = fn->params; i < fn->locals; i++) {
        if (!sjs_vm_push(vm, sjs_undefined())) return false;
    }
    vm_frame *f = &vm->frames[vm->frame_count++];
    sjs_zero(f, sizeof *f);
    f->kind = FRAME_CODE;
    f->pc = fn->code;
    f->base = base;
    f->bottom = bottom;
    f->function = closure->function;
    f->program = closure->program;
    f->closure = closure;
    return true;
}

/* A built-in that calls back; its receiver is at site.bottom. */
static bool enter_loop(sjs_vm *vm, sjs_builtin_id id, call_site site) {
    uint32_t receiver = site.bottom;
    uint32_t argc = site.argc;
    if (vm->frame_count >= vm->frame_capacity) {
        sjs_error_set(&vm->error, fault_position(vm), (sjs_problem){ SJS_ERROR_LIMIT, "the evaluation needs more than max_frames frames" });
        vm->status = VM_FAULT;
        return false;
    }
    sjs_position at = fault_position(vm);
    uint32_t first = receiver + LOOP_CALLBACK;
    if (argc > 2) cut(vm, first + 2);
    for (uint32_t i = argc; i < 2; i++) {
        if (!sjs_vm_push(vm, sjs_undefined())) return false;
    }
    if (!sjs_vm_push(vm, sjs_undefined())) return false;
    vm_frame *f = &vm->frames[vm->frame_count++];
    sjs_zero(f, sizeof *f);
    f->kind = FRAME_LOOP;
    f->builtin = (uint16_t)id;
    f->base = receiver;
    f->bottom = receiver;
    f->argc = argc;
    f->at = at;
    return sjs_loop_start(vm, f) == SJS_OK;
}

/* Call the callee below the `argc` arguments on top of the stack. */
static bool call(sjs_vm *vm, uint32_t argc, uint8_t flags) {
    if ((flags & SJS_CALL_SPREAD) != 0 && !expand_spread(vm, &argc)) return false;
    uint32_t callee = vm->sp - argc - 1;
    uint32_t bottom = (flags & SJS_CALL_METHOD) != 0 ? callee - 1 : callee;
    sjs_value f = vm->stack[callee];
    if (f.kind != SJS_FUNCTION) {
        sjs_vm_fault(vm, "this is not a function");
        return false;
    }
    sjs_leaf_call leaf = { 0 };
    leaf.args = &vm->stack[callee + 1];
    leaf.count = argc;
    if (f.internal == SJS_INTERNAL_METHOD) {
        sjs_builtin_id id = (sjs_builtin_id)f.boolean;
        if (sjs_builtin_loops(id)) return enter_loop(vm, id, (call_site){ callee, argc, bottom });
        leaf.receiver = vm->stack[callee - 1];
        if (sjs_builtin_call(vm, id, &leaf) != SJS_OK) {
            sjs_vm_release(vm, leaf.result);
            return false;
        }
    } else if (f.as.heap->type == SJS_HEAP_CLOSURE) {
        return enter_closure(vm, (call_site){ callee, argc, bottom });
    } else if (f.as.heap->type == SJS_HEAP_BUILTIN) {
        sjs_builtin_id id = (sjs_builtin_id)((const sjs_builtin_function *)(const void *)f.as.heap)->id;
        if (sjs_builtin_call(vm, id, &leaf) != SJS_OK) {
            sjs_vm_release(vm, leaf.result);
            return false;
        }
    } else {
        const sjs_native_function *native = (const sjs_native_function *)(const void *)f.as.heap;
        sjs_native_call nc = { vm->context, leaf.args, argc, sjs_undefined(), { "", 0 } };
        sjs_result r = native->native.v->call(native->native.self, &nc);
        if (r == SJS_ERR_PROGRAM) {
            fault_named(vm, "a native function failed", nc.fault);
            return false;
        }
        if (r != SJS_OK) {
            sjs_vm_fail(vm, r);
            return false;
        }
        if (nc.result.internal != 0) {
            sjs_vm_fault(vm, "a native function returned a value it may not");
            return false;
        }
        leaf.result = nc.result;
    }
    vm->fuel_used += leaf.cost;
    cut(vm, bottom);
    return sjs_vm_push(vm, leaf.result);
}

static void finish_loop(sjs_vm *vm, vm_frame *f) {
    sjs_value result = vm->stack[f->base + LOOP_OUTPUT];
    vm->stack[f->base + LOOP_OUTPUT] = sjs_undefined();
    sjs_loop_free(vm, f);
    cut(vm, f->bottom);
    vm->frame_count--;
    sjs_vm_push(vm, result);
}

/* ------------------------------------------------------------------------
 * Patterns
 */

/* A string as an array pattern sees it: by code point. The one at `index`,
 * or undefined past the end. */
static sjs_result string_point(sjs_vm *vm, char_ref ref, sjs_value *out) {
    uint32_t index = (uint32_t)ref.index;
    sjs_str text = sjs_string_view(ref.string);
    uint32_t i = 0;
    for (uint32_t n = 0; n < index && i < text.length; n++) sjs_utf8_next(text, &i);
    vm->fuel_used += index / 8u;
    *out = sjs_undefined();
    if (i >= text.length) return SJS_OK;
    uint32_t start = i;
    sjs_utf8_next(text, &i);
    sjs_string *c;
    sjs_result r = sjs_string_make(vm->context, (sjs_str){ text.ptr + start, i - start }, &c);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    *out = sjs_heap_value(SJS_STRING, &c->h);
    return SJS_OK;
}

/* `[a, ...rest]`: the elements of the source on top of the stack from
 * `start` on, in a new array. */
static sjs_result rest_elements(sjs_vm *vm, uint32_t start, sjs_value *out) {
    sjs_value source = *stack_top(vm);
    if (source.kind == SJS_STRING) {
        sjs_str text = sjs_string_view(SJS_AS_STRING(source));
        uint32_t i = 0;
        for (uint32_t n = 0; n < start && i < text.length; n++) sjs_utf8_next(text, &i);
        vm->fuel_used += start / 8u;
        sjs_value rest;
        sjs_result r = sjs_array_new(vm->context, 0, &rest);
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        while (i < text.length) {
            uint32_t from = i;
            sjs_utf8_next(text, &i);
            sjs_string *c;
            r = sjs_string_make(vm->context, (sjs_str){ text.ptr + from, i - from }, &c);
            if (r != SJS_OK) {
                sjs_release(vm->context, rest);
                return sjs_vm_fail(vm, r);
            }
            if (array_append(vm, &rest, sjs_heap_value(SJS_STRING, &c->h)) != SJS_OK) {
                sjs_release(vm->context, rest);
                return SJS_ERR_PROGRAM;
            }
        }
        *out = rest;
        return SJS_OK;
    }
    const sjs_array *a = SJS_AS_ARRAY(source);
    uint32_t n = a->length > start ? a->length - start : 0;
    sjs_array *rest;
    sjs_result r = sjs_array_make(vm->context, n, &rest);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    for (uint32_t i = 0; i < n; i++) {
        sjs_retain(a->items[start + i]);
        rest->items[i] = a->items[start + i];
    }
    rest->length = n;
    vm->fuel_used += n / 8u;
    *out = sjs_heap_value(SJS_ARRAY, &rest->h);
    return SJS_OK;
}

/* An object rest element's source, and the keys it leaves out: the
 * pattern's static keys, an array constant, and its computed ones, strings
 * on the stack. */
typedef struct {
    sjs_value source;
    const sjs_array *named;  /* NULL when there are none */
    const sjs_value *computed;
    uint32_t computed_count;
} excluded_keys;

static bool excludes(const excluded_keys *x, const sjs_string *key) {
    sjs_str k = sjs_string_view(key);
    uint32_t named = x->named != NULL ? x->named->length : 0;
    for (uint32_t i = 0; i < named; i++) {
        if (sjs_str_equal(sjs_string_view(SJS_AS_STRING(x->named->items[i])), k)) return true;
    }
    for (uint32_t i = 0; i < x->computed_count; i++) {
        if (sjs_str_equal(sjs_string_view(SJS_AS_STRING(x->computed[i])), k)) return true;
    }
    return false;
}

static bool excludes_index(const excluded_keys *x, uint32_t index) {
    uint32_t named = x->named != NULL ? x->named->length : 0;
    for (uint32_t i = 0; i < named + x->computed_count; i++) {
        const sjs_string *k = SJS_AS_STRING(i < named ? x->named->items[i] : x->computed[i - named]);
        if ((k->h.flags & SJS_STRING_INDEX) != 0 && k->index == index) return true;
    }
    return false;
}

/* `{a, ...rest}`: a new object of the source's own enumerable properties,
 * in their order, but those the pattern named. */
static sjs_result rest_properties(sjs_vm *vm, const excluded_keys *x, sjs_value *out) {
    sjs_value source = x->source;
    uint32_t keys = (x->named != NULL ? x->named->length : 0) + x->computed_count;
    sjs_object *rest;
    sjs_result r;
    if (source.kind == SJS_STRING) return sjs_vm_fault(vm, "staticjs does not spread a string into an object");
    if (source.kind == SJS_OBJECT) {
        const sjs_object *o = SJS_AS_OBJECT(source);
        r = sjs_object_make(vm->context, o->count, &rest);
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        rest->count = 0;
        for (uint32_t i = 0; i < o->count; i++) {
            const sjs_entry *entry = &o->entries[i];
            if (excludes(x, entry->key)) continue;
            sjs_retain(sjs_heap_value(SJS_STRING, &entry->key->h));
            sjs_retain(entry->value);
            rest->entries[rest->count++] = *entry;
        }
        vm->fuel_used += (uint64_t)o->count * (keys + 1u) / 8u;
    } else if (source.kind == SJS_ARRAY) {
        const sjs_array *a = SJS_AS_ARRAY(source);
        r = sjs_object_make(vm->context, a->length, &rest);
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        rest->count = 0;
        for (uint32_t i = 0; i < a->length; i++) {
            if (excludes_index(x, i)) continue;
            sjs_string *k;
            r = sjs_string_from_number(vm->context, (double)i, &k);
            if (r != SJS_OK) {
                sjs_release(vm->context, sjs_heap_value(SJS_OBJECT, &rest->h));
                return sjs_vm_fail(vm, r);
            }
            sjs_retain(a->items[i]);
            rest->entries[rest->count].key = k;
            rest->entries[rest->count++].value = a->items[i];
        }
        vm->fuel_used += (uint64_t)a->length * (keys + 1u) / 8u;
    } else {
        /* undefined and null fault before; numbers, booleans, sets and
         * functions have no own enumerable properties. */
        r = sjs_object_make(vm->context, 0, &rest);
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        rest->count = 0;
    }
    *out = sjs_heap_value(SJS_OBJECT, &rest->h);
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * One instruction
 */

static void jump_to(vm_frame *f, const uint8_t *code, uint32_t pc) {
    f->pc = sjs_read32(code + pc);
}

static void execute(sjs_vm *vm, vm_frame *f) {
    const uint8_t *code = f->program->code;
    uint32_t pc = f->pc;
    vm->op_pc = pc;
    uint8_t opcode = code[pc++];
    sjs_value result;
    switch (opcode) {
    case SJS_OP_UNDEFINED_:
        f->pc = pc;
        sjs_vm_push(vm, sjs_undefined());
        return;
    case SJS_OP_NULL_:
        f->pc = pc;
        sjs_vm_push(vm, sjs_null());
        return;
    case SJS_OP_TRUE_:
    case SJS_OP_FALSE_:
        f->pc = pc;
        sjs_vm_push(vm, sjs_boolean(opcode == SJS_OP_TRUE_));
        return;
    case SJS_OP_NUMBER_:
        f->pc = pc + 4;
        sjs_vm_push(vm, sjs_number(f->program->numbers[sjs_read32(code + pc)]));
        return;
    case SJS_OP_CONSTANT: {
        f->pc = pc + 4;
        sjs_value v = f->program->constants[sjs_read32(code + pc)];
        sjs_retain(v);
        sjs_vm_push(vm, v);
        return;
    }
    case SJS_OP_LOCAL: {
        f->pc = pc + 2;
        sjs_value v = vm->stack[f->base + sjs_read16(code + pc)];
        sjs_retain(v);
        sjs_vm_push(vm, v);
        return;
    }
    case SJS_OP_SET_LOCAL: {
        f->pc = pc + 2;
        sjs_value *slot = &vm->stack[f->base + sjs_read16(code + pc)];
        sjs_value v = sjs_vm_pop(vm);
        sjs_vm_release(vm, *slot);
        *slot = v;
        return;
    }
    case SJS_OP_CAPTURE: {
        f->pc = pc + 2;
        sjs_value v = f->closure->captures[sjs_read16(code + pc)];
        sjs_retain(v);
        sjs_vm_push(vm, v);
        return;
    }
    case SJS_OP_GLOBAL: {
        f->pc = pc + 2;
        sjs_value v = vm->globals[sjs_read16(code + pc)];
        sjs_retain(v);
        sjs_vm_push(vm, v);
        return;
    }
    case SJS_OP_POP:
        f->pc = pc;
        sjs_vm_release(vm, sjs_vm_pop(vm));
        return;
    case SJS_OP_GET_MEMBER: {
        f->pc = pc + 4;
        const sjs_string *key = SJS_AS_STRING(f->program->constants[sjs_read32(code + pc)]);
        if (sjs_property_get(vm, (sjs_member){ *stack_top(vm), key }, &result) != SJS_OK) return;
        sjs_vm_release(vm, *stack_top(vm));
        *stack_top(vm) = result;
        return;
    }
    case SJS_OP_GET_INDEX: {
        f->pc = pc;
        sjs_value key = sjs_vm_pop(vm);
        sjs_value object = *stack_top(vm);
        sjs_result r;
        if (object.kind == SJS_ARRAY && key.kind == SJS_NUMBER) {
            const sjs_array *a = SJS_AS_ARRAY(object);
            double d = key.as.number;
            result = sjs_undefined();
            if (d >= 0 && d < (double)a->length && d == sjs_number_trunc(d)) {
                result = a->items[(uint32_t)d];
                sjs_retain(result);
            }
            r = SJS_OK;
        } else if (object.kind == SJS_STRING && key.kind == SJS_NUMBER) {
            r = string_at(vm, (char_ref){ SJS_AS_STRING(object), key.as.number }, &result);
        } else {
            sjs_string *k;
            r = key_of(vm, key, &k);
            if (r == SJS_OK) {
                r = sjs_property_get(vm, (sjs_member){ object, k }, &result);
                sjs_release(vm->context, sjs_heap_value(SJS_STRING, &k->h));
            }
        }
        sjs_vm_release(vm, key);
        if (r != SJS_OK) return;
        sjs_vm_release(vm, *stack_top(vm));
        *stack_top(vm) = result;
        return;
    }
    case SJS_OP_METHOD_MEMBER: {
        f->pc = pc + 4;
        const sjs_string *key = SJS_AS_STRING(f->program->constants[sjs_read32(code + pc)]);
        if (method(vm, (sjs_member){ *stack_top(vm), key }, &result) != SJS_OK) return;
        sjs_vm_push(vm, result);
        return;
    }
    case SJS_OP_METHOD_INDEX: {
        f->pc = pc;
        sjs_value key = sjs_vm_pop(vm);
        sjs_string *k;
        sjs_result r = key_of(vm, key, &k);
        sjs_vm_release(vm, key);
        if (r != SJS_OK) return;
        r = method(vm, (sjs_member){ *stack_top(vm), k }, &result);
        sjs_release(vm->context, sjs_heap_value(SJS_STRING, &k->h));
        if (r != SJS_OK) return;
        sjs_vm_push(vm, result);
        return;
    }
    case SJS_OP_CALL: {
        f->pc = pc + 3;
        call(vm, sjs_read16(code + pc), code[pc + 2]);
        return;
    }
    case SJS_OP_NEW_SET: {
        f->pc = pc + 3;
        uint32_t argc = sjs_read16(code + pc);
        if ((code[pc + 2] & SJS_CALL_SPREAD) != 0 && !expand_spread(vm, &argc)) return;
        if (new_set(vm, (sjs_values){ &vm->stack[vm->sp - argc], argc }, &result) != SJS_OK) return;
        cut(vm, vm->sp - argc);
        sjs_vm_push(vm, result);
        return;
    }
    case SJS_OP_SPREAD:
        f->pc = pc;
        stack_top(vm)->internal = SJS_INTERNAL_SPREAD;
        return;
    case SJS_OP_JUMP:
        jump_to(f, code, pc);
        return;
    case SJS_OP_JUMP_FALSY_KEEP:
    case SJS_OP_JUMP_TRUTHY_KEEP:
    case SJS_OP_JUMP_DEFINED_KEEP: {
        sjs_value v = *stack_top(vm);
        bool go = opcode == SJS_OP_JUMP_FALSY_KEEP ? !sjs_truthy(v)
                : opcode == SJS_OP_JUMP_TRUTHY_KEEP ? sjs_truthy(v)
                : (v.kind != SJS_UNDEFINED && v.kind != SJS_NULL);
        if (go) {
            jump_to(f, code, pc);
            return;
        }
        f->pc = pc + 4;
        sjs_vm_release(vm, sjs_vm_pop(vm));
        return;
    }
    case SJS_OP_JUMP_FALSY: {
        sjs_value v = sjs_vm_pop(vm);
        bool go = !sjs_truthy(v);
        sjs_vm_release(vm, v);
        if (go) jump_to(f, code, pc);
        else f->pc = pc + 4;
        return;
    }
    case SJS_OP_JUMP_NULLISH:
        if (stack_top(vm)->kind == SJS_UNDEFINED || stack_top(vm)->kind == SJS_NULL) {
            *stack_top(vm) = sjs_undefined();
            jump_to(f, code, pc);
        } else {
            f->pc = pc + 4;
        }
        return;
    case SJS_OP_UNARY_: {
        f->pc = pc + 1;
        if (apply_unary(vm, code[pc], &result) != SJS_OK) return;
        sjs_vm_release(vm, *stack_top(vm));
        *stack_top(vm) = result;
        return;
    }
    case SJS_OP_BINARY_: {
        f->pc = pc + 1;
        if (apply_binary(vm, code[pc], &result) != SJS_OK) return;
        cut(vm, vm->sp - 2);
        sjs_vm_push(vm, result);
        return;
    }
    case SJS_OP_CONCAT: {
        f->pc = pc + 2;
        uint32_t n = sjs_read16(code + pc);
        if (concat(vm, (sjs_values){ &vm->stack[vm->sp - n], n }, &result) != SJS_OK) return;
        cut(vm, vm->sp - n);
        sjs_vm_push(vm, result);
        return;
    }
    case SJS_OP_ARRAY_NEW: {
        f->pc = pc;
        sjs_result r = sjs_array_new(vm->context, 4, &result);
        if (r != SJS_OK) {
            sjs_vm_fail(vm, r);
            return;
        }
        sjs_vm_push(vm, result);
        return;
    }
    case SJS_OP_ARRAY_PUSH: {
        f->pc = pc;
        sjs_value v = sjs_vm_pop(vm);
        array_append(vm, stack_top(vm), v);
        return;
    }
    case SJS_OP_ARRAY_SPREAD: {
        f->pc = pc;
        sjs_value v = sjs_vm_pop(vm);
        spread_into(vm, stack_top(vm), v);
        sjs_vm_release(vm, v);
        return;
    }
    case SJS_OP_OBJECT_NEW: {
        f->pc = pc;
        sjs_result r = sjs_object_new(vm->context, 4, &result);
        if (r != SJS_OK) {
            sjs_vm_fail(vm, r);
            return;
        }
        sjs_vm_push(vm, result);
        return;
    }
    case SJS_OP_OBJECT_PUT: {
        f->pc = pc;
        sjs_value value = sjs_vm_pop(vm);
        sjs_value key = sjs_vm_pop(vm);
        object_put(vm, stack_top(vm), (sjs_put){ SJS_AS_STRING(key), value });
        return;
    }
    case SJS_OP_OBJECT_SPREAD: {
        f->pc = pc;
        sjs_value v = sjs_vm_pop(vm);
        object_spread(vm, stack_top(vm), v);
        sjs_vm_release(vm, v);
        return;
    }
    case SJS_OP_TO_KEY: {
        f->pc = pc;
        sjs_string *k;
        if (key_of(vm, *stack_top(vm), &k) != SJS_OK) return;
        sjs_vm_release(vm, *stack_top(vm));
        *stack_top(vm) = sjs_heap_value(SJS_STRING, &k->h);
        return;
    }
    case SJS_OP_CLOSURE: {
        f->pc = pc + 4;
        uint32_t index = sjs_read32(code + pc);
        const sjs_function_info *fn = &f->program->functions[index];
        sjs_closure *c;
        sjs_result r = sjs_closure_make(vm->context, fn->capture_count, &c);
        if (r != SJS_OK) {
            sjs_vm_fail(vm, r);
            return;
        }
        c->program = f->program;
        sjs_program_retain(f->program);
        c->function = index;
        for (uint32_t i = 0; i < fn->capture_count; i++) {
            const sjs_capture_info *ci = &f->program->captures[fn->captures + i];
            sjs_value v = ci->from_capture ? f->closure->captures[ci->index] : vm->stack[f->base + ci->index];
            sjs_retain(v);
            c->captures[c->count++] = v;
        }
        sjs_vm_push(vm, sjs_heap_value(SJS_FUNCTION, &c->h));
        return;
    }
    case SJS_OP_RETURN: {
        sjs_value v = sjs_vm_pop(vm);
        cut(vm, f->bottom);
        vm->frame_count--;
        if (vm->frame_count == 0) {
            vm->result = v;
            vm->status = VM_DONE;
            return;
        }
        sjs_vm_push(vm, v);
        return;
    }
    case SJS_OP_PATTERN_ARRAY:
        f->pc = pc;
        if (stack_top(vm)->kind != SJS_ARRAY && stack_top(vm)->kind != SJS_SET && stack_top(vm)->kind != SJS_STRING) {
            sjs_vm_fault(vm, "only an array, a set or a string can be destructured with [ ]");
        }
        return;
    case SJS_OP_PATTERN_OBJECT:
        f->pc = pc;
        if (stack_top(vm)->kind == SJS_UNDEFINED || stack_top(vm)->kind == SJS_NULL) {
            sjs_vm_fault(vm, "undefined and null cannot be destructured");
        }
        return;
    case SJS_OP_PATTERN_INDEX: {
        f->pc = pc + 4;
        uint32_t i = sjs_read32(code + pc);
        if (stack_top(vm)->kind == SJS_STRING) {
            if (string_point(vm, (char_ref){ SJS_AS_STRING(*stack_top(vm)), (double)i }, &result) != SJS_OK) return;
            sjs_vm_push(vm, result);
            return;
        }
        const sjs_array *a = SJS_AS_ARRAY(*stack_top(vm));
        sjs_value v = i < a->length ? a->items[i] : sjs_undefined();
        sjs_retain(v);
        sjs_vm_push(vm, v);
        return;
    }
    case SJS_OP_PATTERN_KEY: {
        f->pc = pc + 4;
        const sjs_string *key = SJS_AS_STRING(f->program->constants[sjs_read32(code + pc)]);
        if (sjs_property_get(vm, (sjs_member){ *stack_top(vm), key }, &result) != SJS_OK) return;
        sjs_vm_push(vm, result);
        return;
    }
    case SJS_OP_PATTERN_COMPUTED: {
        f->pc = pc + 2;
        sjs_value key = sjs_vm_pop(vm);
        sjs_string *k;
        sjs_result r = key_of(vm, key, &k);
        sjs_vm_release(vm, key);
        if (r != SJS_OK) return;
        sjs_value *slot = &vm->stack[f->base + sjs_read16(code + pc)];
        sjs_vm_release(vm, *slot);
        *slot = sjs_heap_value(SJS_STRING, &k->h);
        if (sjs_property_get(vm, (sjs_member){ *stack_top(vm), k }, &result) != SJS_OK) return;
        sjs_vm_push(vm, result);
        return;
    }
    case SJS_OP_PATTERN_REST:
        f->pc = pc + 4;
        if (rest_elements(vm, sjs_read32(code + pc), &result) != SJS_OK) return;
        sjs_vm_push(vm, result);
        return;
    case SJS_OP_PATTERN_REST_OBJECT: {
        f->pc = pc + 6;
        uint32_t named = sjs_read32(code + pc);
        uint32_t n = sjs_read16(code + pc + 4);
        excluded_keys x = { vm->stack[vm->sp - n - 1u], NULL, &vm->stack[vm->sp - n], n };
        if (named != UINT32_MAX) x.named = SJS_AS_ARRAY(f->program->constants[named]);
        if (rest_properties(vm, &x, &result) != SJS_OK) return;
        cut(vm, vm->sp - n);
        sjs_vm_push(vm, result);
        return;
    }
    case SJS_OP_DEFAULT:
        if (stack_top(vm)->kind != SJS_UNDEFINED) {
            jump_to(f, code, pc);
            return;
        }
        f->pc = pc + 4;
        sjs_vm_release(vm, sjs_vm_pop(vm));
        return;
    case SJS_OP_DEFAULT_LOCAL:
        if (vm->stack[f->base + sjs_read16(code + pc)].kind != SJS_UNDEFINED) {
            jump_to(f, code, pc + 2);
            return;
        }
        f->pc = pc + 6;
        return;
    default:
        sjs_vm_fault(vm, "the program is damaged");
        return;
    }
}

/* ------------------------------------------------------------------------
 * The public API
 */

sjs_result sjs_vm_init(sjs_vm **out, const sjs_vm_config *config) {
    if (out == NULL || config == NULL || config->context == NULL) return SJS_ERR_INVALID;
    sjs_context *context = config->context;
    const sjs_allocator *a = &context->counting;
    sjs_vm *vm = sjs_mem_alloc(a, sizeof *vm);
    if (vm == NULL) return SJS_ERR_NOMEM;
    sjs_zero(vm, sizeof *vm);
    vm->context = context;
    vm->stack_capacity = context->limits.max_stack;
    vm->frame_capacity = context->limits.max_frames;
    vm->stack = sjs_mem_alloc(a, (size_t)vm->stack_capacity * sizeof(sjs_value));
    vm->frames = sjs_mem_alloc(a, (size_t)vm->frame_capacity * sizeof(vm_frame));
    if (vm->stack == NULL || vm->frames == NULL) {
        sjs_vm_destroy(vm);
        return SJS_ERR_NOMEM;
    }
    *out = vm;
    return SJS_OK;
}

void sjs_vm_reset(sjs_vm *vm) {
    if (vm == NULL) return;
    for (uint32_t i = 0; i < vm->frame_count; i++) {
        if (vm->frames[i].kind == FRAME_LOOP) sjs_loop_free(vm, &vm->frames[i]);
    }
    vm->frame_count = 0;
    cut(vm, 0);
    sjs_vm_release(vm, vm->result);
    vm->result = sjs_undefined();
    sjs_program_release(vm->program);
    vm->program = NULL;
    vm->globals = NULL;
    vm->global_count = 0;
    vm->status = VM_IDLE;
    vm->error.error.code = SJS_ERROR_NONE;
}

sjs_result sjs_vm_start(sjs_vm *vm, const sjs_run_params *params) {
    if (vm == NULL || params == NULL || params->program == NULL) return SJS_ERR_INVALID;
    sjs_program *program = params->program;
    if (program->context != vm->context || params->global_count != program->global_count ||
        (params->globals == NULL && params->global_count != 0)) {
        return SJS_ERR_INVALID;
    }
    for (uint32_t i = 0; i < params->global_count; i++) {
        if (params->globals[i].internal != 0) return SJS_ERR_INVALID;
    }
    sjs_vm_reset(vm);
    sjs_program_retain(program);
    vm->program = program;
    vm->globals = params->globals;
    vm->global_count = params->global_count;
    vm->fuel_used = 0;
    vm->status = VM_RUNNING;
    const sjs_function_info *fn = &program->functions[0];
    if (fn->locals > vm->stack_capacity) {
        sjs_vm_reset(vm);
        return SJS_ERR_LIMIT;
    }
    for (uint32_t i = 0; i < fn->locals; i++) vm->stack[vm->sp++] = sjs_undefined();
    vm_frame *f = &vm->frames[vm->frame_count++];
    sjs_zero(f, sizeof *f);
    f->kind = FRAME_CODE;
    f->pc = fn->code;
    f->program = program;
    return SJS_OK;
}

sjs_run sjs_vm_run(sjs_vm *vm, uint64_t fuel) {
    if (vm == NULL) return SJS_RUN_FAULT;
    if (vm->status == VM_IDLE) {
        sjs_error_set(&vm->error, (sjs_position){ 0, 1, 1 }, (sjs_problem){ SJS_ERROR_FAULT, "nothing is running: call sjs_vm_start()" });
        return SJS_RUN_FAULT;
    }
    uint64_t limit = fuel > UINT64_MAX - vm->fuel_used ? UINT64_MAX : vm->fuel_used + fuel;
    while (vm->status == VM_RUNNING) {
        if (vm->fuel_used >= limit) return SJS_RUN_PAUSED;
        vm->fuel_used++;
        vm_frame *f = &vm->frames[vm->frame_count - 1];
        if (f->kind == FRAME_CODE) {
            execute(vm, f);
            continue;
        }
        switch (sjs_loop_step(vm, f)) {
        case LOOP_CALL:
            call(vm, f->argc, 0);
            break;
        case LOOP_DONE:
            finish_loop(vm, f);
            break;
        default:
            break;
        }
    }
    return vm->status == VM_DONE ? SJS_RUN_DONE : SJS_RUN_FAULT;
}

sjs_result sjs_vm_take_result(sjs_vm *vm, sjs_value *out) {
    if (vm == NULL || out == NULL || vm->status != VM_DONE) return SJS_ERR_INVALID;
    *out = vm->result;
    vm->result = sjs_undefined();
    return SJS_OK;
}

sjs_result sjs_vm_error(const sjs_vm *vm, sjs_error *out) {
    if (vm == NULL || out == NULL) return SJS_ERR_INVALID;
    if (vm->error.error.code == SJS_ERROR_NONE) return SJS_ERR_NOT_FOUND;
    *out = vm->error.error;
    return SJS_OK;
}

uint64_t sjs_vm_fuel_used(const sjs_vm *vm) {
    return vm == NULL ? 0 : vm->fuel_used;
}

void sjs_vm_destroy(sjs_vm *vm) {
    if (vm == NULL) return;
    const sjs_allocator *a = &vm->context->counting;
    if (vm->stack != NULL && vm->frames != NULL) sjs_vm_reset(vm);
    sjs_mem_free(a, vm->stack, (size_t)vm->stack_capacity * sizeof(sjs_value));
    sjs_mem_free(a, vm->frames, (size_t)vm->frame_capacity * sizeof(vm_frame));
    sjs_mem_free(a, vm, sizeof *vm);
}
