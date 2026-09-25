# Loading actors on demand: how virtual-actor runtimes hold mail while an actor starts

**Date:** 2026-09-25
**Used by:** [napi ADR 0020](../../tinyactors-napi/docs/adr/0020-mail-for-an-unloaded-actor-waits-while-a-loader-brings-it-back.md) (Proposed)

The question: when mail arrives for an actor that is not in memory, where
does the mail wait while the actor is brought back, how is that bounded,
does order hold, what happens when bringing it back fails or is slow, and
how does a runtime avoid bringing actors back while it shuts down? This
note records what Orleans, Akka Cluster Sharding, Dapr, Proto.Actor and
Erlang/OTP do.

## Summary

| Runtime | Where waiting mail lives | Bound | Order across activation | Activation fails or is slow | Shutdown |
|---|---|---|---|---|---|
| Orleans | the activation's own queue, created at once for the first message | none documented per grain; requests time out | kept per activation | pending requests are rejected; deactivating activations forward mail, at most `MaxForwardCount` (2) times | silo deactivates its grains; mail is rerouted to other silos |
| Akka Cluster Sharding | the `ShardRegion` (location unknown, rebalancing) and the `Shard` (between `Passivate` and stop) | `buffer-size` = 100,000 per region; messages beyond are **dropped** and counted | kept per sender through one `ShardRegion` | at-most-once; no retry | shards are handed off, messages buffered and then delivered at the new home |
| Dapr actors | the sidecar's per-actor lock (turn-based) | none documented; calls time out | turn-based, one call at a time | the call fails to the caller | `drainRebalancedActors` waits `drainOngoingCallTimeout` (2 s) for the running call, then deactivates |
| Proto.Actor | nowhere: the sender resolves a PID through identity lookup, then sends | n/a | per PID | in-flight requests time out; no automatic retry | idle timeout stops the actor; late mail is a dead letter |
| Erlang/OTP | nowhere: a mailbox belongs to a live process | n/a | per process | sending to an unregistered name raises `badarg`; `gen_server:call` exits `noproc` | the supervisor stops children; mail to them is lost |

Two families emerge. **Runtimes that buffer** (Orleans, Akka) keep mail in
the runtime while the actor starts, bound or time it out, and deliver it
in order afterwards. **Runtimes that don't** (Proto.Actor, Erlang) leave
waiting and retrying to the caller. tinyactors' mail between actors never
leaves native code (napi ADR 0006), and its senders are statecharts that
cannot retry, so it belongs in the first family.

## Orleans

- **Activation on the first message.** A grain "always exists"; the runtime
  creates an activation when a message arrives for a grain with none, and
  the grain directory keeps one activation per grain ID. Grain state is
  loaded in the `SetupState` lifecycle stage, before `OnActivateAsync` runs
  ([grain lifecycle][orleans-lifecycle]).
