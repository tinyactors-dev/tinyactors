/*
 * The evaluator's state, shared by vm.c and builtins.c. Private.
 */
#pragma once

#include "internal.h"

enum { FRAME_CODE = 1, FRAME_LOOP };

/*
 * A frame: a function running bytecode, or a built-in that calls a function
 * for each element (map, reduce, toSorted, ...). A loop frame owns the stack
 * slots from `base`: the receiver, the method, the callback, a second
 * argument, and its result so far.
 *
 * A loop whose callback is a closure keeps one frame for it while the loop
 * runs (sjs_vm_call_back): once `entered`, the callback's frame is at
 * `frames[the loop's index + 1]` and its slots at `base + LOOP_SLOTS`. While
 * the body runs, the frame is counted in `frame_count`; between elements it
 * is parked just above it, its slots still on the stack. Nothing pushes a
 * frame while the loop frame is on top, so the parked record stays as it
 * was. It holds no references of its own: cutting the stack frees all of it.
 * The slots keep an element's values from the body's return to the loop's
 * next step, which clears them (clear_callback); an evaluation paused in
 * between holds them that long.
 */
typedef struct {
    uint8_t kind;
    uint8_t phase;
    uint16_t builtin;
    uint32_t pc;
    uint32_t base;
    uint32_t bottom;      /* the stack height the frame leaves behind; a loop's
                           * kept callback frame leaves its own slots */
    uint32_t function;
    /* Loop frames: an iterator's map, filter or reduce, which runs as an
     * array's loop over what the iterator yields (late_start): its
     * callback gets the value and a counter, and map's and filter's array
     * becomes an iterator when the loop ends. In the padding before
     * `program`. */
    bool iterated;
    /* toSorted's loop, and so sort's, without a compare function: it
     * compares the elements' strings, which the second half of the scratch
     * holds (text_sort_step in builtins.c). Also in that padding. */
    bool sorts_text;
    sjs_program *program;
    union {
        sjs_closure *closure;        /* code frames: NULL for a program's main function */
        const sjs_string *shape_key; /* loop frames with a shape: its key (fn->shape_args[0]) */
    };
    sjs_position at;      /* loop frames: where the call was */
    uint32_t index;
    uint32_t length;
    uint32_t argc;        /* loop frames: the arguments the built-in was called with */
    bool entered;         /* loop frames: the callback is a closure, and its frame is kept */
    /* Loop frames: the callback's sjs_shape when this built-in has a C path
     * for it (docs/matcher.md, sections 7 and 11), or SJS_SHAPE_NONE; its
     * flag: for the comparators, descending; for PLUCK and KEY_COMPARE, the
     * key is on Object.prototype; and for KEY_COMPARE and ELEMENT_COMPARE,
     * the sjs_operator. */
    uint8_t shape;
    uint8_t shape_flag;
    uint8_t shape_op;
    uint32_t shape_guess;  /* loop frames with a shape: where the last object had the key */
    union {
        /* toSorted: a bottom-up merge sort over two scratch arrays (four
         * with sorts_text), of scratch_count values while scratch is set. */
        struct {
            uint32_t width;
            uint32_t left;
            uint32_t mid;
            uint32_t right;
            uint32_t i;
            uint32_t j;
            uint32_t k;
            uint32_t scratch_count;
        };
        /* KEY_COMPARE and ELEMENT_COMPARE: the literal, borrowed from the
         * callback's program, which LOOP_CALLBACK holds. */
        sjs_value shape_value;
    };
    sjs_value *scratch;    /* outside the union: sjs_loop_free reads it for every loop */
} vm_frame;

/* The shape fields sit in padding and share the closure's word: a frame is
 * 112 bytes on a 64-bit target, as it was before the shapes. */
_Static_assert(sizeof(vm_frame) <= 112, "vm_frame grew: check its field order");

#define VM_MEMBER_GUESSES 256u
/* Words of the evaluator's buffer for a regular expression's match run as
 * a leaf: one that needs more runs as a loop, its state allocated. */
#define SJS_REGEX_LEAF_WORDS 2048u

enum { VM_IDLE = 0, VM_RUNNING, VM_DONE, VM_FAULT };

