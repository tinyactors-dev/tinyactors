# `value.h`

A `ta_value` is an opaque host value plus vtable.

Required semantics:

- `retain` / `release` keep host garbage collectors and reference counts correct;
- `clone` creates an independently owned value suitable for event/timer/session retention;
- `serialize` writes a representation sufficient for persistence when supported.

A value implementation may be non-serializable. In that case snapshot/unload fails rather than inventing semantics.

Bindings must obey their host runtime's object-lifetime rules. For example, a CPython value wrapper must hold a reference to its `PyObject *`; a Ruby wrapper must remain visible to Ruby's GC; a Node wrapper must use the appropriate N-API lifetime mechanism.
