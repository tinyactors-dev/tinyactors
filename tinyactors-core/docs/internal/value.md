# Host values (`ta_value`)

A `ta_value` is an opaque host value plus vtable, declared in
`include/tinyactors.h`; core has no private value module.

Required semantics:

- `retain` / `release` keep host garbage collectors and reference counts correct.

That is the whole contract. Core never copies a value: an event, a timer, a
completion or a session image that outlives its source retains the same value
(ADR 0050).

Core deliberately has no universal `serialize` operation. A session image can retain a Python/Ruby/JavaScript/native host reference in-process without copying the referenced object. A portable durability module may provide a separate codec, stable object identity, or application-specific rebinding rule.

Bindings must obey their host runtime's object-lifetime rules. For example, a CPython value wrapper must hold a reference to its `PyObject *`; a Ruby wrapper must remain visible to Ruby's GC; a Node wrapper must use the appropriate N-API lifetime mechanism.
