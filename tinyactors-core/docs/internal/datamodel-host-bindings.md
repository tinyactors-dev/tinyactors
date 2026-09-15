# Host-language data-model bindings

The semantic vtable is designed so host runtimes can provide natural data models without forcing concurrency or asynchronous behavior into the core.

## CPython

A `ta_value` may wrap a `PyObject *` with normal reference counting. Data-model callbacks invoked from Python-driven execution can call CPython directly while the appropriate interpreter thread state/GIL discipline is satisfied by the embedding.

Possible mappings:

- named bindings -> globals/dict/custom scope;
- value expression -> compiled Python code object;
- location -> plugin-defined lvalue representation/evaluation;
- parameter aggregate -> Python mapping/custom duplicate-preserving object;
- literal import -> bytes/string/JSON/custom decoder.

## CRuby

A `ta_value` can wrap a rooted `VALUE`; the extension is responsible for Ruby GC visibility. The same semantic mapping applies to Ruby expressions and objects.

## Node / N-API

A value can wrap an N-API reference or plugin-owned native representation. If the host chooses to execute actor steps only on the JS thread, callbacks into JavaScript are straightforward. If it schedules engine work on native threads, it is the embedder's responsibility to marshal callbacks according to N-API rules.

## WASM in a browser

A native/freestanding data model can remain entirely inside linear memory. A JS-backed plugin can route semantic operations through imports. Asynchronous browser operations should leave the data-model callback synchronously and later re-enter the actor system as events.

## Embedded C / ESP32

A fixed-slot data model can make every expression and binding a compact integer ID and keep per-actor storage to a few bytes. Unsupported facilities can be omitted if the compiled definition does not require them.
