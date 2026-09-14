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
    tar_value_record_t *next;
};
typedef struct { tar_value_record_t *record; } tar_value_t;
typedef struct { unsigned char unused; } tar_value_registry_data_t;
static tar_value_domain_t tar_domain = { 0x54415256U };
static tar_value_record_t *tar_value_records;
static uintptr_t tar_value_next_handle = 1u;
static VALUE tar_value_class;
static unsigned int tar_value_class_rooted;
static VALUE tar_value_root_registry = Qnil;
static size_t tar_fixture_retain_total;
static size_t tar_fixture_release_total;
static size_t tar_fixture_snapshot_total;
static size_t tar_fixture_live_core_refs;
static unsigned int tar_fixture_tracking;
static size_t tar_fixture_seen_events;
static uintptr_t tar_fixture_last_event_handle;

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
static void tar_value_mark_object(VALUE object)
{
    if (!RB_SPECIAL_CONST_P(object)) rb_gc_mark_movable(object);
}

static void tar_value_registry_mark(void *ptr)
{
    tar_value_record_t *record;
    (void)ptr;
    for (record = tar_value_records; record; record = record->next)
        if (record->core_refs) tar_value_mark_object(record->object);
}

static void tar_value_registry_compact(void *ptr)
{
    tar_value_record_t *record;
    (void)ptr;
    for (record = tar_value_records; record; record = record->next)
        if (record->core_refs && !RB_SPECIAL_CONST_P(record->object)) {
            record->object = rb_gc_location(record->object);
        }
}

static void tar_value_registry_free(void *ptr) { xfree(ptr); }
static size_t tar_value_registry_memsize(const void *ptr)
{
    return ptr ? sizeof(tar_value_registry_data_t) : 0u;
}
static const rb_data_type_t tar_value_registry_type = {
    "TinyActors::Raw::ValueRootRegistry",
    { tar_value_registry_mark, tar_value_registry_free, tar_value_registry_memsize,
      tar_value_registry_compact },
    0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED
};

static void tar_value_mark(void *ptr)
{
    tar_value_t *value = (tar_value_t *)ptr;
    if (value && value->record && !value->record->core_refs)
        tar_value_mark_object(value->record->object);
}

static void tar_value_compact(void *ptr)
{
    tar_value_t *value = (tar_value_t *)ptr;
    if (value && value->record && !value->record->core_refs &&
        !RB_SPECIAL_CONST_P(value->record->object))
        value->record->object = rb_gc_location(value->record->object);
}
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
                                   0u, core_refs, 0u, 0u, tar_value_records};
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

ta_value_t tar_value_to_native(VALUE object)
{
    tar_value_t *value;
    if (NIL_P(object)) return (ta_value_t){0};
    TypedData_Get_Struct(object, tar_value_t, &tar_value_type, value);
    if (!value->record) rb_raise(rb_eRuntimeError, "released TinyActors::Raw::Value");
    return tar_value_handle(value->record);
}
static void tar_value_retain(void *user, ta_value_t value) {
    tar_value_record_t *r=tar_value_record(value);
    if (!r || user != r->domain) return;
    /* A value can only be retained by the domain which admitted it.  The
     * core supplies the configured user pointer on every callback; do not
     * turn a foreign handle into a root merely because its bits resemble a
     * local record. */
    if (r->core_refs == (size_t)-1) return;
    ++r->retain_calls;
    if (tar_fixture_tracking) {
        ++tar_fixture_retain_total;
        ++tar_fixture_live_core_refs;
    }
    ++r->core_refs;
}
static void tar_value_release(void *user, ta_value_t value) {
    tar_value_record_t *r=tar_value_record(value);
    if (!r || user != r->domain) return;
    ++r->release_calls;
    if (tar_fixture_tracking) {
        ++tar_fixture_release_total;
    }
    if (!r->core_refs) return;
    if (tar_fixture_tracking && tar_fixture_live_core_refs) --tar_fixture_live_core_refs;
    if (--r->core_refs == 0u) {
        if (!r->wrapper_refs) tar_value_dispose(r);
    }
}
static VALUE tar_value_dump(VALUE object) { return rb_marshal_dump(object,Qnil); }
static VALUE tar_value_load(VALUE bytes) { return rb_marshal_load(bytes); }

