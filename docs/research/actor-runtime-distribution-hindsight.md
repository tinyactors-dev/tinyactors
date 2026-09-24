# Distribution in actor runtimes, in hindsight: Orleans, Akka, Erlang/Elixir

**Date:** 2026-09-24
**Question:** What do the designers and heavy users of Orleans, Akka and
Erlang/Elixir say, in hindsight, about distribution: routing, addressing,
location transparency, directories, membership, delivery guarantees and
serialization? And should cross-system routing in tinyactors live in a
shared runtime layer, or in each binding and application?
**Method:** web research, one research agent per ecosystem. The lessons are
paraphrased, with sources. Quotes appear only where a source was quoted
directly.

## Caveats

- None of the three ecosystems has a single "what we would do differently"
  retrospective about distribution. Most of the hindsight shows up as design
  reversals: features that were removed, deprecated, turned off by default,
  corrected or rewritten. Each lesson says which kind it is.
- Not found:
  - direct statements from Bonér, Nordwall or Andrén about routers or
    sharding;
  - a retrospective from Bykov or Bond on the Orleans design as a whole;
  - direct statements from Armstrong, Virding or Valim regretting
    transparent distribution.

  Anything attributed to them on these topics is unverified.

## Orleans

1. **The eventually consistent directory was replaced.** The 2014 design
   chose availability and "eventual single activation", reconciling after
   failures. Orleans 9 replaced that with a strongly consistent directory
   built on membership views and virtual nodes.
   2014 technical report §3.2 and §3.9,
   https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/Orleans-MSR-TR-2014-41.pdf;
   https://learn.microsoft.com/en-us/dotnet/orleans/implementation/grain-directory
2. **A consistent directory alone does not guarantee a single activation.**
   Bond writes that "zombie" silos, which have been evicted but have not yet
   noticed, can still run duplicates. He calls leases "the only practical
   way": tombstones for evicted silos, plus silos that shut themselves down
   when their lease expires. https://github.com/dotnet/orleans/issues/9225
3. **The directory became pluggable per grain type, and multi-cluster was
   removed.** Orleans 3.2 let each grain class choose its own directory
   (for example Redis). The release notes say multi-cluster support "had to"
   be removed as part of that change, with a plan to bring it back as a
   directory plugin; epic #7485 is still open.
   https://github.com/dotnet/orleans/releases/tag/v3.2.0-rc1;
   https://github.com/dotnet/orleans/issues/7485
4. **Geo-distribution research put the protocols inside the runtime; the
   product later removed them.** The GEO paper argues that hiding these
   protocols under a simple API makes geo-distribution usable by mainstream
   developers. The shipped feature was later removed (lesson 3).
   Bernstein et al., "Geo-Distribution of Actor-Based Services",
   MSR-TR-2017-3 / OOPSLA'17
5. **The delivery guarantee was corrected down to at-most-once.** The 2014
   technical report claimed at-least-once delivery through resends. The
   current docs say the default is at-most-once with no automatic retries,
   and that the report "accidentally" described only the retry mode.
   - Deduplication was rejected as too costly and left to applications.
   - Per-actor-pair FIFO was dropped early because it needs n² state.
   - Orleans 10 marked `[Unordered]` obsolete, because ordering was never
     guaranteed.

   https://learn.microsoft.com/en-us/dotnet/orleans/implementation/messaging-delivery-guarantees;
   2014 technical report §3.10;
   https://learn.microsoft.com/en-us/dotnet/orleans/migration-guide
6. **Transparent random placement costs latency.** With random placement,
   about 90% of actor-to-actor messages were remote. ActOp moved actors
   that talk often onto the same server and cut p99 latency by more than 3x.
   The directory cache makes placement "sticky", so deactivating a grain does
   not rebalance it. This work led to activation repartitioning in
   Orleans 8.2. ActOp, EuroSys'16; https://github.com/dotnet/orleans/issues/7692
7. **The wire format and identity scheme needed a breaking rewrite.**
   Orleans 7 broke the wire protocol and rolling upgrades on purpose, for two
   reasons. The old serializer could not tolerate even an added field. The
   old grain and stream identities (numeric type codes and GUIDs) could not
   represent generic types. The new identities are strings of the form
   `type/key`. https://learn.microsoft.com/en-us/dotnet/orleans/migration-guide
8. **Streams: the same abstraction hid different semantics.**
   SimpleMessageStreams were removed: they shared the persistent-stream
   interface but were really direct grain calls. The Streaming v2 proposal
   lists four problems:
   - complex configuration;
   - behaviour that differs by provider;
   - spurious cache misses;
   - events within one cluster routed through external queues.

   Migration guide, section "SMS to BroadcastChannel";
   https://github.com/dotnet/orleans/issues/9734
