# `internal.h`

`internal.h` is the umbrella include for Tinyactors implementation files. It pulls the internal module headers together in dependency order.

External embedders should include only the public header(s), primarily `include/tinyactors.h`. Internal headers are not ABI promises and may change as layouts are specialized for memory density.

The current umbrella includes:

```text
types -> arena/atom -> event_name/value/event/fault/clock
      -> configuration/history/continuation/timer
      -> datamodel/io/invoke
      -> definition/builder/actor/snapshot/store/system/interpreter
```

Keep module headers individually self-contained enough for compile smoke tests; do not rely on accidental include ordering beyond explicit dependencies.
