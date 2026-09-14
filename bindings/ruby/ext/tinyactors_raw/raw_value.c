#include "ruby.h"
#include "tinyactors.h"
#include "support.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct tar_value_domain tar_value_domain_t;
typedef struct tar_value_record tar_value_record_t;
struct tar_value_domain { uint32_t cookie; };
struct tar_value_record {
    uint32_t magic;
    uintptr_t handle;
    VALUE object;
    tar_value_domain_t *domain;
    size_t wrapper_refs;
    size_t core_refs;
    size_t retain_calls;
    size_t release_calls;
    unsigned int rooted;
    tar_value_record_t *next;
};
typedef struct { tar_value_record_t *record; } tar_value_t;
static tar_value_domain_t tar_domain = { 0x54415256U };
static tar_value_record_t *tar_value_records;
static uintptr_t tar_value_next_handle = 1u;
static VALUE tar_value_class;

static tar_value_record_t *tar_value_record(ta_value_t value) {
    tar_value_record_t *record;
    if (value.handle == 0u) return NULL;
    /* Never dereference an untrusted opaque handle. Handles are private,
     * monotonic registry IDs rather than native addresses. */
    for (record = tar_value_records; record; record = record->next)
        if (record->handle == value.handle &&
            record->magic == UINT32_C(0x54415652)) return record;
    return NULL;
}
static ta_value_t tar_value_handle(tar_value_record_t *record) { ta_value_t value; value.handle=record ? record->handle : 0u; return value; }
static void tar_value_mark(void *ptr) { tar_value_t *v=ptr; if (v && v->record) rb_gc_mark(v->record->object); }
static void tar_value_compact(void *ptr) { tar_value_t *v=ptr; if (v && v->record) v->record->object=rb_gc_location(v->record->object); }
static void tar_value_dispose(tar_value_record_t *record) {
    tar_value_record_t **slot;
    if (!record) return;
    for (slot = &tar_value_records; *slot; slot = &(*slot)->next) {
        if (*slot == record) { *slot = record->next; break; }
    }
    record->magic = 0u;
    record->handle = 0u;
    xfree(record);
}
static void tar_value_free(void *ptr) {
    tar_value_t *v=ptr;
    if (v && v->record) {
        if (v->record->wrapper_refs) v->record->wrapper_refs--;
        if (!v->record->wrapper_refs && !v->record->core_refs)
            tar_value_dispose(v->record);
    }
    xfree(ptr);
}
static size_t tar_value_memsize(const void *ptr) { return ptr ? sizeof(tar_value_t) : 0; }
static const rb_data_type_t tar_value_type={"TinyActors::Raw::Value",{tar_value_mark,tar_value_free,tar_value_memsize,tar_value_compact},0,0,RUBY_TYPED_FREE_IMMEDIATELY|RUBY_TYPED_WB_PROTECTED};
static VALUE tar_value_alloc(VALUE klass) { tar_value_t *v=ALLOC(tar_value_t); v->record=0; return TypedData_Wrap_Struct(klass,&tar_value_type,v); }
static tar_value_record_t *tar_value_record_new(VALUE object, tar_value_domain_t *domain, size_t core_refs) {
    tar_value_record_t *record;
    if (tar_value_next_handle == 0u) rb_raise(rb_eRangeError, "native value handle space exhausted");
    record = ALLOC(tar_value_record_t);
    *record = (tar_value_record_t){UINT32_C(0x54415652), tar_value_next_handle++, object, domain,
                                   0u, core_refs, 0u, 0u, 0u, tar_value_records};
    tar_value_records = record;
    return record;
}
static VALUE tar_value_initialize(VALUE self, VALUE object) {
    tar_value_t *v;
    tar_value_record_t *r;
    TypedData_Get_Struct(self,tar_value_t,&tar_value_type,v);
    if (v->record) rb_raise(rb_eTypeError, "TinyActors::Raw::Value is already initialized");
    r = tar_value_record_new(object, &tar_domain, 0u);
    r->wrapper_refs = 1u;
    v->record = r;
    rb_obj_freeze(self);
    return self;
}
static VALUE tar_value_object(VALUE self) { tar_value_t *v; TypedData_Get_Struct(self,tar_value_t,&tar_value_type,v); if (!v->record) rb_raise(rb_eRuntimeError,"released TinyActors::Raw::Value"); return v->record->object; }

