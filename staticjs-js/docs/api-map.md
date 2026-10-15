# The API map: @tinyactors/staticjs and staticjs

Each item of the package's API next to the C layer's call (`src/js.c`) and
the staticjs calls behind it (`staticjs/include/staticjs.h`). An item with
no staticjs call behind it does not belong, except the conversion budget,
which ADR 0013 (decision 7) makes the package's: nothing in staticjs bounds
a conversion. Kept in step with `index.d.ts`, in the commit that changes
an item.

| API item | C layer | staticjs |
|---|---|---|
| `load()` | the module's instance; imports `staticjs_js.native`, `staticjs_js.release` | `sjs_version`, `sjs_wasm_allocator` |
| `StaticJS.version` | | `sjs_version` |
| `createContext({ limits, maxItems })` | `sjs_js_context_new` (the limits as eleven words) | `sjs_context_init`, `sjs_vm_init` |
| `Context.compile(source, { module, globals, modules })` | `sjs_js_compile_begin`, `sjs_js_compile_global`, `sjs_js_compile_module`, `sjs_js_compile` | `sjs_compile` with `sjs_compile_params.subject`; an `sjs_modules_vtable` over the modules given |
| `Context.value(v)` | conversion in, `sjs_js_keep` | the value API, `sjs_retain` |
| `Context.fromJSON(text)`, `fromCBOR(bytes)` | `sjs_js_read` | `sjs_value_read` (`SJS_FORMAT_JSON`, `SJS_FORMAT_CBOR`) |
| `Context.setClock`, `advanceClock`, `setSeed` | `sjs_js_set_clock`, `sjs_js_advance_clock`, `sjs_js_set_seed` | `sjs_vm_set_clock`, `sjs_vm_advance_clock`, `sjs_vm_set_seed` |
| `Context.liveBytes` | `sjs_js_live_bytes` | `sjs_context_live_bytes` |
| `Context.dispose()` | `sjs_js_cancel`, `sjs_js_drop_rows`, `sjs_js_context_free` | `sjs_vm_reset`, `sjs_release`, `sjs_program_release`, `sjs_vm_destroy`, `sjs_context_destroy` |
| `Program.functions()` | `sjs_js_function_count`, `sjs_js_function` | `sjs_program_function_count`, `sjs_program_function` |
| `Program.start(globals)` | conversion in, `sjs_js_start` | `sjs_vm_reset`, `sjs_vm_start` |
| `Program.dispose()` | `sjs_js_program_release` | `sjs_program_release` |
| `Run.step(fuel)` | `sjs_js_step`, `sjs_js_fuel_used`, `sjs_js_take`; on a fault `sjs_js_error`, the frames | `sjs_vm_run`, `sjs_vm_fuel_used`, `sjs_vm_take_result`, `sjs_vm_error` |
| `Run.frames()`, `StaticJSError.frames` | `sjs_js_frame_count`, `sjs_js_frame` | `sjs_vm_frame_count`, `sjs_vm_frame` (with `program` and `function`) |
| `Run.result()`, `resultValue()` | the result kept as a row; conversion out | `sjs_retain` |
| `Run.cancel()` | `sjs_js_cancel` | `sjs_vm_reset` |
| `Value.kind` | `sjs_js_row`, `sjs_js_kind` | `sjs_value.kind` |
| `Value.toJS()` | conversion out | the value API (below) |
| `Value.toString()`, `json()`, `cbor()` | `sjs_js_write` | `sjs_writer_init`, `sjs_writer_write` with `format` |
| `Value.start(args)` | conversion in, `sjs_js_call` | `sjs_vm_start` with `sjs_run_params.function` |
| `Value.dispose()` | `sjs_js_drop` | `sjs_release` |
| a function among the globals: a native | `sjs_js_native`; the import `staticjs_js.native`, `sjs_js_native_fault`; `staticjs_js.release` | `sjs_function_native`, `sjs_native_vtable.call` and `.release` |
| conversion in | `sjs_js_undefined`, `_null`, `_boolean`, `_number`, `_string`, `_bigint`, `_container`, `_append`, `_object_put`, `_map_set`, `_time`, `_row` | `sjs_string_new`, `sjs_bigint_new`, `sjs_array_new`, `sjs_array_append`, `sjs_object_new`, `sjs_object_put`, `sjs_set_new`, `sjs_set_add`, `sjs_map_new`, `sjs_map_set`, `sjs_time_new` |
| conversion out | `sjs_js_kind`, `_number_of`, `_boolean_of`, `_length`, `_string_of`, `_bigint_of`, `_item`, `_property`, `_entry`, `_time_of`, `_regexp_of` | `sjs_length`, `sjs_array_at`, `sjs_set_at`, `sjs_object_at`, `sjs_map_size`, `sjs_map_at`, `sjs_iterator_count`, `sjs_iterator_at`, `sjs_string_text`, `sjs_bigint_text`, `sjs_time_read`, `sjs_regexp_read` |
| `StaticJSError` | `sjs_js_error` | `sjs_error` of the call or the evaluator |

Not exposed: `sjs_vm_clock`, `sjs_vm_random`, `sjs_vm_set_random` (saving a
paused run elsewhere, which the package does not do); the parser's event
stream to a sink (a sink in JavaScript would cross per token; not asked
for); `sjs_object_get`, `sjs_map_get` (a conversion reads every entry);
`no_shapes`, `no_regex_leaf`, `no_regex_dfa` (staticjs's own tests).
