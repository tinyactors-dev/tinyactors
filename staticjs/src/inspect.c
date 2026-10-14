/* Inspection: what a paused evaluation is doing, and what a program holds
 * (staticjs.h, "Inspection"). Every function reads on demand; the
 * evaluator does nothing for them while it runs. */
#include "vm.h"
#include "shapes.h"

/* The name a built-in is called by: a function's namespace and name from
 * the function table, or a method's name from the table of names. A scan
 * of both tables, for inspection only. */
static void builtin_name(uint32_t id, sjs_frame *out) {
    if (id >= SJS_REGEX_LOOP_FIRST) {
        out->object = SJS_STR("RegExp");
        out->name = SJS_STR("match");
        return;
    }
    uint32_t count;
    const sjs_function_entry *functions = sjs_functions(&count);
    for (uint32_t i = 0; i < count; i++) {
        if (functions[i].id == id) {
            out->object = sjs_cstr(functions[i].space);
            out->name = sjs_cstr(functions[i].name);
            return;
        }
    }
    const sjs_name *names = sjs_names(&count);
    for (uint32_t i = 0; i < count; i++) {
        bool found = names[i].mutation == id;
        for (uint32_t k = 0; !found && k < sizeof names[i].methods / sizeof names[i].methods[0]; k++) {
            found = names[i].methods[k] == id;
        }
        if (found) {
            out->name = names[i].name;
            return;
        }
    }
}

/* A loop that passes over its elements in order, so that index of length
 * says how far it has gone: not a sort, whose index is bookkeeping, and not
 * a regular expression's loop. */
static bool walks_elements(const vm_frame *f) {
    return f->builtin < SJS_REGEX_LOOP_FIRST && f->builtin != SJS_METHOD_ARRAY_TO_SORTED &&
           f->builtin != SJS_METHOD_ARRAY_SORT && !f->sorts_text && f->index <= f->length;
}

uint32_t sjs_vm_frame_count(const sjs_vm *vm) {
    return vm == NULL || (vm->status != VM_RUNNING && vm->status != VM_FAULT) ? 0 : vm->frame_count;
}

sjs_result sjs_vm_frame(const sjs_vm *vm, uint32_t index, sjs_frame *out) {
    if (vm == NULL || out == NULL) return SJS_ERR_INVALID;
    if (index >= sjs_vm_frame_count(vm)) return SJS_ERR_NOT_FOUND;
    const vm_frame *f = &vm->frames[index];
    *out = (sjs_frame){ 0 };
    if (f->kind == FRAME_LOOP) {
        out->kind = SJS_FRAME_LOOP;
        out->at = f->at;
        builtin_name(f->builtin, out);
        if (walks_elements(f)) {
            out->index = f->index;
            out->length = f->length;
        }
        return SJS_OK;
    }
    out->kind = SJS_FRAME_FUNCTION;
    /* The innermost code frame of a paused evaluation runs its pc next; of
     * a faulted one, op_pc faulted. Any other frame waits at its pc, just
     * past the call it made. */
    bool innermost = index + 1u == vm->frame_count;
    uint32_t pc = f->pc;
    if (innermost && vm->status == VM_FAULT) pc = vm->op_pc;
    else if (!innermost && pc > 0) pc -= 1u;
    out->at = sjs_program_position(f->program, pc);
    return SJS_OK;
}

uint32_t sjs_program_function_count(const sjs_program *program) {
    return program == NULL ? 0 : program->function_count;
}

sjs_result sjs_program_function(const sjs_program *program, uint32_t index, sjs_function_summary *out) {
    if (program == NULL || out == NULL) return SJS_ERR_INVALID;
    if (index >= program->function_count) return SJS_ERR_NOT_FOUND;
    const sjs_function_info *fn = &program->functions[index];
    *out = (sjs_function_summary){ sjs_program_position(program, fn->code), fn->params, { "", 0 } };
    if (fn->shape != SJS_SHAPE_NONE) out->shape = sjs_cstr(sjs_shape_name(fn->shape));
    return SJS_OK;
}
