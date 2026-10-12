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

static bool array_extra_get(const sjs_array *a, sjs_str name, sjs_value *out);

sjs_result sjs_array_extra_fault(sjs_vm *vm) {
    return sjs_vm_fault(vm, "this array has properties besides its elements, which this would show or lose");
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
    sjs_unref(vm->context, v);
}

bool sjs_vm_stack_full(sjs_vm *vm, sjs_value v) {
    sjs_vm_release(vm, v);
    if (vm->status != VM_FAULT) {
        sjs_error_set(&vm->error, fault_position(vm), (sjs_problem){ SJS_ERROR_LIMIT, "the evaluation needs more than max_stack values" });
        vm->status = VM_FAULT;
    }
    return false;
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

/* Maps' properties, and the fault for a value that has none: at the end of
 * the file. */
static sjs_result property_other(sjs_vm *vm, sjs_member member, sjs_value *out);

/* Whether JavaScript finds `key` on the prototype of a value of `kind`. */
static bool on_prototype(uint8_t kind, const sjs_string *key) {
    const sjs_name *n = sjs_name_of(key);
    if (n == NULL) return false;
    switch (kind) {
    case SJS_ARRAY:
        return (n->flags & SJS_NAME_ON_ARRAY) != 0;
    case SJS_STRING:
        return (n->flags & SJS_NAME_ON_STRING) != 0;
    case SJS_SET:
        return (n->flags & SJS_NAME_ON_SET) != 0;
    case SJS_MAP:
        return (n->flags & SJS_NAME_ON_MAP) != 0;
    case SJS_ITERATOR:
        return (n->flags & SJS_NAME_ON_ITERATOR) != 0;
    case SJS_REGEXP:
        return (n->flags & SJS_NAME_ON_REGEXP) != 0;
    default:
        return (n->flags & SJS_NAME_OBJECT) != 0;
    }
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
            sjs_ref(*out);
            return SJS_OK;
        }
        if (on_prototype(SJS_OBJECT, key)) return fault_named(vm, "staticjs objects have no prototype", name);
        return SJS_OK;
    }
    case SJS_ARRAY: {
        const sjs_array *a = SJS_AS_ARRAY(object);
        if (index) {
            if (key->index < a->length) {
                *out = a->items[key->index];
                sjs_ref(*out);
            }
            return SJS_OK;
        }
        if (sjs_str_equal(name, SJS_STR("length"))) {
            *out = sjs_num((double)a->length);
            return SJS_OK;
        }
        if ((a->h.flags & SJS_ARRAY_EXTRA) != 0 && array_extra_get(a, name, out)) return SJS_OK;
        break;
    }
    case SJS_STRING: {
        const sjs_string *s = SJS_AS_STRING(object);
        if (index) return string_at(vm, (char_ref){ s, (double)key->index }, out);
        if (sjs_str_equal(name, SJS_STR("length"))) {
            *out = sjs_num((double)s->utf16_length);
            return SJS_OK;
        }
        break;
    }
    case SJS_SET:
        if (sjs_str_equal(name, SJS_STR("size"))) {
            *out = sjs_num((double)SJS_AS_ARRAY(object)->length);
            return SJS_OK;
        }
        break;
    default:
        return property_other(vm, member, out);
    }
    if (on_prototype(object.kind, key)) {
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
    case SJS_SET:
    case SJS_BIGINT:
    case SJS_MAP:
    case SJS_ITERATOR:
    case SJS_REGEXP:
    case SJS_NUMBER: {
        const sjs_name *n = sjs_name_of(key);
        sjs_builtin_id id = sjs_method_of(n, receiver.kind);
        if (id == SJS_BUILTIN_NONE) {
            if (receiver.kind == SJS_NUMBER) {
                return fault_named(vm, "staticjs has methods on arrays, strings, sets, maps, iterators and BigInts only, and on numbers only those that format digits", name);
            }
            /* MUTATION: the message. Restore "staticjs values do not change, so
             * this method is not there". */
            if (n != NULL && (n->flags & SJS_NAME_MUTATING) != 0) {
                return fault_named(vm, "this method changes its receiver: staticjs has some such methods, and calls them only on a variable of the function that owns its value, or on a path from one", name); /* MUTATION: or a path */
            }
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
    case SJS_TEMPORAL:
        return sjs_temporal_callee(vm, member, out);
    default:
        return fault_named(vm, "staticjs has methods on arrays, strings, sets, maps, iterators and BigInts only", name);
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

/* **, BigInts, and the faults of arithmetic: at the end of the file. */
static sjs_result power(sjs_vm *vm, sjs_value *out);
static sjs_result bigint_binary(sjs_vm *vm, uint8_t operator, sjs_value *out);
static sjs_result bigint_unary(sjs_vm *vm, uint8_t operator, sjs_value *out);
static int bigint_order(uint8_t operator, sjs_value a, sjs_value b);
static sjs_result order_fault(sjs_vm *vm, sjs_value a, sjs_value b);

/* STRING_APPEND: at the end of the file. */
static uint32_t appendable_part(sjs_values parts);
static sjs_result string_append(sjs_vm *vm, sjs_values parts, sjs_value *out);
static sjs_result concat_copy(sjs_vm *vm, sjs_values parts, sjs_value *out);

/* ToString of each part, joined. */
static sjs_result concat(sjs_vm *vm, sjs_values parts, sjs_value *out) {
    if (appendable_part(parts) != 0) return string_append(vm, parts, out); /* STRING_APPEND */
    return concat_copy(vm, parts, out);
}

/* The join, into a new string. A BigInt's digits cost fuel to make. */
static sjs_result concat_copy(sjs_vm *vm, sjs_values parts, sjs_value *out) {
    uint32_t count = parts.count;
    const sjs_allocator *alloc = &vm->context->counting;
    sjs_buf text = { 0 };
    sjs_result r = SJS_OK;
    for (uint32_t i = 0; r == SJS_OK && i < count; i++) {
        sjs_string *s;
        if (parts.ptr[i].kind == SJS_BIGINT) vm->fuel_used += sjs_bigint_text_cost(SJS_AS_BIGINT(parts.ptr[i])->length, 10u);
        r = sjs_to_string(vm->context, parts.ptr[i], &s);
        if (r != SJS_OK) break;
        if ((uint64_t)text.length + s->length > vm->context->limits.max_string_bytes) r = SJS_ERR_LIMIT;
        else r = sjs_buf_append(alloc, &text, (sjs_bytes){ (const uint8_t *)s->bytes, s->length });
        sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &s->h));
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
        *out = sjs_num(a.as.number + b.as.number);
        return SJS_OK;
    }
    if ((a.kind == SJS_STRING || b.kind == SJS_STRING) && sjs_is_primitive(a) && sjs_is_primitive(b)) {
        return concat(vm, (sjs_values){ pair, 2 }, out);
    }
    return bigint_binary(vm, SJS_OP_ADD, out);
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
    } else if (sjs_array_extra(object) && sjs_object_find((const sjs_object *)(const void *)SJS_AS_ARRAY(object)->extra, name) != NULL) {
        *out = sjs_boolean(true);
    } else if (on_prototype(object.kind, k)) {
        r = fault_named(vm, "staticjs objects have no prototype", name);
    } else {
        *out = sjs_boolean(false);
    }
    sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &k->h));
    return r;
}

int sjs_vm_strict_equal(sjs_value a, sjs_value b) {
    if (sjs_is_primitive(a) && sjs_is_primitive(b)) return sjs_strict_equal_primitive(a, b);
    if (sjs_is_primitive(a) || sjs_is_primitive(b)) return 0;
    return SJS_VM_UNDECIDED;
}

int sjs_vm_order(uint8_t operator, sjs_value a, sjs_value b) {
    if (a.kind == SJS_NUMBER && b.kind == SJS_NUMBER) {
        double x = a.as.number;
        double y = b.as.number;
        return operator == SJS_OP_LESS ? x < y : operator == SJS_OP_LESS_EQUAL ? x <= y
             : operator == SJS_OP_GREATER ? x > y : x >= y;
    }
    if (a.kind == SJS_STRING && b.kind == SJS_STRING) {
        int32_t c = sjs_utf16_compare(sjs_string_view(SJS_AS_STRING(a)), sjs_string_view(SJS_AS_STRING(b)));
        return operator == SJS_OP_LESS ? c < 0 : operator == SJS_OP_LESS_EQUAL ? c <= 0
             : operator == SJS_OP_GREATER ? c > 0 : c >= 0;
    }
    return bigint_order(operator, a, b);
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
        int equal = sjs_vm_strict_equal(a, b);
        if (equal == SJS_VM_UNDECIDED) {
            return sjs_vm_fault(vm, "=== on two objects compares identity, which staticjs does not have");
        }
        *out = sjs_boolean(operator == SJS_OP_STRICT_EQUAL ? equal != 0 : equal == 0);
        return SJS_OK;
    }
    case SJS_OP_IN:
        return in(vm, pair, out);
    case SJS_OP_LESS:
    case SJS_OP_LESS_EQUAL:
    case SJS_OP_GREATER:
    case SJS_OP_GREATER_EQUAL: {
        int result = sjs_vm_order(operator, a, b);
        if (result == SJS_VM_UNDECIDED) return order_fault(vm, a, b);
        *out = sjs_boolean(result != 0);
        return SJS_OK;
    }
    case SJS_OP_EXPONENT:
        return power(vm, out);
    default: {
        if (a.kind != SJS_NUMBER || b.kind != SJS_NUMBER) return bigint_binary(vm, operator, out);
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
        *out = sjs_num(z);
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
        if (v.kind != SJS_NUMBER) return bigint_unary(vm, operator, out);
        *out = sjs_num(operator == SJS_OP_NEGATE ? -v.as.number : v.as.number);
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

/* Spreading a map, and the fault for what does not spread: at the end of
 * the file. */
static sjs_result spread_other(sjs_vm *vm, sjs_value *array, sjs_value source);
static bool spread_size(sjs_vm *vm, sjs_value source, uint64_t *total);

/* Each element a spread yields: arrays, sets, and strings by code point;
 * and a map's entries (spread_other). */
static sjs_result spread_into(sjs_vm *vm, sjs_value *array, sjs_value source) {
    if (source.kind == SJS_ARRAY || source.kind == SJS_SET) {
        const sjs_array *s = SJS_AS_ARRAY(source);
        for (uint32_t i = 0; i < s->length; i++) {
            sjs_ref(s->items[i]);
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
    return spread_other(vm, array, source);
}

/* Takes over the property's key and value. */
static sjs_result object_put(sjs_vm *vm, sjs_value *object, sjs_put property) {
    sjs_string *key = property.key;
    sjs_value value = property.value;
    if (sjs_str_equal(sjs_string_view(key), SJS_STR("__proto__"))) {
        sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &key->h));
        sjs_vm_release(vm, value);
        return sjs_vm_fault(vm, "staticjs objects have no prototype");
    }
    sjs_object *o = SJS_AS_OBJECT(*object);
    sjs_result r = sjs_object_put_key(vm->context, &o, (sjs_put){ key, value });
    object->as.heap = &o->h;
    if (r != SJS_OK) {
        sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &key->h));
        sjs_vm_release(vm, value);
        return sjs_vm_fail(vm, r);
    }
    return SJS_OK;
}

/* Put each own enumerable property of `source` into the object being built
 * at `*object`. Borrows `source`. */
static sjs_result spread_properties(sjs_vm *vm, sjs_value *object, sjs_value source) {
    if (source.kind == SJS_OBJECT) {
        const sjs_object *s = SJS_AS_OBJECT(source);
        for (uint32_t i = 0; i < s->count; i++) {
            sjs_ref(sjs_heap_value(SJS_STRING, &s->entries[i].key->h));
            sjs_ref(s->entries[i].value);
            if (object_put(vm, object, (sjs_put){ s->entries[i].key, s->entries[i].value }) != SJS_OK) return SJS_ERR_PROGRAM;
        }
        vm->fuel_used += s->count / 8u;
        return SJS_OK;
    }
    if (source.kind == SJS_ARRAY) {
        if (sjs_array_extra(source)) return sjs_array_extra_fault(vm);
        const sjs_array *a = SJS_AS_ARRAY(source);
        for (uint32_t i = 0; i < a->length; i++) {
            sjs_string *k;
            sjs_result r = sjs_string_from_number(vm->context, (double)i, &k);
            if (r != SJS_OK) return sjs_vm_fail(vm, r);
            sjs_ref(a->items[i]);
            if (object_put(vm, object, (sjs_put){ k, a->items[i] }) != SJS_OK) return SJS_ERR_PROGRAM;
        }
        return SJS_OK;
    }
    if (source.kind == SJS_STRING) return sjs_vm_fault(vm, "staticjs does not spread a string into an object");
    /* undefined, null, numbers, booleans, sets, maps and functions have no
     * own enumerable properties. */
    return SJS_OK;
}

