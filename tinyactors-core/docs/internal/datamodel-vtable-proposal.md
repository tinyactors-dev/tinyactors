# Proposed data-model vtable

This is the intended semantic surface. Exact naming and argument packing can still change before ABI stabilization.

```c
typedef struct ta_datamodel_vtable {
    /* Definition lifetime. */
    ta_result (*prepare_definition)(
        void *self,
        const ta_definition *definition,
        ta_allocator allocator,
        void **shared);

    void (*destroy_definition)(
        void *self,
        void *shared,
        ta_allocator allocator);

    /* Per-actor adapter storage and host binding. */
    uint32_t (*instance_size)(void *self, void *shared);
    uint32_t (*instance_alignment)(void *self, void *shared);

    ta_result (*init_instance)(
        void *self,
        void *shared,
        void *instance,
        const ta_value *binding,
        const ta_dm_host *host);

    void (*destroy_instance)(
        void *self,
        void *shared,
        void *instance);

    /* Named data bindings. */
    ta_op_status (*declare)(
        void *self,
        void *shared,
        void *instance,
        ta_atom name,
        const ta_dm_host *host,
        ta_fault *fault);

    ta_op_status (*set_named)(
        void *self,
        void *shared,
        void *instance,
        ta_atom name,
        const ta_value *value,
        const ta_dm_host *host,
        ta_fault *fault);

    /* Expressions and locations. */
    ta_op_status (*eval_condition)(
        void *self,
        void *shared,
        void *instance,
        ta_expr_id expr,
        const ta_dm_host *host,
        bool *out,
        ta_fault *fault);

    ta_op_status (*eval_value)(
        void *self,
        void *shared,
        void *instance,
        ta_expr_id expr,
        const ta_dm_host *host,
        ta_value *out,
        ta_fault *fault);

    ta_op_status (*read_location)(
        void *self,
        void *shared,
        void *instance,
        ta_expr_id location,
        const ta_dm_host *host,
        ta_value *out,
        ta_fault *fault);

    ta_op_status (*assign)(
        void *self,
        void *shared,
        void *instance,
        ta_expr_id location,
        const ta_value *value,
        const ta_dm_host *host,
        ta_fault *fault);

    /* Opaque literal/content conversion. */
    ta_op_status (*import_literal)(
        void *self,
        void *shared,
        void *instance,
        const ta_literal *literal,
        const ta_dm_host *host,
        ta_value *out,
        ta_fault *fault);

    ta_op_status (*import_event_data)(
        void *self,
        void *shared,
        void *instance,
        const ta_value *foreign,
        const ta_dm_host *host,
        ta_value *out,
        ta_fault *fault);

    /* Script action. */
    ta_op_status (*execute_script)(
        void *self,
        void *shared,
        void *instance,
        ta_expr_id script,
        const ta_dm_host *host,
        ta_fault *fault);

    /* Stable shallow-copy iteration. */
    ta_op_status (*foreach_snapshot)(
        void *self,
        void *shared,
        void *instance,
        ta_expr_id collection_expr,
        const ta_dm_host *host,
        ta_value *snapshot,
        uint32_t *count,
        ta_fault *fault);

    ta_op_status (*foreach_item)(
        void *self,
        void *shared,
        void *instance,
        const ta_value *snapshot,
        uint32_t index,
        const ta_dm_host *host,
        ta_value *item,
        ta_value *index_value,
        ta_fault *fault);

    ta_op_status (*bind_iteration)(
        void *self,
        void *shared,
        void *instance,
        ta_atom item_name,
        ta_atom index_name, /* TA_ATOM_NONE if absent */
        const ta_value *item,
        const ta_value *index_value,
        const ta_dm_host *host,
        ta_fault *fault);

    /* Named key/value payload construction. */
    ta_op_status (*make_params)(
        void *self,
        void *shared,
        void *instance,
        const ta_named_value *params,
        uint32_t count,
        const ta_dm_host *host,
        ta_value *out,
        ta_fault *fault);

    /* Optional operational root view. */
    ta_result (*inspect)(
        void *self,
        void *shared,
        void *instance,
        const ta_dm_host *host,
        ta_value *out);
} ta_datamodel_vtable;
```

## Host binding

The per-instance adapter may be only a small binding around a host-owned object.
For CPython, `binding` can wrap the existing `PyObject *`; for CRuby a `VALUE`;
for Node a JavaScript reference; for C a native application object. Tinyactors
does not copy that object graph into interpreter state.

## `declare`

Creates a named data binding without necessarily assigning its eventual initial value. It exists because late binding requires declaration and initialization to be separate semantic steps.

## `set_named`

Sets a top-level named binding directly. The core uses it for initializers and environment-provided top-level overrides. It is not a replacement for `assign`, which operates on arbitrary data-model locations.

## `import_literal`

Consumes an opaque typed literal from the definition IR. This is intentionally not named `import_content` because the same mechanism can initialize data, provide assignment values, build content payloads, and support non-XML frontends.

## `foreach_snapshot`

Must produce the equivalent of SCXML's shallow copy. The returned `ta_value` is retained by the continuation while execution is suspended and by an in-process session image if captured there.

## `make_params`

Receives an ordered list and must not assume names are unique.

## `inspect`

Returns an independently owned host-native root value for operational tooling; the caller releases it using normal `ta_value` lifetime rules. Inspection is an operational API result, not statechart execution, so failures are `ta_result` values rather than `error.*` events. Tinyactors does not recursively materialize or serialize the graph, which keeps large host models such as LLM transcripts cheap to inspect at the runtime boundary.

## No persistence callbacks in core

There are deliberately no data-model `snapshot` / `restore` callbacks and no universal `ta_value.serialize`. Reconstruction receives the host binding again. Portable checkpoint codecs and host-reference resolution belong to an optional durability component.

## Optional operations

A plugin may leave unsupported capabilities null if the definition compiler can prove that the definition never requires them. Definition preparation must reject a definition that requires an unsupported semantic capability.
