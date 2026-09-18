# `internal.h`

`internal.h` is the umbrella include for Tinyactors implementation files. It
pulls the private module headers together in dependency order, and the
amalgamation task uses the same order to lay out `tinyactors.c`.

The public API is `include/tinyactors.h` alone (ADR 0041). Private module
headers reach it through `types.h` and add only layouts and internal functions;
they must never redeclare a `TA_API` function. Exact struct layouts remain free
to change as the implementation is specialized for memory density.

The current umbrella order is:

```text
types -> arena/atom -> event_name/value/event/fault/routing/clock
      -> configuration/history/continuation/timer
      -> datamodel/io/invoke
      -> definition/builder/actor/session_image/store/system/interpreter
```

`store.h` is only a compatibility boundary at present; the core has no storage protocol. Keep module headers individually self-contained enough for compile smoke tests rather than relying on accidental include ordering.
