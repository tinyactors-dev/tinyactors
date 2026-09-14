#include "ruby.h"
#include "ruby/encoding.h"
#include "tinyactors.h"
#include "support.h"

#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum {
    TAR_MEMORY_CALLER = 1u,
    TAR_MEMORY_ARENA = 2u,
    TAR_MEMORY_CONTEXT = 3u,
    TAR_MEMORY_MIGRATION = 4u,
    TAR_MEMORY_INSPECTION = 5u
};

typedef struct tar_memory_storage {
    void *data;
    void *allocation;
    size_t size;
    size_t alignment;
    unsigned int kind;
    unsigned int writable;
    unsigned int releasable;
    unsigned int loans;
    VALUE loan_root;
    unsigned int rooted;
} tar_memory_storage_t;

static VALUE tar_memory_class;

static void
tar_memory_require_size(VALUE value, size_t *out, const char *name)
{
    if (!RB_INTEGER_TYPE_P(value))
        rb_raise(rb_eTypeError, "%s must be an Integer", name);
    if ((RB_FIXNUM_P(value) && FIX2LONG(value) < 0) ||
        (RB_TYPE_P(value, T_BIGNUM) && RBIGNUM_NEGATIVE_P(value)))
        rb_raise(rb_eRangeError, "%s must not be negative", name);
    tar_size_to_size_t(value, out);
}

static void
tar_memory_storage_free(void *pointer)
{
    tar_memory_storage_t *storage = (tar_memory_storage_t *)pointer;
    if (!storage) return;
    /* A live loan roots the wrapper, so normal finalization cannot reach this
     * function while rooted. Loan release unregisters the root first. */
    free(storage->allocation);
    xfree(storage);
}

static tar_memory_storage_t *
tar_memory_storage_get(VALUE self)
{
    tar_handle_t *handle = tar_handle_from(self, &tar_memory_type);
    tar_memory_storage_t *storage;

    tar_check_handle(handle);
    storage = (tar_memory_storage_t *)handle->native;
    if (!storage || (!storage->data && storage->size != 0u))
        rb_raise(rb_eRuntimeError, "closed TinyActors::Raw::Memory");
    return storage;
}

static void
tar_memory_check_range(tar_memory_storage_t *storage, VALUE offset_value,
                       size_t length, size_t *offset_out)
{
    size_t offset;

    tar_memory_require_size(offset_value, &offset, "offset");
    if (offset > storage->size)
        rb_raise(rb_eArgError, "memory offset is outside the allocation");
    if (length > SIZE_MAX - offset)
        rb_raise(rb_eRangeError, "memory access length overflows size_t");
    if (length > storage->size - offset)
        rb_raise(rb_eArgError, "memory access exceeds the allocation");
    if (offset_out) *offset_out = offset;
}

static void
tar_memory_allocate_storage(size_t size, size_t alignment,
                            tar_memory_storage_t **out_storage)
{
    size_t payload_size, total;
    uintptr_t raw_address, aligned_address;
    void *allocation;
    tar_memory_storage_t *storage;

    /* Allocate bookkeeping before the native chunk so later failures can
     * release both pieces. Ruby's ALLOC may raise, but no native allocation
     * exists yet at that point. */
    storage = ALLOC(tar_memory_storage_t);
    payload_size = size == 0u ? 1u : size;
    if (payload_size > SIZE_MAX - (alignment - 1u) ||
        payload_size + alignment - 1u > SIZE_MAX - sizeof(void *)) {
        xfree(storage);
        rb_raise(rb_eRangeError, "memory allocation size overflows size_t");
    }
    total = payload_size + alignment - 1u + sizeof(void *);
    allocation = malloc(total);
    if (!allocation) {
        xfree(storage);
        rb_raise(rb_eNoMemError, "unable to allocate native memory");
    }

    raw_address = (uintptr_t)allocation;
    if (raw_address > UINTPTR_MAX - sizeof(void *) - (alignment - 1u)) {
        free(allocation);
        xfree(storage);
        rb_raise(rb_eRangeError, "memory address alignment overflows uintptr_t");
    }
    aligned_address = (raw_address + sizeof(void *) + alignment - 1u) &
                      ~(uintptr_t)(alignment - 1u);
    ((void **)aligned_address)[-1] = allocation;

    storage->data = (void *)aligned_address;
    storage->allocation = allocation;
    storage->size = size;
    storage->alignment = alignment;
    storage->kind = TAR_MEMORY_CALLER;
    storage->writable = 1u;
    storage->releasable = 1u;
    storage->loans = 0u;
    storage->loan_root = Qnil;
    storage->rooted = 0u;
    *out_storage = storage;
}