/* An empty object being built gets the properties of the object `source`
 * in one step: its keys are distinct and in order already, so the entries
 * go in as they are. Borrows `source`. */
static sjs_result copy_properties(sjs_vm *vm, sjs_value *object, sjs_value source) {
    const sjs_object *s = SJS_AS_OBJECT(source);
    sjs_object *o = SJS_AS_OBJECT(*object);
    sjs_result r = sjs_object_grow(vm->context, &o, s->count);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    object->as.heap = &o->h;
    for (uint32_t i = 0; i < s->count; i++) {
        sjs_ref(sjs_heap_value(SJS_STRING, &s->entries[i].key->h));
        sjs_ref(s->entries[i].value);
        o->entries[i] = s->entries[i];
    }
    o->count = s->count;
    vm->fuel_used += s->count / 8u;
    return SJS_OK;
}

/*
 * `{...source}` in the object literal being built at `*object`, which
 * nothing else holds. Takes over the reference to `source`. While the
 * literal is still empty, an object nothing else holds becomes the literal
 * itself (sjs_value_unique says why no program can tell), and the literal's
 * later properties update it in place, as a put does: a key it has keeps its
 * place, as in JavaScript, whose order the source has already; but for a
 * frozen or sealed object, as the literal is extensible. An object
 * something else holds is copied in one step. Anything else is spread
 * property by property. Neither the takeover nor the copy goes through
 * object_put's check for __proto__: no object holds that key, as every
 * way to make one refuses it (the parser, object_put, sjs_object_put and
 * Object.fromEntries), and a module's namespace with it fails to compile.
 */
static sjs_result object_spread(sjs_vm *vm, sjs_value *object, sjs_value source) {
    sjs_result r;
    if (source.kind == SJS_OBJECT && SJS_AS_OBJECT(*object)->count == 0) {
        if (sjs_value_unique(source) && (source.as.heap->flags & SJS_INTEGRITY) == 0) {
            sjs_unref(vm->context, *object);
            *object = source;
            return SJS_OK;
        }
        r = copy_properties(vm, object, source);
    } else {
        r = spread_properties(vm, object, source);
    }
    sjs_vm_release(vm, source);
    return r;
}

static sjs_result new_set(sjs_vm *vm, sjs_values args, sjs_value *out) {
    sjs_value source = args.count > 0 ? args.ptr[0] : sjs_undefined();
    sjs_value elements = sjs_undefined();
    if (source.kind != SJS_UNDEFINED && source.kind != SJS_NULL) {
        sjs_result r = sjs_array_new(vm->context, 4, &elements);
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        if (spread_into(vm, &elements, source) != SJS_OK) {
            sjs_unref(vm->context, elements);
            return SJS_ERR_PROGRAM;
        }
    }
    uint32_t n = sjs_length(elements);
    sjs_value set = sjs_undefined();
    sjs_result r = sjs_set_new(vm->context, n, &set);
    for (uint32_t i = 0; i < n && r == SJS_OK; i++) {
        sjs_value e = sjs_array_at(elements, i);
        sjs_array *a = SJS_AS_ARRAY(set);
        sjs_ref(e);
        r = sjs_set_insert(vm->context, &a, e);
        set.as.heap = &a->h;
        if (r != SJS_OK) sjs_unref(vm->context, e);
    }
    sjs_unref(vm->context, elements);
    vm->fuel_used += n;
    if (r != SJS_OK) {
        if (set.kind == SJS_SET) sjs_unref(vm->context, set);
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
        else if (!spread_size(vm, v, &total)) return false;
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
                sjs_unref(vm->context, list);
                return false;
            }
        } else {
            sjs_ref(v);
            if (array_append(vm, &list, v) != SJS_OK) {
                sjs_unref(vm->context, list);
                return false;
            }
        }
    }
    cut(vm, start);
    const sjs_array *a = SJS_AS_ARRAY(list);
    for (uint32_t i = 0; i < a->length; i++) {
        sjs_ref(a->items[i]);
        vm->stack[vm->sp++] = a->items[i];
    }
    *argc = a->length;
    sjs_unref(vm->context, list);
    return true;
}

/* A call on the stack: the callee's slot, its arguments, and the height the
 * call leaves the stack at when it returns. */
typedef struct {
    uint32_t callee;
    uint32_t argc;
    uint32_t bottom;
} call_site;

/* A rest parameter's value: an array of `values`, each retained. */
static bool rest_array(sjs_vm *vm, sjs_values values, sjs_value *out) {
    sjs_array *rest;
    sjs_result r = sjs_array_make(vm->context, values.count, &rest);
    if (r != SJS_OK) {
        sjs_vm_fail(vm, r);
        return false;
    }
    for (uint32_t i = 0; i < values.count; i++) {
        sjs_ref(values.ptr[i]);
        rest->items[i] = values.ptr[i];
    }
    rest->length = values.count;
    vm->fuel_used += values.count / 8u;
    *out = sjs_heap_value(SJS_ARRAY, &rest->h);
    return true;
}

/* A rest parameter: the arguments from site.bottom (the parameters before
 * it) on become one array in its slot, the last parameter's. */
static bool gather_rest(sjs_vm *vm, call_site site) {
    uint32_t start = site.callee + 1u + site.bottom;
    uint32_t n = site.argc > site.bottom ? site.argc - site.bottom : 0;
    sjs_value rest;
    if (!rest_array(vm, (sjs_values){ n != 0 ? &vm->stack[start] : NULL, n }, &rest)) return false;
    cut(vm, start);
    for (uint32_t i = site.argc; i < site.bottom; i++) {
        if (!sjs_vm_push(vm, sjs_undefined())) {
            sjs_unref(vm->context, rest);
            return false;
        }
    }
    return sjs_vm_push(vm, rest);
}

/* Fault when the closure's function has a frame: staticjs has no recursion. */
static bool closure_running(sjs_vm *vm, const sjs_closure *closure) {
    for (uint32_t i = 0; i < vm->frame_count; i++) {
        const vm_frame *f = &vm->frames[i];
        if (f->kind == FRAME_CODE && f->program == closure->program && f->function == closure->function) {
            sjs_vm_fault(vm, "this function is already running: staticjs functions cannot call themselves");
            return true;
        }
    }
    return false;
}

/* Fault when every frame is taken. */
static bool frames_full(sjs_vm *vm) {
    if (vm->frame_count < vm->frame_capacity) return false;
    sjs_error_set(&vm->error, fault_position(vm), (sjs_problem){ SJS_ERROR_LIMIT, "the evaluation needs more than max_frames frames" });
    vm->status = VM_FAULT;
    return true;
}

/* Enter a closure that closure_running has cleared. */
static bool enter_closure(sjs_vm *vm, call_site site) {
    uint32_t callee = site.callee;
    uint32_t argc = site.argc;
    uint32_t bottom = site.bottom;
    sjs_closure *closure = SJS_AS_CLOSURE(vm->stack[callee]);
    const sjs_function_info *fn = &closure->program->functions[closure->function];
    if (frames_full(vm)) return false;
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
    vm->frames[vm->frame_count++] = (vm_frame){
        .kind = FRAME_CODE,
        .pc = fn->code,
        .base = base,
        .bottom = bottom,
        .function = closure->function,
        .program = closure->program,
        .closure = closure,
    };
    return true;
}

