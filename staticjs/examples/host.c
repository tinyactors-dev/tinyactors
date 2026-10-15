/* host: a host that runs a plugin its users wrote to transform records.
 *
 * - a library module, compiled and evaluated once; its exports are a value;
 * - the plugin, an expression compiled once with the names the host binds
 *   (globals), evaluated for each record with that record's values;
 * - a native function, rate(), answering from the host's own tables;
 * - a second module importing the first by name, through a modules adapter;
 * - the clock, pushed by the host: the library never reads one.
 *
 *   ./host
 */
#include "staticjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *allocate(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment <= _Alignof(max_align_t)) return malloc(size);
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
}

static void release(void *self, const sjs_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const sjs_allocator_vtable heap = { allocate, release };

/* What every step needs: the context, one evaluator kept for every run, and
 * the library module's exports. */
typedef struct {
    sjs_context *context;
    sjs_vm *vm;
    sjs_value money;
} host;

static void fail(const char *what, const sjs_error *error) {
    fprintf(stderr, "%s: %u:%u: %.*s\n", what, error->at.line, error->at.column, (int)error->message.length,
            error->message.ptr);
    exit(1);
}

static void check(sjs_result result, const char *what) {
    if (result != SJS_OK) {
        fprintf(stderr, "%s: %d\n", what, (int)result);
        exit(1);
    }
}

static void print(host *h, sjs_value value) {
    sjs_writer *writer;
    check(sjs_writer_init(&writer, &(sjs_writer_config){ .context = h->context, .value = value }), "writer");
    char text[256];
    sjs_buffer buffer = { (uint8_t *)text, sizeof text, 0 };
    check(sjs_writer_write(writer, &buffer), "write");
    printf("%.*s\n", (int)buffer.length, text);
    sjs_writer_destroy(writer);
}

static sjs_program *compile(const sjs_compiler_config *config, const char *source) {
    sjs_program *program;
    sjs_error error = { 0 };
    sjs_compile_params params = { *config, { source, (uint32_t)strlen(source) }, &error, NULL };
    if (sjs_compile(&params, &program) != SJS_OK) fail("compile", &error);
    return program;
}

/* Evaluate a program with values for its globals; the caller owns the result. */
static sjs_result evaluate(host *h, const sjs_run_params *run, sjs_value *out) {
    check(sjs_vm_start(h->vm, run), "start");
    sjs_run status = sjs_vm_run(h->vm, 100000);
    if (status == SJS_RUN_DONE) return sjs_vm_take_result(h->vm, out);
    sjs_error error = { 0 };
    if (status == SJS_RUN_FAULT) sjs_vm_error(h->vm, &error);
    else sjs_vm_reset(h->vm); /* out of fuel: abandon the evaluation */
    printf("fault: %.*s", (int)error.message.length, error.message.ptr);
    if (error.subject.length > 0) printf(" (%.*s)", (int)error.subject.length, error.subject.ptr);
    putchar('\n');
    return SJS_ERR_PROGRAM;
}

/* The host's exchange rates to euros: the table rate() answers from. */
typedef struct {
    const char *currency;
    double euros;
} rate_row;

static const rate_row rates[] = { { "EUR", 1 }, { "GBP", 1.17 }, { "USD", 0.92 } };

/* rate(currency): one unit of a currency, in euros. The table is the
 * native's `self`; a native must not change anything a program can see. */
static sjs_result rate(void *self, sjs_native_call *call) {
    const rate_row *row = self;
    if (call->count == 1 && call->arguments[0].kind == SJS_STRING) {
        sjs_str code = sjs_string_text(call->arguments[0]);
        for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
            if (strlen(row[i].currency) == code.length && memcmp(row[i].currency, code.ptr, code.length) == 0) {
                call->result = sjs_number(row[i].euros);
                return SJS_OK;
            }
        }
    }
    call->fault = SJS_STR("rate() takes a currency the host has a rate for");
    return SJS_ERR_PROGRAM;
}

static const sjs_native_vtable rate_vtable = { rate };

/* The modules adapter: one module, "money", whose exports the host holds. */
static sjs_result find_module(void *self, sjs_str name, sjs_value *exports) {
    host *h = self;
    if (name.length != 5 || memcmp(name.ptr, "money", 5) != 0) return SJS_ERR_NOT_FOUND;
    *exports = h->money; /* borrowed: the program retains what it imports */
    return SJS_OK;
}

static const sjs_modules_vtable modules_vtable = { find_module };

/* A record as the host builds it: { id, amount, currency }. */
typedef struct {
    const char *id;
    double amount;
    const char *currency;
} order;