static VALUE
tar_memory_allocate(int argc, VALUE *argv, VALUE klass)
{
    VALUE keywords = Qnil;
    VALUE values[2];
    static ID keyword_ids[2];
    size_t size, alignment;
    tar_memory_storage_t *storage;
    tar_lifetime_t *token;
    VALUE object;
    tar_handle_t *handle;

    (void)klass;
    if (!keyword_ids[0]) {
        keyword_ids[0] = rb_intern("size");
        keyword_ids[1] = rb_intern("alignment");
    }
    rb_scan_args(argc, argv, ":", &keywords);
    if (NIL_P(keywords))
        rb_raise(rb_eArgError, "missing keywords: size, alignment");
    rb_get_kwargs(keywords, keyword_ids, 2, 0, values);
    tar_memory_require_size(values[0], &size, "size");
    tar_memory_require_size(values[1], &alignment, "alignment");
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u)
        rb_raise(rb_eArgError, "alignment must be a nonzero power of two");
    if (alignment > (size_t)UINTPTR_MAX)
        rb_raise(rb_eRangeError, "alignment is not representable as a pointer alignment");

    tar_memory_allocate_storage(size, alignment, &storage);
    token = tar_lifetime_new(NULL);
    object = tar_memory_wrap(storage, token, Qnil);
    handle = tar_handle_from(object, &tar_memory_type);
    tar_handle_set_cleanup(handle, tar_memory_storage_free);
    return object;
}

static VALUE
tar_memory_size(VALUE self)
{
    return SIZET2NUM(tar_memory_storage_get(self)->size);
}

static VALUE
tar_memory_read(VALUE self, VALUE offset_value, VALUE length_value)
{
    tar_memory_storage_t *storage = tar_memory_storage_get(self);
    size_t length, offset;
    VALUE result;

    tar_memory_require_size(length_value, &length, "length");
    tar_memory_check_range(storage, offset_value, length, &offset);
    if (length > (size_t)LONG_MAX)
        rb_raise(rb_eRangeError, "read length is too large for a Ruby String");
    result = rb_str_new(length == 0u ? "" : (const char *)storage->data + offset,
                       (long)length);
    rb_enc_associate(result, rb_ascii8bit_encoding());
    return result;
}

static VALUE
tar_memory_write(VALUE self, VALUE offset_value, VALUE bytes)
{
    tar_memory_storage_t *storage = tar_memory_storage_get(self);
    size_t offset, length;
    long string_length;

    if (!RB_TYPE_P(bytes, T_STRING))
        rb_raise(rb_eTypeError, "bytes must be a String");
    if (!storage->writable)
        rb_raise(rb_eRuntimeError, "memory is read-only");
    string_length = RSTRING_LEN(bytes);
    if (string_length < 0 ||
        (unsigned long)string_length > (unsigned long)SIZE_MAX)
        rb_raise(rb_eRangeError, "bytes length is too large for size_t");
    length = (size_t)string_length;
    tar_memory_check_range(storage, offset_value, length, &offset);
    if (length != 0u)
        memcpy((unsigned char *)storage->data + offset, RSTRING_PTR(bytes), length);
    return Qnil;
}

static VALUE
tar_memory_closed_p(VALUE self)
{
    tar_handle_t *handle = tar_handle_from(self, &tar_memory_type);
    return (!handle || !handle->native || !tar_lifetime_live(handle->token)) ? Qtrue : Qfalse;
}

