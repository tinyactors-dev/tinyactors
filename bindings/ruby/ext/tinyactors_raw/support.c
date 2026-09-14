#include "support.h"

#include <stdint.h>
#include <stdlib.h>

typedef struct tar_deferred {
    tar_cleanup_fn fn;
    void *native;
    tar_root_t *roots;
    struct tar_deferred *next;
} tar_deferred_t;
static tar_deferred_t *tar_deferred_head;

static void tar_release_roots(tar_root_t *roots)
{
    while (roots) {
        tar_root_t *next = roots->next;
        rb_gc_unregister_address(&roots->value);
        xfree(roots);
        roots = next;
    }
}

static void tar_defer(tar_cleanup_fn fn, void *native, tar_root_t *roots)
{
    tar_deferred_t *item;
    if (!fn || !native) return;
    item = (tar_deferred_t *)malloc(sizeof(*item));
    /* A finalizer must not raise, and cleanup functions are deliberately
     * native-only.  If the bookkeeping allocation fails, perform the native
     * cleanup synchronously and release the registered roots. */
    if (!item) {
        fn(native);
        tar_release_roots(roots);
        return;
    }
    item->fn = fn; item->native = native; item->roots = roots; item->next = tar_deferred_head;
    tar_deferred_head = item;
}

static void tar_handle_mark(void *ptr)
{
    tar_handle_t *handle = (tar_handle_t *)ptr;
    tar_root_t *root;
    if (!handle) return;
    if (!RB_SPECIAL_CONST_P(handle->owner))
        rb_gc_mark_movable(handle->owner);
    for (root = handle->roots; root; root = root->next) {
        if (root->active) rb_gc_mark(root->value);
    }
}

static void tar_handle_compact(void *ptr)
{
    tar_handle_t *handle = (tar_handle_t *)ptr;
    tar_root_t *root;
    if (!handle) return;
    handle->owner = rb_gc_location(handle->owner);
    for (root = handle->roots; root; root = root->next)
        if (root->active) root->value = rb_gc_location(root->value);
}

static void tar_handle_free(void *ptr)
{
    tar_handle_t *handle = (tar_handle_t *)ptr;
    if (!handle) return;
    if (handle->native && handle->cleanup) {
        tar_defer(handle->cleanup, handle->native, handle->roots);
        handle->native = 0;
        handle->roots = 0;
    } else {
        tar_root_remove_all(handle);
    }
    tar_lifetime_release(handle->token);
    xfree(handle);
}

static size_t tar_handle_memsize(const void *ptr) { return ptr ? sizeof(tar_handle_t) : 0; }

const rb_data_type_t tar_system_type = { "TinyActors::Raw::System", {
    tar_handle_mark, tar_handle_free, tar_handle_memsize, tar_handle_compact,
}, 0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED };
const rb_data_type_t tar_definition_type = { "TinyActors::Raw::Definition", { tar_handle_mark, tar_handle_free, tar_handle_memsize, tar_handle_compact }, 0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED };
const rb_data_type_t tar_arena_type = { "TinyActors::Raw::Arena", { tar_handle_mark, tar_handle_free, tar_handle_memsize, tar_handle_compact }, 0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED };
const rb_data_type_t tar_context_type = { "TinyActors::Raw::Context", { tar_handle_mark, tar_handle_free, tar_handle_memsize, tar_handle_compact }, 0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED };
const rb_data_type_t tar_actor_ref_type = { "TinyActors::Raw::ActorRef", { tar_handle_mark, tar_handle_free, tar_handle_memsize, tar_handle_compact }, 0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED };
const rb_data_type_t tar_request_ref_type = { "TinyActors::Raw::RequestRef", { tar_handle_mark, tar_handle_free, tar_handle_memsize, tar_handle_compact }, 0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED };
const rb_data_type_t tar_invocation_ref_type = { "TinyActors::Raw::InvocationRef", { tar_handle_mark, tar_handle_free, tar_handle_memsize, tar_handle_compact }, 0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED };
const rb_data_type_t tar_cursor_type = { "TinyActors::Raw::InspectCursor", { tar_handle_mark, tar_handle_free, tar_handle_memsize, tar_handle_compact }, 0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED };
const rb_data_type_t tar_memory_type = { "TinyActors::Raw::Memory", { tar_handle_mark, tar_handle_free, tar_handle_memsize, tar_handle_compact }, 0, 0, RUBY_TYPED_FREE_IMMEDIATELY | RUBY_TYPED_WB_PROTECTED };

VALUE tar_mTinyActors;
VALUE tar_mRaw;

