# Open API details before ABI freeze

The semantic surface is decided; these representational details should remain open until implementation pressure resolves them.

## 1. `ta_atom` versus dedicated binding IDs

Names can initially use interned `ta_atom`. A compiled definition may later replace hot-path names with compact data-slot IDs while retaining atoms for diagnostics and dynamic models.

## 2. Location handles

The current proposal re-evaluates a location expression for read/assign. A future optimization could add an opaque prepared location handle, but only if real profiles benefit.

## 3. Foreach count width

`uint32_t` is appropriate for the compact runtime goal. A plugin requiring collections larger than this can reject them or a future ABI can add a wider profile.

## 4. Parameter duplicates

`make_params` receives an ordered list with duplicates preserved. Each data-model profile must document how its aggregate value represents duplicate names.

## 5. Literal metadata

`ta_literal` needs enough metadata for importers and native builders without becoming an XML DOM. Media type + opaque bytes + optional frontend tag is the current direction.

## 6. Incoming foreign values

`import_event_data(const ta_value *foreign, ...)` assumes a value can identify its own implementation through its vtable. If transports need byte payloads without creating a `ta_value`, a second literal/bytes path may be added.

## 7. Definition capability discovery

The compiler should compute a bitset of required data-model capabilities. `prepare_definition` can validate the plugin in O(1) before inspecting detailed IR.

None of these questions change the architectural decision that the vtable is semantic rather than XML-shaped.
