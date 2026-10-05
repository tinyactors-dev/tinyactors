# `internal.h`

`internal.h` is the umbrella include for Tinyactors implementation files. It
pulls the private module headers together in dependency order, and the
amalgamation task uses the same order to lay out `tinyactors.c`.

The public API is `include/tinyactors.h` alone (ADR 0041). There is no
private base header: each module includes what it uses itself, the public
header or the units that bring it. Module headers add only layouts and
internal functions, and must never redeclare a `TA_API` function. A unit that
defines only public functions is a source alone (`version.c`), which the
amalgamation appends after the listed modules' sources. Exact struct layouts remain free
to change as the implementation is specialized for memory density.

The current umbrella order is:

```text
memory/str/vec/bitset/span/text_writer/hash/handle
      -> arena/table/atom -> trie/value/event/fault/routing/clock
      -> configuration/history/continuation/timer
      -> datamodel/io/invoke
      -> definition/builder/actor/session_image/store/system/interpreter
```

`store.h` is only a compatibility boundary at present; the core has no storage protocol. Keep module headers individually self-contained enough for compile smoke tests rather than relying on accidental include ordering.
