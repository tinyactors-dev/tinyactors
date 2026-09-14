#include "ruby.h"
#include "tinyactors.h"
#include "support.h"
#include <stdint.h>

typedef struct tar_value_domain tar_value_domain_t;
typedef struct tar_value_record tar_value_record_t;
struct tar_value_domain { uint32_t cookie; };
struct tar_value_record {
    uint32_t magic;
    VALUE object;
    tar_value_domain_t *domain;
    unsigned int wrapper_refs;
    unsigned int core_refs;
    tar_value_record_t *next;
};
typedef struct { tar_value_record_t *record; } tar_value_t;
static tar_value_domain_t tar_domain = { 0x54415256U };
static tar_value_record_t *tar_value_records;
static VALUE tar_value_class;

static tar_value_record_t *tar_value_record(ta_value_t value) {
    tar_value_record_t *record;
    if (value.handle == 0u) return NULL;
    /* Never dereference an untrusted opaque handle.  Lookup in this domain's
     * private registry first; a handle from another extension/system is then
     * rejected without interpreting its address. */
    for (record = tar_value_records; record; record = record->next)
        if ((uintptr_t)record == value.handle &&
            record->magic == UINT32_C(0x54415652)) return record;
    return NULL;
}
static ta_value_t tar_value_handle(tar_value_record_t *record) { ta_value_t value; value.handle=(uintptr_t)record; return value; }
static void tar_value_mark(void *ptr) { tar_value_t *v=ptr; if (v && v->record) rb_gc_mark(v->record->object); }
static void tar_value_compact(void *ptr) { tar_value_t *v=ptr; if (v && v->record) v->record->object=rb_gc_location(v->record->object); }
static void tar_value_dispose(tar_value_record_t *record) {
    tar_value_record_t **slot;
    if (!record) return;
    for (slot = &tar_value_records; *slot; slot = &(*slot)->next) {
        if (*slot == record) { *slot = record->next; break; }
    }
    record->magic = 0u;
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
static VALUE tar_value_initialize(VALUE self, VALUE object) {
    tar_value_t *v;
    tar_value_record_t *r;
    TypedData_Get_Struct(self,tar_value_t,&tar_value_type,v);
    if (v->record) rb_raise(rb_eTypeError, "TinyActors::Raw::Value is already initialized");
    r = ALLOC(tar_value_record_t);
    r->magic = UINT32_C(0x54415652);
    r->object = object;
    r->domain = &tar_domain;
    r->wrapper_refs = 1;
    r->core_refs = 0;
    r->next = tar_value_records;
    tar_value_records = r;
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
    if (r->core_refs++ == 0u) rb_gc_register_address(&r->object);
}
static void tar_value_release(void *user, ta_value_t value) {
    tar_value_record_t *r=tar_value_record(value);
    if (!r || user != r->domain || !r->core_refs) return;
    if (--r->core_refs == 0u) {
        rb_gc_unregister_address(&r->object);
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
    r=ALLOC(tar_value_record_t);
    r->magic=UINT32_C(0x54415652);
    r->object=copy;
    r->domain=(tar_value_domain_t *)args->user;
    r->wrapper_refs=0;
    r->core_refs=1;
    r->next=tar_value_records;
    tar_value_records=r;
    rb_gc_register_address(&r->object);
    *(args->out)=tar_value_handle(r);
    return Qnil;
}
static ta_status_t tar_value_snapshot(void *user, ta_value_t value, ta_value_t *out) {
    int state = 0;
    tar_snapshot_args_t args = { user, value, out };
    if (!out) return TA_INVALID;
    out->handle = 0;
    rb_protect(tar_value_snapshot_protected, (VALUE)&args, &state);
    return state ? TA_EXECUTION_ERROR : TA_OK;
}
const ta_value_ops_t *tar_value_ops(void) { static const ta_value_ops_t ops={tar_value_retain,tar_value_release,tar_value_snapshot}; return &ops; }
void *tar_value_domain_user(void) { return &tar_domain; }

void tar_init_value(VALUE raw) { ID id=rb_intern("Value"); (void)raw; if (rb_const_defined(tar_mRaw,id)) { tar_value_class=rb_const_get(tar_mRaw,id); if (!RB_TYPE_P(tar_value_class,T_CLASS)) rb_raise(rb_eTypeError,"TinyActors::Raw::Value must be a class"); return; } tar_value_class=rb_define_class_under(tar_mRaw,"Value",rb_cObject); rb_define_alloc_func(tar_value_class,tar_value_alloc); rb_define_method(tar_value_class,"initialize",tar_value_initialize,1); rb_define_method(tar_value_class,"object",tar_value_object,0); }
