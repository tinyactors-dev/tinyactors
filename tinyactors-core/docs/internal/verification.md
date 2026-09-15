# Verification and simulation

The first tool is a simulator/fuzzer built around the ordinary Tinyactors interpreter.

## Controlled environment

A simulator provides:

- deterministic seeded scheduler;
- simulated logical clock;
- modeled I/O processors;
- modeled invokers;
- generated external events;
- invariant callbacks;
- trace recorder.

## Choices to randomize

Examples:

- which runnable actor receives CPU next;
- ordering of independent external events;
- which modeled service response occurs;
- delay before a response;
- when logical time advances;
- when actors are unloaded/reloaded;
- persistence/crash/recovery fault injection.

## Trace

A failing trace should contain enough logical operations to replay:

```text
seed
spawn/find actor
external event deliveries
time advances
modeled I/O/invocation outcomes
scheduler/resume choices
unload/load/crash points
```

The verifier can later add coverage guidance, shrinking, state hashing, partial-order reduction, bounded exhaustive search, or formal-property frontends without changing production semantics.
