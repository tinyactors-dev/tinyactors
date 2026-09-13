#include <ruby.h>
#include <stdlib.h>
#include <string.h>

/* Include the public contract now so the extension's names stay aligned with
 * the C ABI while the implementation is filled in incrementally. */
#include "tinyactors.h"

static VALUE mTinyActors;
static VALUE cSystem;
static VALUE cArena;
static VALUE cSystemConfig;
static VALUE cDefinition;
static VALUE cActor;
static VALUE cEvent;
static VALUE cMessage;
static VALUE cTarget;
static VALUE cSend;
static VALUE cInvoke;
static VALUE cIORequest;
static VALUE cInvocation;
static VALUE cActorRef;
static VALUE cRequestRef;
static VALUE cChart;
static VALUE cChartState;
static VALUE cChartTransition;
static VALUE cContext;

static VALUE eTinyActorsError;
static VALUE eUnsupported;
static VALUE rb_hash_get(VALUE h, const char *name);
static VALUE rb_text_value(VALUE value);

typedef struct { ta_chunk_allocator_t allocator; ta_arena_t *arena; VALUE callbacks; } rb_arena_t;
typedef struct { ta_system_t *system; ta_arena_t *arena; int closed; } rb_system_t;
typedef struct { ta_definition_t *definition; VALUE system; } rb_definition_t;

static ta_status_t rb_acquire(void *user, const ta_layout_t *minimum, ta_chunk_t *out)
{
    (void)user;
    size_t alignment = minimum->alignment ? minimum->alignment : sizeof(void *);
    size_t size = minimum->size ? minimum->size : alignment;
    void *raw = malloc(size + alignment - 1u + sizeof(void *));
    if (!raw) return TA_NO_MEMORY;
    uintptr_t address = (uintptr_t)raw + sizeof(void *) + alignment - 1u;
    void *p = (void *)(address & ~(uintptr_t)(alignment - 1u));
    ((void **)p)[-1] = raw;
    out->data = p; out->layout = *minimum; out->layout.size = size;
    return TA_OK;
}
static void rb_release(void *user, const ta_chunk_t *chunk) { (void)user; free(((void **)chunk->data)[-1]); }

static void rb_system_free(void *ptr) {
    rb_system_t *s = ptr;
    if (s->system) ta_system_destroy(s->system);
    if (s->arena) ta_arena_close(s->arena);
    xfree(s);
}
static const rb_data_type_t rb_system_type = { "TinyActors::System", { 0, rb_system_free, 0 }, 0, 0, RUBY_TYPED_FREE_IMMEDIATELY };
static void rb_arena_free(void *ptr) { rb_arena_t *a = ptr; if (a->arena) ta_arena_close(a->arena); xfree(a); }
static void rb_arena_mark(void *ptr) { rb_arena_t *a = ptr; if (a) rb_gc_mark(a->callbacks); }
static const rb_data_type_t rb_arena_type = { "TinyActors::Arena", { rb_arena_mark, rb_arena_free, 0 }, 0, 0, RUBY_TYPED_FREE_IMMEDIATELY };
static void rb_definition_mark(void *ptr) { rb_definition_t *d = ptr; if (d) rb_gc_mark(d->system); }
static void rb_definition_free(void *ptr) { rb_definition_t *d = ptr; if (d->definition) ta_definition_release(d->definition); xfree(d); }
static const rb_data_type_t rb_definition_type = { "TinyActors::Definition", { rb_definition_mark, rb_definition_free, 0 }, 0, 0, RUBY_TYPED_FREE_IMMEDIATELY };