9. **Membership and transport were rebuilt.**
   - Orleans 3 moved to a transport-agnostic networking layer with
     middleware and full-duplex connections, mainly to get TLS.
   - Clients became co-hosted with ASP.NET Core, and membership snapshots are
     versioned, immutable and gossiped.
   - Orleans 9 cut failure detection from 10 minutes to 90 seconds, with
     Lifeguard-style indirect probes.

   https://devblogs.microsoft.com/dotnet/orleans-3-0/; migration guide
10. **"Actor" names two different problems.** Bykov argues that local and
    distributed actors are different problems that the word blurs, and that
    Orleans's default of asynchronous RPC confuses people who expect one-way
    messages. https://temporal.io/blog/sergey-the-curse-of-the-a-word

## Akka and Akka.NET

1. **Hide where an actor lives, but never make remote look like local.**
   The docs present remote-to-local as an optimization, and argue that going
   from local to remote by generalization is bound to fail (citing Waldo,
   "A Note on Distributed Computing"). Remote sends can be slow and lost, and
   every message must be serializable.
   https://doc.akka.io/libraries/akka-core/current/general/remoting.html
2. **Only guarantee what still holds when distributed.** Kuhn's rule is not
   to do anything that cannot be guaranteed under distribution. Reliable
   delivery was left to the user, because real reliability needs persistence
   anyway. Roland Kuhn, "My journey towards understanding distribution",
   https://github.com/rkuhn/blog/blob/master/01_my_journey_towards_understanding_distribution.md
3. **Remote supervision and remote DeathWatch are expensive.** They need
   cluster-wide agreement. A node declared dead cannot rejoin without
   "zombie" behaviour. (Same source.)
4. **The implicit `sender()` and untyped messages were dropped.** Akka Typed
   has no `sender()`, and an explicit `replyTo` field in each message makes
   the protocol clearer.
   https://doc.akka.io/libraries/akka-core/current/typed/from-classic.html;
   https://www.bbva.com/en/innovation/akka-actors-evolution-type-safe-distributed-computations/
5. **Path-based lookup was replaced by registration.** Typed Akka has no
   `ActorSelection`. Actors register under a key with the Receptionist,
   which spreads the list across the cluster, and lookups return a listing.
   https://doc.akka.io/docs/akka/current/typed/actor-discovery.html
6. **Raw remoting is too low-level for applications.** The Artery docs
   state: "We recommend Akka Cluster over using remoting directly."
   - Artery replaced classic remoting, whose single TCP connection caused
     head-of-line blocking.
   - It moved system messages to a separate control channel.
   - Akka.NET names the same single-socket problem as the reason for its
     QUIC transport.

   https://doc.akka.io/docs/akka/2.6/remoting-artery.html;
   https://petabridge.com/blog/akkadotnet-v1.6-roadmap/
7. **Deciding which nodes are "down" is the dangerous part.** Akka 2.6
   removed timeout-based auto-downing. During a partition it creates
   split-brain clusters: two sharded entities or singletons with the same
   identity, and corrupted persistence journals. The Split Brain Resolver
   replaced it.
   https://doc.akka.io/libraries/akka-core/2.6/project/migration-guide-2.5.x-2.6.x.html
8. **Serialization defaults were a security and performance mistake.** Java
   serialization was the remoting default and could be exploited with
   attacker-chosen types. It was fixed in 2.4.17 and turned off by default
   in 2.6, with Jackson recommended instead.
   - Stannard regrets that Akka.NET relied on outside serializers and
     transports (Protobuf, Newtonsoft, Helios, DotNetty), which lagged behind
     or were abandoned.
   - The v1.6 roadmap calls hand-written Protobuf serializers "extremely
     tedious".

   https://doc.akka.io/docs/akka/2.4/security/2017-02-10-java-serialization.html;
   https://petabridge.com/blog/10-years-of-akkadotnet/
9. **Reliable delivery was added later, as an opt-in layer.** At-most-once
   stayed the default. At-least-once delivery arrived in 2.6 as a separate
   module (point-to-point, work-pulling, sharding).
   https://doc.akka.io/libraries/akka-core/current/typed/reliable-delivery.html

## Erlang/OTP and Elixir

1. **Location transparency is skin-deep.** Local and remote sends look the
   same, but their latency and failure semantics differ, so code must expect
   lost messages. The cookie is closer to a username than a password, and
   distribution assumes a trusted network.
   Hébert, "Distribunomicon", https://learnyousomeerlang.com/distribunomicon
