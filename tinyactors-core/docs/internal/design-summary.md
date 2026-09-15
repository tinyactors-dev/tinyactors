# Design summary

TA can be summarized as one equation:

```text
immutable statechart definition
+ mutable actor session
+ caller-supplied event/time/CPU
+ host data/I/O/invocation adapters
= next actor session and host-visible callbacks/events
```

The core is deliberately not an actor *server*. It is the state transition substrate from which a server, browser runtime, Python package, Ruby extension, Node native addon, simulator, or embedded firmware can be built.

The semantic center is one actor macrostep. Everything that makes the environment real—scheduling across actors, network operations, persistence backend, and physical clock—is controlled by the embedder.