static rb_system_t *get_system(VALUE self) { rb_system_t *s; TypedData_Get_Struct(self, rb_system_t, &rb_system_type, s); if (!s->system) rb_raise(eTinyActorsError, "system is closed"); return s; }
static VALUE system_alloc(VALUE klass) { rb_system_t *s = ALLOC(rb_system_t); memset(s, 0, sizeof *s); return TypedData_Wrap_Struct(klass, &rb_system_type, s); }
static VALUE arena_alloc(VALUE klass) { rb_arena_t *a = ALLOC(rb_arena_t); memset(a, 0, sizeof *a); return TypedData_Wrap_Struct(klass, &rb_arena_type, a); }
static VALUE definition_alloc(VALUE klass) { rb_definition_t *d = ALLOC(rb_definition_t); memset(d, 0, sizeof *d); return TypedData_Wrap_Struct(klass, &rb_definition_type, d); }
static rb_arena_t *get_arena(VALUE self) { rb_arena_t *a; TypedData_Get_Struct(self, rb_arena_t, &rb_arena_type, a); if (!a->arena) rb_raise(eTinyActorsError, "arena is closed"); return a; }
static void rb_arena_cleanup(void *argument) {
    VALUE callback = (VALUE)argument;
    rb_funcall(callback, rb_intern("call"), 0);
}
static VALUE arena_defer(VALUE self) {
    rb_need_block();
    rb_arena_t *a = get_arena(self);
    VALUE callback = rb_block_proc();
    if (NIL_P(a->callbacks)) a->callbacks = rb_ary_new();
    rb_ary_push(a->callbacks, callback);
    ta_status_t st = ta_arena_defer(a->arena, rb_arena_cleanup, (void *)callback);
    if (st != TA_OK) {
        rb_ary_pop(a->callbacks);
        rb_raise(eTinyActorsError, "arena defer failed (status %d)", st);
    }
    return self;
}
static VALUE arena_create(int argc, VALUE *argv, VALUE klass) {
    VALUE options; rb_scan_args(argc, argv, "01", &options); (void)options;
    VALUE obj = rb_obj_alloc(klass); rb_arena_t *a; TypedData_Get_Struct(obj, rb_arena_t, &rb_arena_type, a); a->callbacks = rb_ary_new();
    a->allocator = (ta_chunk_allocator_t){ NULL, rb_acquire, rb_release };
    ta_arena_options_t o = {4096, 65536, TA_UNLIMITED, 65536};
    ta_status_t st = ta_arena_create(&a->allocator, &o, &a->arena);
    if (st != TA_OK) rb_raise(eTinyActorsError, "arena creation failed (status %d)", st);
    return obj;
}
static VALUE arena_close(VALUE self) { rb_arena_t *a = get_arena(self); ta_arena_close(a->arena); a->arena = NULL; return Qnil; }
static VALUE arena_stats(VALUE self) { ta_memory_stats_t s; ta_arena_stats(get_arena(self)->arena, &s); VALUE h=rb_hash_new(); rb_hash_aset(h,ID2SYM(rb_intern("reserved_bytes")),ULL2NUM(s.reserved_bytes)); rb_hash_aset(h,ID2SYM(rb_intern("used_bytes")),ULL2NUM(s.used_bytes)); rb_hash_aset(h,ID2SYM(rb_intern("chunk_count")),ULL2NUM(s.chunk_count)); return h; }
static VALUE arena_set_limit(VALUE self, VALUE value) { ta_status_t st=ta_arena_set_limit(get_arena(self)->arena, NUM2ULL(value)); if(st!=TA_OK) rb_raise(eTinyActorsError,"arena limit failed (status %d)",st); return Qnil; }
static VALUE arena_open(int argc, VALUE *argv, VALUE self) { VALUE options; rb_scan_args(argc,argv,"01",&options); (void)options; VALUE obj=rb_obj_alloc(CLASS_OF(self)); rb_arena_t *child; TypedData_Get_Struct(obj,rb_arena_t,&rb_arena_type,child); child->callbacks = rb_ary_new(); ta_arena_options_t o={4096,65536,TA_UNLIMITED,65536}; ta_status_t st=ta_arena_open(get_arena(self)->arena,&o,&child->arena); if(st!=TA_OK) rb_raise(eTinyActorsError,"arena open failed (status %d)",st); return obj; }
static VALUE arena_alloc_memory(VALUE self, VALUE layout) { Check_Type(layout,T_HASH); VALUE size=rb_hash_aref(layout,ID2SYM(rb_intern("size"))); VALUE alignment=rb_hash_aref(layout,ID2SYM(rb_intern("alignment"))); ta_layout_t l={(size_t)NUM2ULL(size),(size_t)NUM2ULL(alignment)}; void *memory=NULL; ta_status_t st=ta_arena_alloc(get_arena(self)->arena,&l,&memory); if(st!=TA_OK) rb_raise(eTinyActorsError,"arena allocation failed (status %d)",st); return rb_str_new((const char *)memory,(long)l.size); }
static VALUE target_make(VALUE klass, VALUE kind, VALUE value) { VALUE o=rb_obj_alloc(klass); rb_ivar_set(o,rb_intern("@kind"),kind); rb_ivar_set(o,rb_intern("@value"),value); return o; }
static VALUE target_self(VALUE klass) { return target_make(klass, ID2SYM(rb_intern("self")), Qnil); }
static VALUE target_internal(VALUE klass) { return target_make(klass, ID2SYM(rb_intern("internal")), Qnil); }
static VALUE target_parent(VALUE klass) { return target_make(klass, ID2SYM(rb_intern("parent")), Qnil); }
static VALUE target_with(VALUE klass, VALUE value) { StringValue(value); return target_make(klass, rb_class_name(klass), value); }
static VALUE target_instance(VALUE klass, VALUE value) { return target_make(klass, ID2SYM(rb_intern("instance")), ULL2NUM(NUM2ULL(value))); }
static VALUE target_inspect(VALUE self) { VALUE k=rb_obj_as_string(rb_ivar_get(self,rb_intern("@kind"))),v=rb_ivar_get(self,rb_intern("@value")); VALUE vs=NIL_P(v)?rb_str_new_cstr(""):rb_obj_as_string(v); return rb_sprintf("#<TinyActors::Target %s%s>",StringValueCStr(k),StringValueCStr(vs)); }
static VALUE target_kind(VALUE self) { return rb_ivar_get(self,rb_intern("@kind")); }
static VALUE target_value(VALUE self) { return rb_ivar_get(self,rb_intern("@value")); }
static ta_target_t rb_target(VALUE obj) { ta_target_t t={0}; VALUE k=rb_ivar_get(obj,rb_intern("@kind")),v=rb_ivar_get(obj,rb_intern("@value")); ID id=SYMBOL_P(k)?SYM2ID(k):rb_intern(StringValueCStr(k)); if(id==rb_intern("instance")){t.kind=TA_TARGET_INSTANCE;t.instance.id=NUM2ULL(v);} else if(id==rb_intern("name")){t.kind=TA_TARGET_NAME;} else if(id==rb_intern("invoke")){t.kind=TA_TARGET_INVOKE;} else if(id==rb_intern("uri")){t.kind=TA_TARGET_URI;} else if(id==rb_intern("internal")){t.kind=TA_TARGET_INTERNAL;} else if(id==rb_intern("parent")){t.kind=TA_TARGET_PARENT;} else t.kind=TA_TARGET_SELF; if(t.kind!=TA_TARGET_INSTANCE&&!NIL_P(v)){StringValue(v);t.text=(ta_text_t){StringValueCStr(v),RSTRING_LEN(v)};} return t; }
static VALUE value_initialize(int argc, VALUE *argv, VALUE self) { VALUE attrs; rb_scan_args(argc, argv, "01", &attrs); if (NIL_P(attrs)) attrs=rb_hash_new(); else Check_Type(attrs,T_HASH); rb_ivar_set(self,rb_intern("@attributes"),attrs); return self; }
static VALUE value_attributes(VALUE self) { VALUE h=rb_ivar_get(self,rb_intern("@attributes")); return NIL_P(h)?rb_hash_new():h; }
static VALUE value_aref(VALUE self, VALUE key) { return rb_hash_aref(value_attributes(self),key); }
static VALUE value_to_h(VALUE self) { return rb_hash_dup(value_attributes(self)); }
static VALUE context_in_state(VALUE self, VALUE state_name) {
    VALUE native = rb_ivar_get(self, rb_intern("@native_context"));
    if (NIL_P(native)) rb_raise(eUnsupported, "native operation requires an active host callback context");
    StringValue(state_name);
    const ta_context_t *context = (const ta_context_t *)(uintptr_t)NUM2ULL(native);
    return ta_context_in(context, (ta_text_t){RSTRING_PTR(state_name), RSTRING_LEN(state_name)}) ? Qtrue : Qfalse;
}
static const ta_context_t *rb_native_context(VALUE self) {
    VALUE native = rb_ivar_get(self, rb_intern("@native_context"));
    if (NIL_P(native)) rb_raise(eUnsupported, "native operation requires an active host callback context");
    return (const ta_context_t *)(uintptr_t)NUM2ULL(native);
}
static VALUE context_raise_event(int argc, VALUE *argv, VALUE self) {
    VALUE event, data;
    rb_scan_args(argc, argv, "11", &event, &data);
    StringValue(event);
    ta_payload_t payload; memset(&payload, 0, sizeof payload);
    if (!NIL_P(data)) { StringValue(data); payload.kind = TA_PAYLOAD_BYTES; payload.as.bytes = (ta_bytes_t){RSTRING_PTR(data), RSTRING_LEN(data)}; }
    ta_status_t st = ta_context_raise((ta_context_t *)rb_native_context(self), (ta_text_t){RSTRING_PTR(event), RSTRING_LEN(event)}, &payload);
    if (st != TA_OK) rb_raise(eTinyActorsError, "context raise failed (status %d)", st);
    return Qnil;
}
static VALUE context_cancel(VALUE self, VALUE sendid) {
    StringValue(sendid);
    ta_status_t st = ta_context_cancel((ta_context_t *)rb_native_context(self), (ta_text_t){RSTRING_PTR(sendid), RSTRING_LEN(sendid)});
    if (st != TA_OK) rb_raise(eTinyActorsError, "context cancel failed (status %d)", st);
    return Qnil;
}
static VALUE context_send(VALUE self, VALUE attributes) {
    Check_Type(attributes, T_HASH);
    VALUE event = rb_hash_get(attributes, "event");
    if (NIL_P(event)) rb_raise(rb_eArgError, "send requires an event");
    event = rb_text_value(event);
    ta_send_t send; memset(&send, 0, sizeof send);
    send.event = (ta_text_t){RSTRING_PTR(event), RSTRING_LEN(event)};
    VALUE type = rb_hash_get(attributes, "type");
    if (!NIL_P(type)) { type = rb_text_value(type); send.type = (ta_text_t){RSTRING_PTR(type), RSTRING_LEN(type)}; }
    VALUE target = rb_hash_get(attributes, "target");
    if (NIL_P(target)) target = target_make(cTarget, ID2SYM(rb_intern("self")), Qnil);
    send.target = rb_target(target);
    VALUE sendid = rb_hash_get(attributes, "sendid");
    if (!NIL_P(sendid)) { sendid = rb_text_value(sendid); send.sendid = (ta_text_t){RSTRING_PTR(sendid), RSTRING_LEN(sendid)}; }
    VALUE delay = rb_hash_get(attributes, "delay_ns");
    if (!NIL_P(delay)) send.delay_ns = NUM2ULL(delay);
    VALUE data = rb_hash_get(attributes, "data");
    if (!NIL_P(data)) { data = rb_text_value(data); send.body.kind = TA_BODY_CONTENT; send.body.content.kind = TA_PAYLOAD_BYTES; send.body.content.as.bytes = (ta_bytes_t){RSTRING_PTR(data), RSTRING_LEN(data)}; }
    uint64_t message_id = 0;
    ta_status_t st = ta_context_send((ta_context_t *)rb_native_context(self), &send, &message_id);
    if (st != TA_OK) rb_raise(eTinyActorsError, "context send failed (status %d)", st);
    return ULL2NUM(message_id);
}
static VALUE context_inspect_view(VALUE self) {
    ta_context_view_t view; memset(&view, 0, sizeof view);
    ta_context_inspect(rb_native_context(self), &view);
    VALUE out = rb_hash_new();
    rb_hash_aset(out, ID2SYM(rb_intern("actor")), ULL2NUM(view.actor.id));
    rb_hash_aset(out, ID2SYM(rb_intern("phase")), INT2NUM(view.phase));
    rb_hash_aset(out, ID2SYM(rb_intern("now_ns")), ULL2NUM(view.now_ns));
    rb_hash_aset(out, ID2SYM(rb_intern("state_id")), UINT2NUM(view.state_id));
    rb_hash_aset(out, ID2SYM(rb_intern("transition_id")), UINT2NUM(view.transition_id));
    if (view.state.storage == TA_DATA_BYTES && view.state.as.bytes.data)
        rb_hash_aset(out, ID2SYM(rb_intern("data")), rb_str_new(view.state.as.bytes.data, (long)view.state.as.bytes.size));
    return out;
}
static VALUE context_event(VALUE self) {
    VALUE event = rb_ivar_get(self, rb_intern("@event"));
    if (NIL_P(event)) rb_raise(eUnsupported, "event is unavailable outside an active callback");
    return event;
}
static VALUE context_actor(VALUE self) {
    VALUE view = context_inspect_view(self);
    return rb_hash_aref(view, ID2SYM(rb_intern("actor")));
}
static VALUE context_data(VALUE self) {
    VALUE view = context_inspect_view(self);
    return rb_hash_aref(view, ID2SYM(rb_intern("data")));
}
static VALUE context_host_user(VALUE self) {
    void *user = ta_context_host_user(rb_native_context(self));
    return ULL2NUM((uintptr_t)user);
}
static VALUE context_bytes(VALUE self) {
    ta_mut_bytes_t bytes; memset(&bytes, 0, sizeof bytes);
    ta_status_t st = ta_context_bytes((ta_context_t *)rb_native_context(self), &bytes);
    if (st != TA_OK) rb_raise(eTinyActorsError, "context bytes failed (status %d)", st);
    return rb_str_new((const char *)bytes.data, (long)bytes.size);
}
static VALUE context_arena(int argc, VALUE *argv, VALUE self) {
    VALUE lifetime; rb_scan_args(argc, argv, "01", &lifetime);
    ta_lifetime_t kind = TA_LIFETIME_MACROSTEP;
    if (!NIL_P(lifetime)) kind = (ta_lifetime_t)NUM2INT(lifetime);
    ta_arena_t *arena = NULL;
    ta_status_t st = ta_context_arena((ta_context_t *)rb_native_context(self), kind, &arena);
    if (st != TA_OK) rb_raise(eTinyActorsError, "context arena failed (status %d)", st);
    return ULL2NUM((uintptr_t)arena);
}

