# `internal.h`

`internal.h` is the umbrella include for Tinyactors implementation files. It pulls the module headers together in dependency order.

External embedders should normally include `include/tinyactors.h`. Exact struct layouts remain free to change as the implementation is specialized for memory density.

The current umbrella order is:

```text
types -> arena/atom -> event_name/value/event/fault/clock
      -> configuration/history/continuation/timer
      -> datamodel/io/invoke
      -> definition/builder/actor/session_image/store/system/interpreter
```

`store.h` is only a compatibility boundary at present; the core has no storage protocol. Keep module headers individually self-contained enough for compile smoke tests rather than relying on accidental include ordering.