tar_lifetime_t *tar_lifetime_new(tar_lifetime_t *parent) {
    tar_lifetime_t *token = ALLOC(tar_lifetime_t);
    token->generation = parent ? parent->generation + 1 : 1;
    token->alive = 1; token->parent = parent;
    token->system_identity = parent ? parent->system_identity : token;
    token->callback = 0; token->references = 1;
    token->system_state = parent ? parent->system_state : (tar_system_state_t *)calloc(1, sizeof(*token->system_state));
    if (!token->system_state) { xfree(token); rb_raise(rb_eNoMemError, "unable to allocate system state"); }
    if (parent) tar_lifetime_retain(parent);
    return token;
}
void tar_lifetime_invalidate(tar_lifetime_t *token) { if (token) token->alive = 0; }
void tar_lifetime_retain(tar_lifetime_t *token) { if (token) token->references++; }
void tar_lifetime_release(tar_lifetime_t *token) {
    if (!token || !token->references) return;
    if (--token->references == 0) {
        tar_lifetime_t *parent = token->parent;
        tar_system_state_t *state = token->system_state;
        xfree(token);
        if (!parent) free(state);
        tar_lifetime_release(parent);
    }
}
int tar_lifetime_live(const tar_lifetime_t *token) { return token && token->alive && (!token->parent || tar_lifetime_live(token->parent)); }
void tar_root_add(tar_handle_t *h, VALUE v) {
    tar_root_t *r;
    if (!h) return;
    r = ALLOC(tar_root_t);
    r->value=v; r->active=1; r->next=h->roots; h->roots=r;
    rb_gc_register_address(&r->value);
}
void tar_root_remove_all(tar_handle_t *h) { if (!h) return; tar_release_roots(h->roots); h->roots=0; }
/* tar_lifetime_new returns one reference.  A newly-created handle consumes
 * that reference; callers wrapping an already-owned token must retain it
 * before calling this initializer. */
void tar_handle_init(tar_handle_t *h, void *n, tar_lifetime_t *t) { h->native=n; h->token=t; h->owner=Qnil; h->roots=0; h->kind=0; h->cleanup=0; h->id=0; h->system_identity=t ? t->system_identity : 0; }
void tar_handle_release(tar_handle_t *h) {
    if (!h || !h->native) return;
    if (h->cleanup) {
        tar_defer(h->cleanup, h->native, h->roots);
        h->roots=0;
    } else {
        tar_root_remove_all(h);
    }
    h->native=0;
    tar_lifetime_invalidate(h->token);
}
void tar_handle_set_cleanup(tar_handle_t *h, tar_cleanup_fn fn) { if (h) h->cleanup=fn; }
void tar_deferred_cleanup_drain(void) { tar_deferred_t *item; while ((item=tar_deferred_head)) { tar_deferred_head=item->next; item->fn(item->native); tar_release_roots(item->roots); free(item); } }
void tar_deferred_cleanup_boundary(void) { tar_deferred_cleanup_drain(); }
void tar_check_handle(const tar_handle_t *h) { if (!h || !tar_lifetime_live(h->token)) rb_raise(rb_eRuntimeError, "expired TinyActors native handle"); }
void tar_callback_enter(tar_lifetime_t *t) { if (t) { t->callback++; if (t->system_state) t->system_state->callback_depth++; } }
void tar_callback_exit(tar_lifetime_t *t) { if (t && t->callback) { t->callback--; if (t->system_state && t->system_state->callback_depth) t->system_state->callback_depth--; if (!t->callback) tar_lifetime_invalidate(t); } }
int tar_callback_reentry(tar_lifetime_t *t) { return t && (t->callback != 0 || (t->system_state && t->system_state->callback_depth != 0)); }
int tar_same_system(const tar_handle_t *a, const tar_handle_t *b) { return a && b && a->system_identity == b->system_identity; }
static VALUE tar_protected_trampoline(VALUE arg) { VALUE (*fn)(VALUE) = (VALUE (*)(VALUE))((void **)arg)[0]; return fn(((VALUE *)arg)[1]); }
VALUE tar_protected_call(tar_protected_fn fn, VALUE arg, int *raised) { int state=0; VALUE pair[2]; VALUE result; pair[0]=(VALUE)fn; pair[1]=arg; result=rb_protect(tar_protected_trampoline,(VALUE)pair,&state); if (raised) *raised=state; return result; }

int tar_size_to_size_t(VALUE value, size_t *out) {
    unsigned long long n;
    if (!RB_INTEGER_TYPE_P(value)) rb_raise(rb_eTypeError, "expected Integer");
    if (RB_TYPE_P(value, T_BIGNUM) && RBIGNUM_NEGATIVE_P(value)) rb_raise(rb_eRangeError, "Integer out of range");
    n = NUM2ULL(value);
    if (n > (unsigned long long)SIZE_MAX) rb_raise(rb_eRangeError, "Integer out of range");
    *out = (size_t)n;
    return 1;
}