static VALUE
version(VALUE self)
{
    (void)self;
    return rb_str_new_cstr("0.1.0.pre");
}

static VALUE system_initialize(int argc, VALUE *argv, VALUE self)
{
    VALUE options; rb_scan_args(argc, argv, "01", &options);
    rb_system_t *s; TypedData_Get_Struct(self, rb_system_t, &rb_system_type, s);
    ta_chunk_allocator_t alloc = { NULL, rb_acquire, rb_release };
    ta_arena_options_t ao = { 4096, 65536, TA_UNLIMITED, 65536 };
    ta_status_t st = ta_arena_create(&alloc, &ao, &s->arena);
    if (st != TA_OK) rb_raise(eTinyActorsError, "arena creation failed (status %d)", st);
    ta_system_config_t config; ta_system_config_init(&config);
    config.session_prefix = ta_text_from_cstr("ruby");
    config.data_model = ta_text_from_cstr("bytes");
    for (size_t i = 0; i < TA_LIFETIME_COUNT; ++i)
        config.arenas[i] = (ta_arena_options_t){ 4096, 65536, TA_UNLIMITED, 65536 };
    ta_ioprocessor_desc_t *processors = NULL;
    if (!NIL_P(options) && TYPE(options) == T_HASH) {
        VALUE list = rb_hash_aref(options, ID2SYM(rb_intern("ioprocessors")));
        if (!NIL_P(list)) {
            Check_Type(list, T_ARRAY);
            long count = RARRAY_LEN(list);
            processors = ALLOC_N(ta_ioprocessor_desc_t, (size_t)count + 1u);
            memset(processors, 0, sizeof(*processors) * ((size_t)count + 1u));
            for (long i = 0; i < count; ++i) {
                VALUE desc = rb_ary_entry(list, i); Check_Type(desc, T_HASH);
                VALUE type = rb_hash_aref(desc, ID2SYM(rb_intern("type"))); StringValue(type);
                processors[i].type = StringValueCStr(type);
                VALUE aliases = rb_hash_aref(desc, ID2SYM(rb_intern("aliases")));
                if (!NIL_P(aliases)) {
                    Check_Type(aliases, T_ARRAY); long n = RARRAY_LEN(aliases);
                    const char **a = ALLOC_N(const char *, (size_t)n + 1u);
                    for (long j = 0; j < n; ++j) { VALUE item = rb_ary_entry(aliases, j); StringValue(item); a[j] = StringValueCStr(item); }
                    a[n] = NULL; processors[i].aliases = a;
                }
            }
            config.ioprocessors = processors;
        }
    }
    st = ta_system_create(s->arena, &config, &s->system);
    if (processors) { for (size_t i = 0; i < (size_t)RARRAY_LEN(rb_hash_aref(options, ID2SYM(rb_intern("ioprocessors")))); ++i) if (processors[i].aliases) xfree((void *)processors[i].aliases); xfree(processors); }
    if (st != TA_OK) { ta_arena_close(s->arena); s->arena = NULL; rb_raise(eTinyActorsError, "system creation failed (status %d)", st); }
    return self;
}
static VALUE system_run(int argc, VALUE *argv, VALUE self)
{
    VALUE options; rb_scan_args(argc, argv, "01", &options); (void)options;
    rb_system_t *s = get_system(self); ta_run_options_t o; memset(&o, 0, sizeof o);
    o.budget.work_units = 1000; o.budget.actor_quantum = 100; o.now_ns = 0; ta_run_result_t result;
    ta_status_t st = ta_system_run(s->system, &o, &result);
    if (st != TA_OK) rb_raise(eTinyActorsError, "run failed (status %d)", st);
    VALUE out = rb_hash_new(); rb_hash_aset(out, ID2SYM(rb_intern("work_units")), ULL2NUM(result.work_units)); rb_hash_aset(out, ID2SYM(rb_intern("runnable")), result.runnable ? Qtrue : Qfalse); return out;
}
static VALUE system_close(VALUE self) { rb_system_t *s = get_system(self); ta_system_destroy(s->system); s->system = NULL; return Qnil; }
static VALUE system_destroy(VALUE self) { rb_system_t *s = get_system(self); ta_status_t st = ta_system_destroy(s->system); if (st != TA_OK) rb_raise(eTinyActorsError, "destroy failed (status %d)", st); s->system = NULL; return Qnil; }
static VALUE system_shutdown(VALUE self) { rb_system_t *s = get_system(self); ta_status_t st = ta_system_shutdown(s->system); if (st != TA_OK) rb_raise(eTinyActorsError, "shutdown failed (status %d)", st); return Qnil; }
static VALUE system_next_deadline(VALUE self) { return ULL2NUM(ta_system_next_deadline(get_system(self)->system)); }
static VALUE system_stats(VALUE self) {
    ta_system_stats_t v; ta_system_stats(get_system(self)->system, &v);
    VALUE h = rb_hash_new();
    rb_hash_aset(h, ID2SYM(rb_intern("actors")), ULL2NUM(v.actors));
    rb_hash_aset(h, ID2SYM(rb_intern("runnable_actors")), ULL2NUM(v.runnable_actors));
    rb_hash_aset(h, ID2SYM(rb_intern("definitions")), ULL2NUM(v.definitions));
    rb_hash_aset(h, ID2SYM(rb_intern("queued_events")), ULL2NUM(v.queued_events));
    rb_hash_aset(h, ID2SYM(rb_intern("io_requests")), ULL2NUM(v.io_requests));
    rb_hash_aset(h, ID2SYM(rb_intern("accepted_messages")), ULL2NUM(v.accepted_messages));
    rb_hash_aset(h, ID2SYM(rb_intern("microsteps")), ULL2NUM(v.microsteps)); return h;
}
static VALUE system_runnable(VALUE self) { ta_system_stats_t v; ta_system_stats(get_system(self)->system, &v); return v.runnable_actors ? Qtrue : Qfalse; }
static VALUE system_io_next(VALUE self) {
    const ta_io_request_t *r=NULL; ta_status_t st=ta_io_next(get_system(self)->system,&r);
    if(st==TA_EMPTY) return Qnil;
    if(st!=TA_OK) rb_raise(eTinyActorsError,"io_next failed (status %d)",st);
    VALUE h=rb_hash_new(); rb_hash_aset(h,ID2SYM(rb_intern("request")),ULL2NUM(r->request.id)); rb_hash_aset(h,ID2SYM(rb_intern("kind")),INT2NUM(r->kind)); rb_hash_aset(h,ID2SYM(rb_intern("actor")),ULL2NUM(r->actor.id)); rb_hash_aset(h,ID2SYM(rb_intern("type")),rb_str_new(r->type.data,(long)r->type.size)); rb_hash_aset(h,ID2SYM(rb_intern("target")),rb_str_new(r->target.data,(long)r->target.size)); return h;
}
static VALUE system_io_complete(int argc, VALUE *argv, VALUE self) { VALUE request,result; rb_scan_args(argc,argv,"11",&request,&result); ta_status_t st=ta_io_complete(get_system(self)->system,(ta_request_ref_t){NUM2ULL(request)},NIL_P(result)?TA_OK:(ta_status_t)NUM2INT(result)); if(st!=TA_OK) rb_raise(eTinyActorsError,"io_complete failed (status %d)",st); return Qnil; }
static VALUE system_invocation_cancelled(VALUE self, VALUE id) { uint8_t cancelled=0; ta_status_t st=ta_invocation_is_cancelled(get_system(self)->system,(ta_invocation_ref_t){NUM2ULL(id)},&cancelled); if(st!=TA_OK) rb_raise(eTinyActorsError,"invocation query failed (status %d)",st); return cancelled?Qtrue:Qfalse; }
static VALUE system_post(int argc, VALUE *argv, VALUE self) { VALUE target,event,data; rb_scan_args(argc,argv,"21",&target,&event,&data); Check_Type(target,T_OBJECT); StringValue(event); ta_post_options_t o={0}; o.target=rb_target(target); o.message.name=(ta_text_t){StringValueCStr(event),RSTRING_LEN(event)}; if(!NIL_P(data)){StringValue(data);o.message.data.kind=TA_PAYLOAD_BYTES;o.message.data.as.bytes=(ta_bytes_t){StringValueCStr(data),RSTRING_LEN(data)};} uint64_t id=0; ta_status_t st=ta_post(get_system(self)->system,&o,&id); if(st!=TA_OK) rb_raise(eTinyActorsError,"post failed (status %d)",st); return ULL2NUM(id); }
static VALUE system_lookup(VALUE self, VALUE name) { StringValue(name); ta_actor_ref_t ref; ta_status_t st=ta_name_lookup(get_system(self)->system,(ta_text_t){RSTRING_PTR(name),RSTRING_LEN(name)},&ref); if(st!=TA_OK) rb_raise(eTinyActorsError,"name lookup failed (status %d)",st); return ULL2NUM(ref.id); }
static VALUE system_session_lookup(VALUE self, VALUE name) { StringValue(name); ta_actor_ref_t ref; ta_status_t st=ta_session_lookup(get_system(self)->system,(ta_text_t){RSTRING_PTR(name),RSTRING_LEN(name)},&ref); if(st!=TA_OK) rb_raise(eTinyActorsError,"session lookup failed (status %d)",st); return ULL2NUM(ref.id); }
static VALUE system_io_receive(int argc, VALUE *argv, VALUE self) { VALUE processor,destination,event; rb_scan_args(argc,argv,"3",&processor,&destination,&event); Check_Type(event,T_HASH); StringValue(processor); VALUE name=rb_hash_aref(event,ID2SYM(rb_intern("name"))); VALUE data=rb_hash_aref(event,ID2SYM(rb_intern("data"))); StringValue(name); ta_event_t e={0}; e.name=(ta_text_t){RSTRING_PTR(name),RSTRING_LEN(name)}; if(!NIL_P(data)){StringValue(data);e.data.kind=TA_PAYLOAD_BYTES;e.data.as.bytes=(ta_bytes_t){RSTRING_PTR(data),RSTRING_LEN(data)};} ta_io_route_t route={(ta_text_t){RSTRING_PTR(processor),RSTRING_LEN(processor)},(ta_actor_ref_t){NUM2ULL(destination)}}; ta_status_t st=ta_io_receive(get_system(self)->system,&route,&e); if(st!=TA_OK) rb_raise(eTinyActorsError,"io_receive failed (status %d)",st); return Qnil; }
static VALUE system_io_receive_error(int argc, VALUE *argv, VALUE self) { VALUE processor,destination,sendid; rb_scan_args(argc,argv,"3",&processor,&destination,&sendid); StringValue(processor); StringValue(sendid); ta_io_route_t route={(ta_text_t){RSTRING_PTR(processor),RSTRING_LEN(processor)},(ta_actor_ref_t){NUM2ULL(destination)}}; ta_status_t st=ta_io_receive_error(get_system(self)->system,&route,(ta_text_t){RSTRING_PTR(sendid),RSTRING_LEN(sendid)}); if(st!=TA_OK) rb_raise(eTinyActorsError,"io_receive_error failed (status %d)",st); return Qnil; }
static VALUE system_invocation_done(VALUE self, VALUE id) { ta_body_t body={0}; ta_status_t st=ta_invocation_done(get_system(self)->system,(ta_invocation_ref_t){NUM2ULL(id)},&body); if(st!=TA_OK) rb_raise(eTinyActorsError,"invocation_done failed (status %d)",st); return Qnil; }
static VALUE system_invocation_fail(VALUE self, VALUE id) { ta_status_t st=ta_invocation_fail(get_system(self)->system,(ta_invocation_ref_t){NUM2ULL(id)}); if(st!=TA_OK) rb_raise(eTinyActorsError,"invocation_fail failed (status %d)",st); return Qnil; }
static VALUE system_invocation_emit(int argc, VALUE *argv, VALUE self) { VALUE id,event,data; rb_scan_args(argc,argv,"21",&id,&event,&data); StringValue(event); ta_message_t m={0}; m.name=(ta_text_t){RSTRING_PTR(event),RSTRING_LEN(event)}; if(!NIL_P(data)){StringValue(data);m.data.kind=TA_PAYLOAD_BYTES;m.data.as.bytes=(ta_bytes_t){RSTRING_PTR(data),RSTRING_LEN(data)};} ta_status_t st=ta_invocation_emit(get_system(self)->system,(ta_invocation_ref_t){NUM2ULL(id)},&m); if(st!=TA_OK) rb_raise(eTinyActorsError,"invocation_emit failed (status %d)",st); return Qnil; }
static VALUE system_actor_stop(VALUE self, VALUE id) { ta_status_t st=ta_actor_stop(get_system(self)->system,(ta_actor_ref_t){NUM2ULL(id)}); if(st!=TA_OK) rb_raise(eTinyActorsError,"actor_stop failed (status %d)",st); return Qnil; }
static VALUE system_actor_release(VALUE self, VALUE id) { ta_status_t st=ta_actor_release(get_system(self)->system,(ta_actor_ref_t){NUM2ULL(id)}); if(st!=TA_OK) rb_raise(eTinyActorsError,"actor_release failed (status %d)",st); return Qnil; }
static VALUE system_actor_inspect(VALUE self, VALUE id) { ta_actor_info_t i; ta_status_t st=ta_actor_inspect(get_system(self)->system,(ta_actor_ref_t){NUM2ULL(id)},&i); if(st!=TA_OK) rb_raise(eTinyActorsError,"actor_inspect failed (status %d)",st); VALUE h=rb_hash_new(); rb_hash_aset(h,ID2SYM(rb_intern("status")),INT2NUM(i.status)); rb_hash_aset(h,ID2SYM(rb_intern("reason")),INT2NUM(i.reason)); rb_hash_aset(h,ID2SYM(rb_intern("revision")),ULL2NUM(i.revision)); rb_hash_aset(h,ID2SYM(rb_intern("stable")),i.stable?Qtrue:Qfalse); rb_hash_aset(h,ID2SYM(rb_intern("external_events")),ULL2NUM(i.external_events)); rb_hash_aset(h,ID2SYM(rb_intern("internal_events")),ULL2NUM(i.internal_events)); if(i.sessionid.data) rb_hash_aset(h,ID2SYM(rb_intern("session_id")),rb_str_new(i.sessionid.data,(long)i.sessionid.size)); if(i.registered_name.data) rb_hash_aset(h,ID2SYM(rb_intern("name")),rb_str_new(i.registered_name.data,(long)i.registered_name.size)); if(i.state.storage==TA_DATA_BYTES&&i.state.as.bytes.data) rb_hash_aset(h,ID2SYM(rb_intern("data")),rb_str_new(i.state.as.bytes.data,(long)i.state.as.bytes.size)); return h; }
static VALUE system_set_limits(VALUE self, VALUE hash) { Check_Type(hash,T_HASH); ta_system_limits_t l; memset(&l,0,sizeof l); size_t *fields=(size_t *)&l; size_t n=sizeof(l)/sizeof(size_t); for(size_t i=0;i<n;++i) fields[i]=TA_UNLIMITED; fields[10]=64; fields[11]=64; fields[12]=1024; fields[13]=4096; const char *names[]={"maximum_bytes","actors","definitions","names","queued_events","delayed_sends","invocations","io_requests","definition_states","definition_transitions","definition_actions","state_depth","action_depth","event_name_bytes","trace_metadata_bytes"}; for(size_t i=0;i<15;++i){VALUE v=rb_hash_aref(hash,ID2SYM(rb_intern(names[i])));if(!NIL_P(v))fields[i]=NUM2ULL(v);} ta_status_t st=ta_system_set_limits(get_system(self)->system,&l);if(st!=TA_OK)rb_raise(eTinyActorsError,"set_limits failed (status %d)",st);return Qnil; }
typedef struct { VALUE records; } rb_inspect_sink_t;
static ta_visit_t rb_inspect_visit(void *user, const ta_inspect_record_t *record) { rb_inspect_sink_t *sink=user; if(record->kind==TA_INSPECT_ACTOR){VALUE h=rb_hash_new();rb_hash_aset(h,ID2SYM(rb_intern("actor")),ULL2NUM(record->actor.id));rb_hash_aset(h,ID2SYM(rb_intern("status")),INT2NUM(record->as.info.status));rb_hash_aset(h,ID2SYM(rb_intern("revision")),ULL2NUM(record->as.info.revision));rb_hash_aset(h,ID2SYM(rb_intern("stable")),record->as.info.stable?Qtrue:Qfalse);rb_ary_push(sink->records,h);} return TA_VISIT_CONTINUE; }
static VALUE system_inspect_all(VALUE self) { rb_inspect_sink_t sink={rb_ary_new()}; ta_inspect_options_t o; memset(&o,0,sizeof o); o.work_units=1000; o.details=0; o.sink=(ta_inspect_sink_t){&sink,rb_inspect_visit}; ta_inspect_cursor_t cursor={{0}}; ta_status_t st=ta_system_inspect(get_system(self)->system,&o,&cursor); if(st!=TA_OK) rb_raise(eTinyActorsError,"system_inspect failed (status %d)",st); return sink.records; }
static VALUE config_default(VALUE klass) { VALUE h=rb_hash_new(); rb_hash_aset(h,ID2SYM(rb_intern("session_prefix")),rb_str_new_cstr("ruby")); rb_hash_aset(h,ID2SYM(rb_intern("data_model")),rb_str_new_cstr("bytes")); rb_hash_aset(h,ID2SYM(rb_intern("limits")),rb_hash_new()); return h; }

