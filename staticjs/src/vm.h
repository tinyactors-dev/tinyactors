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
 */
typedef struct {
    uint8_t kind;
    uint8_t phase;
    uint16_t builtin;
    uint32_t pc;
    uint32_t base;
    uint32_t bottom;      /* the stack height the frame leaves behind */
    uint32_t function;
    sjs_program *program;
    sjs_closure *closure; /* NULL for a program's main function */
    sjs_position at;      /* loop frames: where the call was */
    uint32_t index;
    uint32_t length;
    uint32_t argc;        /* loop frames: the arguments of the callback to call */
    uint32_t pass;        /* loop frames: the arguments the callback can read; more are not pushed */
    bool entered;         /* loop frames: the callback passed the check that it is not running */
    /* toSorted: a bottom-up merge sort over two scratch arrays. */
    uint32_t width;
    uint32_t left;
    uint32_t mid;
    uint32_t right;
    uint32_t i;
    uint32_t j;
    uint32_t k;
    sjs_value *scratch;
    uint32_t scratch_count;
} vm_frame;

#define VM_MEMBER_GUESSES 256u

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
};

/* Loop frame slots, from base. reduce moves LOOP_OUTPUT, its accumulator,
 * into the callback's first argument while the callback runs, and the
 * result back, so that the callback holds the accumulator alone. */
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
    uint32_t cost;         /* fuel beyond the call's own unit */
} sjs_leaf_call;

sjs_result sjs_builtin_call(sjs_vm *vm, sjs_builtin_id id, sjs_leaf_call *call);

typedef enum { LOOP_CALL = 0, LOOP_DONE, LOOP_FAULT } loop_step;

/* Start a loop frame whose slots are in place. */
sjs_result sjs_loop_start(sjs_vm *vm, vm_frame *frame);
/* Advance a loop frame; with LOOP_CALL it has pushed a callee and frame->argc
 * arguments for the evaluator to call, and will see the result on top of the
 * stack when it is next stepped. */
loop_step sjs_loop_step(sjs_vm *vm, vm_frame *frame);
void sjs_loop_free(sjs_vm *vm, vm_frame *frame);

/* Values side by side: arguments, parts of a template. */
typedef struct {
    const sjs_value *ptr;
    uint32_t count;
} sjs_values;

/* A property of a value: `object.key`. */
typedef struct {
    sjs_value object;
    const sjs_string *key;
} sjs_member;

/* JavaScript's [[Get]] of a property, for the values staticjs has. */
sjs_result sjs_property_get(sjs_vm *vm, sjs_member member, sjs_value *out);
/* ToNumber, for the values it does not need to parse. */
sjs_result sjs_vm_to_number(sjs_vm *vm, sjs_value v, double *out);