VALUE tar_make_handle_class(const char *name, const rb_data_type_t *type) {
    ID id = rb_intern(name);
    VALUE klass;

    /* Keep native initialization idempotent.  This matters for embedded
     * loaders and tests that evaluate the entry point more than once: a
     * redefinition would replace the class's TypedData contract. */
    if (rb_const_defined(tar_mRaw, id)) {
        klass = rb_const_get(tar_mRaw, id);
        if (!RB_TYPE_P(klass, T_CLASS))
            rb_raise(rb_eTypeError, "TinyActors::Raw::%s is not a class", name);
        return klass;
    }
    klass = rb_define_class_under(tar_mRaw, name, rb_cObject);
    rb_undef_alloc_func(klass);
    (void)type;
    return klass;
}
VALUE tar_handle_wrap(const tar_wrap_spec_t *spec, void *n, tar_lifetime_t *t) { tar_handle_t *h=ALLOC(tar_handle_t); tar_handle_init(h,n,t); h->owner=spec->owner; h->kind=spec->kind; return TypedData_Wrap_Struct(spec->klass,spec->type,h); }
void *tar_handle_get(VALUE object, const rb_data_type_t *type) { tar_handle_t *h; TypedData_Get_Struct(object,tar_handle_t,type,h); tar_check_handle(h); if (!h->native) rb_raise(rb_eRuntimeError, "expired TinyActors native handle"); return h->native; }
tar_handle_t *tar_handle_from(VALUE object, const rb_data_type_t *type) { tar_handle_t *h; TypedData_Get_Struct(object,tar_handle_t,type,h); return h; }

static VALUE tar_wrap_kind(const char *name, const rb_data_type_t *type, void *n, tar_lifetime_t *t, VALUE o, unsigned int kind) { tar_wrap_spec_t spec={rb_const_get(tar_mRaw, rb_intern(name)),type,o,kind}; return tar_handle_wrap(&spec,n,t); }
VALUE tar_system_wrap(void *n, tar_lifetime_t *t, VALUE o) { return tar_wrap_kind("System",&tar_system_type,n,t,o,1); }
VALUE tar_definition_wrap(void *n, tar_lifetime_t *t, VALUE o) { return tar_wrap_kind("Definition",&tar_definition_type,n,t,o,2); }
VALUE tar_arena_wrap(void *n, tar_lifetime_t *t, VALUE o) { return tar_wrap_kind("Arena",&tar_arena_type,n,t,o,3); }
VALUE tar_context_wrap(void *n, tar_lifetime_t *t, VALUE o) { return tar_wrap_kind("Context",&tar_context_type,n,t,o,4); }
VALUE tar_memory_wrap(void *n, tar_lifetime_t *t, VALUE o) { return tar_wrap_kind("Memory",&tar_memory_type,n,t,o,5); }
VALUE tar_cursor_wrap(void *n, tar_lifetime_t *t, VALUE o) { return tar_wrap_kind("InspectCursor",&tar_cursor_type,n,t,o,6); }
static VALUE tar_ref_wrap(const char *name, const rb_data_type_t *type, uint64_t id, void *sys, tar_lifetime_t *t, VALUE o) { VALUE v=tar_wrap_kind(name,type,0,t,o,7); tar_handle_t *h=tar_handle_from(v,type); h->id=id; h->system_identity=sys; return v; }
VALUE tar_actor_ref_wrap(uint64_t id, void *s, tar_lifetime_t *t, VALUE o) { return tar_ref_wrap("ActorRef",&tar_actor_ref_type,id,s,t,o); }
VALUE tar_request_ref_wrap(uint64_t id, void *s, tar_lifetime_t *t, VALUE o) { return tar_ref_wrap("RequestRef",&tar_request_ref_type,id,s,t,o); }
VALUE tar_invocation_ref_wrap(uint64_t id, void *s, tar_lifetime_t *t, VALUE o) { return tar_ref_wrap("InvocationRef",&tar_invocation_ref_type,id,s,t,o); }

void tar_init_support(VALUE raw)
{
    static unsigned int globals_rooted;

    /* VALUEs held in C statics must participate in compaction.  The extension
     * invokes its initializer before later wrappers create handles, so root
     * the namespace once and keep the address stable for the life of Ruby. */
    if (!globals_rooted) {
        rb_global_variable(&tar_mTinyActors);
        rb_global_variable(&tar_mRaw);
        globals_rooted = 1u;
    }
    /* These are the stable opaque representations shared by later raw
     * assignments.  No runtime procedures are registered here. */
    (void)tar_make_handle_class("System", &tar_system_type);
    (void)tar_make_handle_class("Definition", &tar_definition_type);
    (void)tar_make_handle_class("Arena", &tar_arena_type);
    (void)tar_make_handle_class("Context", &tar_context_type);
    (void)tar_make_handle_class("ActorRef", &tar_actor_ref_type);
    (void)tar_make_handle_class("RequestRef", &tar_request_ref_type);
    (void)tar_make_handle_class("InvocationRef", &tar_invocation_ref_type);
    (void)tar_make_handle_class("InspectCursor", &tar_cursor_type);
    (void)tar_make_handle_class("Memory", &tar_memory_type);
    (void)raw;
}