/* A built-in that calls back; its receiver is at site.bottom. */
static bool enter_loop(sjs_vm *vm, sjs_builtin_id id, call_site site) {
    uint32_t receiver = site.bottom;
    uint32_t argc = site.argc;
    if (frames_full(vm)) return false;
    sjs_position at = fault_position(vm);
    uint32_t first = receiver + LOOP_CALLBACK;
    if (argc > 2) cut(vm, first + 2);
    for (uint32_t i = argc; i < 2; i++) {
        if (!sjs_vm_push(vm, sjs_undefined())) return false;
    }
    if (!sjs_vm_push(vm, sjs_undefined())) return false;
    vm_frame *f = &vm->frames[vm->frame_count++];
    *f = (vm_frame){
        .kind = FRAME_LOOP,
        .builtin = (uint16_t)id,
        .base = receiver,
        .bottom = receiver,
        .argc = argc,
        .at = at,
    };
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
        leaf.receiver_slot = &vm->stack[callee - 1];
        sjs_result r = sjs_builtin_call(vm, id, &leaf);
        if (r != SJS_OK) {
            sjs_vm_release(vm, leaf.result);
            /* A leaf that goes on as a loop: a long match. */
            if (r == SJS_LEAF_LOOP) return enter_loop(vm, (sjs_builtin_id)leaf.loop, (call_site){ callee, argc, bottom });
            return false;
        }
    } else if (f.as.heap->type == SJS_HEAP_CLOSURE) {
        if (closure_running(vm, SJS_AS_CLOSURE(f))) return false;
        return enter_closure(vm, (call_site){ callee, argc, bottom });
    } else if (f.as.heap->type == SJS_HEAP_BUILTIN) {
        sjs_builtin_id id = (sjs_builtin_id)((const sjs_builtin_function *)(const void *)f.as.heap)->id;
        if (sjs_builtin_loops(id)) {
            /* A function that calls back (Object.groupBy, Array.from) is a loop laid out
             * like a method's: its first argument is the receiver, below the
             * function. With no arguments the receiver is undefined, and the
             * built-in's own check of its items faults. */
            if (argc == 0) {
                if (!sjs_vm_push(vm, (sjs_value){ .kind = SJS_UNDEFINED })) return false;
                argc = 1;
            }
            sjs_value items = vm->stack[callee + 1];
            vm->stack[callee + 1] = vm->stack[callee];
            vm->stack[callee] = items;
            return enter_loop(vm, id, (call_site){ callee + 1, argc - 1u, bottom });
        }
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

/* The first call of a loop's closure callback: the frame it keeps for the
 * whole loop, its slots undefined above the loop's, the record parked above
 * the loop frame (vm.h). The closure is checked once per loop: neither it
 * nor the frames below the loop change while the loop runs. LOOP_CALLBACK
 * keeps the closure alive, and the body cannot reach that slot. */
bool sjs_vm_enter_callback(sjs_vm *vm, vm_frame *loop) {
    sjs_closure *closure = SJS_AS_CLOSURE(vm->stack[loop->base + LOOP_CALLBACK]);
    if (closure_running(vm, closure)) return false;
    if (frames_full(vm)) return false;
    const sjs_function_info *fn = &closure->program->functions[closure->function];
    uint32_t base = loop->base + LOOP_SLOTS;
    for (uint32_t i = 0; i < fn->locals; i++) {
        if (!sjs_vm_push(vm, (sjs_value){ .kind = SJS_UNDEFINED })) return false;
    }
    vm->frames[vm->frame_count] = (vm_frame){
        .kind = FRAME_CODE,
        .base = base,
        .bottom = base + fn->locals,
        .function = closure->function,
        .program = closure->program,
        .closure = closure,
    };
    loop->entered = true;
    return true;
}

/* The callback is a heap value: sjs_loop_start accepts only callables, and
 * is_callable excludes internal methods. A closure reads only its
 * parameters (staticjs has no `arguments`), so arguments past them are not
 * written. Its slots are undefined here (clear_callback), as a new frame's
 * are. RETURN cuts the stack to the frame's bottom, the top of its slots,
 * parks the record and leaves the result on top for the loop's next step. */
bool sjs_vm_call_back(sjs_vm *vm, vm_frame *loop, sjs_values args) {
    sjs_value callback = vm->stack[loop->base + LOOP_CALLBACK];
    if (callback.as.heap->type != SJS_HEAP_CLOSURE) {
        sjs_ref(callback);
        if (!sjs_vm_push(vm, callback)) return false;
        for (uint32_t i = 0; i < args.count; i++) {
            sjs_ref(args.ptr[i]);
            if (!sjs_vm_push(vm, args.ptr[i])) return false;
        }
        return call(vm, args.count, 0);
    }
    if (!loop->entered && !sjs_vm_enter_callback(vm, loop)) return false;
    vm_frame *callee = &vm->frames[vm->frame_count];
    const sjs_function_info *fn = &callee->program->functions[callee->function];
    sjs_value *slots = &vm->stack[callee->base];
    bool rest = (fn->flags & SJS_FUNCTION_REST) != 0;
    uint32_t fixed = rest ? fn->params - 1u : fn->params;
    uint32_t n = args.count < fixed ? args.count : fixed;
    for (uint32_t i = 0; i < n; i++) {
        sjs_ref(args.ptr[i]);
        slots[i] = args.ptr[i];
    }
    /* The record is counted last, so that a fault here is the loop's. */
    if (rest && !rest_array(vm, (sjs_values){ args.ptr + n, args.count - n }, &slots[fixed])) return false;
    callee->pc = fn->code;
    vm->frame_count++;
    return true;
}

/* Undefine a kept callback frame's slots after it returned, as a return from
 * a frame of its own would have released them, and before the loop reads its
 * result. The loop frame is on top, its callback's record just above it. */
static void clear_callback(sjs_vm *vm) {
    const vm_frame *callee = &vm->frames[vm->frame_count];
    for (uint32_t i = callee->base; i < callee->bottom; i++) {
        sjs_value v = vm->stack[i];
        vm->stack[i] = (sjs_value){ .kind = SJS_UNDEFINED }; /* sjs_undefined() is out of line */
        sjs_vm_release(vm, v);
    }
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
                sjs_unref(vm->context, rest);
                return sjs_vm_fail(vm, r);
            }
            if (array_append(vm, &rest, sjs_heap_value(SJS_STRING, &c->h)) != SJS_OK) {
                sjs_unref(vm->context, rest);
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
        sjs_ref(a->items[start + i]);
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
            sjs_ref(sjs_heap_value(SJS_STRING, &entry->key->h));
            sjs_ref(entry->value);
            rest->entries[rest->count++] = *entry;
        }
        vm->fuel_used += (uint64_t)o->count * (keys + 1u) / 8u;
    } else if (source.kind == SJS_ARRAY) {
        if (sjs_array_extra(source)) return sjs_array_extra_fault(vm);
        const sjs_array *a = SJS_AS_ARRAY(source);
        r = sjs_object_make(vm->context, a->length, &rest);
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        rest->count = 0;
        for (uint32_t i = 0; i < a->length; i++) {
            if (excludes_index(x, i)) continue;
            sjs_string *k;
            r = sjs_string_from_number(vm->context, (double)i, &k);
            if (r != SJS_OK) {
                sjs_unref(vm->context, sjs_heap_value(SJS_OBJECT, &rest->h));
                return sjs_vm_fail(vm, r);
            }
            sjs_ref(a->items[i]);
            rest->entries[rest->count].key = k;
            rest->entries[rest->count++].value = a->items[i];
        }
        vm->fuel_used += (uint64_t)a->length * (keys + 1u) / 8u;
    } else {
        /* undefined and null fault before; numbers, booleans, sets, maps
         * and functions have no own enumerable properties. */
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

/* MUTATION: at the end of the file. */
static void execute_other(sjs_vm *vm, vm_frame *f);
/* An array pattern over a map, or its fault: at the end of the file. */
static void pattern_other(sjs_vm *vm);

static void jump_to(vm_frame *f, const uint8_t *code, uint32_t pc) {
    f->pc = sjs_read32(code + pc);
}

/* Inlined into sjs_vm_run's loop, its one caller, always: clang -O2 does
 * that by itself only while the function stays under its size threshold,
 * and a call per instruction costs 5 to 10% of every evaluation
 * (eval.array.*, vm.fuel.*) when one more case tips it over. */
__attribute__((always_inline)) static inline void execute(sjs_vm *vm, vm_frame *f) {
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
        sjs_vm_push(vm, sjs_num(f->program->numbers[sjs_read32(code + pc)]));
        return;
    case SJS_OP_CONSTANT: {
        f->pc = pc + 4;
        sjs_value v = f->program->constants[sjs_read32(code + pc)];
        sjs_ref(v);
        sjs_vm_push(vm, v);
        return;
    }
    case SJS_OP_LOCAL: {
        f->pc = pc + 2;
        sjs_value v = vm->stack[f->base + sjs_read16(code + pc)];
        sjs_ref(v);
        sjs_vm_push(vm, v);
        return;
    }
    case SJS_OP_TAKE_LOCAL: {
        /* The slot's last read (compiler.c): its reference moves to the stack. */
        f->pc = pc + 2;
        sjs_value *slot = &vm->stack[f->base + sjs_read16(code + pc)];
        sjs_value v = *slot;
        *slot = sjs_undefined();
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
        sjs_ref(v);
        sjs_vm_push(vm, v);
        return;
    }
    case SJS_OP_GLOBAL: {
        f->pc = pc + 2;
        sjs_value v = vm->globals[sjs_read16(code + pc)];
        sjs_ref(v);
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
        sjs_value *top = stack_top(vm);
        if (top->kind == SJS_OBJECT && top->internal == SJS_INTERNAL_NONE) {
            /* The common case inline: a property the object has. A small
             * object is scanned here; a large one has an index. */
            const sjs_object *o = SJS_AS_OBJECT(*top);
            sjs_str name = sjs_string_view(key);
            const sjs_entry *e = NULL;
            /* First where this instruction last found its key: an object
             * holds a key once, so the entry there with the key is the one.
             * Keys are mostly interned, so the pointers mostly match. */
            uint32_t *guess = &vm->member_guess[pc & (VM_MEMBER_GUESSES - 1u)];
            if (*guess < o->count) {
                const sjs_string *k = o->entries[*guess].key;
                if (k == key || sjs_str_equal(sjs_string_view(k), name)) e = &o->entries[*guess];
            }
            if (e == NULL) {
                if (o->index != NULL) {
                    e = sjs_object_find(o, name);
                } else {
                    for (uint32_t i = 0; i < o->count; i++) {
                        const sjs_string *k = o->entries[i].key;
                        if (k == key || sjs_str_equal(sjs_string_view(k), name)) {
                            e = &o->entries[i];
                            break;
                        }
                    }
                }
                if (e != NULL) *guess = (uint32_t)(e - o->entries);
            }
            if (e != NULL) {
                sjs_value v = e->value;
                sjs_ref(v);
                sjs_unref(vm->context, *top);
                *top = v;
                return;
            }
        }
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
                sjs_ref(result);
            }
            r = SJS_OK;
        } else if (object.kind == SJS_STRING && key.kind == SJS_NUMBER) {
            r = string_at(vm, (char_ref){ SJS_AS_STRING(object), key.as.number }, &result);
        } else {
            sjs_string *k;
            r = key_of(vm, key, &k);
            if (r == SJS_OK) {
                r = sjs_property_get(vm, (sjs_member){ object, k }, &result);
                sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &k->h));
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
        sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &k->h));
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
        sjs_value *array = stack_top(vm);
        /* The literal is still empty and nothing else holds the array: the
         * array becomes the literal, which appends what follows in place;
         * not a match result, whose index, input and groups a spread drops,
         * nor a frozen or sealed array, as the literal is extensible. */
        if (v.kind == SJS_ARRAY && SJS_AS_ARRAY(*array)->length == 0 && sjs_value_unique(v) &&
            (v.as.heap->flags & (SJS_ARRAY_EXTRA | SJS_INTEGRITY)) == 0) {
            sjs_unref(vm->context, *array);
            *array = v;
            return;
        }
        spread_into(vm, array, v);
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
    case SJS_OP_OBJECT_LITERAL: {
        f->pc = pc + 5;
        uint32_t index = sjs_read32(code + pc);
        uint8_t flags = code[pc + 4];
        uint32_t above = (flags & SJS_LITERAL_ABOVE) != 0 ? 1u : 0u;
        const sjs_array *keys = index == UINT32_MAX ? NULL : SJS_AS_ARRAY(f->program->constants[index]);
        uint32_t n = keys == NULL ? 0 : keys->length;
        /* Exact when the literal is complete. With room for a few more, as
         * OBJECT_NEW gives, when properties follow, and for `{}`, which
         * often becomes an accumulator that a spread takes over and grows. */
        uint32_t capacity = n < 4u && (n == 0 || (flags & SJS_LITERAL_OPEN) != 0) ? 4u : n;
        uint32_t base = vm->sp - above - n;
        if (n == 0 && vm->sp >= vm->stack_capacity) {
            sjs_vm_stack_full(vm, sjs_undefined());
            return;
        }
        sjs_result r = sjs_object_literal(vm->context, (sjs_literal){ keys, &vm->stack[base], capacity }, &result);
        if (r != SJS_OK) {
            sjs_vm_fail(vm, r);
            return;
        }
        /* The object took the values over and goes in their place, under
         * the one value above them, if any (a spread's source or a
         * computed key). */
        if (above != 0) vm->stack[base + 1u] = vm->stack[base + n];
        vm->stack[base] = result;
        vm->sp = base + 1u + above;
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
            sjs_ref(v);
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
            pattern_other(vm);
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
        sjs_ref(v);
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
        /* MUTATION: the instructions of mutation, or the fault. Restore:
         * sjs_vm_fault(vm, "the program is damaged"). */
        execute_other(vm, f);
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
    vm->no_regex_leaf = config->no_regex_leaf;
    vm->no_regex_dfa = config->no_regex_dfa;
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
    vm->frames[vm->frame_count++] = (vm_frame){ .kind = FRAME_CODE, .pc = fn->code, .program = program };
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
        if (f->entered) clear_callback(vm);
        if (sjs_loop_step(vm, f) == LOOP_DONE) finish_loop(vm, f);
    }
    return vm->status == VM_DONE ? SJS_RUN_DONE : SJS_RUN_FAULT;
}

