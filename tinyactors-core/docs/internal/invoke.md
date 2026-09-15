# `invoke.h`

Each active invocation records:

- invoke ID;
- invocation type;
- host service handle;
- autoforward flag;
- active/cancelled state.

The invoker vtable starts, cancels, and forwards events to a logical external service.

For a statechart child actor, the service handle can resolve to another `ta_actor_id`. Parent/child communication still uses ordinary events and origin/invoke metadata. Normal child completion sends `done.invoke.<id>` to the parent's **external** event flow; cancellation on parent-state exit must not manufacture a normal completion event.

Active invocation bookkeeping is persistent session state. Whether an external service itself can survive process restart is a contract of that invoker/store integration.
