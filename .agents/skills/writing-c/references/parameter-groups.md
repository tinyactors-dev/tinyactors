# Parameter groups

The snippets are proposed interfaces, not a standalone header. Existing types,
headers and declaration attributes must be supplied by the target project. The
examples use stdxml's prefix and types. Every example obeys the three-parameter
limit. Copy only the families that actual call sites need.

## Contents

1. Buffer views
2. Initialization configurations
3. Transfers and progress
4. Semantic keys
5. Borrowed dependencies
6. Transactions
7. Outcomes and cursors
8. Control operations
9. Public operations
10. Test plans and generator builders
11. Callbacks

## 1. Buffer views: pointer plus extent

Use an existing read-only byte view for source bytes. Add writable and typed
views only where their units or mutability differ. Validate pointer/extent pairs
before access. Do not convert scalar counts into byte counts implicitly.

```c
/* Borrowed views; lifetime ends on return, including suspension [project R4]. */
typedef struct stdxml__byte_buffer_call_scoped {
    u8 *pointer;
    usize capacity;
} stdxml__byte_buffer_call_scoped;

typedef struct stdxml__scalar_buffer_call_scoped {
    u32 *pointer;
    usize capacity;
} stdxml__scalar_buffer_call_scoped;

typedef struct stdxml__segment_map_call_scoped {
    const u16 *pointer;
    u32 count;
} stdxml__segment_map_call_scoped;
```

`capacity` is bytes for the byte buffer and scalars for the scalar buffer.
Reuse `stdxml__bytes_call_scoped` for immutable source bytes. Add a typed input
view when required; do not invent one solely for symmetry.

## 2. Initialization: allocator, layout, limits

Create subsystem-specific configurations. Share element layout, but preserve
distinct base/overlay budgets, depth limits, capacities and allocation policies.
Initialization settings are input; fuel is a separate in/out execution budget.

```c
/* Allocation layout [project §5.2, R2–R4]. */
typedef struct stdxml__element_layout {
    u32 bytes;
    u32 alignment;
} stdxml__element_layout;

/* Allocator pointer is borrowed for this call; owner copies the allowed
 * allocator representation, not this configuration [project R4, §5.2]. */
typedef struct stdxml__typed_column_configuration_call_scoped {
    const stdxml_allocator *allocator;
    stdxml__element_layout element;
    u32 base_elements;
    u64 budget_bytes;
} stdxml__typed_column_configuration_call_scoped;

typedef struct stdxml__unicode_normalizer_configuration_call_scoped {
    const stdxml_allocator *allocator;
    u32 form;
    u64 budget_bytes;
} stdxml__unicode_normalizer_configuration_call_scoped;

STDXML_INTERNAL STDXML_MUST_USE stdxml__result
stdxml__typed_column_initialize(
    stdxml__typed_column *column,
    const stdxml__typed_column_configuration_call_scoped *configuration,
    u32 *fuel);
```

Use analogous configurations for trees, arenas, rings, stacks, registries and
handle tables. Preserve existing validation and defaults. Public configuration
structs use the project's size/defaulting contract; internal structs do not need
public ABI machinery.

## 3. Streaming: offered transfer versus committed progress

The offered input, EOF indication, writable output and fuel describe one step.
Consumed and produced counts describe committed progress. A const argument
struct can contain a mutable fuel pointer: const protects the description, not
the pointed-to counter. This preserves sharing one counter across nested calls.

```c
/* Borrowed for one invocation, never retained across suspension
 * [project R4–R5, R18–R19]. */
typedef struct stdxml__byte_transfer_call_scoped {
    stdxml__bytes_call_scoped input;
    stdxml__byte_buffer_call_scoped output;
    bool final;
    u32 *fuel;
} stdxml__byte_transfer_call_scoped;

typedef struct stdxml__scalar_decode_transfer_call_scoped {
    stdxml__bytes_call_scoped input;
    stdxml__scalar_buffer_call_scoped output;
    bool final;
    u32 *fuel;
} stdxml__scalar_decode_transfer_call_scoped;

typedef struct stdxml__transfer_progress {
    usize consumed;
    usize produced;
} stdxml__transfer_progress;

STDXML_INTERNAL STDXML_MUST_USE stdxml__result
stdxml__unicode_normalize(
    stdxml__unicode_normalizer *normalizer,
    const stdxml__byte_transfer_call_scoped *transfer,
    stdxml__transfer_progress *output);
```

For byte-to-byte operations both counts are bytes. For decoding, consumed counts
bytes and produced counts scalars. Document this per operation. Validation and
single-scalar encoding use smaller dedicated operation types, rather than dummy
fields in a universal transfer. Existing streaming functions publish progress
on yields; atomic lookups can leave outputs untouched on yields. Do not impose
one output policy on both.