static int tar_value_immutable_shared(VALUE object)
{
    if (RB_SPECIAL_CONST_P(object) || RB_INTEGER_TYPE_P(object) ||
        RB_TYPE_P(object, T_FLOAT) || RB_TYPE_P(object, T_SYMBOL)) return 1;
    if (RB_TYPE_P(object, T_STRING) && OBJ_FROZEN(object)) return 1;
    if ((RB_TYPE_P(object, T_ARRAY) || RB_TYPE_P(object, T_HASH) ||
         RB_TYPE_P(object, T_STRUCT)) && OBJ_FROZEN(object)) return 1;
    return 0;
}

static void tar_value_collect_reachable(VALUE object, VALUE seen);

static int tar_value_collect_hash_entry(VALUE key, VALUE value, VALUE seen)
{
    tar_value_collect_reachable(key, seen);
    tar_value_collect_reachable(value, seen);
    return ST_CONTINUE;
}

static void tar_value_collect_reachable(VALUE object, VALUE seen)
{
    VALUE id, child, variables;
    long i, length;

    if (RB_SPECIAL_CONST_P(object)) return;
    id = rb_obj_id(object);
    if (rb_hash_lookup2(seen, id, Qundef) != Qundef) return;
    rb_hash_aset(seen, id, Qtrue);

    if (RB_TYPE_P(object, T_ARRAY)) {
        length = RARRAY_LEN(object);
        for (i = 0; i < length; ++i)
            tar_value_collect_reachable(RARRAY_AREF(object, i), seen);
        goto collect_instance_variables;
    }
    if (RB_TYPE_P(object, T_HASH)) {
        rb_hash_foreach(object, tar_value_collect_hash_entry, seen);
        child = rb_funcall(object, rb_intern("default"), 0);
        if (!NIL_P(child)) tar_value_collect_reachable(child, seen);
        goto collect_instance_variables;
    }
    if (RB_TYPE_P(object, T_STRUCT)) {
        length = NUM2LONG(rb_struct_size(object));
        for (i = 0; i < length; ++i)
            tar_value_collect_reachable(rb_struct_aref(object, LONG2NUM(i)), seen);
        goto collect_instance_variables;
    }

collect_instance_variables:
    variables = rb_obj_instance_variables(object);
    length = RARRAY_LEN(variables);
    for (i = 0; i < length; ++i) {
        ID name = SYM2ID(RARRAY_AREF(variables, i));
        tar_value_collect_reachable(rb_ivar_get(object, name), seen);
    }
}

static int tar_value_has_shared_reachable(VALUE object, VALUE source_seen);

typedef struct {
    VALUE source_seen;
    int shared;
} tar_value_shared_hash_args_t;

static int tar_value_has_shared_hash_entry(VALUE key, VALUE value, VALUE argument)
{
    tar_value_shared_hash_args_t *args = (tar_value_shared_hash_args_t *)argument;
    if (tar_value_has_shared_reachable(key, args->source_seen) ||
        tar_value_has_shared_reachable(value, args->source_seen)) {
        args->shared = 1;
        return ST_STOP;
    }
    return ST_CONTINUE;
}

static int tar_value_has_shared_reachable(VALUE object, VALUE source_seen)
{
    VALUE id, child, variables;
    long i, length;

    if (RB_SPECIAL_CONST_P(object)) return 0;
    id = rb_obj_id(object);
    if (rb_hash_lookup2(source_seen, id, Qundef) != Qundef &&
        !tar_value_immutable_shared(object)) return 1;

    if (RB_TYPE_P(object, T_ARRAY)) {
        length = RARRAY_LEN(object);
        for (i = 0; i < length; ++i)
            if (tar_value_has_shared_reachable(RARRAY_AREF(object, i), source_seen)) return 1;
        goto check_instance_variables;
    }
    if (RB_TYPE_P(object, T_HASH)) {
        tar_value_shared_hash_args_t args = { source_seen, 0 };
        rb_hash_foreach(object, tar_value_has_shared_hash_entry, (VALUE)&args);
        if (args.shared) return 1;
        child = rb_funcall(object, rb_intern("default"), 0);
        if (!NIL_P(child) && tar_value_has_shared_reachable(child, source_seen)) return 1;
        goto check_instance_variables;
    }
    if (RB_TYPE_P(object, T_STRUCT)) {
        length = NUM2LONG(rb_struct_size(object));
        for (i = 0; i < length; ++i)
            if (tar_value_has_shared_reachable(rb_struct_aref(object, LONG2NUM(i)), source_seen)) return 1;
        goto check_instance_variables;
    }

check_instance_variables:
    variables = rb_obj_instance_variables(object);
    length = RARRAY_LEN(variables);
    for (i = 0; i < length; ++i) {
        ID name = SYM2ID(RARRAY_AREF(variables, i));
        if (tar_value_has_shared_reachable(rb_ivar_get(object, name), source_seen)) return 1;
    }
    return 0;
}