static sjs_value record(host *h, const order *o) {
    sjs_value r, id, currency;
    check(sjs_object_new(h->context, 3, &r), "record");
    check(sjs_string_new(h->context, (sjs_str){ o->id, (uint32_t)strlen(o->id) }, &id), "id");
    check(sjs_string_new(h->context, (sjs_str){ o->currency, (uint32_t)strlen(o->currency) }, &currency), "currency");
    check(sjs_object_put(h->context, &r, &(sjs_property){ SJS_STR("id"), id }), "put id");
    check(sjs_object_put(h->context, &r, &(sjs_property){ SJS_STR("amount"), sjs_number(o->amount) }), "put amount");
    check(sjs_object_put(h->context, &r, &(sjs_property){ SJS_STR("currency"), currency }), "put currency");
    return r;
}

int main(void) {
    host h = { 0 };
    check(sjs_context_init(&h.context, &(sjs_context_config){ { NULL, &heap }, SJS_LIMITS_DEFAULT }), "context");
    check(sjs_vm_init(&h.vm, &(sjs_vm_config){ .context = h.context }), "evaluator");

    /* 1. A library module: compiled and evaluated once. Its value is its
     * namespace, an object of its exports. */
    sjs_program *money = compile(&(sjs_compiler_config){ .context = h.context, .goal = SJS_GOAL_MODULE },
                                 "export const cents = (x) => Math.round(x * 100) / 100;\n"
                                 "export const size = (eur) => eur >= 100 ? \"large\" : eur >= 20 ? \"medium\" : \"small\";\n");
    if (evaluate(&h, &(sjs_run_params){ .program = money }, &h.money) != SJS_OK) return 1;
    sjs_program_release(money);

    /* 2. The plugin: an expression a user wrote, compiled once against the
     * names the host binds, then evaluated for each record. */
    static const sjs_str names[] = { SJS_STR("rate"), SJS_STR("cents"), SJS_STR("record") };
    sjs_program *plugin = compile(
                                  &(sjs_compiler_config){ .context = h.context, .goal = SJS_GOAL_EXPRESSION,
                                                          .globals = names, .global_count = 3 },
                                  "({ id: record.id, eur: cents(record.amount * rate(record.currency)) })");
    sjs_value rate_fn, cents;
    check(sjs_function_native(h.context, (sjs_native){ (void *)rates, &rate_vtable }, &rate_fn), "native");
    check(sjs_object_get(h.money, SJS_STR("cents"), &cents), "cents"); /* borrowed from the exports */
    static const order orders[] = { { "a1", 12, "USD" }, { "a2", 80, "GBP" }, { "a3", 5.5, "EUR" } };
    for (int i = 0; i < 3; i++) {
        sjs_value values[] = { rate_fn, cents, record(&h, &orders[i]) };
        sjs_value result;
        if (evaluate(&h, &(sjs_run_params){ plugin, values, 3 }, &result) == SJS_OK) {
            print(&h, result);
            sjs_release(h.context, result);
        }
        sjs_release(h.context, values[2]);
    }
    sjs_program_release(plugin);

    /* A native that refuses its arguments faults the evaluation. */
    sjs_program *misuse = compile(
                                  &(sjs_compiler_config){ .context = h.context, .goal = SJS_GOAL_EXPRESSION,
                                                          .globals = names, .global_count = 1 },
                                  "rate(\"JPY\")");
    sjs_value result;
    if (evaluate(&h, &(sjs_run_params){ misuse, &rate_fn, 1 }, &result) == SJS_OK) sjs_release(h.context, result);
    sjs_program_release(misuse);
    sjs_release(h.context, rate_fn);

    /* 3. A module importing "money" by name, through the adapter. */
    sjs_program *report = compile(
                                  &(sjs_compiler_config){ .context = h.context, .goal = SJS_GOAL_MODULE,
                                                          .modules = { &h, &modules_vtable } },
                                  "import { cents, size } from \"money\";\n"
                                  "export const sizes = [5.004, 42, 250].map((eur) => size(cents(eur)));\n");
    if (evaluate(&h, &(sjs_run_params){ .program = report }, &result) == SJS_OK) {
        print(&h, result);
        sjs_release(h.context, result);
    }
    sjs_program_release(report);

    /* 4. The clock: set by the host, advanced between runs, never read by
     * the library itself. 2024-01-31T10:00:00Z, then 90 minutes later. */
    sjs_program *now = compile(&(sjs_compiler_config){ .context = h.context, .goal = SJS_GOAL_EXPRESSION },
                               "Temporal.Now.plainDateTimeISO().toString()");
    check(sjs_vm_set_clock(h.vm, &(sjs_epoch){ 1706695200, 0 }), "clock");
    for (int i = 0; i < 2; i++) {
        if (evaluate(&h, &(sjs_run_params){ .program = now }, &result) == SJS_OK) {
            print(&h, result);
            sjs_release(h.context, result);
        }
        check(sjs_vm_advance_clock(h.vm, 90ull * 60u * 1000000000u), "advance");
    }
    sjs_program_release(now);

    sjs_release(h.context, h.money);
    sjs_vm_destroy(h.vm);
    if (sjs_context_live_bytes(h.context) != 0) return fprintf(stderr, "leaked\n"), 1;
    sjs_context_destroy(h.context);
    return 0;
}