sjs_result sjs_vm_take_result(sjs_vm *vm, sjs_value *out) {
    if (vm == NULL || out == NULL || vm->status != VM_DONE) return SJS_ERR_INVALID;
    /* A module's value is its namespace, which JavaScript seals: it takes
     * no new key and loses none (Object.isSealed). */
    if (vm->program->goal == SJS_GOAL_MODULE && vm->result.kind == SJS_OBJECT) vm->result.as.heap->flags |= SJS_INTEGRITY_SEALED;
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

/* The host clock: within Temporal's limits, 8.64 x 10^21 ns either way,
 * which are 8.64 x 10^12 whole seconds. */
#define CLOCK_SECONDS_MAX INT64_C(8640000000000)
#define CLOCK_NS_PER_SECOND 1000000000u

static bool clock_within_limits(const sjs_epoch *t) {
    if (t->nanoseconds >= CLOCK_NS_PER_SECOND) return false;
    if (t->seconds < -CLOCK_SECONDS_MAX || t->seconds > CLOCK_SECONDS_MAX) return false;
    return t->seconds != CLOCK_SECONDS_MAX || t->nanoseconds == 0;
}

sjs_result sjs_vm_set_clock(sjs_vm *vm, const sjs_epoch *now) {
    if (vm == NULL || now == NULL || !clock_within_limits(now)) return SJS_ERR_INVALID;
    vm->clock = *now;
    vm->clock_set = true;
    return SJS_OK;
}

sjs_result sjs_vm_advance_clock(sjs_vm *vm, uint64_t nanoseconds) {
    if (vm == NULL || !vm->clock_set) return SJS_ERR_INVALID;
    /* At most 1.8 x 10^10 seconds: no overflow past the limit's 8.64 x 10^12. */
    uint64_t past = vm->clock.nanoseconds + nanoseconds % CLOCK_NS_PER_SECOND;
    sjs_epoch next = { vm->clock.seconds + (int64_t)(nanoseconds / CLOCK_NS_PER_SECOND + past / CLOCK_NS_PER_SECOND),
                       (uint32_t)(past % CLOCK_NS_PER_SECOND) };
    if (!clock_within_limits(&next)) return SJS_ERR_INVALID;
    vm->clock = next;
    return SJS_OK;
}

sjs_result sjs_vm_clock(const sjs_vm *vm, sjs_epoch *out) {
    if (vm == NULL || out == NULL) return SJS_ERR_INVALID;
    if (!vm->clock_set) return SJS_ERR_NOT_FOUND;
    *out = vm->clock;
    return SJS_OK;
}

/* The host seed: Math.random's generator (number.c), kept like the clock. */
sjs_result sjs_vm_set_seed(sjs_vm *vm, int64_t seed) {
    if (vm == NULL) return SJS_ERR_INVALID;
    sjs_random_seed(&vm->random, seed);
    return SJS_OK;
}

sjs_result sjs_vm_random(const sjs_vm *vm, sjs_random *out) {
    if (vm == NULL || out == NULL) return SJS_ERR_INVALID;
    if ((vm->random.v8.state[0] | vm->random.v8.state[1]) == 0) return SJS_ERR_NOT_FOUND;
    *out = vm->random.v8;
    return SJS_OK;
}

sjs_result sjs_vm_set_random(sjs_vm *vm, const sjs_random *state) {
    if (vm == NULL || state == NULL || !sjs_random_restore(&vm->random, state)) return SJS_ERR_INVALID;
    return SJS_OK;
}

void sjs_vm_destroy(sjs_vm *vm) {
    if (vm == NULL) return;
    const sjs_allocator *a = &vm->context->counting;
    if (vm->stack != NULL && vm->frames != NULL) sjs_vm_reset(vm);
    sjs_mem_free(a, vm->stack, (size_t)vm->stack_capacity * sizeof(sjs_value));
    sjs_mem_free(a, vm->frames, (size_t)vm->frame_capacity * sizeof(vm_frame));
    sjs_mem_free(a, vm, sizeof *vm);
}

/* ------------------------------------------------------------------------
 * MUTATION: methods that change the variable they are called on
 * (docs/mutation.md). This section exists only for it, at the end of the
 * file, so that the code before it is laid out as it was.
 */

/* A reference to the variable at stack index `at` (SJS_OP_RECEIVER). */
static sjs_value slot_reference(uint32_t at) {
    return (sjs_value){ .kind = SJS_UNDEFINED, .internal = SJS_INTERNAL_SLOT, .boolean = at };
}

/* A method that returns its receiver gives the variable's value, a copy,
 * or in a chain the reference again, for the next method to change. */
static bool push_receiver(sjs_vm *vm, uint32_t at, bool chain) {
    if (chain) return sjs_vm_push(vm, slot_reference(at));
    sjs_value v = vm->stack[at];
    sjs_ref(v);
    return sjs_vm_push(vm, v);
}

/* METHOD_MUTATE: the callee for `x.m(...)`, with a reference to x on top:
 * the method of x's kind that changes it, or, for a value of another kind,
 * what an ordinary call finds (an object's property) or its fault. */
__attribute__((noinline)) static void mutation_method(sjs_vm *vm, const sjs_string *key) {
    /* MUTATION: shared names. A chain that goes on from a method that changed
     * nothing (a Temporal value's add) has its result on top, a value: an
     * ordinary call's receiver. A set there faults in method(). */
    if (stack_top(vm)->internal != SJS_INTERNAL_SLOT) {
        sjs_value callee;
        if (method(vm, (sjs_member){ *stack_top(vm), key }, &callee) == SJS_OK) sjs_vm_push(vm, callee);
        return;
    }
    sjs_value receiver = vm->stack[stack_top(vm)->boolean];
    sjs_builtin_id id = sjs_mutation_of(sjs_name_of(key), receiver);
    sjs_value callee = { .kind = SJS_FUNCTION, .internal = SJS_INTERNAL_METHOD, .boolean = (uint32_t)id };
    if (id == SJS_BUILTIN_NONE && method(vm, (sjs_member){ receiver, key }, &callee) != SJS_OK) return;
    sjs_vm_push(vm, callee);
}

/* MUTATE: [reference, callee, arguments]. The variable's value must have
 * one holder, the variable: anything else that holds it (an argument, a
 * caller, the embedder, a closure) would see it change. */
__attribute__((noinline)) static bool mutate(sjs_vm *vm, uint32_t argc, uint8_t flags) {
    if ((flags & SJS_CALL_SPREAD) != 0 && !expand_spread(vm, &argc)) return false;
    uint32_t callee = vm->sp - argc - 1u;
    uint32_t at = vm->stack[callee - 1u].boolean;
    bool chain = (flags & SJS_CALL_CHAIN) != 0;
    sjs_value f = vm->stack[callee];
    /* MUTATION: shared names. A value, not a reference, mid-chain: what a
     * method that changed nothing gave (mutation_method). */
    bool plain = vm->stack[callee - 1u].internal != SJS_INTERNAL_SLOT;
    if (f.internal != SJS_INTERNAL_METHOD || f.boolean < SJS_METHOD_MUTATION_FIRST || plain) {
        /* An object's property, or a method of the name that changes no
         * value (a Temporal value's add), called as any method is: the
         * variable keeps its value. A sort's SORTED, next, would put the
         * call's result in the variable, so it is skipped. With CHAIN the
         * result goes on as a value, an ordinary call's receiver
         * (MUTATION: shared names; it faulted before). */
        if (plain && f.internal == SJS_INTERNAL_METHOD && f.boolean >= SJS_METHOD_MUTATION_FIRST) {
            sjs_vm_fault(vm, "the program is damaged: a method that changes its receiver has no variable to change");
            return false;
        }
        vm_frame *frame = &vm->frames[vm->frame_count - 1u];
        const uint8_t *next = frame->program->code + frame->pc;
        if (*next == SJS_OP_SORTED) frame->pc += 4u;
        if (!plain) {
            /* MUTATION: the receiver is the variable's value, as in any
             * method call (a Temporal value's add makes a new value): the
             * reference becomes a holder of it, which the call releases. */
            sjs_value receiver = vm->stack[at];
            sjs_ref(receiver);
            vm->stack[callee - 1u] = receiver;
        }
        return call(vm, argc, SJS_CALL_METHOD);
    }
    sjs_value *slot = &vm->stack[at];
    if (!sjs_value_unique(*slot)) {
        sjs_vm_fault(vm, "staticjs changes a value only while one variable holds it, and this one is held elsewhere too");
        return false;
    }
    sjs_builtin_id id = (sjs_builtin_id)f.boolean;
    if (id == SJS_METHOD_ARRAY_SORT) {
        /* MUTATION: a match result sorted in place keeps its index, input and
         * groups in JavaScript; toSorted's loop gives a plain array. */
        if (sjs_array_extra(*slot)) {
            sjs_array_extra_fault(vm);
            return false;
        }
        /* MUTATION: sort writes the elements of a frozen array of two or more. */
        if (SJS_AS_ARRAY(*slot)->length >= 2u && !sjs_integrity_allows(vm, *slot, SJS_INTEGRITY_FROZEN)) return false;
        /* toSorted's loop, which takes over a receiver it holds alone; the
         * SORTED after this instruction puts its result back. */
        vm->stack[callee - 1u] = *slot;
        *slot = sjs_undefined();
        return enter_loop(vm, SJS_METHOD_ARRAY_TO_SORTED, (call_site){ callee, argc, callee - 1u });
    }
    sjs_leaf_call leaf = { 0 };
    leaf.receiver = *slot;
    leaf.receiver_slot = slot;
    leaf.args = &vm->stack[callee + 1u];
    leaf.count = argc;
    if (sjs_mutation_call(vm, id, &leaf) != SJS_OK) {
        sjs_vm_release(vm, leaf.result);
        return false;
    }
    vm->fuel_used += leaf.cost;
    cut(vm, callee - 1u);
    if (sjs_mutation_returns_receiver(id)) return push_receiver(vm, at, chain);
    return sjs_vm_push(vm, leaf.result);
}

/* MUTATION: OBJECT_ASSIGN: [callee, reference, sources], Object.assign of
 * a variable the function owns (docs/mutation.md, "Object.assign"). When a
 * source has a property to put, the variable's value must be an object
 * with one holder, the variable; with none, Object.assign gives its target
 * as it is, which any value but a primitive may be (JavaScript makes an
 * object of a primitive). Then a copy of the value, as a method that
 * returns its receiver gives it. */
__attribute__((noinline)) static bool assign_variable(sjs_vm *vm, uint32_t argc, uint8_t flags) {
    if ((flags & SJS_CALL_SPREAD) != 0 && !expand_spread(vm, &argc)) return false;
    uint32_t reference = vm->sp - argc - 1u;
    uint32_t at = vm->stack[reference].boolean;
    sjs_value *slot = &vm->stack[at];
    sjs_values sources = { &vm->stack[reference + 1u], argc };
    /* MUTATION: Object.freeze and Object.seal of a variable the function
     * owns, compiled as Object.assign's: the variable's value frozen or
     * sealed (its other arguments are ignored), then a copy of it. */
    sjs_heap *callee = vm->stack[reference - 1u].as.heap;
    if (callee != sjs_builtin_value(SJS_BUILTIN_OBJECT_ASSIGN).as.heap) {
        bool freeze = callee == sjs_builtin_value(SJS_BUILTIN_OBJECT_FREEZE).as.heap;
        if (sjs_integrity_set(vm, *slot, freeze ? SJS_INTEGRITY_FROZEN : SJS_INTEGRITY_SEALED) != SJS_OK) return false;
        cut(vm, reference - 1u);
        return push_receiver(vm, at, false);
    }
    if (sjs_is_primitive(*slot)) {
        sjs_vm_fault(vm, "Object.assign takes an object as its target, where JavaScript would make one of a primitive");
        return false;
    }
    if (sjs_assign_puts(sources)) {
        if (slot->kind != SJS_OBJECT) {
            sjs_vm_fault(vm, "staticjs's Object.assign puts properties only into an object");
            return false;
        }
        if (!sjs_value_unique(*slot)) {
            sjs_vm_fault(vm, "staticjs changes a value only while one variable holds it, and this one is held elsewhere too");
            return false;
        }
        if (!sjs_assign_allowed(vm, *slot, sources)) return false; /* MUTATION: frozen and sealed values */
        if (sjs_vm_object_assign(vm, slot, sources) != SJS_OK) return false;
    }
    cut(vm, reference - 1u);
    return push_receiver(vm, at, false);
}

/* MUTATION: assignment to a property or an element (docs/mutation.md,
 * "Assignment"), through a path of keys from a variable's slot. */
static void path_op(sjs_vm *vm, uint32_t keys, uint8_t mode);

/* MUTATION: a method called on a path (docs/mutation.md, "Methods through a
 * path"): PATH_CALL's operands, and the functions after path_op. */
typedef struct {
    uint32_t keys;
    uint32_t argc;
    uint8_t flags;
} path_call_site;

static void path_call(sjs_vm *vm, path_call_site site);
static void path_sorted(sjs_vm *vm, uint32_t keys, uint8_t flags);

/* new Map(...): at the end of the file. */
static sjs_result new_map(sjs_vm *vm, sjs_values args, sjs_value *out);

/* The instructions of mutation, reached from the default case of
 * execute()'s switch, which they share with the fault for an opcode there
 * is none of, so that the evaluator's loop compiles as it did. */
__attribute__((noinline)) static void execute_other(sjs_vm *vm, vm_frame *f) {
    const uint8_t *code = f->program->code;
    uint32_t pc = f->pc;
    uint8_t opcode = code[pc++];
    switch (opcode) {
    case SJS_OP_RECEIVER:
        f->pc = pc + 2;
        sjs_vm_push(vm, slot_reference(f->base + sjs_read16(code + pc)));
        return;
    case SJS_OP_METHOD_MUTATE:
        f->pc = pc + 4;
        mutation_method(vm, SJS_AS_STRING(f->program->constants[sjs_read32(code + pc)]));
        return;
    case SJS_OP_MUTATE:
        f->pc = pc + 3;
        mutate(vm, sjs_read16(code + pc), code[pc + 2]);
        return;
    case SJS_OP_OBJECT_ASSIGN: /* MUTATION: Object.assign of a variable */
        f->pc = pc + 3;
        assign_variable(vm, sjs_read16(code + pc), code[pc + 2]);
        return;
    case SJS_OP_SORTED: {
        /* The variable, undefined while toSorted's loop sorted its value,
         * holds the sorted array again. */
        f->pc = pc + 3;
        uint32_t at = f->base + sjs_read16(code + pc);
        vm->stack[at] = sjs_vm_pop(vm);
        push_receiver(vm, at, (code[pc + 2] & SJS_CALL_CHAIN) != 0);
        return;
    }
    case SJS_OP_PATH_INDEX: {
        f->pc = pc;
        if (stack_top(vm)->kind == SJS_NUMBER) return;
        sjs_string *k;
        if (key_of(vm, *stack_top(vm), &k) != SJS_OK) return;
        sjs_vm_release(vm, *stack_top(vm));
        *stack_top(vm) = sjs_heap_value(SJS_STRING, &k->h);
        return;
    }
    case SJS_OP_PATH:
        f->pc = pc + 3;
        path_op(vm, sjs_read16(code + pc), code[pc + 2]);
        return;
    case SJS_OP_PATH_METHOD: {
        /* MUTATION: the method's name, and room for its receiver. */
        f->pc = pc + 4;
        sjs_value name = f->program->constants[sjs_read32(code + pc)];
        sjs_ref(name);
        if (sjs_vm_push(vm, name)) sjs_vm_push(vm, sjs_undefined());
        return;
    }
    case SJS_OP_PATH_CALL:
        f->pc = pc + 5;
        path_call(vm, (path_call_site){ sjs_read16(code + pc), sjs_read16(code + pc + 2), code[pc + 4] });
        return;
    case SJS_OP_PATH_SORTED:
        f->pc = pc + 3;
        path_sorted(vm, sjs_read16(code + pc), code[pc + 2]);
        return;
    case SJS_OP_NEW_MAP: {
        /* Not mutation: maps are values, as sets are. Rolling mutation
         * back keeps this case, and this function for it. */
        f->pc = pc + 3;
        uint32_t argc = sjs_read16(code + pc);
        if ((code[pc + 2] & SJS_CALL_SPREAD) != 0 && !expand_spread(vm, &argc)) return;
        sjs_value result;
        if (new_map(vm, (sjs_values){ &vm->stack[vm->sp - argc], argc }, &result) != SJS_OK) return;
        cut(vm, vm->sp - argc);
        sjs_vm_push(vm, result);
        return;
    }
    default:
        /* Restore this fault alone in execute()'s default case. */
        sjs_vm_fault(vm, "the program is damaged");
        return;
    }
}

/* ------------------------------------------------------------------------
 * MUTATION: assignment to a property or an element (docs/mutation.md,
 * "Assignment"). A path is a reference to a variable's slot and its keys,
 * strings: the place each names is in the container the place before it
 * holds. A store changes each container on the way in place, so each must
 * be held by its place alone; a value that holds one of them, the variable's
 * own value included, would be a second holder, so a store makes no cycle.
 */

static const char *const path_held = "staticjs changes a value only while one variable holds it, and this one is held elsewhere too";

/* MUTATION: a key of a path is a string, or a number (PATH_INDEX), which
 * indexes an array as it is and becomes a string for anything else. */
static sjs_str path_key_name(sjs_value key, char *buffer) {
    if (key.kind == SJS_NUMBER) return (sjs_str){ buffer, sjs_number_format(key.as.number, buffer) };
    return sjs_string_view(SJS_AS_STRING(key));
}

/* The key, a number, as an array's index: an integer from 0 below 2^32 - 1. */
static bool path_array_index(sjs_value key, uint32_t *index) {
    if (key.kind != SJS_NUMBER) return false;
    double d = key.as.number;
    if (!(d >= 0 && d < 4294967295.0) || d != sjs_number_trunc(d)) return false;
    *index = (uint32_t)d;
    return true;
}

/* The key on the stack at `key`, a string from now on. */
static bool path_key_string(sjs_vm *vm, sjs_value *key) {
    if (key->kind != SJS_NUMBER) return true;
    sjs_string *s;
    sjs_result r = sjs_string_from_number(vm->context, key->as.number, &s);
    if (r != SJS_OK) {
        sjs_vm_fail(vm, r);
        return false;
    }
    *key = sjs_heap_value(SJS_STRING, &s->h);
    return true;
}

/* A container the path goes into at `key`, to LOAD, STORE or DELETE
 * (SJS_PATH_*; DROP walks nothing). */
typedef struct {
    sjs_value value;
    sjs_value key;
    uint8_t mode;
} path_into;

/* An object or an array, which a store changes only while its place holds
 * it alone. JavaScript sets a property of anything but undefined and null,
 * or ignores it; staticjs has no other value that can hold one. */
static bool path_container(sjs_vm *vm, path_into into) {
    sjs_value v = into.value;
    char buffer[32];
    if (v.kind == SJS_UNDEFINED || v.kind == SJS_NULL) {
        bool undefined = v.kind == SJS_UNDEFINED;
        const char *message = into.mode == SJS_PATH_STORE ? (undefined ? "cannot set a property of undefined" : "cannot set a property of null")
                                                          : (undefined ? "cannot read a property of undefined" : "cannot read a property of null");
        /* MUTATION: step 3, delete. */
        if (into.mode == SJS_PATH_DELETE) message = undefined ? "cannot delete a property of undefined" : "cannot delete a property of null";
        fault_named(vm, message, path_key_name(into.key, buffer));
        return false;
    }
    if ((v.kind != SJS_OBJECT && v.kind != SJS_ARRAY) || v.internal != SJS_INTERNAL_NONE) {
        /* MUTATION: step 3, delete's message; a method's. */
        const char *message = into.mode == SJS_PATH_DELETE ? "staticjs deletes properties of objects only, on a path through objects and arrays"
                                                           : "staticjs assigns to properties of objects and elements of arrays only";
        if (into.mode == SJS_PATH_CALL) message = "staticjs calls a method on a path through objects and arrays only"; /* MUTATION */
        fault_named(vm, message, path_key_name(into.key, buffer));
        return false;
    }
    if (into.mode != SJS_PATH_LOAD && !sjs_value_unique(v)) {
        sjs_vm_fault(vm, path_held);
        return false;
    }
    return true;
}

/* One step of a path: from the container at `place`, at `key`. */
typedef struct {
    sjs_value *place;
    const sjs_string *key;
    sjs_value next;
    uint8_t mode;
} path_walk;

/* The place of the value at the step's key, which the path goes into next;
 * NULL, after a fault, when there is none: reading it faults (a prototype's
 * name), or gives a value no key goes into (undefined, a length). */
static sjs_value *path_child(sjs_vm *vm, const path_walk *w) {
    sjs_value v = *w->place;
    if (v.kind == SJS_OBJECT) {
        sjs_entry *e = (sjs_entry *)sjs_object_find(SJS_AS_OBJECT(v), sjs_string_view(w->key));
        if (e != NULL) return &e->value;
    } else if ((w->key->h.flags & SJS_STRING_INDEX) != 0 && w->key->index < SJS_AS_ARRAY(v)->length) {
        return &SJS_AS_ARRAY(v)->items[w->key->index];
    }
    sjs_value child;
    if (sjs_property_get(vm, (sjs_member){ v, w->key }, &child) != SJS_OK) return NULL;
    bool container = path_container(vm, (path_into){ child, w->next, w->mode });
    sjs_vm_release(vm, child);
    if (container) sjs_vm_fault(vm, "the program is damaged");
    return NULL;
}

/* The last step of a store: `value` goes in at `key`. */
typedef struct {
    sjs_value *place;
    const sjs_string *key;
    sjs_value value;
} path_store;

/* An array's element at `index`, at most its length: replaced, or
 * appended. Takes the value over. */
static bool path_put_element(sjs_vm *vm, const path_store *s, uint32_t index) {
    sjs_array *a = SJS_AS_ARRAY(*s->place);
    /* MUTATION: frozen and sealed values: an element replaced, or one
     * appended. */
    if (!sjs_integrity_allows(vm, *s->place, index < a->length ? SJS_INTEGRITY_FROZEN : SJS_INTEGRITY_SEALED)) {
        sjs_vm_release(vm, s->value);
        return false;
    }
    if (index < a->length) {
        sjs_value old = a->items[index];
        a->items[index] = s->value;
        sjs_vm_release(vm, old);
        return true;
    }
    sjs_result r = sjs_array_grow(vm->context, &a, 1);
    s->place->as.heap = &a->h;
    if (r != SJS_OK) {
        sjs_vm_release(vm, s->value);
        sjs_vm_fail(vm, r);
        return false;
    }
    a->items[a->length++] = s->value;
    return true;
}

/* An object's key keeps its place when its value is replaced, as in
 * JavaScript, and a new one goes in as a literal's would (an array index in
 * order, any other key last), its index kept current. An array's element is
 * replaced, or appended just past its end; past that JavaScript leaves
 * holes, which staticjs arrays have none of. Takes the value over. */
static bool path_put(sjs_vm *vm, const path_store *s) {
    sjs_str name = sjs_string_view(s->key);
    sjs_value value = s->value;
    const char *refused;
    if (s->place->kind == SJS_OBJECT) {
        sjs_object *o = SJS_AS_OBJECT(*s->place);
        sjs_entry *e = (sjs_entry *)sjs_object_find(o, name);
        /* MUTATION: frozen and sealed values: a key's value replaced. */
        if (e != NULL && !sjs_integrity_allows(vm, *s->place, SJS_INTEGRITY_FROZEN)) {
            sjs_vm_release(vm, value);
            return false;
        }
        if (e != NULL) {
            sjs_value old = e->value;
            e->value = value;
            sjs_vm_release(vm, old);
            return true;
        }
        if (sjs_str_equal(name, SJS_STR("__proto__")) || on_prototype(SJS_OBJECT, s->key)) {
            refused = "staticjs objects have no prototype";
        } else if (!sjs_integrity_allows(vm, *s->place, SJS_INTEGRITY_SEALED)) { /* MUTATION: a new key */
            sjs_vm_release(vm, value);
            return false;
        } else {
            sjs_string *key = (sjs_string *)(uintptr_t)s->key;
            sjs_ref(sjs_heap_value(SJS_STRING, &key->h));
            sjs_result r = sjs_object_put_key(vm->context, &o, (sjs_put){ key, value });
            s->place->as.heap = &o->h;
            if (r == SJS_OK) return true;
            sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &key->h));
            sjs_vm_release(vm, value);
            sjs_vm_fail(vm, r);
            return false;
        }
    } else if ((s->key->h.flags & SJS_STRING_INDEX) != 0) {
        if (s->key->index <= SJS_AS_ARRAY(*s->place)->length) return path_put_element(vm, s, s->key->index);
        refused = "staticjs arrays have no holes: an element is assigned inside the array, or just past its end";
    } else if (sjs_str_equal(name, SJS_STR("length"))) {
        refused = "staticjs does not assign an array's length";
    } else {
        refused = "staticjs assigns to an array only its elements, by index";
    }
    sjs_vm_release(vm, value);
    fault_named(vm, refused, name);
    return false;
}