- **Mail during activation and deactivation.** Messages for an activation
  that is still activating wait in its queue. Orleans "holds messages
  while deactivating and reroutes them afterwards", forwarding them to a
  new activation, and "messages are rejected only when activation itself
  failed" ([thresh issue #91][thresh-91], describing Orleans' behaviour).
  Forwarding is bounded: after `MaxForwardCount` hops (default 2) the
  message is rejected with `OrleansMessageRejectionException` ("Forwarding
  failed: tried to forward message ... for 2 times")
  ([MaxForwardCount][orleans-maxforward], [issue #8713][orleans-8713]).
- **Idle collection.** Grains idle for `CollectionAge` (15 minutes) are
  deactivated; memory pressure can shed activations too
  ([activation collection][orleans-collection], [grain lifecycle][orleans-lifecycle]).
- **Shutdown.** A stopping silo deactivates its grains, and mail for them
  is rerouted to other silos. Getting that right has been a recurring
  source of bugs during rolling deployments
  ([issue #4757][orleans-4757], [issue #5085][orleans-5085]).
- **Lesson:** the waiting queue is per activation, and failure is explicit
  (a rejection the caller sees). Forwarding needs a bound, or a message
  can bounce forever.

## Akka Cluster Sharding

- **Buffering while a location is unknown.** "While resolving the location
  of a shard incoming messages for that shard are buffered and later
  delivered when the shard home is known." During rebalancing, the
  `ShardRegion` starts "buffering incoming messages for that shard, in the
  same way as if the shard location is unknown"
  ([sharding concepts][akka-concepts]).
- **The bound.** `akka.cluster.sharding.buffer-size = 100000` per region;
  when it is full, messages are dropped and counted as "Shard region
  message dropped", with a gauge for the buffer's size
  ([reference.conf][akka-reference], [Akka Insights][akka-insights]).
- **Passivation.** "Incoming messages will be buffered by the `Shard`
  between reception of `Passivate` and termination of the entity. Such
  buffered messages are thereafter delivered to a new incarnation of the
  entity." Idle passivation defaults to 2 minutes
  ([cluster sharding][akka-sharding]).
- **Order.** "As long as a sender uses the same `ShardRegion` actor to
  deliver messages to an entity actor the order of the messages is
  preserved." Delivery is at-most-once ([sharding concepts][akka-concepts]).
- **Lesson:** a hard cap with visible drops, and passivation that races
  with incoming mail resolved by buffering and restarting the entity.

## Dapr actors

- Actors activate on the first call, process one call at a time, and are
  deactivated after `actorIdleTimeout` (60 minutes), checked every
  `actorScanInterval` (30 s).
- On rebalancing, `drainRebalancedActors` (default true) waits
  `drainOngoingCallTimeout` (2 s) "to allow a current actor call to
  complete before trying to deactivate an actor"
  ([runtime configuration][dapr-config]).
- **Lesson:** a drain window with a timeout for graceful moves; callers
  see failures and retry.

## Proto.Actor virtual actors

- The sender hashes the identity to find its owner, which returns the
  PID of an existing activation or spawns one ([clustering][protoactor-blog],
  [cluster introduction][protoactor-intro]).
- Actors can stop themselves after an idle timeout. "In-flight requests
  will time out when actors change location, as there is no automatic
  retry mechanism in the message routing"
  ([topology changes][protoactor-topology]).
- **Lesson:** without runtime buffering, every caller needs timeouts and
  retries.

## Erlang/OTP

- A mailbox belongs to a live process. Sending to a registered name with
  no process raises `badarg`; `gen_server:call` to a dead process exits
  with `noproc` ([processes][erlang-processes]).
- The "look up or start" pattern (a `Registry` plus a `DynamicSupervisor`)
  races: two callers may both start the child, and one gets
  `{:error, {:already_started, pid}}` ([Registry][elixir-registry]).
- **Lesson:** single-flight activation per name has to be built; the
  runtime does not do it.

## What this means for tinyactors

1. **Buffer in the runtime, per address.** Senders are statecharts; they
   cannot retry, so the runtime must hold their mail (Orleans, Akka).
2. **Bound it and say so.** A per-address cap with drops that are counted
   and logged (Akka), plus the system-wide envelope limit already in napi
   ADR 0005.
3. **One activation in flight per address** (Orleans' directory), with a
   timeout (Dapr, Proto.Actor), and failure the sender can see (Orleans'
   rejection, our dead letters and `send()` rejections).
4. **Deliver buffered mail first, in order,** to the new incarnation
   (Akka's passivation buffer, napi ADR 0005 point 12's replacement move).
5. **Shutdown must not reactivate.** Orleans reroutes and Dapr drains; a
   single process has nowhere to reroute to, so the loader has to be able
   to refuse, and refused mail has to reach the host so it can be kept
   across the restart.
6. **Timers.** Orleans separates timers, which die with the activation,
   from reminders, which reactivate a grain. An unloaded tinyactors actor
   carries its pending delayed sends in its image and does not wake for
   them today.

[orleans-lifecycle]: https://learn.microsoft.com/en-us/dotnet/orleans/grains/grain-lifecycle
[orleans-collection]: https://learn.microsoft.com/en-us/dotnet/orleans/host/configuration-guide/activation-collection
[orleans-maxforward]: https://learn.microsoft.com/en-us/dotnet/api/orleans.runtime.configuration.messagingconfiguration.maxforwardcount?view=orleans-2.0
[orleans-8713]: https://github.com/dotnet/orleans/issues/8713
[orleans-4757]: https://github.com/dotnet/orleans/issues/4757
[orleans-5085]: https://github.com/dotnet/orleans/issues/5085
[thresh-91]: https://github.com/dave-hillier-co/thresh/issues/91
[akka-sharding]: https://doc.akka.io/libraries/akka-core/current/typed/cluster-sharding.html
[akka-concepts]: https://doc.akka.io/libraries/akka-core/current/typed/cluster-sharding-concepts.html
[akka-reference]: https://github.com/akka/akka-core/blob/main/akka-cluster-sharding/src/main/resources/reference.conf
[akka-insights]: https://doc.akka.io/libraries/akka-insights/current/instrumentations/akka-cluster-sharding/akka-cluster-sharding.html
[dapr-config]: https://docs.dapr.io/developing-applications/building-blocks/actors/actors-runtime-config/
[protoactor-blog]: https://blog.oklahome.net/2021/05/protoactor-clustering.html
[protoactor-intro]: https://proto.actor/docs/clusterintro/
[protoactor-topology]: https://www.etteplan.com/about-us/insights/how-virtual-actor-frameworks-deal-cluster-topology-change/
[erlang-processes]: https://www.erlang.org/doc/system/ref_man_processes.html
[elixir-registry]: https://hexdocs.pm/elixir/Registry.html
