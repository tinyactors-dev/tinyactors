# `fault.h`

Faults bridge host/plugin failures into SCXML error events.

A callback reports:

```text
TA_OP_ERROR + ta_fault
```

The interpreter knows the operation context and chooses the SCXML event class. Examples:

- condition/value/location/assignment/script failure -> `error.execution...`;
- invalid/evaluation-time send target/type -> `error.execution...`;
- inability of an I/O processor to dispatch -> `error.communication...`;
- invocation communication failure -> `error.communication...`;
- application-defined platform failure -> `error.platform...`.

The W3C standard allows suffixes on automatically generated errors, so TA may provide more detail while preserving prefix compatibility.

Error events are appended to the internal event queue and processed in normal FIFO order; they are not exception-like immediate jumps.