static VALUE rb_hash_get(VALUE h, const char *name) {
    VALUE v = rb_hash_aref(h, ID2SYM(rb_intern(name)));
    if (NIL_P(v)) v = rb_hash_aref(h, rb_str_new_cstr(name));
    return v;
}
static VALUE rb_text_value(VALUE value) {
    if (SYMBOL_P(value)) value = rb_sym2str(value);
    StringValue(value);
    return value;
}
struct rb_hash_collect { VALUE *keys; VALUE *values; long count; };
static int rb_collect_hash(VALUE key, VALUE value, VALUE arg) {
    struct rb_hash_collect *c = (struct rb_hash_collect *)arg;
    c->keys[c->count] = key; c->values[c->count] = value; c->count++;
    return ST_CONTINUE;
}
static void rb_chart_free_states(ta_chart_state_t *states) {
    if (!states) return;
    for (size_t i = 0; states[i].id; ++i) {
        if (states[i].transitions) { for (size_t j=0; states[i].transitions[j].event || states[i].transitions[j].target || states[i].transitions[j].content.actions; ++j) if (states[i].transitions[j].content.actions) xfree((void *)states[i].transitions[j].content.actions); xfree((void *)states[i].transitions); }
        if (states[i].onentry.blocks) { for (uint32_t j=0;j<states[i].onentry.count;++j) if (states[i].onentry.blocks[j].actions) xfree((void *)states[i].onentry.blocks[j].actions); xfree((void *)states[i].onentry.blocks); }
        if (states[i].onexit.blocks) { for (uint32_t j=0;j<states[i].onexit.count;++j) if (states[i].onexit.blocks[j].actions) xfree((void *)states[i].onexit.blocks[j].actions); xfree((void *)states[i].onexit.blocks); }
        rb_chart_free_states((ta_chart_state_t *)states[i].states);
    }
    xfree(states);
}
static ta_block_t rb_chart_actions(VALUE value) {
    ta_block_t block = {0}; if (NIL_P(value)) return block; Check_Type(value, T_ARRAY); long n=RARRAY_LEN(value); ta_action_t *actions=ALLOC_N(ta_action_t,(size_t)n); memset(actions,0,sizeof(*actions)*(size_t)n); long used=0;
    for(long i=0;i<n;++i){ VALUE a=rb_ary_entry(value,i); if(!RB_TYPE_P(a,T_HASH)) continue; VALUE event=rb_hash_get(a,"event"); VALUE type=rb_hash_get(a,"type"); ID kind=NIL_P(type)?rb_intern("raise"):SYM2ID(rb_to_symbol(type)); if(!NIL_P(event) && kind==rb_intern("raise")){ event=rb_text_value(event); actions[used].kind=TA_ACTION_RAISE; actions[used].as.raise.event=(ta_text_t){StringValueCStr(event),RSTRING_LEN(event)}; used++; } else if(kind==rb_intern("send")){ event=rb_text_value(event); actions[used].kind=TA_ACTION_SEND; actions[used].as.send.event.literal=(ta_text_t){StringValueCStr(event),RSTRING_LEN(event)}; VALUE target=rb_hash_get(a,"target"); if(NIL_P(target)) target=target_make(cTarget,ID2SYM(rb_intern("self")),Qnil); actions[used].as.send.target=rb_target(target); VALUE sendid=rb_hash_get(a,"sendid"); if(!NIL_P(sendid)){sendid=rb_text_value(sendid); actions[used].as.send.id=(ta_text_t){StringValueCStr(sendid),RSTRING_LEN(sendid)};} VALUE delay=rb_hash_get(a,"delay_ns"); if(!NIL_P(delay)) actions[used].as.send.delay_ns=NUM2ULL(delay); used++; } else if(kind==rb_intern("cancel")){ VALUE sendid=rb_hash_get(a,"sendid"); if(!NIL_P(sendid)){sendid=rb_text_value(sendid); actions[used].kind=TA_ACTION_CANCEL; actions[used].as.cancel.sendid.literal=(ta_text_t){StringValueCStr(sendid),RSTRING_LEN(sendid)}; used++; } } }
    block.actions=actions; block.count=(uint32_t)used; return block;
}
static ta_chart_state_t *rb_chart_states(VALUE hash) {
    Check_Type(hash, T_HASH);
    long n = RHASH_SIZE(hash); VALUE *keys = ALLOC_N(VALUE, (size_t)n); VALUE *vals = ALLOC_N(VALUE, (size_t)n);
    struct rb_hash_collect c = { keys, vals, 0 }; rb_hash_foreach(hash, rb_collect_hash, (VALUE)&c);
    ta_chart_state_t *out = ALLOC_N(ta_chart_state_t, (size_t)n + 1u); memset(out, 0, sizeof(*out) * ((size_t)n + 1u));
    for (long i = 0; i < n; ++i) {
        VALUE id = rb_text_value(keys[i]); VALUE spec = vals[i]; if (SYMBOL_P(spec)) spec = rb_hash_new(); Check_Type(spec, T_HASH); out[i].id = StringValueCStr(id);
        out[i].kind = RTEST(rb_hash_get(spec, "final")) ? TA_CHART_FINAL : (RTEST(rb_hash_get(spec, "parallel")) ? TA_CHART_PARALLEL : TA_CHART_STATE);
        VALUE initial = rb_hash_get(spec, "initial"); if (!NIL_P(initial)) { initial = rb_text_value(initial); out[i].initial.target = StringValueCStr(initial); }
        VALUE children = rb_hash_get(spec, "states"); if (!NIL_P(children)) out[i].states = rb_chart_states(children);
        VALUE entry = rb_hash_get(spec, "entry"); if (!NIL_P(entry)) { ta_block_t *blocks=ALLOC_N(ta_block_t,1); blocks[0]=rb_chart_actions(entry); out[i].onentry=(ta_blocks_t){blocks,1}; }
        VALUE exit = rb_hash_get(spec, "exit"); if (!NIL_P(exit)) { ta_block_t *blocks=ALLOC_N(ta_block_t,1); blocks[0]=rb_chart_actions(exit); out[i].onexit=(ta_blocks_t){blocks,1}; }
        VALUE transitions = rb_hash_get(spec, "transitions"); if (NIL_P(transitions)) transitions = rb_hash_get(spec, "on");
        if (!NIL_P(transitions)) { Check_Type(transitions, T_ARRAY); long tn = RARRAY_LEN(transitions); ta_chart_transition_t *ct = ALLOC_N(ta_chart_transition_t, (size_t)tn + 1u); memset(ct, 0, sizeof(*ct) * ((size_t)tn + 1u)); for (long j=0;j<tn;++j) { VALUE e=rb_ary_entry(transitions,j); Check_Type(e,T_HASH); VALUE ev=rb_hash_get(e,"event"); VALUE tr=rb_hash_get(e,"transition"); if(NIL_P(tr)) tr=e; VALUE target=rb_hash_get(tr,"target"); if(!NIL_P(ev)){ev=rb_text_value(ev);ct[j].event=StringValueCStr(ev);} if(!NIL_P(target)){target=rb_text_value(target);ct[j].target=StringValueCStr(target);} ct[j].content=rb_chart_actions(rb_hash_get(tr,"actions")); ct[j].kind=TA_TRANSITION_EXTERNAL; } out[i].transitions=ct; }
    }
    xfree(keys); xfree(vals); return out;
}