static void tar_value_require_isolated(VALUE source, VALUE copy)
{
    VALUE source_seen = rb_hash_new();
    if (RB_TYPE_P(source, T_DATA) || RB_TYPE_P(copy, T_DATA))
        rb_raise(rb_eArgError, "native value snapshot has opaque mutable state");
    tar_value_collect_reachable(source, source_seen);
    if (tar_value_has_shared_reachable(copy, source_seen))
        rb_raise(rb_eArgError, "native value snapshot shares mutable state");
}
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
    if (rb_obj_class(copy) != rb_obj_class(src->object))
        rb_raise(rb_eArgError, "native value snapshot changed value type");
    tar_value_require_isolated(src->object, copy);
    r=tar_value_record_new(copy, (tar_value_domain_t *)args->user, 1u);
    if (tar_fixture_tracking) {
        ++tar_fixture_snapshot_total;
        ++tar_fixture_live_core_refs;
    }
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
    if (state) rb_set_errinfo(Qnil);
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
    ta_actor_ref_t actor;
} tar_value_fixture_t;
static tar_value_fixture_t tar_value_fixture;
static tar_value_domain_t tar_foreign_domain = { 0x46565247U };

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

static ta_status_t tar_fixture_native_create(ta_context_t *context, ta_value_t seed,
                                              ta_value_t *out_state)
{
    tar_value_record_t *record;
    (void)context;
    (void)seed;
    if (!out_state) return TA_INVALID;
    record = tar_value_record_new(Qnil, &tar_domain, 0u);
    *out_state = tar_value_handle(record);
    tar_value_retain(tar_value_domain_user(), *out_state);
    return TA_OK;
}

static ta_status_t tar_fixture_native_bind_system(ta_context_t *context,
                                                  const ta_system_values_t *values)
{
    (void)context;
    (void)values;
    return TA_OK;
}

static ta_status_t tar_fixture_native_bind_event(ta_context_t *context,
                                                 const ta_event_t *event)
{
    (void)context;
    if (event && event->data.kind == TA_PAYLOAD_NATIVE) {
        ++tar_fixture_seen_events;
        tar_fixture_last_event_handle = event->data.as.native.handle;
    }
    return TA_OK;
}

static ta_status_t tar_fixture_native_run(ta_context_t *context,
                                          ta_action_kind_t action_kind, ta_code_t code)
{
    ta_context_view_t view;
    (void)action_kind;
    (void)code;
    ta_context_inspect(context, &view);
    if (view.system && view.system->event &&
        view.system->event->data.kind == TA_PAYLOAD_NATIVE) {
        ++tar_fixture_seen_events;
        tar_fixture_last_event_handle = view.system->event->data.as.native.handle;
    }
    return TA_OK;
}

static ta_status_t tar_value_fixture_close_status(void)
{
    ta_status_t status = TA_OK;
    if (tar_value_fixture.actor.id != 0u && tar_value_fixture.system) {
        status = ta_actor_stop(tar_value_fixture.system, tar_value_fixture.actor);
        if (status == TA_OK) {
            ta_run_options_t options = {0, {10000u, 100u}};
            status = ta_system_run(tar_value_fixture.system, &options,
                                   &(ta_run_result_t){0});
        }
        if (status == TA_OK)
            status = ta_actor_release(tar_value_fixture.system, tar_value_fixture.actor);
        if (status == TA_OK) tar_value_fixture.actor = (ta_actor_ref_t){0};
    }
    if (tar_value_fixture.definition) {
        ta_definition_release(tar_value_fixture.definition);
        tar_value_fixture.definition = NULL;
    }
    if (tar_value_fixture.system) {
        ta_status_t destroy_status = ta_system_destroy(tar_value_fixture.system);
        if (status == TA_OK) status = destroy_status;
        if (destroy_status != TA_OK) return status;
        tar_value_fixture.system = NULL;
    }
    if (tar_value_fixture.arena) {
        ta_arena_close(tar_value_fixture.arena);
        tar_value_fixture.arena = NULL;
    }
    return status;
}