static VALUE
tar_memory_release(VALUE self)
{
    tar_handle_t *handle = tar_handle_from(self, &tar_memory_type);
    tar_memory_storage_t *storage;

    if (!handle || !handle->native) return Qnil;
    if (!tar_lifetime_live(handle->token))
        rb_raise(rb_eRuntimeError, "expired TinyActors native memory handle");
    storage = (tar_memory_storage_t *)handle->native;
    if (!storage->releasable)
        rb_raise(rb_eRuntimeError, "scoped memory cannot be released by the caller");
    if (storage->loans != 0u)
        rb_raise(rb_eRuntimeError, "memory has an outstanding arena loan");
    tar_memory_storage_free(storage);
    handle->native = NULL;
    tar_root_remove_all(handle);
    tar_lifetime_invalidate(handle->token);
    return Qnil;
}

/* The token is an owned reference consumed by the resulting Memory handle.
 * The owner is retained by the wrapper while the borrowed bytes are valid. */
VALUE
tar_memory_wrap_view(void *data, size_t size, size_t alignment,
                     tar_lifetime_t *token, VALUE owner, unsigned int kind,
                     unsigned int writable)
{
    tar_memory_storage_t *storage;
    VALUE object;
    tar_handle_t *handle;

    if (size != 0u && !data)
        rb_raise(rb_eArgError, "nonempty memory view has no data");
    if (!token)
        rb_raise(rb_eArgError, "memory view requires a lifetime token");
    storage = ALLOC(tar_memory_storage_t);
    storage->data = data;
    storage->allocation = NULL;
    storage->size = size;
    storage->alignment = alignment;
    storage->kind = kind;
    storage->writable = writable ? 1u : 0u;
    storage->releasable = 0u;
    storage->loans = 0u;
    storage->loan_root = Qnil;
    storage->rooted = 0u;
    object = tar_memory_wrap(storage, token, owner);
    handle = tar_handle_from(object, &tar_memory_type);
    tar_handle_set_cleanup(handle, tar_memory_storage_free);
    return object;
}

/* Allocator adapters use these C-only helpers to borrow a caller-owned chunk.
 * A loan roots the Memory wrapper until the matching release, so a callback
 * cannot leave the core pointing at storage reclaimed by Ruby GC. */
ta_status_t
tar_memory_chunk_acquire(VALUE object, ta_chunk_t *out_chunk)
{
    tar_handle_t *handle = tar_handle_from(object, &tar_memory_type);
    tar_memory_storage_t *storage;

    if (!out_chunk) return TA_INVALID;
    *out_chunk = (ta_chunk_t){0};
    tar_check_handle(handle);
    storage = (tar_memory_storage_t *)handle->native;
    if (!storage || storage->kind != TAR_MEMORY_CALLER || !storage->releasable)
        return TA_INVALID;
    if (storage->loans == UINT_MAX) return TA_LIMIT;
    if (!storage->rooted) {
        storage->loan_root = object;
        rb_gc_register_address(&storage->loan_root);
        storage->rooted = 1u;
    }
    ++storage->loans;
    out_chunk->data = storage->data;
    out_chunk->layout.size = storage->size;
    out_chunk->layout.alignment = storage->alignment;
    return TA_OK;
}

ta_status_t
tar_memory_chunk_release(VALUE object, const ta_chunk_t *chunk)
{
    tar_handle_t *handle = tar_handle_from(object, &tar_memory_type);
    tar_memory_storage_t *storage;

    if (!chunk) return TA_INVALID;
    tar_check_handle(handle);
    storage = (tar_memory_storage_t *)handle->native;
    if (!storage || storage->kind != TAR_MEMORY_CALLER || storage->loans == 0u ||
        chunk->data != storage->data || chunk->layout.size != storage->size ||
        chunk->layout.alignment != storage->alignment)
        return TA_INVALID;
    --storage->loans;
    if (storage->loans == 0u && storage->rooted) {
        rb_gc_unregister_address(&storage->loan_root);
        storage->rooted = 0u;
        storage->loan_root = Qnil;
    }
    return TA_OK;
}