/* The last step of a path, at `key` of the container at `place`: LOAD
 * pushes the value there, STORE puts the value on top of the stack. An
 * array's element at a number goes without the key's string. */
typedef struct {
    sjs_value *place;
    sjs_value *key;
    bool store;
} path_last;

static bool path_end(sjs_vm *vm, const path_last *l) {
    uint32_t index;
    bool element = l->place->kind == SJS_ARRAY && path_array_index(*l->key, &index) &&
                   index <= SJS_AS_ARRAY(*l->place)->length;
    if (!l->store) {
        if (element && index < SJS_AS_ARRAY(*l->place)->length) {
            sjs_value v = SJS_AS_ARRAY(*l->place)->items[index];
            sjs_ref(v);
            return sjs_vm_push(vm, v);
        }
        if (!path_key_string(vm, l->key)) return false;
        sjs_value v;
        if (sjs_property_get(vm, (sjs_member){ *l->place, SJS_AS_STRING(*l->key) }, &v) != SJS_OK) return false;
        return sjs_vm_push(vm, v);
    }
    if (!element && !path_key_string(vm, l->key)) return false;
    path_store s = { l->place, element ? NULL : SJS_AS_STRING(*l->key), vm->stack[vm->sp - 1u] };
    vm->stack[vm->sp - 1u] = sjs_undefined();
    return element ? path_put_element(vm, &s, index) : path_put(vm, &s);
}