ta_value_t tar_value_to_native(VALUE object) { tar_value_t *v; TypedData_Get_Struct(object,tar_value_t,&tar_value_type,v); if (!v->record) rb_raise(rb_eRuntimeError,"released TinyActors::Raw::Value"); return tar_value_handle(v->record); }
static void tar_value_retain(void *user, ta_value_t value) {
    tar_value_record_t *r=tar_value_record(value);
    if (!r || user != r->domain) return;
    /* A value can only be retained by the domain which admitted it.  The
     * core supplies the configured user pointer on every callback; do not
     * turn a foreign handle into a root merely because its bits resemble a
     * local record. */
    if (r->core_refs == (size_t)-1) return;
    ++r->retain_calls;
    if (r->core_refs++ == 0u) {
        rb_gc_register_address(&r->object);
        r->rooted = 1u;
    }
}
static void tar_value_release(void *user, ta_value_t value) {
    tar_value_record_t *r=tar_value_record(value);
    if (!r || user != r->domain || !r->core_refs) return;
    ++r->release_calls;
    if (--r->core_refs == 0u) {
        if (r->rooted) {
            rb_gc_unregister_address(&r->object);
            r->rooted = 0u;
        }
        if (!r->wrapper_refs) tar_value_dispose(r);
    }
}
static VALUE tar_value_dump(VALUE object) { return rb_marshal_dump(object,Qnil); }
static VALUE tar_value_load(VALUE bytes) { return rb_marshal_load(bytes); }
typedef struct { void *user; ta_value_t value; ta_value_t *out; } tar_snapshot_args_t;

static VALUE tar_value_snapshot_protected(VALUE argument) {
    tar_snapshot_args_t *args = (tar_snapshot_args_t *)argument;
    int raised = 0;
    VALUE dump, copy;
    tar_value_record_t *src=tar_value_record(args->value), *r;
    args->out->handle=0;
    if (!src || src->domain != args->user) rb_raise(rb_eArgError, "foreign native value domain");
    dump=tar_protected_call(tar_value_dump, src->object, &raised);
    if (raised) rb_jump_tag(raised);
    copy=tar_protected_call(tar_value_load, dump, &raised);
    if (raised) rb_jump_tag(raised);
    r=tar_value_record_new(copy, (tar_value_domain_t *)args->user, 1u);
    rb_gc_register_address(&r->object);
    r->rooted = 1u;
    *(args->out)=tar_value_handle(r);
    return Qnil;
}
static ta_status_t tar_value_snapshot(void *user, ta_value_t value, ta_value_t *out) {
    int state = 0;
    tar_snapshot_args_t args = { user, value, out };
    if (!out) return TA_INVALID;
    out->handle = 0;
    if (!tar_value_record(value) || tar_value_record(value)->domain != user)
        return TA_INVALID;
    rb_protect(tar_value_snapshot_protected, (VALUE)&args, &state);
    return state ? TA_EXECUTION_ERROR : TA_OK;
}
const ta_value_ops_t *tar_value_ops(void) { static const ta_value_ops_t ops={tar_value_retain,tar_value_release,tar_value_snapshot}; return &ops; }
void *tar_value_domain_user(void) { return &tar_domain; }

/* Private focused-test fixture. It calls only public TinyActors C entry
 * points, and is not part of Raw's public Ruby API. Later raw wrappers can
 * remove it when they provide equivalent native ownership coverage. */
typedef struct tar_value_fixture {
    ta_arena_t *arena;
    ta_system_t *system;
    ta_definition_t *definition;
} tar_value_fixture_t;
static tar_value_fixture_t tar_value_fixture;

static ta_status_t tar_fixture_acquire(void *user, const ta_layout_t *minimum,
                                       ta_chunk_t *out)
{
    uintptr_t address;
    void *raw;
    size_t alignment, size;
    (void)user;
    if (!minimum || !out || minimum->alignment == 0u) return TA_INVALID;
    alignment = minimum->alignment;
    size = minimum->size;
    if (alignment > (size_t)-1 - sizeof(void *) ||
        size > (size_t)-1 - alignment - sizeof(void *)) return TA_LIMIT;
    raw = malloc(size + alignment + sizeof(void *));
    if (!raw) return TA_NO_MEMORY;
    address = (uintptr_t)raw + sizeof(void *) + alignment - 1u;
    address &= ~(uintptr_t)(alignment - 1u);
    ((void **)address)[-1] = raw;
    out->data = (void *)address;
    out->layout = *minimum;
    return TA_OK;
}