## 4. Semantic keys: identity fields

Group values that jointly identify an object. Keep the key pointer-free. Arity
at lookup is one value; registration covers a range and keeps its existing
description type. Prefix spelling is distinct from expanded-name equality.

```c
/* Name and command identity [project §5.4; XP31 §2.1.1]. */
typedef struct stdxml__expanded_name {
    u32 namespace_atom;
    u32 local_name_atom;
} stdxml__expanded_name;

typedef struct stdxml__lexical_name {
    stdxml__expanded_name expanded;
    u32 prefix_atom;
} stdxml__lexical_name;

typedef struct stdxml__command_key {
    u32 kind;
    stdxml__expanded_name name;
    u32 arity;
} stdxml__command_key;

/* Fuel is borrowed for the lookup invocation [project R4, R19]. */
typedef struct stdxml__command_lookup_call_scoped {
    stdxml__command_key key;
    u32 *fuel;
} stdxml__command_lookup_call_scoped;

typedef struct stdxml__identifier_lookup {
    bool found;
    u32 identifier;
} stdxml__identifier_lookup;

STDXML_INTERNAL STDXML_MUST_USE stdxml__result
stdxml__command_registry_lookup(
    const stdxml__command_registry *registry,
    const stdxml__command_lookup_call_scoped *lookup,
    stdxml__identifier_lookup *output);
```

Map integer keys and byte keys should have explicit operation variants where
possible. If retaining the existing dual-key interface, document its existing
inactive-field constraints rather than silently changing them.

## 5. Borrowed dependencies: metadata paired with storage

Use call-scoped views for separately owned rows/order arrays, a matching atom
table, comparator/context or scheduler units. Persistent metadata remains free
of these pointers. Use read-only counterparts for read-only operations.

```c
/* Paired storage, borrowed until return/suspension [project R4, R18, R22]. */
typedef struct stdxml__hash_map_storage_call_scoped {
    stdxml__hash_map *map;
    stdxml__hash_map_row *rows;
    u32 *order;
} stdxml__hash_map_storage_call_scoped;

typedef struct stdxml__integer_map_assignment_call_scoped {
    u32 key;
    u32 value;
    u32 *fuel;
} stdxml__integer_map_assignment_call_scoped;

STDXML_INTERNAL STDXML_MUST_USE stdxml__result
stdxml__hash_map_put(
    const stdxml__hash_map_storage_call_scoped *storage,
    const stdxml__integer_map_assignment_call_scoped *assignment);
```

For qualified-name interning, the operation can contain the borrowed atom table,
lexical name and fuel. This does not authorize storing the atom-table pointer in
the name table. Do not cast away const to share a mutable view.

## 6. Transactions: coordinated mutation

An append describes source values, the common column count, row count and fuel.
The destination stays separate. Grouping does not change allocation, rollback,
fuel reservation or commit order.

```c
/* Atomic lockstep append; views live only during the invocation
 * [project §5.2, §5.5, R4, R18]. */
typedef struct stdxml__column_append_call_scoped {
    const stdxml__column_value_call_scoped *values;
    u32 column_count;
    u32 row_count;
    u32 *fuel;
} stdxml__column_append_call_scoped;

STDXML_INTERNAL STDXML_MUST_USE stdxml__result
stdxml__typed_column_append_rows(
    stdxml__typed_column *columns,
    const stdxml__column_append_call_scoped *append,
    u32 *output);

/* Copy source and remapping borrowed until return/suspension [project §5.5, R4]. */
typedef struct stdxml__packed_tree_copy_step_call_scoped {
    const stdxml__packed_tree *source;
    stdxml__segment_map_call_scoped segments;
    u32 *fuel;
} stdxml__packed_tree_copy_step_call_scoped;

STDXML_INTERNAL STDXML_MUST_USE stdxml__result
stdxml__packed_tree_copy_step(
    stdxml__packed_tree *destination,
    const stdxml__packed_tree_copy_step_call_scoped *operation);
```

Keep integer copy cursors in the existing destination state. Reconstruct borrowed
source views on retry. A one-row append can use a smaller operation type.

## 7. Outcomes and cursors

Group outputs returned together: found/identifier, handle/coalesced,
description/implementation, chunk/slot, count/end-of-stream. Outcomes with
borrowed pointers also need the call-scoped lifetime classification. Do not
invent a null sentinel where index zero is valid.