struct sjs_vm {
    sjs_context *context;
    sjs_value *stack;
    uint32_t sp;
    uint32_t stack_capacity;
    vm_frame *frames;
    uint32_t frame_count;
    uint32_t frame_capacity;
    sjs_program *program;
    const sjs_value *globals;
    uint32_t global_count;
    uint64_t fuel_used;
    uint8_t status;
    uint32_t op_pc;       /* where the instruction running started */
    sjs_error_slot error;
    sjs_value result;
    /* GET_MEMBER's guesses: where the last object read at an instruction had
     * its key, by the instruction's offset. Programs are shared, so the
     * guesses live here; a guess is only a place to look first, checked
     * against the key, so instructions of other programs that share a slot
     * cost a scan and nothing else. */
    uint32_t member_guess[VM_MEMBER_GUESSES];
    /* Regular expressions: every match runs as a loop frame, and every
     * test by the automaton alone, without the cache of its steps (tests'
     * switches, sjs_vm_config); and the state of one short enough to run
     * whole as a leaf (regex_match.c). */
    bool no_regex_leaf;
    bool no_regex_dfa;
    uint64_t regex_leaf[SJS_REGEX_LEAF_WORDS / 2u];  /* aligned for the job's pointers */
    /* The host clock (sjs_vm_set_clock): what Temporal.Now reads, valid
     * when clock_set. */
    bool clock_set;
    sjs_epoch clock;
    /* Math.random's generator (sjs_vm_set_seed); its state all zero until
     * the host sets the seed. */
    sjs_random_generator random;
};

/* Loop frame slots, from base; a kept callback frame's slots follow them.
 * reduce and reduceRight move LOOP_OUTPUT, the accumulator, into the
 * callback's first argument while the callback runs, and the result back,
 * so that the callback holds the accumulator alone. */
#define LOOP_RECEIVER 0u
#define LOOP_CALLBACK 2u
#define LOOP_SECOND 3u
#define LOOP_OUTPUT 4u
#define LOOP_SLOTS 5u

/* Stop the evaluation: a fault at the current instruction. */
sjs_result sjs_vm_fault(sjs_vm *vm, const char *message);
/* A failed allocation or limit, as a fault. */
sjs_result sjs_vm_fail(sjs_vm *vm, sjs_result r);
/* Push takes over `v`'s reference. On a full stack it releases `v`, faults
 * (sjs_vm_stack_full) and gives false. */
bool sjs_vm_stack_full(sjs_vm *vm, sjs_value v);
static inline bool sjs_vm_push(sjs_vm *vm, sjs_value v) {
    if (vm->sp >= vm->stack_capacity) return sjs_vm_stack_full(vm, v);
    vm->stack[vm->sp++] = v;
    return true;
}
static inline sjs_value sjs_vm_pop(sjs_vm *vm) {
    return vm->stack[--vm->sp];
}
/* Release a stack value, whatever its internal tag. */
void sjs_vm_release(sjs_vm *vm, sjs_value v);

/* Arguments of a built-in that does not call back: all borrowed. A method
 * may take its receiver over when nothing else holds it (sjs_value_unique):
 * it moves the value out of `receiver_slot`, leaving undefined there. */
typedef struct {
    sjs_value receiver;
    sjs_value *receiver_slot;  /* the receiver's stack slot; NULL for a function */
    const sjs_value *args;
    uint32_t count;
    sjs_value result;      /* set by the built-in; owned */
    uint64_t cost;         /* fuel beyond the call's own unit */
    uint16_t loop;         /* with SJS_LEAF_LOOP: the loop the call goes on as */
} sjs_leaf_call;

/* What a leaf returns that hands its call to a loop (call->loop), which
 * starts from the call's slots as if the method looped (sjs_loop_start). */
#define SJS_LEAF_LOOP 2