static ta_arena_t *tar_memory_fixture_arena;
static tar_lifetime_t *tar_memory_fixture_arena_token;
static void *tar_memory_fixture_data;
static size_t tar_memory_fixture_size;
static ta_chunk_t tar_memory_fixture_loan;

static ta_status_t
tar_memory_fixture_acquire(void *user, const ta_layout_t *minimum, ta_chunk_t *out)
{
    size_t alignment, size, total;
    uintptr_t address;
    void *base;

    (void)user;
    if (!minimum || !out || minimum->alignment == 0u) return TA_INVALID;
    alignment = minimum->alignment;
    size = minimum->size;
    if (alignment > SIZE_MAX - sizeof(void *) ||
        size > SIZE_MAX - alignment - sizeof(void *)) return TA_LIMIT;
    total = size + alignment + sizeof(void *);
    base = malloc(total);
    if (!base) return TA_NO_MEMORY;
    address = ((uintptr_t)base + sizeof(void *) + alignment - 1u) &
              ~(uintptr_t)(alignment - 1u);
    ((void **)address)[-1] = base;
    out->data = (void *)address;
    out->layout = *minimum;
    return TA_OK;
}

static void
tar_memory_fixture_release(void *user, const ta_chunk_t *chunk)
{
    (void)user;
    if (chunk && chunk->data) free(((void **)chunk->data)[-1]);
}

static VALUE
tar_memory_test_arena_view(VALUE self, VALUE size_value, VALUE alignment_value)
{
    static const ta_chunk_allocator_t allocator = {
        NULL, tar_memory_fixture_acquire, tar_memory_fixture_release
    };
    ta_arena_options_t options = {256u, 4096u, 4096u, 1024u};
    ta_layout_t layout;
    ta_status_t status;
    tar_lifetime_t *view_token;

    (void)self;
    if (tar_memory_fixture_arena)
        rb_raise(rb_eRuntimeError, "arena memory fixture is busy");
    tar_memory_require_size(size_value, &layout.size, "size");
    tar_memory_require_size(alignment_value, &layout.alignment, "alignment");
    if (layout.size == 0u || layout.alignment == 0u ||
        (layout.alignment & (layout.alignment - 1u)) != 0u)
        rb_raise(rb_eArgError, "fixture requires a nonzero allocation and power-of-two alignment");
    status = ta_arena_create(&allocator, &options, &tar_memory_fixture_arena);
    if (status == TA_OK)
        status = ta_arena_alloc(tar_memory_fixture_arena, &layout,
                                &tar_memory_fixture_data);
    if (status != TA_OK) {
        if (tar_memory_fixture_arena) ta_arena_close(tar_memory_fixture_arena);
        tar_memory_fixture_arena = NULL;
        rb_raise(rb_eRuntimeError, "arena memory fixture failed with status %d", (int)status);
    }
    tar_memory_fixture_size = layout.size;
    tar_memory_fixture_arena_token = tar_lifetime_new(NULL);
    view_token = tar_lifetime_new(tar_memory_fixture_arena_token);
    return tar_memory_wrap_view(tar_memory_fixture_data, tar_memory_fixture_size,
                                layout.alignment, view_token, Qnil,
                                TAR_MEMORY_ARENA, 1u);
}

static VALUE
tar_memory_test_arena_close(VALUE self)
{
    (void)self;
    if (tar_memory_fixture_arena) {
        tar_lifetime_invalidate(tar_memory_fixture_arena_token);
        tar_lifetime_release(tar_memory_fixture_arena_token);
        tar_memory_fixture_arena_token = NULL;
        ta_arena_close(tar_memory_fixture_arena);
        tar_memory_fixture_arena = NULL;
        tar_memory_fixture_data = NULL;
        tar_memory_fixture_size = 0u;
    }
    return Qnil;
}