/* MUTATION: step 3, delete (docs/mutation.md, "delete"): the property of
 * the object at `place` named by `key` goes, and the object shrinks as it
 * empties (sjs_object_remove). One the object does not have is fine, as in
 * JavaScript, but for __proto__ and a name of Object.prototype, which
 * faults as reading it does. An array's element would leave a hole, which
 * staticjs arrays have none of; JavaScript leaves its length alone. */
static bool path_delete(sjs_vm *vm, sjs_value *place, sjs_value *key) {
    if (place->kind != SJS_OBJECT) {
        char buffer[32];
        fault_named(vm, "staticjs arrays have no holes, which delete would leave: remove elements with splice", path_key_name(*key, buffer));
        return false;
    }
    if (!path_key_string(vm, key)) return false;
    const sjs_string *name = SJS_AS_STRING(*key);
    sjs_object *o = SJS_AS_OBJECT(*place);
    /* MUTATION: frozen and sealed values: a key they have stays; one they
     * do not have is fine, as in JavaScript. */
    if ((o->h.flags & SJS_INTEGRITY) != 0 && sjs_object_find(o, sjs_string_view(name)) != NULL &&
        !sjs_integrity_allows(vm, *place, SJS_INTEGRITY_SEALED)) {
        return false;
    }
    sjs_removal removal = sjs_object_remove(vm->context, &o, sjs_string_view(name));
    place->as.heap = &o->h;
    vm->fuel_used += removal.cost;
    if (!removal.removed && (sjs_str_equal(sjs_string_view(name), SJS_STR("__proto__")) || on_prototype(SJS_OBJECT, name))) {
        fault_named(vm, "staticjs objects have no prototype", sjs_string_view(name));
        return false;
    }
    return true;
}

/* PATH: [reference, keys] (LOAD, DELETE) or [reference, keys, value] (STORE,
 * DROP). Each step costs one unit of fuel. */
__attribute__((noinline)) static void path_op(sjs_vm *vm, uint32_t keys, uint8_t mode) {
    uint32_t above = mode == SJS_PATH_LOAD || mode == SJS_PATH_DELETE ? 0u : 1u; /* MUTATION: DELETE, step 3 */
    if (keys == 0 || mode > SJS_PATH_DELETE || vm->sp < keys + above + 1u) { /* MUTATION: DELETE, step 3 */
        sjs_vm_fault(vm, "the program is damaged");
        return;
    }
    uint32_t base = vm->sp - above - keys - 1u;
    sjs_value reference = vm->stack[base];
    bool damaged = reference.internal != SJS_INTERNAL_SLOT || reference.boolean >= base;
    for (uint32_t i = 1; i <= keys; i++) {
        sjs_value key = vm->stack[base + i];
        if ((key.kind != SJS_STRING && key.kind != SJS_NUMBER) || key.internal != SJS_INTERNAL_NONE) damaged = true;
    }
    if (damaged) {
        sjs_vm_fault(vm, "the program is damaged");
        return;
    }
    if (mode == SJS_PATH_DROP) {
        cut(vm, base);
        return;
    }
    vm->fuel_used += keys - 1u;
    bool store = mode == SJS_PATH_STORE;
    sjs_value *place = &vm->stack[reference.boolean];
    for (uint32_t i = 1;; i++) {
        sjs_value *key = &vm->stack[base + i];
        if (!path_container(vm, (path_into){ *place, *key, mode })) return;
        if (i == keys) break;
        uint32_t index;
        if (place->kind == SJS_ARRAY && path_array_index(*key, &index) && index < SJS_AS_ARRAY(*place)->length) {
            place = &SJS_AS_ARRAY(*place)->items[index];
            continue;
        }
        if (!path_key_string(vm, key)) return;
        path_walk w = { place, SJS_AS_STRING(*key), vm->stack[base + i + 1u], mode };
        place = path_child(vm, &w);
        if (place == NULL) return;
    }
    /* MUTATION: step 3, delete. */
    if (mode == SJS_PATH_DELETE) {
        if (path_delete(vm, place, &vm->stack[base + keys])) cut(vm, base);
        return;
    }
    path_last l = { place, &vm->stack[base + keys], store };
    if (path_end(vm, &l) && store) cut(vm, base);
}

/* ------------------------------------------------------------------------
 * MUTATION: a method that changes its receiver, called on a path from a
 * variable (docs/mutation.md, "Methods through a path"). The compiler made
 * `o.items.push(x)` [reference, keys, name, room, arguments]: the keys from
 * the variable's slot to the container, the method's name and room for the
 * container, as MUTATE has it. PATH_CALL finds the container when it runs,
 * after the arguments, through containers each held by its place alone, and
 * changes it in place.
 */

/* The place of the value the first `keys` keys above `base` name: each
 * container on the way held by its place alone, as a store needs; the key
 * above the last names what the walk reads next, for a fault's subject.
 * NULL after a fault. */
static sjs_value *path_receiver(sjs_vm *vm, uint32_t base, uint32_t keys) {
    sjs_value *place = &vm->stack[vm->stack[base].boolean];
    for (uint32_t i = 1; i <= keys; i++) {
        sjs_value *key = &vm->stack[base + i];
        if (!path_container(vm, (path_into){ *place, *key, SJS_PATH_CALL })) return NULL;
        uint32_t index;
        if (place->kind == SJS_ARRAY && path_array_index(*key, &index) && index < SJS_AS_ARRAY(*place)->length) {
            place = &SJS_AS_ARRAY(*place)->items[index];
            continue;
        }
        if (!path_key_string(vm, key)) return NULL;
        path_walk w = { place, SJS_AS_STRING(*key), vm->stack[base + i + 1u], SJS_PATH_CALL };
        place = path_child(vm, &w);
        if (place == NULL) return NULL;
    }
    return place;
}

/* The reference and keys from `base` are a path's: anything else is code
 * the compiler did not make. */
static bool path_damaged(sjs_vm *vm, uint32_t base, uint32_t keys) {
    sjs_value reference = vm->stack[base];
    if (reference.internal != SJS_INTERNAL_SLOT || reference.boolean >= base) return true;
    for (uint32_t i = 1; i <= keys; i++) {
        sjs_value key = vm->stack[base + i];
        if ((key.kind != SJS_STRING && key.kind != SJS_NUMBER) || key.internal != SJS_INTERNAL_NONE) return true;
    }
    return false;
}

/* What a mutation through a path gives: with CHAIN, the reference and the
 * keys stay for the next, and `result` is nothing; else the container
 * (sort, reverse, fill, add), a copy, or the method's result, which it
 * takes over, above `base`. */
typedef struct {
    uint32_t base;
    uint32_t keys;
    bool chain;
} path_result;

static bool path_give(sjs_vm *vm, path_result at, sjs_value result) {
    if (at.chain) {
        cut(vm, at.base + at.keys + 1u);
        return true;
    }
    cut(vm, at.base);
    return sjs_vm_push(vm, result);
}

/* The container is not of a kind with the method: call what an ordinary
 * call finds, a property of an object, or fault as it faults. A sort or a
 * chain needs the container back, which a property need not give. */
static void path_call_other(sjs_vm *vm, sjs_value *place, path_call_site site) {
    uint32_t base = vm->sp - site.argc - site.keys - 3u;
    uint32_t room = base + site.keys + 1u;
    const sjs_string *name = SJS_AS_STRING(vm->stack[room]);
    sjs_value callee;
    if (method(vm, (sjs_member){ *place, name }, &callee) != SJS_OK) return;
    if ((site.flags & SJS_CALL_CHAIN) != 0 || sjs_mutation_named(sjs_name_of(name)) == SJS_METHOD_ARRAY_SORT) {
        sjs_vm_release(vm, callee);
        fault_named(vm, "staticjs calls a method that changes its receiver, through a path, on an array, a set or a map only", sjs_string_view(name)); /* MUTATION: a map */
        return;
    }
    sjs_value receiver = *place;
    sjs_ref(receiver);
    for (uint32_t i = base + 1u; i <= room + 1u; i++) sjs_vm_release(vm, vm->stack[i]);
    vm->stack[base] = receiver;
    vm->stack[base + 1u] = callee;
    for (uint32_t i = 0; i < site.argc; i++) vm->stack[base + 2u + i] = vm->stack[room + 2u + i];
    vm->sp = base + 2u + site.argc;
    call(vm, site.argc, SJS_CALL_METHOD);
}

/* PATH_CALL: [reference, keys, name, room, arguments]. Each step costs one
 * unit of fuel, and the method what it costs on a variable. */
__attribute__((noinline)) static void path_call(sjs_vm *vm, path_call_site site) {
    if ((site.flags & SJS_CALL_SPREAD) != 0 && !expand_spread(vm, &site.argc)) return;
    uint32_t keys = site.keys;
    if (keys == 0 || vm->sp < keys + site.argc + 3u) {
        sjs_vm_fault(vm, "the program is damaged");
        return;
    }
    uint32_t base = vm->sp - site.argc - keys - 3u;
    uint32_t room = base + keys + 1u;
    sjs_value name = vm->stack[room];
    if (path_damaged(vm, base, keys) || name.kind != SJS_STRING || name.internal != SJS_INTERNAL_NONE) {
        sjs_vm_fault(vm, "the program is damaged");
        return;
    }
    vm->fuel_used += keys;
    sjs_value *place = path_receiver(vm, base, keys);
    if (place == NULL) return;
    if (place->kind == SJS_UNDEFINED || place->kind == SJS_NULL) {
        path_container(vm, (path_into){ *place, name, SJS_PATH_LOAD });
        return;
    }
    sjs_builtin_id id = sjs_mutation_of(sjs_name_of(SJS_AS_STRING(name)), *place);
    if (id == SJS_BUILTIN_NONE) {
        path_call_other(vm, place, site);
        return;
    }
    if (!sjs_value_unique(*place)) {
        sjs_vm_fault(vm, path_held);
        return;
    }
    if (id == SJS_METHOD_ARRAY_SORT) {
        /* MUTATION: a match result sorted in place keeps its index, input and
         * groups in JavaScript; toSorted's loop gives a plain array. */
        if (sjs_array_extra(*place)) {
            sjs_array_extra_fault(vm);
            return;
        }
        /* MUTATION: sort writes the elements of a frozen array of two or more. */
        if (SJS_AS_ARRAY(*place)->length >= 2u && !sjs_integrity_allows(vm, *place, SJS_INTEGRITY_FROZEN)) return;
        /* toSorted's loop takes the array over from its place, which holds
         * undefined until PATH_SORTED puts the sorted array back. */
        sjs_vm_release(vm, name);
        vm->stack[room] = *place;
        *place = sjs_undefined();
        vm->stack[room + 1u] = (sjs_value){ .kind = SJS_FUNCTION, .internal = SJS_INTERNAL_METHOD, .boolean = (uint32_t)id };
        enter_loop(vm, SJS_METHOD_ARRAY_TO_SORTED, (call_site){ room + 1u, site.argc, room });
        return;
    }
    sjs_leaf_call leaf = { 0 };
    leaf.receiver = *place;
    leaf.receiver_slot = place;
    leaf.args = &vm->stack[room + 2u];
    leaf.count = site.argc;
    if (sjs_mutation_call(vm, id, &leaf) != SJS_OK) {
        sjs_vm_release(vm, leaf.result);
        return;
    }
    vm->fuel_used += leaf.cost;
    bool chain = (site.flags & SJS_CALL_CHAIN) != 0;
    sjs_value result = leaf.result;
    if (sjs_mutation_returns_receiver(id) && !chain) {
        result = *place;
        sjs_ref(result);
    }
    path_give(vm, (path_result){ base, keys, chain }, result);
}