2. **Ordering holds only while the link is up.** Messages between a pair of
   processes stay in order, but an unknown number of messages at the end can
   be lost when a link fails. There is no end-to-end guarantee between nodes.
   https://www.erlang.org/blog/message-passing/
3. **Full mesh limits cluster size.** Hundreds of nodes are impractical
   because of connections and heartbeats. Benchmarks show near-linear
   scaling to about 150 nodes when no global operations are used.
   Distribunomicon; Ghaffari, "Investigating the Scalability Limits of
   Distributed Erlang", https://www.dcs.gla.ac.uk/~amirg/publications/DE-Bench.pdf
4. **A replicated, globally locked name registry is the bottleneck.** One
   `global:register_name` per 10,000 point-to-point operations stopped
   scaling at about 60 nodes. OTP 25 made `global` disconnect nodes by
   default, because overlapping partitions could leave it inconsistent after
   healing. DE-Bench; https://www.erlang.org/doc/apps/kernel/global.html
5. **One TCP connection per node pair causes head-of-line blocking.** One
   large message could delay heartbeats until a live node was declared down.
   OTP 22 added fragmentation, cutting worst-case latency from about 50 s to
   about 0.4 s. Partisan argues the deeper problem is that control and data
   traffic share one channel.
   https://www.erlang.org/blog/otp-22-highlights/; Meiklejohn et al.,
   "PARTISAN: Scaling the Distributed Actor Runtime", USENIX ATC 2019
6. **Topology should be chosen per application.** Partisan keeps the Erlang
   API but lets operators choose the topology at runtime (full mesh,
   client-server, peer-to-peer, publish/subscribe) and uses several
   connections. It reports up to 38x throughput, 13.5x lower latency, and
   scaling from 256 to 1,024 nodes.
   https://www.usenix.org/system/files/atc19-meiklejohn.pdf
7. **Group membership should be eventually consistent and scoped.** WhatsApp
   could not stabilise `pg2` after a mass reconnect and had to restart the
   whole cluster. Its replacement, `pg` (OTP 23, by Maxim Fedorov of
   WhatsApp), is eventually consistent, with "scopes" that separate overlay
   networks. Rick Reed, "That's 'Billion' with a 'B'", Erlang Factory 2014;
   https://www.erlang.org/doc/apps/kernel/pg.html
8. **WhatsApp built its own WAN routing.**
   - `wandist` is a distribution-like transport over `gen_tcp`, with meshes
     between groups of servers.
   - A routing layer above `pg2` makes every message a single hop.
   - Services are partitioned into 2–32 primary/secondary pairs.

   (Reed 2014, as above.)
9. **Discord built its directory and fan-out in the application.**
   - Guild placement uses a consistent-hash ring, served through a
     read-only global (FastGlobal) because one ring-owner process could not
     keep up.
   - Manifold groups pids by node, so one publish costs one send per
     remote node, in order.
   - Concurrency limits (Semaphore) on the registry path prevent cascading
     failures.

   https://discord.com/blog/how-discord-scaled-elixir-to-5-000-000-concurrent-users
10. **Registries converge on CRDTs.** Phoenix Presence/Tracker replicates
    state with heartbeats, gossip and a CRDT, with no central process.
    Horde's registry and supervisor use delta-CRDTs and pick a winner for a
    conflicting name after a netsplit. https://hexdocs.pm/phoenix/presence.html;
    Derek Kraan, "Introducing Horde"

## Across all three (synthesis, not stated by the sources)

| Theme | Evidence |
|---|---|
| Remote must not look local | Akka 1; Orleans 5 and 10; Erlang 1 and 2 |
| Identity should not encode location; resolve location through a registry | Akka 5; Orleans 7; Erlang 4, 8 and 9 |
| Directory, placement and membership policy keeps changing | Akka 7; Orleans 1, 3, 6 and 9; Erlang 3, 4, 6, 7 and 10 |
| Stronger guarantees are opt-in layers | Akka 2 and 9; Orleans 5; Erlang 2 |
| Control traffic must be kept apart from data | Akka 6; Erlang 5 and 6 |
| The wire format must be explicit and versioned from day one | Akka 8; Orleans 7 |
| Single activation needs leases, not only a directory | Orleans 2; Akka 7; Erlang 10 |
| Supervision across nodes is expensive | Akka 3 |

**What the three ecosystems converged on:**
- The **mechanism** of distribution is shared and owned by the runtime:
  transport interfaces, links, envelopes, explicit outcomes.
- The **policy** is pluggable and owned by the application: placement,
  directory consistency, membership, topology.
- None of them moved routing out into each application or language. The
  regrets were about hard-wiring one policy (full mesh, `global`,
  multi-cluster, auto-downing), and about letting remote look local.