static tar_lifetime_t *tar_memory_context_token;
static ta_arena_t *tar_memory_context_arena;
static ta_system_t *tar_memory_context_system;
static ta_definition_t *tar_memory_context_definition;
static ta_actor_ref_t tar_memory_context_actor;
static void *tar_memory_context_data;
static size_t tar_memory_context_size;
static void *tar_memory_context_inspection_data;
static size_t tar_memory_context_inspection_size;
static VALUE tar_memory_context_view = Qnil;
static VALUE tar_memory_context_inspection_view = Qnil;
static unsigned int tar_memory_context_view_rooted;
static unsigned int tar_memory_context_inspection_rooted;
static unsigned int tar_memory_context_readonly_rejected;

static void
tar_memory_context_root(VALUE *slot, unsigned int *rooted, VALUE object)
{
    *slot = object;
    if (!*rooted) {
        rb_gc_register_address(slot);
        *rooted = 1u;
    }
}

static VALUE
tar_memory_context_write(VALUE self)
{
    static const unsigned char pattern[6] = {0x01u, 0xffu, 0x00u,
                                              0x61u, 0x62u, 0x63u};
    VALUE bytes;
    VALUE memory;
    tar_lifetime_t *view_token;

    (void)self;
    view_token = tar_lifetime_new(tar_memory_context_token);
    memory = tar_memory_wrap_view(tar_memory_context_data,
                                  tar_memory_context_size, 1u, view_token,
                                  Qnil, TAR_MEMORY_CONTEXT, 1u);
    tar_memory_context_root(&tar_memory_context_view,
                            &tar_memory_context_view_rooted, memory);
    bytes = rb_str_new((const char *)pattern, (long)sizeof(pattern));
    rb_enc_associate(bytes, rb_ascii8bit_encoding());
    return rb_funcall(memory, rb_intern("write"), 2, INT2NUM(0), bytes);
}

static VALUE
tar_memory_context_reject_readonly_write(VALUE self)
{
    VALUE memory;
    tar_lifetime_t *view_token;

    (void)self;
    view_token = tar_lifetime_new(tar_memory_context_token);
    memory = tar_memory_wrap_view(tar_memory_context_inspection_data,
                                  tar_memory_context_inspection_size, 1u,
                                  view_token, Qnil, TAR_MEMORY_INSPECTION, 0u);
    tar_memory_context_root(&tar_memory_context_inspection_view,
                            &tar_memory_context_inspection_rooted, memory);
    return rb_funcall(memory, rb_intern("write"), 2, INT2NUM(0),
                      rb_str_new_cstr("x"));
}

static ta_status_t
tar_memory_context_run(ta_context_t *context, ta_action_kind_t action_kind,
                        ta_code_t code)
{
    ta_mut_bytes_t bytes;
    ta_context_view_t view;
    ta_status_t status;
    int raised;

    (void)action_kind;
    (void)code;
    status = ta_context_bytes(context, &bytes);
    if (status != TA_OK) return status;
    tar_memory_context_data = bytes.data;
    tar_memory_context_size = bytes.size;
    if (bytes.size < 6u) return TA_LIMIT;

    /* Inspection exposes the same bytes through a read-only ta_bytes_t.
     * Protect the Ruby call so its expected exception cannot unwind through
     * the core's execution frames. */
    ta_context_inspect(context, &view);
    if (view.state.storage != TA_DATA_BYTES ||
        view.state.as.bytes.data != bytes.data ||
        view.state.as.bytes.size != bytes.size)
        return TA_EXECUTION_ERROR;
    tar_memory_context_inspection_data = (void *)view.state.as.bytes.data;
    tar_memory_context_inspection_size = view.state.as.bytes.size;
    raised = 0;
    (void)rb_protect(tar_memory_context_reject_readonly_write, Qnil, &raised);
    tar_memory_context_readonly_rejected = raised ? 1u : 0u;
    if (!tar_memory_context_readonly_rejected) return TA_EXECUTION_ERROR;

    raised = 0;
    (void)rb_protect(tar_memory_context_write, Qnil, &raised);
    if (raised) return TA_EXECUTION_ERROR;
    return TA_OK;
}