static void tar_value_fixture_close(void)
{
    (void)tar_value_fixture_close_status();
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

static VALUE tar_value_fixture_failed_admit(VALUE self, VALUE object, VALUE invalid_object)
{
    ta_system_config_t config;
    ta_arena_options_t options;
    ta_chart_state_t states[2] = {{0}};
    ta_chart_t chart = {0};
    ta_definition_result_t result;
    ta_parameter_t parameters[2] = {{0}};
    ta_spawn_options_t spawn = {0};
    ta_value_t invalid;
    ta_status_t status;
    (void)self;
    tar_fixture_retain_total = 0u;
    tar_fixture_release_total = 0u;
    tar_fixture_snapshot_total = 0u;
    tar_fixture_live_core_refs = 0u;
    tar_fixture_seen_events = 0u;
    tar_fixture_last_event_handle = 0u;
    tar_fixture_tracking = 1u;
    invalid = tar_value_to_native(invalid_object);
    ta_system_config_init(&config);
    config.data_model = ta_text_from_cstr("bytes");
    config.session_prefix = ta_text_from_cstr("rawvaluefailure");
    config.host_user = tar_value_domain_user();
    config.values = *tar_value_ops();
    options = config.arenas[TA_LIFETIME_SYSTEM];
    status = ta_arena_create(&tar_fixture_allocator, &options, &tar_value_fixture.arena);
    if (status != TA_OK) {
        tar_fixture_tracking = 0u;
        return INT2NUM(status);
    }
    status = ta_system_create(tar_value_fixture.arena, &config, &tar_value_fixture.system);
    if (status == TA_OK) {
        states[0].id = "idle";
        states[0].kind = TA_CHART_STATE;
        chart.name = "failed-value-fixture";
        chart.data_model = "bytes";
        chart.code_owner = tar_value_to_native(object);
        chart.data_layout.storage = TA_DATA_BYTES;
        chart.data_layout.bytes_layout.alignment = 1u;
        chart.states = states;
        status = ta_definition_build(tar_value_fixture.system, &chart, &result);
    }
    if (status == TA_OK) {
        tar_value_fixture.definition = result.definition;
        parameters[0].name = ta_text_from_cstr("valid");
        parameters[0].value = tar_value_to_native(object);
        parameters[1].name = ta_text_from_cstr("invalid");
        parameters[1].value = invalid;
        spawn.definition = tar_value_fixture.definition;
        spawn.parameters = parameters;
        spawn.parameter_count = 2u;
        status = ta_actor_spawn(tar_value_fixture.system, &spawn,
                                &tar_value_fixture.actor);
        if (status == TA_OK) {
            /* This is a fault-injection guard. A successful admission would
             * own a live actor and must be cleaned through the normal path. */
            (void)tar_value_fixture_close_status();
            status = TA_EXECUTION_ERROR;
        }
    }
    tar_value_fixture_close();
    tar_fixture_tracking = 0u;
    return INT2NUM(status);
}

static VALUE tar_value_fixture_queue_start(VALUE self, VALUE object)
{
    ta_value_t source;
    ta_system_config_t config;
    ta_arena_options_t options;
    ta_chart_state_t states[2] = {{0}};
    ta_chart_transition_t transitions[2] = {{0}};
    ta_action_t actions[1] = {{0}};
    ta_chart_t chart = {0};
    ta_definition_result_t definition_result;
    ta_run_options_t run_options = {0, {1000u, 100u}};
    ta_run_result_t run_result;
    ta_post_options_t delivery = {0};
    ta_status_t status;
    (void)self;
    if (tar_value_fixture.system)
        rb_raise(rb_eRuntimeError, "native value fixture is busy");
    source = tar_value_to_native(object);

    tar_fixture_retain_total = 0u;
    tar_fixture_release_total = 0u;
    tar_fixture_snapshot_total = 0u;
    tar_fixture_live_core_refs = 0u;
    tar_fixture_seen_events = 0u;
    tar_fixture_last_event_handle = 0u;
    tar_fixture_tracking = 1u;

    ta_system_config_init(&config);
    config.data_model = ta_text_from_cstr("native");
    config.session_prefix = ta_text_from_cstr("rawvaluequeue");
    config.host_user = tar_value_domain_user();
    config.values = *tar_value_ops();
    config.data.create = tar_fixture_native_create;
    config.data.bind_system = tar_fixture_native_bind_system;
    config.data.bind_event = tar_fixture_native_bind_event;
    config.data.run = tar_fixture_native_run;
    options = config.arenas[TA_LIFETIME_SYSTEM];
    status = ta_arena_create(&tar_fixture_allocator, &options, &tar_value_fixture.arena);
    if (status == TA_OK)
        status = ta_system_create(tar_value_fixture.arena, &config, &tar_value_fixture.system);
    if (status != TA_OK) {
        (void)tar_value_fixture_close_status();
        tar_fixture_tracking = 0u;
        return INT2NUM(status);
    }

    actions[0].kind = TA_ACTION_CUSTOM;
    actions[0].as.custom.code = 1u;
    transitions[0].event = "PING";
    transitions[0].content.actions = actions;
    transitions[0].content.count = 1u;
    states[0].id = "idle";
    states[0].kind = TA_CHART_STATE;
    states[0].transitions = transitions;
    chart.name = "value-queue-fixture";
    chart.data_model = "native";
    chart.states = states;
    status = ta_definition_build(tar_value_fixture.system, &chart, &definition_result);
    if (status == TA_OK) {
        tar_value_fixture.definition = definition_result.definition;
        status = ta_actor_spawn(tar_value_fixture.system,
                                &(ta_spawn_options_t){.definition = tar_value_fixture.definition},
                                &tar_value_fixture.actor);
    }
    if (status == TA_OK)
        status = ta_system_run(tar_value_fixture.system, &run_options, &run_result);
    if (status == TA_OK) {
        delivery.target.kind = TA_TARGET_INSTANCE;
        delivery.target.instance = tar_value_fixture.actor;
        delivery.message.name = ta_text_from_cstr("PING");
        delivery.message.data.kind = TA_PAYLOAD_NATIVE;
        delivery.message.data.as.native = source;
        status = ta_post(tar_value_fixture.system, &delivery, NULL);
    }
    if (status != TA_OK) {
        (void)tar_value_fixture_close_status();
        tar_fixture_tracking = 0u;
    }
    return INT2NUM(status);
}

static VALUE tar_value_fixture_queue_step(VALUE self)
{
    ta_run_options_t options = {0, {1u, 1u}};
    ta_run_result_t result;
    (void)self;
    if (!tar_value_fixture.system) rb_raise(rb_eRuntimeError, "native value fixture is closed");
    return INT2NUM(ta_system_run(tar_value_fixture.system, &options, &result));
}

static VALUE tar_value_fixture_queue_replace(VALUE self)
{
    ta_post_options_t delivery = {0};
    ta_status_t status;
    ta_run_options_t options = {0, {2u, 1u}};
    ta_run_result_t result;
    (void)self;
    if (!tar_value_fixture.system) rb_raise(rb_eRuntimeError, "native value fixture is closed");
    delivery.target.kind = TA_TARGET_INSTANCE;
    delivery.target.instance = tar_value_fixture.actor;
    delivery.message.name = ta_text_from_cstr("UNMATCHED");
    delivery.message.data.kind = TA_PAYLOAD_NATIVE;
    delivery.message.data.as.native.handle = tar_fixture_last_event_handle;
    status = ta_post(tar_value_fixture.system, &delivery, NULL);
    if (status == TA_OK) status = ta_system_run(tar_value_fixture.system, &options, &result);
    return INT2NUM(status);
}

static VALUE tar_value_fixture_queue_counts(VALUE self)
{
    VALUE result = rb_hash_new();
    (void)self;
    rb_hash_aset(result, ID2SYM(rb_intern("retain")), SIZET2NUM(tar_fixture_retain_total));
    rb_hash_aset(result, ID2SYM(rb_intern("release")), SIZET2NUM(tar_fixture_release_total));
    rb_hash_aset(result, ID2SYM(rb_intern("snapshot")), SIZET2NUM(tar_fixture_snapshot_total));
    rb_hash_aset(result, ID2SYM(rb_intern("live_core_refs")), SIZET2NUM(tar_fixture_live_core_refs));
    rb_hash_aset(result, ID2SYM(rb_intern("seen_events")), SIZET2NUM(tar_fixture_seen_events));
    rb_hash_aset(result, ID2SYM(rb_intern("last_event_handle")), ULL2NUM((unsigned long long)tar_fixture_last_event_handle));
    return result;
}

static VALUE tar_value_fixture_failure_counts(VALUE self)
{
    (void)self;
    return tar_value_fixture_queue_counts(self);
}

static VALUE tar_value_fixture_queue_release(VALUE self)
{
    ta_status_t status;
    (void)self;
    status = tar_value_fixture_close_status();
    tar_fixture_tracking = 0u;
    return INT2NUM(status);
}

static VALUE tar_value_fixture_release(VALUE self)
{
    (void)self;
    return INT2NUM(tar_value_fixture_close_status());
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
        record->core_refs = 0u;
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
    tar_value_record_t *foreign_record;
    VALUE foreign_object;
    ta_value_t foreign;
    ta_value_t copy = {0};
    VALUE result;
    (void)self;
    foreign_object = rb_ary_new_from_args(1, ID2SYM(rb_intern("foreign")));
    foreign_record = tar_value_record_new(foreign_object, &tar_foreign_domain, 0u);
    foreign = tar_value_handle(foreign_record);
    result = rb_hash_new();
    rb_hash_aset(result, ID2SYM(rb_intern("status")),
                 INT2NUM(tar_value_ops()->snapshot(tar_value_domain_user(), foreign, &copy)));
    rb_hash_aset(result, ID2SYM(rb_intern("value")), Qnil);
    tar_value_dispose(foreign_record);
    return result;
}

void tar_value_shutdown(void) { tar_value_fixture_close(); }

void tar_init_value(VALUE raw) {
    ID id=rb_intern("Value");
    VALUE singleton;
    tar_value_registry_data_t *registry_data;
    (void)raw;
    if (NIL_P(tar_value_root_registry)) {
        registry_data = ALLOC(tar_value_registry_data_t);
        registry_data->unused = 0u;
        tar_value_root_registry = TypedData_Wrap_Struct(rb_cObject,
                                                         &tar_value_registry_type, registry_data);
        rb_gc_register_mark_object(tar_value_root_registry);
    }
    if (rb_const_defined(tar_mRaw,id)) {
        tar_value_class=rb_const_get(tar_mRaw,id);
        if (!RB_TYPE_P(tar_value_class,T_CLASS)) rb_raise(rb_eTypeError,"TinyActors::Raw::Value must be a class");
    } else {
        tar_value_class=rb_define_class_under(tar_mRaw,"Value",rb_cObject);
        rb_define_alloc_func(tar_value_class,tar_value_alloc);
        rb_define_method(tar_value_class,"initialize",tar_value_initialize,1);
        rb_define_method(tar_value_class,"object",tar_value_object,0);
    }
    if (!tar_value_class_rooted) {
        rb_global_variable(&tar_value_class);
        tar_value_class_rooted = 1u;
    }
    singleton = rb_singleton_class(tar_value_class);
    rb_define_private_method(singleton, "__test_admit", tar_value_fixture_admit, 1);
    rb_define_private_method(singleton, "__test_failed_admit", tar_value_fixture_failed_admit, 2);
    rb_define_private_method(singleton, "__test_release_admission", tar_value_fixture_release, 0);
    rb_define_private_method(singleton, "__test_counts", tar_value_fixture_counts, 1);
    rb_define_private_method(singleton, "__test_snapshot", tar_value_fixture_snapshot, 1);
    rb_define_private_method(singleton, "__test_foreign_snapshot", tar_value_fixture_foreign_snapshot, 0);
    rb_define_private_method(singleton, "__test_queue_start", tar_value_fixture_queue_start, 1);
    rb_define_private_method(singleton, "__test_queue_step", tar_value_fixture_queue_step, 0);
    rb_define_private_method(singleton, "__test_queue_replace", tar_value_fixture_queue_replace, 0);
    rb_define_private_method(singleton, "__test_queue_counts", tar_value_fixture_queue_counts, 0);
    rb_define_private_method(singleton, "__test_failure_counts", tar_value_fixture_failure_counts, 0);
    rb_define_private_method(singleton, "__test_queue_release", tar_value_fixture_queue_release, 0);
}