sjs_result sjs_builtin_call(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
/* STRING_APPEND: the text of a primitive as String gives it: a string's own,
 * or written into `room` (32 bytes). Not for an object (sjs_is_primitive),
 * nor a BigInt. */
sjs_str sjs_primitive_text(sjs_value v, char *room);
/* A map's entries as a new array of [key, value] arrays, a unit of fuel
 * each. */
sjs_result sjs_map_entries(sjs_vm *vm, const sjs_array *map, sjs_array **out);
/* What an iterator yields, as an array: its own or a new one, a unit of fuel
 * each. An iterator yields its values once in JavaScript, so this faults
 * unless nothing but the caller's stack slot holds it (sjs_value_unique):
 * then nothing can read it again, and no program can tell. */
sjs_result sjs_iterator_items(sjs_vm *vm, sjs_value iterator, sjs_array **out);
/* MUTATION: a method that changes its receiver, but sort (a loop): the
 * receiver is the value of the variable at `receiver_slot`, which holds it
 * alone; the method updates the slot when the value moves. */
sjs_result sjs_mutation_call(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
/* Frozen and sealed values (docs/mutation.md, "Frozen and sealed values"):
 * false, after a fault, when `v` is an object or an array with `bit`
 * (SJS_INTEGRITY_SEALED before a key or an element comes or goes,
 * SJS_INTEGRITY_FROZEN before one changes), where JavaScript throws a
 * TypeError. */
bool sjs_integrity_allows(sjs_vm *vm, sjs_value v, uint8_t bit);
/* Object.freeze (SJS_INTEGRITY_FROZEN) or Object.seal (SEALED) of `v`: a
 * primitive or a value already so is left as it is; an object or an array
 * nothing else holds takes the bits; anything else faults. */
sjs_result sjs_integrity_set(sjs_vm *vm, sjs_value v, uint8_t level);

/* LOOP_AGAIN: the loop did a unit of work itself, without calling back
 * (a shape's C path), and stays on top to be stepped again. */
typedef enum { LOOP_CALL = 0, LOOP_DONE, LOOP_FAULT, LOOP_AGAIN } loop_step;

/* Start a loop frame whose slots are in place. */
sjs_result sjs_loop_start(sjs_vm *vm, vm_frame *frame);
/* Advance a loop frame. With LOOP_CALL it has called the callback: a
 * closure's frame is running, or a built-in's or a native's result is on
 * top of the stack. It sees the result there when it is next stepped. */
loop_step sjs_loop_step(sjs_vm *vm, vm_frame *frame);
void sjs_loop_free(sjs_vm *vm, vm_frame *frame);

/* Values side by side: arguments, parts of a template. */
typedef struct {
    const sjs_value *ptr;
    uint32_t count;
} sjs_values;

/* Object.assign: does any of its sources have a property to put? */
bool sjs_assign_puts(sjs_values sources);
/* Object.assign into `target`, which has something to put: false, after a
 * fault, when it is frozen, or sealed and a source puts a key it lacks. A
 * unit of fuel per key a sealed target looks up. */
bool sjs_assign_allowed(sjs_vm *vm, sjs_value target, sjs_values sources);
/* Object.assign's sources put, in order, into the object at `*target`,
 * which the caller holds alone and which may move: a key the object has
 * keeps its place, a new one goes in as a put does. A unit of fuel per
 * property. */
sjs_result sjs_vm_object_assign(sjs_vm *vm, sjs_value *target, sjs_values sources);

/* Call a loop's callback with `args` (borrowed). A closure runs in a frame
 * kept for the whole loop, its arguments written into its parameters; a
 * built-in or a native is called as any call is, its result on top. False on
 * a fault. */
bool sjs_vm_call_back(sjs_vm *vm, vm_frame *loop, sjs_values args);
/* Keep a frame for the loop's closure callback, as its first call does:
 * a shape's C path enters it at the step the bytecode would first have
 * called it, so that a running closure, a full frame stack or no room for its
 * slots faults there, alike. False on a fault. */
bool sjs_vm_enter_callback(sjs_vm *vm, vm_frame *loop);

/* The comparisons of the bytecode, shared with the shapes' C paths
 * (docs/matcher.md, 11.7): 1 or 0, or SJS_VM_UNDECIDED where the bytecode
 * faults. a === b, or two objects (identity, which staticjs lacks); a OP b
 * for <, <=, > and >=, on two numbers or two strings, or any other pair. */
#define SJS_VM_UNDECIDED (-1)
int sjs_vm_strict_equal(sjs_value a, sjs_value b);
int sjs_vm_order(uint8_t operator, sjs_value a, sjs_value b);

/* A property of a value: `object.key`. */
typedef struct {
    sjs_value object;
    const sjs_string *key;
} sjs_member;

/* JavaScript's [[Get]] of a property, for the values staticjs has. */
sjs_result sjs_property_get(sjs_vm *vm, sjs_member member, sjs_value *out);
/* ToNumber, for the values it does not need to parse. */
sjs_result sjs_vm_to_number(sjs_vm *vm, sjs_value v, double *out);

/* split's limit, with a string or a regular expression: ToUint32, and
 * 2^32 - 1 for undefined; false for a string, an object or a BigInt, which
 * JavaScript converts (or throws on), and the caller faults. */
static inline bool sjs_split_limit(sjs_value v, uint32_t *out) {
    switch (v.kind) {
    case SJS_UNDEFINED: *out = UINT32_MAX; return true;
    case SJS_NULL: *out = 0; return true;
    case SJS_BOOLEAN: *out = v.boolean ? 1u : 0u; return true;
    case SJS_NUMBER: *out = sjs_number_to_uint32(v.as.number); return true;
    default: return false;
    }
}

/* A property of a regular expression (regex_match.c): its source, its flags
 * and their getters, and lastIndex, which is 0; undefined for a name
 * RegExp.prototype lacks; a fault for one of its methods read as a value. */
sjs_result sjs_regex_property(sjs_vm *vm, sjs_member member, sjs_value *out);

/* What a regular expression method computes: re.test(s), s.search(re),
 * s.match(re), s.split(re, limit), s.replace(re, x) and replaceAll,
 * re.exec(s) and s.matchAll(re). */
enum {
    SJS_REGEX_OP_TEST = 0, SJS_REGEX_OP_SEARCH, SJS_REGEX_OP_MATCH, SJS_REGEX_OP_SPLIT, SJS_REGEX_OP_REPLACE,
    SJS_REGEX_OP_EXEC, SJS_REGEX_OP_MATCH_ALL
};

/* The fault for a use of an array with extra properties (sjs_array_extra)
 * that would show them or lose them; sjs_property_get reads them. */
sjs_result sjs_array_extra_fault(sjs_vm *vm);

typedef struct {
    sjs_leaf_call *call;
    uint8_t op;           /* SJS_REGEX_OP_* */
    bool all;             /* replaceAll, which requires g */
} sjs_regex_call_params;

/* A method's call: checked, then matched whole, or handed to its loop
 * (SJS_LEAF_LOOP) when the match is long or needs a callback. */
sjs_result sjs_regex_call(sjs_vm *vm, sjs_regex_call_params *params);
/* The loop of a regular expression method (SJS_REGEX_LOOP_FIRST on). */
sjs_result sjs_regex_loop_start(sjs_vm *vm, vm_frame *frame);
loop_step sjs_regex_loop_step(sjs_vm *vm, vm_frame *frame);

/* JSON.parse and JSON.stringify (json.c): leaves. */
sjs_result sjs_json_parse(sjs_vm *vm, sjs_leaf_call *call);
sjs_result sjs_json_stringify(sjs_vm *vm, sjs_leaf_call *call);

/* Temporal (temporal.c, docs/temporal.md): its functions and constructors
 * (SJS_BUILTIN_TEMPORAL_FIRST on), a Temporal value's methods
 * (SJS_METHOD_TEMPORAL_*), a property of one, and the callee of
 * `t.name(...)`: its method of the name, or a fault. */
sjs_result sjs_temporal_function(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
sjs_result sjs_temporal_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
sjs_result sjs_temporal_property(sjs_vm *vm, sjs_member member, sjs_value *out);
sjs_result sjs_temporal_callee(sjs_vm *vm, sjs_member member, sjs_value *out);

/* Date (date.c, docs/date.md): a Temporal kind's value of type
 * SJS_TEMPORAL_DATE. Its functions (new Date, Date called, Date.now,
 * Date.parse, Date.UTC) and its methods, which sjs_temporal_method hands it. */
sjs_result sjs_date_function(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
sjs_result sjs_date_method(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
/* MUTATION: a Date's setTime and UTC setters (SJS_METHOD_DATE_SET_*), on
 * a Date call->receiver_slot holds alone: its new time value, written in
 * place, is the result. */
sjs_result sjs_date_set(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);