static void tar_fixture_release(void *user, const ta_chunk_t *chunk)
{
    (void)user;
    if (chunk && chunk->data) free(((void **)chunk->data)[-1]);
}

static const ta_chunk_allocator_t tar_fixture_allocator = {
    NULL, tar_fixture_acquire, tar_fixture_release
};

static void tar_value_fixture_close(void)
{
    if (tar_value_fixture.definition) {
        ta_definition_release(tar_value_fixture.definition);
        tar_value_fixture.definition = NULL;
    }
    if (tar_value_fixture.system) {
        (void)ta_system_destroy(tar_value_fixture.system);
        tar_value_fixture.system = NULL;
    }
    if (tar_value_fixture.arena) {
        ta_arena_close(tar_value_fixture.arena);
        tar_value_fixture.arena = NULL;
    }
}

static VALUE tar_value_fixture_admit(VALUE self, VALUE object)
{
    ta_system_config_t config;
    ta_arena_options_t options;
    ta_chart_state_t states[2] = {{0}};
    ta_chart_t chart = {0};
    ta_definition_result_t result;
    ta_status_t status;
    (void)self;
    if (tar_value_fixture.system) rb_raise(rb_eRuntimeError, "native value fixture is busy");
    chart.code_owner = tar_value_to_native(object);
    ta_system_config_init(&config);
    config.data_model = ta_text_from_cstr("bytes");
    config.session_prefix = ta_text_from_cstr("rawvaluefixture");
    config.host_user = tar_value_domain_user();
    config.values = *tar_value_ops();
    options = config.arenas[TA_LIFETIME_SYSTEM];
    status = ta_arena_create(&tar_fixture_allocator, &options, &tar_value_fixture.arena);
    if (status != TA_OK) return INT2NUM(status);
    status = ta_system_create(tar_value_fixture.arena, &config, &tar_value_fixture.system);
    if (status != TA_OK) { tar_value_fixture_close(); return INT2NUM(status); }
    states[0].id = "idle";
    states[0].kind = TA_CHART_STATE;
    chart.name = "value-fixture";
    chart.data_model = "bytes";
    chart.data_layout.storage = TA_DATA_BYTES;
    chart.data_layout.bytes_layout.alignment = 1u;
    chart.states = states;
    status = ta_definition_build(tar_value_fixture.system, &chart, &result);
    if (status == TA_OK) tar_value_fixture.definition = result.definition;
    else tar_value_fixture_close();
    return INT2NUM(status);
}

static VALUE tar_value_fixture_failed_admit(VALUE self, VALUE object)
{
    ta_system_config_t config;
    ta_arena_options_t options;
    ta_chart_t chart = {0};
    ta_definition_result_t result;
    ta_status_t status;
    (void)self;
    (void)tar_value_to_native(object);
    ta_system_config_init(&config);
    config.data_model = ta_text_from_cstr("bytes");
    config.session_prefix = ta_text_from_cstr("rawvaluefailure");
    config.host_user = tar_value_domain_user();
    config.values = *tar_value_ops();
    options = config.arenas[TA_LIFETIME_SYSTEM];
    status = ta_arena_create(&tar_fixture_allocator, &options, &tar_value_fixture.arena);
    if (status != TA_OK) return INT2NUM(status);
    status = ta_system_create(tar_value_fixture.arena, &config, &tar_value_fixture.system);
    if (status == TA_OK) {
        /* No states is invalid, so the code owner must not be retained. */
        chart.name = "invalid-value-fixture";
        chart.data_model = "bytes";
        chart.code_owner = tar_value_to_native(object);
        chart.data_layout.storage = TA_DATA_BYTES;
        chart.data_layout.bytes_layout.alignment = 1u;
        status = ta_definition_build(tar_value_fixture.system, &chart, &result);
    }
    tar_value_fixture_close();
    return INT2NUM(status);
}

static VALUE tar_value_fixture_release(VALUE self)
{
    ta_status_t status = TA_OK;
    (void)self;
    if (tar_value_fixture.definition) ta_definition_release(tar_value_fixture.definition);
    tar_value_fixture.definition = NULL;
    if (tar_value_fixture.system) status = ta_system_destroy(tar_value_fixture.system);
    tar_value_fixture.system = NULL;
    if (tar_value_fixture.arena) ta_arena_close(tar_value_fixture.arena);
    tar_value_fixture.arena = NULL;
    return INT2NUM(status);
}

