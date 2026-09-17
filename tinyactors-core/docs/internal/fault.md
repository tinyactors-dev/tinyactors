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
- invocation start/cancel/forward failure -> `error.communication...`;
- an invoked service becoming unavailable ->
  `error.communication.invoke.<id>.unavailable`;
- application-defined platform failure -> `error.platform...`.

`ta_fault_event_params` carries both send and invocation IDs. An invocation ID
can select the detailed event name and runtime record without populating the
SCXML event's invoke-ID envelope field. Reconstruction uses
`TA_FAULT_CONTEXT_INVOKE_UNAVAILABLE`; because this failure is detected by core,
the `fault` member may be null.

Both `ta_fault_event_name()` and `ta_fault_to_internal_event()` consume this
parameter object so structured name selection and event-envelope construction
use the same operation identity.

The W3C standard allows suffixes on automatically generated errors, so Tinyactors may provide more detail while preserving prefix compatibility.

Error events are appended to the internal event queue and processed in normal FIFO order; they are not exception-like immediate jumps.