static void
tar_memory_context_close(void)
{
    if (tar_memory_context_view_rooted) {
        rb_gc_unregister_address(&tar_memory_context_view);
        tar_memory_context_view_rooted = 0u;
        tar_memory_context_view = Qnil;
    }
    if (tar_memory_context_inspection_rooted) {
        rb_gc_unregister_address(&tar_memory_context_inspection_view);
        tar_memory_context_inspection_rooted = 0u;
        tar_memory_context_inspection_view = Qnil;
    }
    if (tar_memory_context_actor.id != 0u && tar_memory_context_system) {
        (void)ta_actor_stop(tar_memory_context_system, tar_memory_context_actor);
        (void)ta_system_run(tar_memory_context_system,
                            &(ta_run_options_t){0u, {100u, 100u}},
                            &(ta_run_result_t){0});
        (void)ta_actor_release(tar_memory_context_system, tar_memory_context_actor);
        tar_memory_context_actor = (ta_actor_ref_t){0};
    }
    if (tar_memory_context_definition) {
        ta_definition_release(tar_memory_context_definition);
        tar_memory_context_definition = NULL;
    }
    if (tar_memory_context_system) {
        (void)ta_system_destroy(tar_memory_context_system);
        tar_memory_context_system = NULL;
    }
    if (tar_memory_context_arena) {
        ta_arena_close(tar_memory_context_arena);
        tar_memory_context_arena = NULL;
    }
    if (tar_memory_context_token) {
        tar_lifetime_invalidate(tar_memory_context_token);
        tar_lifetime_release(tar_memory_context_token);
        tar_memory_context_token = NULL;
    }
    tar_memory_context_data = NULL;
    tar_memory_context_size = 0u;
    tar_memory_context_inspection_data = NULL;
    tar_memory_context_inspection_size = 0u;
    tar_memory_context_readonly_rejected = 0u;
}

static VALUE
tar_memory_test_context_view(VALUE self)
{
    static const ta_chunk_allocator_t allocator = {
        NULL, tar_memory_fixture_acquire, tar_memory_fixture_release
    };
    ta_system_config_t config;
    ta_chart_state_t states[2] = {{0}};
    ta_action_t action = {0};
    ta_block_t block = {0};
    ta_blocks_t blocks = {0};
    ta_chart_t chart = {0};
    ta_definition_result_t definition_result;
    ta_status_t status;

    (void)self;
    tar_memory_context_close();
    ta_system_config_init(&config);
    config.data_model = ta_text_from_cstr("bytes");
    config.session_prefix = ta_text_from_cstr("rawmemoryfixture");
    config.data.run = tar_memory_context_run;
    status = ta_arena_create(&allocator, &config.arenas[TA_LIFETIME_SYSTEM],
                             &tar_memory_context_arena);
    if (status == TA_OK)
        status = ta_system_create(tar_memory_context_arena, &config,
                                  &tar_memory_context_system);
    action.kind = TA_ACTION_CUSTOM;
    action.as.custom.code = 1u;
    block.actions = &action;
    block.count = 1u;
    blocks.blocks = &block;
    blocks.count = 1u;
    states[0].id = "idle";
    states[0].kind = TA_CHART_STATE;
    states[0].onentry = blocks;
    chart.name = "memory-fixture";
    chart.data_model = "bytes";
    chart.data_layout.storage = TA_DATA_BYTES;
    chart.data_layout.bytes_layout.size = 8u;
    chart.data_layout.bytes_layout.alignment = 1u;
    chart.states = states;
    if (status == TA_OK)
        status = ta_definition_build(tar_memory_context_system, &chart,
                                     &definition_result);
    if (status == TA_OK) {
        tar_memory_context_definition = definition_result.definition;
        status = ta_actor_spawn(tar_memory_context_system,
                                &(ta_spawn_options_t){.definition = tar_memory_context_definition},
                                &tar_memory_context_actor);
    }
    if (status == TA_OK) {
        tar_memory_context_token = tar_lifetime_new(NULL);
        tar_memory_context_readonly_rejected = 0u;
    }
    if (status == TA_OK)
        status = ta_system_run(tar_memory_context_system,
                               &(ta_run_options_t){0u, {100u, 100u}},
                               &(ta_run_result_t){0});
    if (status != TA_OK) {
        tar_memory_context_close();
        rb_raise(rb_eRuntimeError, "context bytes fixture failed with status %d", (int)status);
    }
    tar_lifetime_invalidate(tar_memory_context_token);
    return tar_memory_context_view;
}

