# Data-model faults and statechart error events

A data-model callback can fail semantically without the C library call failing.

```c
typedef enum {
    TA_OP_OK = 0,
    TA_OP_ERROR = 1
} ta_op_status;
```

On `TA_OP_ERROR`, the callback fills `ta_fault` with diagnostic detail.

## Separation of concerns

The plugin reports **what failed**. The interpreter knows **what semantic operation was being attempted** and determines the statechart consequence.

Examples:

| Operation | Core consequence |
|---|---|
| condition evaluation fails | condition is false; queue `error.execution...` |
| invalid location for assignment | assignment has no effect; queue error |
| value is illegal for destination | assignment/init has no effect or leaves empty binding as required; queue error |
| foreach source not iterable | abort foreach and containing executable block; queue error |
| param evaluation fails | omit parameter; queue error |
| event-data conversion fails | leave event data unbound; process event; queue error |
| protected system variable write | no write; queue error |

## Structured names

The engine may specialize the standard prefix while preserving prefix matching, for example:

```text
error.execution.datamodel.condition
error.execution.datamodel.location
error.execution.datamodel.assign
error.execution.datamodel.foreach
error.execution.datamodel.event_data
```

A transition listening to `error.execution` still matches every one of these.

## C/runtime failures

Allocator corruption, invalid API handles, malformed snapshots, or violated C preconditions are `ta_result` failures, not statechart `error.*` events.