/* PATH_SORTED: [reference, keys, sorted]. The sorted array goes back in the
 * place the keys name, which held undefined while toSorted's loop had it;
 * then it is the result, as PATH_CALL gives one. */
__attribute__((noinline)) static void path_sorted(sjs_vm *vm, uint32_t keys, uint8_t flags) {
    if (keys == 0 || vm->sp < keys + 2u || path_damaged(vm, vm->sp - keys - 2u, keys)) {
        sjs_vm_fault(vm, "the program is damaged");
        return;
    }
    uint32_t base = vm->sp - keys - 2u;
    sjs_value *container = path_receiver(vm, base, keys - 1u);
    if (container == NULL) return;
    sjs_value *key = &vm->stack[base + keys];
    sjs_value *place = NULL;
    uint32_t index;
    if (container->kind == SJS_ARRAY && path_array_index(*key, &index) && index < SJS_AS_ARRAY(*container)->length) {
        place = &SJS_AS_ARRAY(*container)->items[index];
    } else if (container->kind == SJS_OBJECT && path_key_string(vm, key)) {
        sjs_entry *e = (sjs_entry *)sjs_object_find(SJS_AS_OBJECT(*container), sjs_string_view(SJS_AS_STRING(*key)));
        if (e != NULL) place = &e->value;
    }
    if (place == NULL || place->kind != SJS_UNDEFINED) {
        sjs_vm_fault(vm, "the program is damaged");
        return;
    }
    sjs_value sorted = vm->stack[vm->sp - 1u];
    vm->stack[vm->sp - 1u] = sjs_undefined();
    *place = sorted;
    bool chain = (flags & SJS_CALL_CHAIN) != 0;
    if (!chain) sjs_ref(sorted);
    path_give(vm, (path_result){ base, keys, chain }, sorted);
}

/* ------------------------------------------------------------------------
 * STRING_APPEND: + and a template extend, in place, a string nothing else
 * holds (README.md, "A value nothing else holds is updated in place"): the
 * left operand that a last read moved out of its variable, or one just made,
 * such as reduce's accumulator, so that building a string is amortized
 * linear. Not mutation: nothing else sees the string, so no program can
 * tell; rolling mutation back keeps it (docs/mutation.md). At the end of the
 * file, so that the code before it is laid out as it was.
 */

/* The part a join extends, plus one, or 0 for none: the first part, or the
 * second after an empty first one (a template's head, `${s}...`), when it is
 * a string that nothing else holds and that is not interned (the context's
 * table finds an interned string by its text, which must not change). */
static uint32_t appendable_part(sjs_values parts) {
    if (parts.count == 0) return 0;
    uint32_t first = parts.count > 1u && parts.ptr[0].kind == SJS_STRING && SJS_AS_STRING(parts.ptr[0])->length == 0 ? 1u : 0u;
    sjs_value v = parts.ptr[first];
    if (v.kind != SJS_STRING || !sjs_value_unique(v) || (v.as.heap->flags & SJS_STRING_INTERNED) != 0) return 0;
    return first + 1u;
}

/* The text of a primitive, as String gives it: a string's own, or written
 * into `room` (32 bytes), so a number appended allocates nothing. The
 * string methods that convert an argument (padStart, concat, ...) use it
 * too. Not for a BigInt, whose digits must be made (sjs_to_string). */
sjs_str sjs_primitive_text(sjs_value v, char *room) {
    switch (v.kind) {
    case SJS_STRING:
        return sjs_string_view(SJS_AS_STRING(v));
    case SJS_NUMBER:
        return (sjs_str){ room, sjs_number_format(v.as.number, room) };
    case SJS_BOOLEAN:
        return v.boolean ? SJS_STR("true") : SJS_STR("false");
    case SJS_NULL:
        return SJS_STR("null");
    default:
        return SJS_STR("undefined");
    }
}

/* concat's join, with the part appendable_part names extended by the parts
 * after it: the same text, faults and limit as concat, in its order. The
 * parts are on the stack; the one extended leaves it for the result. Fuel:
 * a unit per 64 bytes appended, the bytes this copies. */
__attribute__((noinline)) static sjs_result string_append(sjs_vm *vm, sjs_values parts, sjs_value *out) {
    uint32_t first = appendable_part(parts) - 1u;
    uint32_t at = (uint32_t)(parts.ptr - vm->stack) + first;
    sjs_string *s = SJS_AS_STRING(vm->stack[at]);
    char room[32];
    uint64_t length = s->length;
    for (uint32_t i = first + 1u; i < parts.count; i++) {
        if (!sjs_is_primitive(parts.ptr[i])) return sjs_vm_fault(vm, "staticjs turns only primitives into strings");
        if (parts.ptr[i].kind == SJS_BIGINT) return concat_copy(vm, parts, out); /* its digits are made */
        length += sjs_primitive_text(parts.ptr[i], room).length;
        if (length > vm->context->limits.max_string_bytes) return sjs_vm_fail(vm, SJS_ERR_LIMIT);
    }
    uint32_t extra = (uint32_t)(length - s->length);
    sjs_result r = sjs_string_reserve(vm->context, &s, extra);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    vm->stack[at].as.heap = &s->h;
    for (uint32_t i = first + 1u; i < parts.count; i++) sjs_string_extend(s, sjs_primitive_text(parts.ptr[i], room));
    vm->stack[at] = sjs_undefined();
    vm->fuel_used += extra / 64u;
    *out = sjs_heap_value(SJS_STRING, &s->h);
    return SJS_OK;
}

/* ** of two numbers, as V8 computes it (math.c); of anything else,
 * BigInts' ** or the faults of arithmetic. Out of line, at the end of the
 * file, as the BigInts' operations are. */