static VALUE rb_definition_native_build(VALUE self, VALUE input) {
    rb_system_t *sys = get_system(self);
    if (rb_obj_is_kind_of(input, cDefinition)) input = value_attributes(input);
    Check_Type(input, T_HASH);
    VALUE states = rb_hash_get(input, "states");
    if (NIL_P(states)) states = rb_hash_new();
    Check_Type(states, T_HASH);
    ta_chart_state_t *cs = rb_chart_states(states);
    ta_chart_t chart; memset(&chart, 0, sizeof chart); VALUE name = rb_hash_get(input, "name"); if (!NIL_P(name)) { name = rb_text_value(name); chart.name = StringValueCStr(name); } else chart.name = "ruby.definition"; chart.data_model = "bytes"; chart.binding = TA_BINDING_EARLY; chart.data_layout.storage = TA_DATA_BYTES; chart.data_layout.bytes_layout.alignment = 1; chart.states = cs; VALUE initial = rb_hash_get(input, "initial"); if (!NIL_P(initial)) { initial = rb_text_value(initial); chart.initial.target = StringValueCStr(initial); }
    ta_definition_result_t result; memset(&result, 0, sizeof result); ta_status_t st = ta_definition_build(sys->system, &chart, &result);
    rb_chart_free_states(cs);
    if (st != TA_OK) rb_raise(eTinyActorsError, "definition build failed (status %d, state %u, transition %u): %.*s", st, result.error.state, result.error.transition, (int)result.error.detail.size, result.error.detail.data ? result.error.detail.data : "");
    VALUE out = rb_obj_alloc(cDefinition); rb_definition_t *d; TypedData_Get_Struct(out, rb_definition_t, &rb_definition_type, d); d->definition = result.definition; d->system = self; rb_ivar_set(out, rb_intern("@attributes"), input); return out;
}

