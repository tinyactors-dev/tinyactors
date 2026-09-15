# Interpreter semantics

TA should follow the normative SCXML transition semantics and use Appendix D only as an algorithmic reference, not as a required data structure.

## Macrostep outline

Conceptually:

1. accept one external event when actor is idle/stable;
2. expose it as the current event to the data model;
3. run invocation finalization/autoforward behavior where applicable;
4. select enabled transitions from all active atomic leaves and ancestors;
5. remove conflicts using SCXML priority rules;
6. exit states in exit order, recording history and cancelling invocations whose containing state is exited;
7. execute transition executable content in required order;
8. enter target/default/parallel/history states in entry order;
9. generate `done.state.*` events where completion rules require them;
10. process eventless transitions and internal events until stable;
11. invoke newly entered states at the SCXML-defined point;
12. finish the macrostep and become eligible for another external event.

## Pausing

A caller-supplied execution budget may interrupt the implementation between interpreter instructions. The continuation must encode enough state that resume is semantically equivalent to uninterrupted execution. It may not expose a half-applied transition as a stable actor.

## Infinite control flow

A machine may remain forever in eventless/internal control flow. This is not a runtime error. Repeated `TA_RUN_PAUSED` gives the host cooperative control over CPU usage.