static VALUE tar_value_fixture_counts(VALUE self, VALUE object)
{
    tar_value_t *value;
    VALUE result;
    (void)self;
    TypedData_Get_Struct(object, tar_value_t, &tar_value_type, value);
    if (!value->record) rb_raise(rb_eRuntimeError, "released TinyActors::Raw::Value");
    result = rb_hash_new();
    rb_hash_aset(result, ID2SYM(rb_intern("retain")), SIZET2NUM(value->record->retain_calls));
    rb_hash_aset(result, ID2SYM(rb_intern("release")), SIZET2NUM(value->record->release_calls));
    rb_hash_aset(result, ID2SYM(rb_intern("core_refs")), SIZET2NUM(value->record->core_refs));
    return result;
}

static VALUE tar_value_fixture_snapshot(VALUE self, VALUE object)
{
    ta_value_t source, copy = {0};
    ta_status_t status;
    tar_value_record_t *record;
    VALUE result;
    (void)self;
    source = tar_value_to_native(object);
    status = tar_value_ops()->snapshot(tar_value_domain_user(), source, &copy);
    result = rb_hash_new();
    rb_hash_aset(result, ID2SYM(rb_intern("status")), INT2NUM(status));
    if (status == TA_OK) {
        record = tar_value_record(copy);
        if (!record) rb_raise(rb_eRuntimeError, "snapshot returned an unknown native value");
        /* The callback returned one owned root. Transfer that root to the
         * Ruby wrapper; it is not a retained mutable-object claim. */
        if (record->rooted) rb_gc_unregister_address(&record->object);
        record->core_refs = 0u;
        record->rooted = 0u;
        record->wrapper_refs = 1u;
        {
            tar_value_t *wrapped = ALLOC(tar_value_t);
            wrapped->record = record;
            rb_hash_aset(result, ID2SYM(rb_intern("value")),
                         TypedData_Wrap_Struct(tar_value_class, &tar_value_type, wrapped));
        }
    } else rb_hash_aset(result, ID2SYM(rb_intern("value")), Qnil);
    return result;
}

static VALUE tar_value_fixture_foreign_snapshot(VALUE self)
{
    ta_value_t foreign = {UINTPTR_MAX};
    ta_value_t copy = {0};
    VALUE result;
    (void)self;
    result = rb_hash_new();
    rb_hash_aset(result, ID2SYM(rb_intern("status")),
                 INT2NUM(tar_value_ops()->snapshot(tar_value_domain_user(), foreign, &copy)));
    rb_hash_aset(result, ID2SYM(rb_intern("value")), Qnil);
    return result;
}

void tar_value_shutdown(void) { tar_value_fixture_close(); }

void tar_init_value(VALUE raw) {
    ID id=rb_intern("Value");
    VALUE singleton;
    (void)raw;
    if (rb_const_defined(tar_mRaw,id)) {
        tar_value_class=rb_const_get(tar_mRaw,id);
        if (!RB_TYPE_P(tar_value_class,T_CLASS)) rb_raise(rb_eTypeError,"TinyActors::Raw::Value must be a class");
    } else {
        tar_value_class=rb_define_class_under(tar_mRaw,"Value",rb_cObject);
        rb_define_alloc_func(tar_value_class,tar_value_alloc);
        rb_define_method(tar_value_class,"initialize",tar_value_initialize,1);
        rb_define_method(tar_value_class,"object",tar_value_object,0);
    }
    singleton = rb_singleton_class(tar_value_class);
    rb_define_private_method(singleton, "__test_admit", tar_value_fixture_admit, 1);
    rb_define_private_method(singleton, "__test_failed_admit", tar_value_fixture_failed_admit, 1);
    rb_define_private_method(singleton, "__test_release_admission", tar_value_fixture_release, 0);
    rb_define_private_method(singleton, "__test_counts", tar_value_fixture_counts, 1);
    rb_define_private_method(singleton, "__test_snapshot", tar_value_fixture_snapshot, 1);
    rb_define_private_method(singleton, "__test_foreign_snapshot", tar_value_fixture_foreign_snapshot, 0);
}