static VALUE system_actor_spawn_native(int argc, VALUE *argv, VALUE self) {
    VALUE definition, options; rb_scan_args(argc, argv, "11", &definition, &options); rb_definition_t *d; TypedData_Get_Struct(definition, rb_definition_t, &rb_definition_type, d); if (!d->definition) rb_raise(eTinyActorsError, "definition is not native"); ta_spawn_options_t o; memset(&o, 0, sizeof o); o.definition = d->definition; if (!NIL_P(options)) { Check_Type(options, T_HASH); VALUE name = rb_hash_get(options, "name"); if (!NIL_P(name)) { StringValue(name); o.name = (ta_text_t){RSTRING_PTR(name), RSTRING_LEN(name)}; } } ta_actor_ref_t actor; ta_status_t st = ta_actor_spawn(get_system(self)->system, &o, &actor); if (st != TA_OK) rb_raise(eTinyActorsError, "actor spawn failed (status %d)", st); return ULL2NUM(actor.id);
}
static rb_definition_t *get_definition(VALUE obj) { rb_definition_t *d; TypedData_Get_Struct(obj, rb_definition_t, &rb_definition_type, d); if (!d->definition) rb_raise(eTinyActorsError, "definition is released"); return d; }
static VALUE system_definition_revision(VALUE self, VALUE obj) { (void)self; return ULL2NUM(ta_definition_revision(get_definition(obj)->definition)); }
static VALUE system_definition_description(VALUE self, VALUE obj) { (void)self; const ta_definition_desc_t *d=ta_definition_description(get_definition(obj)->definition); VALUE h=rb_hash_new(); rb_hash_aset(h,ID2SYM(rb_intern("name")),rb_str_new(d->name.data,(long)d->name.size)); rb_hash_aset(h,ID2SYM(rb_intern("data_model")),rb_str_new(d->data_model.data,(long)d->data_model.size)); rb_hash_aset(h,ID2SYM(rb_intern("states")),UINT2NUM(d->state_count)); rb_hash_aset(h,ID2SYM(rb_intern("transitions")),UINT2NUM(d->transition_count)); return h; }
static VALUE system_definition_footprint(VALUE self, VALUE obj) { (void)self; ta_actor_footprint_t f; ta_definition_footprint(get_definition(obj)->definition,&f); VALUE h=rb_hash_new(); rb_hash_aset(h,ID2SYM(rb_intern("control_bytes")),ULL2NUM(f.control_bytes)); rb_hash_aset(h,ID2SYM(rb_intern("state_bytes")),ULL2NUM(f.state_bytes)); rb_hash_aset(h,ID2SYM(rb_intern("minimum_chunk_bytes")),ULL2NUM(f.minimum_chunk_bytes)); return h; }
static VALUE system_definition_retain(VALUE self, VALUE obj) { (void)self; ta_definition_retain(get_definition(obj)->definition); return obj; }
static VALUE system_definition_release(VALUE self, VALUE obj) { (void)self; rb_definition_t *d=get_definition(obj); ta_definition_release(d->definition); d->definition=NULL; return Qnil; }
static VALUE system_definition_publish(int argc, VALUE *argv, VALUE self) { VALUE obj,key,expected; rb_scan_args(argc,argv,"21",&obj,&key,&expected); rb_definition_t *d=get_definition(obj); key=rb_text_value(key); ta_definition_binding_t b={(ta_text_t){RSTRING_PTR(key),RSTRING_LEN(key)},NIL_P(expected)?0:NUM2ULL(expected)}; ta_status_t st=ta_definition_publish(get_system(self)->system,&b,d->definition); if(st!=TA_OK) rb_raise(eTinyActorsError,"definition publish failed (status %d)",st); return Qnil; }
static VALUE system_definition_unpublish(int argc, VALUE *argv, VALUE self) { VALUE key,expected; rb_scan_args(argc,argv,"11",&key,&expected); key=rb_text_value(key); ta_definition_binding_t b={(ta_text_t){RSTRING_PTR(key),RSTRING_LEN(key)},NIL_P(expected)?0:NUM2ULL(expected)}; ta_status_t st=ta_definition_unpublish(get_system(self)->system,&b); if(st!=TA_OK) rb_raise(eTinyActorsError,"definition unpublish failed (status %d)",st); return Qnil; }
static VALUE system_definition_lookup(VALUE self, VALUE key) { key=rb_text_value(key); ta_definition_t *native=NULL; ta_status_t st=ta_definition_lookup(get_system(self)->system,(ta_text_t){RSTRING_PTR(key),RSTRING_LEN(key)},&native); if(st!=TA_OK) rb_raise(eTinyActorsError,"definition lookup failed (status %d)",st); VALUE out=rb_obj_alloc(cDefinition); rb_definition_t *d; TypedData_Get_Struct(out,rb_definition_t,&rb_definition_type,d); d->definition=native; d->system=self; return out; }
static VALUE system_name_bind(int argc, VALUE *argv, VALUE self) { VALUE name,actor,expected; rb_scan_args(argc,argv,"21",&name,&actor,&expected); name=rb_text_value(name); ta_name_binding_t b={(ta_text_t){RSTRING_PTR(name),RSTRING_LEN(name)},NIL_P(expected)?(ta_actor_ref_t){0}:(ta_actor_ref_t){NUM2ULL(expected)}}; ta_status_t st=ta_name_bind(get_system(self)->system,&b,(ta_actor_ref_t){NUM2ULL(actor)}); if(st!=TA_OK) rb_raise(eTinyActorsError,"name bind failed (status %d)",st); return Qnil; }
static VALUE system_name_unbind(int argc, VALUE *argv, VALUE self) { VALUE name,expected; rb_scan_args(argc,argv,"11",&name,&expected); name=rb_text_value(name); ta_name_binding_t b={(ta_text_t){RSTRING_PTR(name),RSTRING_LEN(name)},NIL_P(expected)?(ta_actor_ref_t){0}:(ta_actor_ref_t){NUM2ULL(expected)}}; ta_status_t st=ta_name_unbind(get_system(self)->system,&b); if(st!=TA_OK) rb_raise(eTinyActorsError,"name unbind failed (status %d)",st); return Qnil; }
static VALUE system_actor_set_limits(VALUE self, VALUE id, VALUE hash) { Check_Type(hash,T_HASH); ta_actor_limits_t l; size_t *f=(size_t *)&l; for(size_t i=0;i<sizeof(l)/sizeof(size_t);++i) f[i]=TA_UNLIMITED; const char *names[]={"maximum_bytes","external_events","internal_events","queued_event_bytes","delayed_sends","invocations","io_requests","io_request_bytes","microsteps_per_macrostep","actions_per_macrostep"}; for(size_t i=0;i<10;++i){VALUE v=rb_hash_get(hash,names[i]);if(!NIL_P(v))f[i]=NUM2ULL(v);} ta_status_t st=ta_actor_set_limits(get_system(self)->system,(ta_actor_ref_t){NUM2ULL(id)},&l); if(st!=TA_OK) rb_raise(eTinyActorsError,"actor set limits failed (status %d)",st); return Qnil; }
static VALUE system_actor_upgrade(int argc, VALUE *argv, VALUE self) {
    VALUE actor, replacement, expected;
    rb_scan_args(argc, argv, "21", &actor, &replacement, &expected);
    rb_definition_t *d = get_definition(replacement);
    ta_upgrade_options_t options;
    memset(&options, 0, sizeof options);
    options.actor = (ta_actor_ref_t){NUM2ULL(actor)};
    options.replacement = d->definition;
    options.expected_revision = NIL_P(expected) ? TA_REVISION_ANY : NUM2ULL(expected);
    ta_status_t st = ta_actor_upgrade(get_system(self)->system, &options);
    if (st != TA_OK) rb_raise(eTinyActorsError, "actor upgrade failed (status %d)", st);
    return Qnil;
}
static VALUE target_parse(int argc, VALUE *argv, VALUE self) { VALUE raw; rb_scan_args(argc, argv, "1", &raw); StringValue(raw); ta_target_t t; ta_status_t st = ta_target_parse((ta_text_t){RSTRING_PTR(raw), RSTRING_LEN(raw)}, &t); if (st != TA_OK) rb_raise(eTinyActorsError, "invalid target (status %d)", st); VALUE o=rb_obj_alloc(cTarget); VALUE kind; switch(t.kind){case TA_TARGET_INTERNAL:kind=ID2SYM(rb_intern("internal"));break;case TA_TARGET_PARENT:kind=ID2SYM(rb_intern("parent"));break;case TA_TARGET_INSTANCE:kind=ID2SYM(rb_intern("instance"));break;case TA_TARGET_NAME:kind=ID2SYM(rb_intern("name"));break;case TA_TARGET_INVOKE:kind=ID2SYM(rb_intern("invoke"));break;case TA_TARGET_URI:kind=ID2SYM(rb_intern("uri"));break;default:kind=ID2SYM(rb_intern("self"));} rb_ivar_set(o,rb_intern("@kind"),kind); if(t.kind==TA_TARGET_INSTANCE) rb_ivar_set(o,rb_intern("@value"),ULL2NUM(t.instance.id)); else if(t.text.data) rb_ivar_set(o,rb_intern("@value"),rb_str_new(t.text.data,(long)t.text.size)); else rb_ivar_set(o,rb_intern("@value"),Qnil); return o; }