static VALUE
tar_memory_test_context_state(VALUE self)
{
    ta_actor_info_t info;
    (void)self;
    if (!tar_memory_context_system || tar_memory_context_actor.id == 0u)
        rb_raise(rb_eRuntimeError, "context bytes fixture is not active");
    if (ta_actor_inspect(tar_memory_context_system, tar_memory_context_actor, &info) != TA_OK ||
        info.state.storage != TA_DATA_BYTES)
        rb_raise(rb_eRuntimeError, "context bytes fixture state is unavailable");
    return rb_str_new((const char *)info.state.as.bytes.data,
                      (long)info.state.as.bytes.size);
}

static VALUE
tar_memory_test_context_close(VALUE self)
{
    (void)self;
    tar_memory_context_close();
    return Qnil;
}

static VALUE
tar_memory_test_context_readonly_rejected(VALUE self)
{
    (void)self;
    return tar_memory_context_readonly_rejected ? Qtrue : Qfalse;
}

static VALUE
tar_memory_test_loaned(VALUE self)
{
    tar_memory_storage_t *storage;
    VALUE object;
    ta_status_t status;
    (void)self;
    tar_memory_allocate_storage(8u, 8u, &storage);
    tar_memory_fixture_loan = (ta_chunk_t){0};
    object = tar_memory_wrap(storage, tar_lifetime_new(NULL), Qnil);
    tar_handle_set_cleanup(tar_handle_from(object, &tar_memory_type),
                           tar_memory_storage_free);
    status = tar_memory_chunk_acquire(object, &tar_memory_fixture_loan);
    if (status != TA_OK) rb_raise(rb_eRuntimeError, "memory loan fixture failed");
    return object;
}

static VALUE
tar_memory_test_clear_loan(VALUE self, VALUE object)
{
    ta_status_t status;
    (void)self;
    status = tar_memory_chunk_release(object, &tar_memory_fixture_loan);
    if (status != TA_OK) rb_raise(rb_eRuntimeError, "memory loan fixture release failed");
    tar_memory_fixture_loan = (ta_chunk_t){0};
    return Qnil;
}

void
tar_init_memory(VALUE raw)
{
    VALUE singleton;
    (void)raw;
    tar_memory_class = rb_const_get(tar_mRaw, rb_intern("Memory"));
    if (!RB_TYPE_P(tar_memory_class, T_CLASS))
        rb_raise(rb_eTypeError, "TinyActors::Raw::Memory must be a class");
    singleton = rb_singleton_class(tar_memory_class);
    rb_define_singleton_method(tar_memory_class, "allocate",
                               tar_memory_allocate, -1);
    rb_define_method(tar_memory_class, "size", tar_memory_size, 0);
    rb_define_method(tar_memory_class, "read", tar_memory_read, 2);
    rb_define_method(tar_memory_class, "write", tar_memory_write, 2);
    rb_define_method(tar_memory_class, "closed?", tar_memory_closed_p, 0);
    rb_define_method(tar_memory_class, "release", tar_memory_release, 0);
    rb_define_private_method(singleton, "__test_arena_view",
                             tar_memory_test_arena_view, 2);
    rb_define_private_method(singleton, "__test_arena_close",
                             tar_memory_test_arena_close, 0);
    rb_define_private_method(singleton, "__test_context_view",
                             tar_memory_test_context_view, 0);
    rb_define_private_method(singleton, "__test_context_state",
                             tar_memory_test_context_state, 0);
    rb_define_private_method(singleton, "__test_context_readonly_rejected",
                             tar_memory_test_context_readonly_rejected, 0);
    rb_define_private_method(singleton, "__test_context_close",
                             tar_memory_test_context_close, 0);
    rb_define_private_method(singleton, "__test_loaned",
                             tar_memory_test_loaned, 0);
    rb_define_private_method(singleton, "__test_clear_loan",
                             tar_memory_test_clear_loan, 1);
}