__attribute__((noinline)) static sjs_result power(sjs_vm *vm, sjs_value *out) {
    const sjs_value *pair = &vm->stack[vm->sp - 2];
    if (pair[0].kind != SJS_NUMBER || pair[1].kind != SJS_NUMBER) return bigint_binary(vm, SJS_OP_EXPONENT, out);
    *out = sjs_num(sjs_math_pow(pair[0].as.number, pair[1].as.number));
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * BigInts: + - * / % ** and unary - of two BigInts, and their comparison
 * with each other and with numbers, exactly. Out of line and at the end of
 * the file, so that the numbers' paths before them are laid out as they
 * were: each reaches this only where it would have faulted.
 */

/* The faults of arithmetic that is not on two numbers, and BigInt
 * arithmetic, on the two values on top of the stack. */
__attribute__((noinline)) static sjs_result bigint_binary(sjs_vm *vm, uint8_t operator, sjs_value *out) {
    const sjs_value *pair = &vm->stack[vm->sp - 2];
    bool a = pair[0].kind == SJS_BIGINT;
    bool b = pair[1].kind == SJS_BIGINT;
    if (a != b) return sjs_vm_fault(vm, "JavaScript throws a TypeError for arithmetic that mixes a BigInt with another value");
    if (!a) {
        if (operator == SJS_OP_ADD) return sjs_vm_fault(vm, "staticjs adds two numbers, or a string and a primitive");
        return sjs_vm_fault(vm, "arithmetic in staticjs is on numbers");
    }
    /* BIGINT_REUSE: an operand the stack holds alone may become the result,
     * written in place; its slot then gives its reference up. */
    sjs_bigint_op op = { .op = operator, .a = pair[0], .b = pair[1] };
    op.reuse = (uint8_t)((sjs_value_unique(pair[0]) ? SJS_BIGINT_REUSE_A : 0u) | (sjs_value_unique(pair[1]) ? SJS_BIGINT_REUSE_B : 0u));
    sjs_result r = sjs_bigint_binary(vm->context, &op);
    if (r == SJS_ERR_PROGRAM) return sjs_vm_fault(vm, op.fault);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    if (op.took != 0) vm->stack[vm->sp - (op.took == SJS_BIGINT_REUSE_A ? 2u : 1u)] = sjs_undefined();
    vm->fuel_used += op.cost;
    *out = op.result;
    return SJS_OK;
}

__attribute__((noinline)) static sjs_result bigint_unary(sjs_vm *vm, uint8_t operator, sjs_value *out) {
    sjs_value v = vm->stack[vm->sp - 1];
    if (v.kind != SJS_BIGINT) return sjs_vm_fault(vm, "arithmetic in staticjs is on numbers");
    if (operator != SJS_OP_NEGATE) return sjs_vm_fault(vm, "JavaScript throws a TypeError for unary + of a BigInt");
    sjs_bigint_op op = { .op = SJS_OP_NEGATE, .a = v, .reuse = sjs_value_unique(v) ? SJS_BIGINT_REUSE_A : 0u };
    sjs_result r = sjs_bigint_binary(vm->context, &op);
    if (r != SJS_OK) return sjs_vm_fail(vm, r);
    if (op.took != 0) vm->stack[vm->sp - 1u] = sjs_undefined();
    vm->fuel_used += op.cost;
    *out = op.result;
    return SJS_OK;
}

/* <, <=, > or >= of a BigInt and a BigInt or a number, exactly, as
 * JavaScript compares them: anything with NaN is false. */
__attribute__((noinline)) static int bigint_order(uint8_t operator, sjs_value a, sjs_value b) {
    int c;
    if (a.kind == SJS_BIGINT && b.kind == SJS_BIGINT) {
        c = sjs_bigint_compare(SJS_AS_BIGINT(a), SJS_AS_BIGINT(b));
    } else if (a.kind == SJS_BIGINT && b.kind == SJS_NUMBER) {
        c = sjs_bigint_compare_number(SJS_AS_BIGINT(a), b.as.number);
    } else if (a.kind == SJS_NUMBER && b.kind == SJS_BIGINT) {
        c = sjs_bigint_compare_number(SJS_AS_BIGINT(b), a.as.number);
        if (c != SJS_BIGINT_UNORDERED) c = -c;
    } else {
        return SJS_VM_UNDECIDED;
    }
    if (c == SJS_BIGINT_UNORDERED) return 0;
    return operator == SJS_OP_LESS ? c < 0 : operator == SJS_OP_LESS_EQUAL ? c <= 0
         : operator == SJS_OP_GREATER ? c > 0 : c >= 0;
}

__attribute__((noinline)) static sjs_result order_fault(sjs_vm *vm, sjs_value a, sjs_value b) {
    if (a.kind == SJS_BIGINT || b.kind == SJS_BIGINT) {
        return sjs_vm_fault(vm, "staticjs compares a BigInt with a BigInt or a number");
    }
    return sjs_vm_fault(vm, "staticjs compares two numbers or two strings");
}

/* ------------------------------------------------------------------------
 * Maps (README.md, "Maps"): their properties, spreading one, an array
 * pattern over one, and new Map. Out of line and at the end of the file, so
 * that the code before it is laid out as it was: each is reached only where
 * the code before faulted.
 */

/* A property of a value that is not an object, an array, a string or a set:
 * a regular expression's or a Temporal value's (their own files say), a
 * map's size, and undefined for a key that is not on the prototype of a
 * map or an iterator (a method read as a value faults, as for a set);
 * anything else faults. */
__attribute__((noinline)) static sjs_result property_other(sjs_vm *vm, sjs_member member, sjs_value *out) {
    sjs_value object = member.object;
    sjs_str name = sjs_string_view(member.key);
    if (object.kind == SJS_REGEXP) return sjs_regex_property(vm, member, out);
    if (object.kind == SJS_TEMPORAL) return sjs_temporal_property(vm, member, out);
    if (object.kind != SJS_MAP && object.kind != SJS_ITERATOR) {
        return fault_named(vm, "staticjs reads properties of objects, arrays, strings, sets, maps and iterators", name);
    }
    if (object.kind == SJS_MAP && sjs_str_equal(name, SJS_STR("size"))) {
        *out = sjs_num((double)(SJS_AS_ARRAY(object)->length / 2u));
        return SJS_OK;
    }
    if (on_prototype(object.kind, member.key)) {
        return fault_named(vm, "staticjs calls methods; it does not read them as values", name);
    }
    return SJS_OK;
}

/* `...m`: a map yields its entries, each a new [key, value] array; an
 * iterator what it yields, read once (sjs_iterator_items). */
__attribute__((noinline)) static sjs_result spread_other(sjs_vm *vm, sjs_value *array, sjs_value source) {
    sjs_array *entries;
    if (source.kind == SJS_ITERATOR) {
        if (sjs_iterator_items(vm, source, &entries) != SJS_OK) return SJS_ERR_PROGRAM;
    } else if (source.kind != SJS_MAP) {
        return sjs_vm_fault(vm, "... spreads arrays, sets, maps, iterators and strings");
    } else if (sjs_map_entries(vm, SJS_AS_ARRAY(source), &entries) != SJS_OK) {
        return SJS_ERR_PROGRAM;
    }
    sjs_result r = SJS_OK;
    for (uint32_t i = 0; r == SJS_OK && i < entries->length; i++) {
        sjs_ref(entries->items[i]);
        r = array_append(vm, array, entries->items[i]);
    }
    sjs_unref(vm->context, sjs_heap_value(SJS_ARRAY, &entries->h));
    return r;
}

/* How many arguments a spread of a map or an iterator gives, or its fault. */
__attribute__((noinline)) static bool spread_size(sjs_vm *vm, sjs_value source, uint64_t *total) {
    if (source.kind == SJS_ITERATOR) {
        *total += sjs_iterator_count(SJS_AS_ITERATOR(source));
        return true;
    }
    if (source.kind != SJS_MAP) {
        sjs_vm_fault(vm, "... spreads arrays, sets, maps, iterators and strings");
        return false;
    }
    *total += SJS_AS_ARRAY(source)->length / 2u;
    return true;
}

/* `const [[k, v]] = m`: an array pattern takes a map's entries, as its
 * iterator yields them, or what an iterator yields, read once; they replace
 * the map or the iterator on top of the stack, which only the pattern reads.
 * Anything else but an array, a set or a string faults. */
__attribute__((noinline)) static void pattern_other(sjs_vm *vm) {
    sjs_value *top = stack_top(vm);
    sjs_array *entries;
    if (top->kind == SJS_ITERATOR) {
        if (sjs_iterator_items(vm, *top, &entries) != SJS_OK) return;
    } else if (top->kind != SJS_MAP) {
        sjs_vm_fault(vm, "only an array, a set, a map, an iterator or a string can be destructured with [ ]");
        return;
    } else if (sjs_map_entries(vm, SJS_AS_ARRAY(*top), &entries) != SJS_OK) {
        return;
    }
    sjs_vm_release(vm, *top);
    *top = sjs_heap_value(SJS_ARRAY, &entries->h);
}

/* One [key, value] of new Map's argument into the map at `*map`: an array,
 * whose missing items are undefined. A key that is not a primitive faults:
 * JavaScript keys an object by its identity, which staticjs does not have. */
static sjs_result map_entry_put(sjs_vm *vm, sjs_value *map, sjs_value pair) {
    if (pair.kind != SJS_ARRAY) return sjs_vm_fault(vm, "an entry of new Map is a [key, value] array");
    sjs_map_entry entry = { sjs_array_at(pair, 0), sjs_array_at(pair, 1) };
    if (!sjs_is_primitive(entry.key)) return sjs_vm_fault(vm, "staticjs keys a map by primitives: JavaScript keys an object by its identity");
    sjs_ref(entry.key);
    sjs_ref(entry.value);
    sjs_array *m = SJS_AS_ARRAY(*map);
    sjs_result r = sjs_map_put(vm->context, &m, &entry);
    map->as.heap = &m->h;
    if (r == SJS_OK) return SJS_OK;
    sjs_unref(vm->context, entry.key);
    sjs_unref(vm->context, entry.value);
    return sjs_vm_fail(vm, r);
}

/* new Map(), new Map(undefined or null): empty; new Map(map): a copy; new
 * Map(entries): each [key, value] in order, a later key's value replacing an
 * earlier one's in its place, from an array or anything else a spread
 * takes (a set's members, a string's characters are not entries, and
 * fault). A unit of fuel per entry, and one per lookup. */
__attribute__((noinline)) static sjs_result new_map(sjs_vm *vm, sjs_values args, sjs_value *out) {
    sjs_value source = args.count > 0 ? args.ptr[0] : sjs_undefined();
    if (source.kind == SJS_MAP) {
        sjs_array *copy;
        sjs_result r = sjs_map_copy(vm->context, SJS_AS_ARRAY(source), &copy);
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        vm->fuel_used += copy->length / 2u;
        *out = sjs_heap_value(SJS_MAP, &copy->h);
        return SJS_OK;
    }
    sjs_value list = sjs_undefined();
    if (source.kind == SJS_ARRAY) {
        list = source;
        sjs_ref(list);
    } else if (source.kind != SJS_UNDEFINED && source.kind != SJS_NULL) {
        sjs_result r = sjs_array_new(vm->context, 4, &list);
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        if (spread_into(vm, &list, source) != SJS_OK) {
            sjs_unref(vm->context, list);
            return SJS_ERR_PROGRAM;
        }
    }
    uint32_t n = sjs_length(list);
    sjs_value map = sjs_undefined();
    sjs_array *made;
    sjs_result r = sjs_map_make(vm->context, n, &made);
    if (r != SJS_OK) {
        sjs_unref(vm->context, list);
        return sjs_vm_fail(vm, r);
    }
    map = sjs_heap_value(SJS_MAP, &made->h);
    vm->fuel_used += 2u * (uint64_t)n;
    for (uint32_t i = 0; r == SJS_OK && i < n; i++) r = map_entry_put(vm, &map, sjs_array_at(list, i));
    sjs_unref(vm->context, list);
    if (r != SJS_OK) {
        sjs_unref(vm->context, map);
        return SJS_ERR_PROGRAM;
    }
    *out = map;
    return SJS_OK;
}

/* A match result's extra property (SJS_ARRAY_EXTRA), referenced into `*out`;
 * false when it has none of that name. Out of sjs_property_get and at the
 * end of the file, so that the code before it is laid out as it was. */
__attribute__((noinline)) static bool array_extra_get(const sjs_array *a, sjs_str name, sjs_value *out) {
    const sjs_entry *e = sjs_object_find((const sjs_object *)(const void *)a->extra, name);
    if (e == NULL) return false;
    *out = e->value;
    sjs_ref(*out);
    return true;
}

/* ------------------------------------------------------------------------
 * Object.assign's puts (builtins.c calls them for a target nothing else
 * holds, and OBJECT_ASSIGN for a variable's), at the end of the file, so
 * that the code before it is laid out as it was.
 */

bool sjs_assign_puts(sjs_values sources) {
    for (uint32_t i = 0; i < sources.count; i++) {
        sjs_value s = sources.ptr[i];
        if ((s.kind == SJS_OBJECT || s.kind == SJS_ARRAY) && sjs_length(s) != 0) return true;
        if (sjs_array_extra(s)) return true;
        if (s.kind == SJS_STRING && SJS_AS_STRING(s)->length != 0) return true;
    }
    return false;
}

/* A string's characters, by UTF-16 index, as ToObject makes them its own
 * properties. A character outside the BMP is two of them, each half of it,
 * which a staticjs string cannot hold. */
static sjs_result assign_string(sjs_vm *vm, sjs_value *target, const sjs_string *s) {
    sjs_str text = sjs_string_view(s);
    uint32_t units = 0;
    uint32_t i = 0;
    vm->fuel_used += s->utf16_length;
    while (i < text.length) {
        uint32_t start = i;
        if (sjs_utf8_next(text, &i) >= 0x10000) return sjs_vm_fault(vm, "Object.assign would put half of a character outside the BMP, which a staticjs string cannot hold");
        sjs_string *key;
        sjs_result r = sjs_string_from_number(vm->context, (double)units, &key);
        if (r != SJS_OK) return sjs_vm_fail(vm, r);
        sjs_string *c;
        r = sjs_string_make(vm->context, (sjs_str){ text.ptr + start, i - start }, &c);
        if (r != SJS_OK) {
            sjs_unref(vm->context, sjs_heap_value(SJS_STRING, &key->h));
            return sjs_vm_fail(vm, r);
        }
        if (object_put(vm, target, (sjs_put){ key, sjs_heap_value(SJS_STRING, &c->h) }) != SJS_OK) return SJS_ERR_PROGRAM;
        units++;
    }
    return SJS_OK;
}

/* Each source's own enumerable properties, as a spread into an object puts
 * them (spread_properties): an object's keys, an array's indices, and none
 * of undefined, null, a number, a boolean, a BigInt, a set, a map, an
 * iterator, a function or a regular expression; and a string's characters.
 * No object holds the key __proto__, whose setter JavaScript's assign would
 * call (object_spread says why). */
sjs_result sjs_vm_object_assign(sjs_vm *vm, sjs_value *target, sjs_values sources) {
    for (uint32_t i = 0; i < sources.count; i++) {
        sjs_value s = sources.ptr[i];
        sjs_result r;
        if (s.kind == SJS_STRING) {
            r = assign_string(vm, target, SJS_AS_STRING(s));
        } else {
            if (s.kind == SJS_OBJECT || s.kind == SJS_ARRAY) vm->fuel_used += sjs_length(s);
            r = spread_properties(vm, target, s);
        }
        if (r != SJS_OK) return SJS_ERR_PROGRAM;
    }
    return SJS_OK;
}

/* ------------------------------------------------------------------------
 * Frozen and sealed values (docs/mutation.md, "Frozen and sealed values").
 * Object.freeze and Object.seal set a bit on the object or the array
 * (SJS_INTEGRITY_*), and a change that JavaScript would throw a TypeError
 * at faults: before a key or an element comes or goes on a sealed one, and
 * before one changes on a frozen one. The owned mutations call these
 * (MUTATION, where they do); so does Object.assign's ordinary call, which
 * is not mutation, so rolling mutation back keeps this section.
 */

bool sjs_integrity_allows(sjs_vm *vm, sjs_value v, uint8_t bit) {
    if ((v.kind != SJS_OBJECT && v.kind != SJS_ARRAY) || (v.as.heap->flags & bit) == 0) return true;
    sjs_vm_fault(vm, (v.as.heap->flags & SJS_INTEGRITY_FROZEN) != 0
                         ? "staticjs changes no key or element of a frozen object or array, where JavaScript throws a TypeError"
                         : "staticjs adds and removes no key or element of a sealed object or array, where JavaScript throws a TypeError");
    return false;
}

/* An index's key, as a string, in `buffer` (32 bytes). */
static sjs_str index_key(uint32_t index, char *buffer) {
    return (sjs_str){ buffer, sjs_number_format((double)index, buffer) };
}

bool sjs_assign_allowed(sjs_vm *vm, sjs_value target, sjs_values sources) {
    if (target.kind != SJS_OBJECT || (target.as.heap->flags & SJS_INTEGRITY) == 0) return true;
    if (!sjs_integrity_allows(vm, target, SJS_INTEGRITY_FROZEN)) return false;
    /* Sealed: each key put must be one it has, whose value a put replaces. */
    const sjs_object *o = SJS_AS_OBJECT(target);
    char buffer[32];
    for (uint32_t i = 0; i < sources.count; i++) {
        sjs_value s = sources.ptr[i];
        uint32_t n = 0;
        if (s.kind == SJS_OBJECT) n = SJS_AS_OBJECT(s)->count;
        else if (s.kind == SJS_ARRAY) n = SJS_AS_ARRAY(s)->length;
        else if (s.kind == SJS_STRING) n = SJS_AS_STRING(s)->utf16_length;
        vm->fuel_used += n;
        for (uint32_t k = 0; k < n; k++) {
            sjs_str key = s.kind == SJS_OBJECT ? sjs_string_view(SJS_AS_OBJECT(s)->entries[k].key) : index_key(k, buffer);
            if (sjs_object_find(o, key) == NULL) return sjs_integrity_allows(vm, target, SJS_INTEGRITY_SEALED);
        }
    }
    return true;
}