void
Init_tinyactors(void)
{
    mTinyActors = rb_define_module("TinyActors");
    eTinyActorsError = rb_define_class_under(mTinyActors, "Error", rb_eStandardError);
    eUnsupported = rb_define_class_under(mTinyActors, "UnsupportedError", eTinyActorsError);

    rb_define_singleton_method(mTinyActors, "version", version, 0);
    rb_define_const(mTinyActors, "VERSION", rb_str_new_cstr("0.1.0.pre"));
    rb_define_const(mTinyActors, "OK", INT2NUM(TA_OK));
    rb_define_const(mTinyActors, "EMPTY", INT2NUM(TA_EMPTY));
    rb_define_const(mTinyActors, "AGAIN", INT2NUM(TA_AGAIN));
    rb_define_const(mTinyActors, "INVALID", INT2NUM(TA_INVALID));
    rb_define_const(mTinyActors, "NOT_FOUND", INT2NUM(TA_NOT_FOUND));
    rb_define_const(mTinyActors, "STALE", INT2NUM(TA_STALE));
    rb_define_const(mTinyActors, "BUSY", INT2NUM(TA_BUSY));
    rb_define_const(mTinyActors, "NO_MEMORY", INT2NUM(TA_NO_MEMORY));
    rb_define_const(mTinyActors, "CANCELLED", INT2NUM(TA_CANCELLED));

    /* Direct wrappers for the opaque runtime handles. */
    cSystem = rb_define_class_under(mTinyActors, "System", rb_cObject);
    cArena = rb_define_class_under(mTinyActors, "Arena", rb_cObject);
    rb_define_alloc_func(cArena, arena_alloc);
    rb_define_singleton_method(cArena, "create", arena_create, -1);
    rb_define_method(cArena, "close", arena_close, 0);
    rb_define_method(cArena, "stats", arena_stats, 0);
    rb_define_method(cArena, "set_limit", arena_set_limit, 1);
    rb_define_method(cArena, "open", arena_open, -1);
    rb_define_method(cArena, "alloc", arena_alloc_memory, 1);
    rb_define_method(cArena, "defer", arena_defer, 0);
    cSystemConfig = rb_define_class_under(mTinyActors, "SystemConfig", rb_cObject);
    rb_define_method(cSystemConfig, "initialize", value_initialize, -1);
    rb_define_method(cSystemConfig, "attributes", value_attributes, 0);
    rb_define_method(cSystemConfig, "[]", value_aref, 1);
    rb_define_method(cSystemConfig, "to_h", value_to_h, 0);
    cDefinition = rb_define_class_under(mTinyActors, "Definition", rb_cObject);
    rb_define_alloc_func(cDefinition, definition_alloc);
    cActor = rb_define_class_under(mTinyActors, "Actor", rb_cObject);
    cEvent = rb_define_class_under(mTinyActors, "Event", rb_cObject);
    cMessage = rb_define_class_under(mTinyActors, "Message", rb_cObject);
    cTarget = rb_define_class_under(mTinyActors, "Target", rb_cObject);
    rb_define_singleton_method(cTarget, "self", target_self, 0);
    rb_define_singleton_method(cTarget, "internal", target_internal, 0);
    rb_define_singleton_method(cTarget, "parent", target_parent, 0);
    rb_define_singleton_method(cTarget, "instance", target_instance, 1);
    rb_define_singleton_method(cTarget, "name", target_with, 1);
    rb_define_singleton_method(cTarget, "invoke", target_with, 1);
    rb_define_singleton_method(cTarget, "uri", target_with, 1);
    rb_define_method(cTarget, "kind", target_kind, 0);
    rb_define_method(cTarget, "value", target_value, 0);
    rb_define_method(cTarget, "inspect", target_inspect, 0);
    cSend = rb_define_class_under(mTinyActors, "Send", rb_cObject);
    cInvoke = rb_define_class_under(mTinyActors, "Invoke", rb_cObject);
    cIORequest = rb_define_class_under(mTinyActors, "IORequest", rb_cObject);
    cInvocation = rb_define_class_under(mTinyActors, "Invocation", rb_cObject);
    cActorRef = rb_define_class_under(mTinyActors, "ActorRef", rb_cObject);
    cRequestRef = rb_define_class_under(mTinyActors, "RequestRef", rb_cObject);

    /* Direct wrappers for the nested authoring form in tinyactors.h. */
    cChart = rb_define_class_under(mTinyActors, "Chart", rb_cObject);
    cChartState = rb_define_class_under(mTinyActors, "ChartState", rb_cObject);
    cChartTransition = rb_define_class_under(mTinyActors, "ChartTransition", rb_cObject);
    cContext = rb_define_class_under(mTinyActors, "Context", rb_cObject);

    VALUE value_classes[] = { cEvent, cMessage, cSend, cInvoke, cChart, cChartState, cChartTransition, cDefinition, cActorRef, cRequestRef, cInvocation, cIORequest };
    for (size_t i = 0; i < sizeof(value_classes)/sizeof(value_classes[0]); ++i) {
        rb_define_method(value_classes[i], "initialize", value_initialize, -1);
        rb_define_method(value_classes[i], "attributes", value_attributes, 0);
        rb_define_method(value_classes[i], "[]", value_aref, 1);
        rb_define_method(value_classes[i], "to_h", value_to_h, 0);
    }

    rb_define_alloc_func(cSystem, system_alloc);
    rb_define_method(cSystem, "initialize", system_initialize, -1);
    rb_define_method(cSystem, "run", system_run, -1);
    rb_define_method(cSystem, "close", system_close, 0);
    rb_define_method(cSystem, "destroy", system_destroy, 0);
    rb_define_method(cSystem, "shutdown", system_shutdown, 0);
    rb_define_method(cSystem, "next_deadline", system_next_deadline, 0);
    rb_define_method(cSystem, "stats", system_stats, 0);
    rb_define_method(cSystem, "runnable?", system_runnable, 0);
    rb_define_method(cSystem, "io_next", system_io_next, 0);
    rb_define_method(cSystem, "io_complete", system_io_complete, -1);
    rb_define_method(cSystem, "invocation_is_cancelled", system_invocation_cancelled, 1);
    rb_define_method(cSystem, "post", system_post, -1);
    rb_define_method(cSystem, "name_lookup", system_lookup, 1);
    rb_define_method(cSystem, "session_lookup", system_session_lookup, 1);
    rb_define_method(cSystem, "io_receive", system_io_receive, -1);
    rb_define_method(cSystem, "io_receive_error", system_io_receive_error, -1);
    rb_define_method(cSystem, "invocation_done", system_invocation_done, 1);
    rb_define_method(cSystem, "invocation_fail", system_invocation_fail, 1);
    rb_define_method(cSystem, "invocation_emit", system_invocation_emit, -1);
    rb_define_method(cSystem, "actor_stop", system_actor_stop, 1);
    rb_define_method(cSystem, "actor_release", system_actor_release, 1);
    rb_define_method(cSystem, "actor_inspect", system_actor_inspect, 1);
    rb_define_method(cSystem, "set_limits", system_set_limits, 1);
    rb_define_method(cSystem, "system_inspect", system_inspect_all, 0);
    rb_define_private_method(cSystem, "__native_definition_build", rb_definition_native_build, 1);
    rb_define_private_method(cSystem, "__native_actor_spawn", system_actor_spawn_native, -1);

    /* Class-level constructors and runtime operations. The eventual methods
     * will use TypedData, ta_* status values, and Ruby GC mark functions. */
    rb_define_method(cSystem, "definition_retain", system_definition_retain, 1);
    rb_define_method(cSystem, "definition_release", system_definition_release, 1);
    rb_define_method(cSystem, "definition_revision", system_definition_revision, 1);
    rb_define_method(cSystem, "definition_description", system_definition_description, 1);
    rb_define_method(cSystem, "definition_footprint", system_definition_footprint, 1);
    rb_define_method(cSystem, "definition_publish", system_definition_publish, -1);
    rb_define_method(cSystem, "definition_unpublish", system_definition_unpublish, -1);
    rb_define_method(cSystem, "definition_lookup", system_definition_lookup, 1);
    rb_define_method(cSystem, "name_bind", system_name_bind, -1);
    rb_define_method(cSystem, "name_unbind", system_name_unbind, -1);
    rb_define_method(cSystem, "actor_set_limits", system_actor_set_limits, 2);
    rb_define_method(cSystem, "actor_upgrade", system_actor_upgrade, -1);
    rb_define_method(cSystem, "definition_prepare", rb_definition_native_build, 1);




    /* Value-object constructors/accessors will be generated from the public
     * structs rather than hand-written ad hoc. */

    /* Arena functions mirror ta_arena_*; the allocator callbacks will be
     * represented by Ruby-owned adapter objects in the real implementation. */

    /* Configuration is a Ruby value object for ta_system_config_t and its
     * arrays of processor/service/trace registrations. */
    rb_define_singleton_method(cSystemConfig, "default", config_default, 0);
    rb_define_method(cContext, "inspect", context_inspect_view, 0);
    rb_define_method(cContext, "event", context_event, 0);
    rb_define_method(cContext, "actor", context_actor, 0);
    rb_define_method(cContext, "data", context_data, 0);
    rb_define_method(cContext, "in_state?", context_in_state, 1);
    rb_define_method(cContext, "host_user", context_host_user, 0);
    rb_define_method(cContext, "bytes", context_bytes, 0);
    rb_define_method(cContext, "arena", context_arena, -1);
    rb_define_method(cContext, "raise", context_raise_event, -1);
    rb_define_method(cContext, "send", context_send, 1);
    rb_define_method(cContext, "cancel", context_cancel, 1);

    /* ta_target_parse is a pure public helper, not a System operation. */
    rb_define_singleton_method(mTinyActors, "target_parse", target_parse, -1);
}
