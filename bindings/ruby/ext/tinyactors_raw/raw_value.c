#include "ruby.h"
#include "tinyactors.h"
#include "support.h"
#include <stdint.h>

typedef struct tar_value_domain tar_value_domain_t;
typedef struct tar_value_record tar_value_record_t;
struct tar_value_domain { unsigned int cookie; };
struct tar_value_record { VALUE object; tar_value_domain_t *domain; unsigned int wrapper_refs; unsigned int core_refs; };
typedef struct { tar_value_record_t *record; } tar_value_t;
static tar_value_domain_t tar_domain = { 0x54415256U };
static VALUE tar_value_class;

static tar_value_record_t *tar_value_record(ta_value_t value) { return (tar_value_record_t *)(uintptr_t)value.handle; }
static ta_value_t tar_value_handle(tar_value_record_t *record) { ta_value_t value; value.handle=(uintptr_t)record; return value; }
static void tar_value_mark(void *ptr) { tar_value_t *v=ptr; if (v && v->record) rb_gc_mark(v->record->object); }
static void tar_value_compact(void *ptr) { tar_value_t *v=ptr; if (v && v->record) v->record->object=rb_gc_location(v->record->object); }
static void tar_value_free(void *ptr) { tar_value_t *v=ptr; if (v && v->record) { v->record->wrapper_refs--; if (!v->record->wrapper_refs && !v->record->core_refs) xfree(v->record); } xfree(ptr); }
static size_t tar_value_memsize(const void *ptr) { return ptr ? sizeof(tar_value_t) : 0; }
static const rb_data_type_t tar_value_type={"TinyActors::Raw::Value",{tar_value_mark,tar_value_free,tar_value_memsize,tar_value_compact},0,0,RUBY_TYPED_FREE_IMMEDIATELY|RUBY_TYPED_WB_PROTECTED};
static VALUE tar_value_alloc(VALUE klass) { tar_value_t *v=ALLOC(tar_value_t); v->record=0; return TypedData_Wrap_Struct(klass,&tar_value_type,v); }
static VALUE tar_value_initialize(VALUE self, VALUE object) { tar_value_t *v; tar_value_record_t *r=ALLOC(tar_value_record_t); TypedData_Get_Struct(self,tar_value_t,&tar_value_type,v); r->object=object; r->domain=&tar_domain; r->wrapper_refs=1; r->core_refs=0; v->record=r; rb_obj_freeze(self); return self; }
static VALUE tar_value_object(VALUE self) { tar_value_t *v; TypedData_Get_Struct(self,tar_value_t,&tar_value_type,v); if (!v->record) rb_raise(rb_eRuntimeError,"released TinyActors::Raw::Value"); return v->record->object; }

ta_value_t tar_value_to_native(VALUE object) { tar_value_t *v; TypedData_Get_Struct(object,tar_value_t,&tar_value_type,v); if (!v->record) rb_raise(rb_eRuntimeError,"released TinyActors::Raw::Value"); return tar_value_handle(v->record); }
static void tar_value_retain(void *user, ta_value_t value) { tar_value_record_t *r=tar_value_record(value); if (!r || r->domain!=user) return; r->core_refs++; rb_gc_register_address(&r->object); }
static void tar_value_release(void *user, ta_value_t value) { tar_value_record_t *r=tar_value_record(value); if (!r || r->domain!=user || !r->core_refs) return; r->core_refs--; rb_gc_unregister_address(&r->object); if (!r->wrapper_refs && !r->core_refs) xfree(r); }
static VALUE tar_value_dump(VALUE object) { return rb_marshal_dump(object,Qnil); }
static VALUE tar_value_load(VALUE bytes) { return rb_marshal_load(bytes); }
static ta_status_t tar_value_snapshot(void *user, ta_value_t value, ta_value_t *out) { int raised=0; VALUE dump,copy; tar_value_record_t *src=tar_value_record(value),*r; if (!out) return TA_INVALID; out->handle=0; if (!src || src->domain!=user) return TA_INVALID; dump=tar_protected_call(tar_value_dump,src->object,&raised); if (raised) return TA_EXECUTION_ERROR; copy=tar_protected_call(tar_value_load,dump,&raised); if (raised) return TA_EXECUTION_ERROR; r=ALLOC(tar_value_record_t); r->object=copy; r->domain=user; r->wrapper_refs=0; r->core_refs=1; rb_gc_register_address(&r->object); *out=tar_value_handle(r); return TA_OK; }
const ta_value_ops_t *tar_value_ops(void) { static const ta_value_ops_t ops={tar_value_retain,tar_value_release,tar_value_snapshot}; return &ops; }

void tar_init_value(VALUE raw) { ID id=rb_intern("Value"); (void)raw; if (rb_const_defined(tar_mRaw,id)) { tar_value_class=rb_const_get(tar_mRaw,id); if (!RB_TYPE_P(tar_value_class,T_CLASS)) rb_raise(rb_eTypeError,"TinyActors::Raw::Value must be a class"); return; } tar_value_class=rb_define_class_under(tar_mRaw,"Value",rb_cObject); rb_define_alloc_func(tar_value_class,tar_value_alloc); rb_define_method(tar_value_class,"initialize",tar_value_initialize,1); rb_define_method(tar_value_class,"object",tar_value_object,0); }
