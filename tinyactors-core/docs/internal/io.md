# `io.h`

I/O processors are registered by a type atom/identifier and called synchronously by the interpreter when a send is ready to dispatch.

The callback may:

- perform the operation synchronously;
- enqueue work into an event loop/RTOS queue;
- start asynchronous host work and return;
- record the send in a verifier/simulator;
- synchronously deliver to another actor if the embedder's scheduling policy permits it without violating actor reentrancy rules.

The callback returns `TA_OP_ERROR` plus `ta_fault` when dispatch fails. The interpreter converts this to a queued `error.communication...` event.

Delayed-send waiting/cancellation is not an I/O processor responsibility.