```c
/* Request publication result [project §5.12, R18]. */
typedef struct stdxml__request_push_outcome {
    u32 handle;
    bool coalesced;
} stdxml__request_push_outcome;

STDXML_INTERNAL STDXML_MUST_USE stdxml__result
stdxml__packed_tree_cursor_next(
    const stdxml__packed_tree *tree,
    stdxml__packed_tree_cursor *cursor,
    stdxml__identifier_lookup *output);
```

Cursor is intentional mutable continuation state. Returned item is output.
Keep them separate even when an extra wrapper would reduce parameter count.
For APIs with different miss semantics, retain those semantics explicitly.

## 8. Control operations

Use descriptions for who/what/queue position and execution budget. Reuse the
existing request description and fiber context. Avoid general-purpose unions
with fields for unrelated commands.

```c
/* Borrowed request description and fuel [project §5.12, R4, R18–R19]. */
typedef struct stdxml__request_push_call_scoped {
    const stdxml__request_description *description;
    bool front;
    u32 *fuel;
} stdxml__request_push_call_scoped;

STDXML_INTERNAL STDXML_MUST_USE stdxml__result
stdxml__request_deque_push(
    stdxml__request_deque *deque,
    const stdxml__request_push_call_scoped *operation,
    stdxml__request_push_outcome *output);
```

Fiber transitions group fiber/request handles with fuel. Scheduler execution
groups its borrowed fiber table, unit callbacks and quantum; its existing run
outcome stays the output. Do not introduce hidden ambient fuel or engine state.

## 9. Public operations

The engine owner already consumes one parameter. Group the typed target handle
and offered buffer in an operation. Preserve boundary validation and public
configuration compatibility rules; operations are not automatically configs.

```c
/* Host buffers are borrowed only for the call [project §6.2, R5, R21]. */
typedef struct stdxml_stream_write_operation {
    stdxml_stream stream;
    stdxml_bytes bytes;
} stdxml_stream_write_operation;

typedef struct stdxml_stream_read_operation {
    stdxml_stream stream;
    void *buffer;
    size_t capacity;
} stdxml_stream_read_operation;

typedef struct stdxml_stream_read_outcome {
    size_t read_count;
    bool at_end;
} stdxml_stream_read_outcome;

typedef struct stdxml_diagnostic_location {
    stdxml_job job;
    uint32_t index;
} stdxml_diagnostic_location;

stdxml_status stdxml_stream_write(
    stdxml_engine *engine,
    const stdxml_stream_write_operation *operation,
    size_t *accepted);

stdxml_status stdxml_stream_read(
    stdxml_engine *engine,
    const stdxml_stream_read_operation *operation,
    stdxml_stream_read_outcome *output);

stdxml_status stdxml_diagnostic_read(
    stdxml_engine *engine,
    const stdxml_diagnostic_location *location,
    stdxml_diagnostic_information *output);
```

Public naming follows the public API's lifetime rules; internal call-scoped types
must not leak into the public header. Update ABI/API documentation when needed.

## 10. Tests and generator builders

Separate expected input/output from execution variations. Chunking, output
capacity, fuel, allocation refusal and random seeds belong in a test plan.
Preserve the exact executed cases and expectations when changing helpers.

```c
/* Normalization verification [UAX15 §4; project §10]. */
typedef struct test_normalization_case_call_scoped {
    stdxml__bytes_call_scoped input;
    stdxml__bytes_call_scoped expected;
    u32 form;
} test_normalization_case_call_scoped;

typedef struct test_normalization_execution {
    usize input_chunk_bytes;
    usize output_capacity_bytes;
    u32 fuel_per_call;
    u64 allocation_failure_call;
    u64 seed;
    usize split_boundary;
} test_normalization_execution;

static u64 test_normalization_plan(
    const test_normalization_case_call_scoped *test_case,
    const test_normalization_execution *execution);
```

Hosted generators can group records, decomposition storage, count and capacity
in a builder. Number parsers group base/maximum as constraints. Those builders
can hold hosted pointers; they never enter library storage. Regenerated outputs
must remain byte-identical after a generator-only interface change.

## 11. Callbacks

Externally required callback signatures get an exact exception with an owner and
reason. Project-owned callbacks follow the limit; being in a vtable alone is not
a reason for an exception.

```c
/* Stable-sort comparison [project §8 stable_sort, R18, R22]. */
typedef struct stdxml__sort_operands {
    u32 left;
    u32 right;
} stdxml__sort_operands;

typedef stdxml__result (*stdxml__sort_compare_function)(
    void *context,
    const stdxml__sort_operands *operands,
    i32 *order);
```

Update callback typedefs, implementing functions, assignments and invocations
together. Do not use casts, variadic forwarding or unprototyped functions to
hide mismatches.
