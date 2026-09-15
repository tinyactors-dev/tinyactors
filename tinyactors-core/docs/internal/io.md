# `io.h`

I/O processors are registered by a type atom/identifier and called synchronously by the interpreter when a send is ready to dispatch. `ta_send.io_type` selects the registration; it is the resolved SCXML `<send>` `type` or `typeexpr` value, interned as a `ta_atom`. The `target`, `event`, and `data` fields are delivery arguments for the selected processor, not the processor selector.

When SCXML omits `type` and `typeexpr`, Tinyactors resolves the type to the SCXML Event I/O Processor URI (`http://www.w3.org/TR/scxml/#SCXMLEventProcessor`) before creating `ta_send`. A delayed send copies that resolved atom into `ta_delayed_send.io_type`, so it dispatches through the same registration after its delay, unload/reload, or persistence round trip.

The callback may:

- perform the operation synchronously;
- enqueue work into an event loop/RTOS queue;
- start asynchronous host work and return;
- record the send in a verifier/simulator;
- synchronously deliver to another actor if the embedder's scheduling policy permits it without violating actor reentrancy rules.

The callback returns `TA_OP_ERROR` plus `ta_fault` when dispatch fails. The interpreter converts this to a queued `error.communication...` event.

Delayed-send waiting/cancellation is not an I/O processor responsibility.
