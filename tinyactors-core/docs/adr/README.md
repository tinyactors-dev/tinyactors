# Architecture decision records

ADRs are the primary record of Tinyactors design intent. Unless marked otherwise, the decisions below are **Accepted**.

| ADR | Decision |
|---|---|
| [0001](0001-scxml-semantics-without-an-xml-dependency.md) | [SCXML semantics without an XML dependency](0001-scxml-semantics-without-an-xml-dependency.md) |
| [0002](0002-in-process-library-embedder-owns-concurrency-and-i-o-environment.md) | [In-process library; embedder owns concurrency and I/O environment](0002-in-process-library-embedder-owns-concurrency-and-i-o-environment.md) |
| [0003](0003-immutable-shared-definitions-and-per-actor-sessions.md) | [Immutable shared definitions and per-actor sessions](0003-immutable-shared-definitions-and-per-actor-sessions.md) |
| [0004](0004-named-definitions-named-and-anonymous-actors.md) | [Named definitions; named and anonymous actors](0004-named-definitions-named-and-anonymous-actors.md) |
| [0005](0005-pluggable-data-models-through-fat-pointer-vtables.md) | [Pluggable data models through fat-pointer vtables](0005-pluggable-data-models-through-fat-pointer-vtables.md) |
| [0006](0006-host-defined-values-own-lifetime-and-serialization.md) | [Host-defined values own lifetime and serialization](0006-host-defined-values-own-lifetime-and-serialization.md) |
| [0007](0007-structured-interned-event-names-and-prefix-matching.md) | [Structured interned event names and prefix matching](0007-structured-interned-event-names-and-prefix-matching.md) |
| [0008](0008-error-events-are-control-flow-c-errors-are-separate.md) | [`error.*` events are control flow; C errors are separate](0008-error-events-are-control-flow-c-errors-are-separate.md) |
| [0009](0009-semantic-run-to-completion-with-pausable-resumable-execution.md) | [Semantic run-to-completion with pausable/resumable execution](0009-semantic-run-to-completion-with-pausable-resumable-execution.md) |
| [0010](0010-parallel-configuration-is-a-set-of-active-atomic-states.md) | [Parallel configuration is a set of active atomic states](0010-parallel-configuration-is-a-set-of-active-atomic-states.md) |
| [0011](0011-the-host-owns-external-scheduling-actor-owns-internal-events.md) | [The host owns external scheduling; actor owns internal events](0011-the-host-owns-external-scheduling-actor-owns-internal-events.md) |
| [0012](0012-logical-time-and-engine-owned-delayed-send-semantics.md) | [Logical time and engine-owned delayed-send semantics](0012-logical-time-and-engine-owned-delayed-send-semantics.md) |
| [0013](0013-snapshot-the-complete-running-session.md) | [Snapshot the complete running session](0013-snapshot-the-complete-running-session.md) |
| [0014](0014-unload-is-residency-change-destroy-is-termination.md) | [Unload is residency change; destroy is termination](0014-unload-is-residency-change-destroy-is-termination.md) |
| [0015](0015-synchronous-i-o-callbacks-asynchronous-completion-by-events.md) | [Synchronous I/O callbacks, asynchronous completion by events](0015-synchronous-i-o-callbacks-asynchronous-completion-by-events.md) |
| [0016](0016-invocation-is-a-pluggable-service-boundary.md) | [Invocation is a pluggable service boundary](0016-invocation-is-a-pluggable-service-boundary.md) |
| [0017](0017-no-implicit-thread-safety-external-synchronization.md) | [No implicit thread safety; external synchronization](0017-no-implicit-thread-safety-external-synchronization.md) |
| [0018](0018-optimize-memory-per-actor-share-everything-definition-level.md) | [Optimize memory per actor; share everything definition-level](0018-optimize-memory-per-actor-share-everything-definition-level.md) |
| [0019](0019-definitions-are-built-compiled-to-compact-immutable-layouts.md) | [Definitions are built/compiled to compact immutable layouts](0019-definitions-are-built-compiled-to-compact-immutable-layouts.md) |
| [0020](0020-verification-reuses-the-production-transition-engine.md) | [Verification reuses the production transition engine](0020-verification-reuses-the-production-transition-engine.md) |
| [0021](0021-verification-begins-as-simulation-fuzzing-not-proof.md) | [Verification begins as simulation/fuzzing, not proof](0021-verification-begins-as-simulation-fuzzing-not-proof.md) |
| [0022](0022-actor-event-origin-is-the-implicit-reply-address.md) | [Actor event origin is the implicit reply address](0022-actor-event-origin-is-the-implicit-reply-address.md) |
| [0023](0023-allocator-injection-and-freestanding-friendly-core.md) | [Allocator injection and freestanding-friendly core](0023-allocator-injection-and-freestanding-friendly-core.md) |
| [0024](0024-c-abi-targets-node-cpython-cruby-wasm-and-embedded-hosts.md) | [C ABI targets Node, CPython, CRuby, WASM, and embedded hosts](0024-c-abi-targets-node-cpython-cruby-wasm-and-embedded-hosts.md) |
| [0025](0025-scxml-import-export-is-optional-and-separate.md) | [SCXML import/export is optional and separate](0025-scxml-import-export-is-optional-and-separate.md) |
| [0026](0026-delayed-sends-are-semantically-actor-owned-physically-globally-indexed.md) | [Delayed sends are semantically actor-owned, physically globally indexed](0026-delayed-sends-are-semantically-actor-owned-physically-globally-indexed.md) |
| [0027](0027-detailed-generated-event-names-remain-scxml-prefix-compatible.md) | [Detailed generated event names remain SCXML-prefix-compatible](0027-detailed-generated-event-names-remain-scxml-prefix-compatible.md) |
| [0028](0028-runtime-ids-are-compact-handles-persistence-must-resolve-logical-identity.md) | [Runtime IDs are compact handles; persistence must resolve logical identity](0028-runtime-ids-are-compact-handles-persistence-must-resolve-logical-identity.md) |
| [0029](0029-no-per-actor-external-mailbox-is-required-by-the-core.md) | [No per-actor external mailbox is required by the core](0029-no-per-actor-external-mailbox-is-required-by-the-core.md) |
| [0030](0030-interpreter-extensions-cannot-bypass-statechart-configuration-semantics.md) | [Interpreter extensions cannot bypass statechart configuration semantics](0030-interpreter-extensions-cannot-bypass-statechart-configuration-semantics.md) |
| [0031](0031-data-model-vtable-is-semantic-not-syntactic.md) | [Data-model vtable is semantic, not syntactic](0031-data-model-vtable-is-semantic-not-syntactic.md) |
| [0032](0032-data-bindings-are-declared-before-they-are-initialized.md) | [Data bindings are declared before they are initialized](0032-data-bindings-are-declared-before-they-are-initialized.md) |
| [0033](0033-foreach-iteration-is-stable-and-resumable.md) | [Foreach iteration is stable and resumable](0033-foreach-iteration-is-stable-and-resumable.md) |
| [0034](0034-named-parameter-aggregation-is-a-data-model-operation.md) | [Named parameter aggregation is a data-model operation](0034-named-parameter-aggregation-is-a-data-model-operation.md) |
| [0035](0035-system-variables-are-protected-views.md) | [System variables are protected views](0035-system-variables-are-protected-views.md) |
| [0036](0036-stable-definition-identity-in-snapshots.md) | [Stable definition identity in snapshots](0036-stable-definition-identity-in-snapshots.md) |
| [0037](0037-core-owned-actor-protocol-host-backed-directory.md) | [Core-owned actor protocol; host-backed directory](0037-core-owned-actor-protocol-host-backed-directory.md) |
| [0038](0038-session-images-separate-interpreter-state-from-host-data.md) | [Session images separate interpreter state from host data](0038-session-images-separate-interpreter-state-from-host-data.md) |
| [0039](0039-lost-invocations-become-reconstruction-communication-errors.md) | [Lost invocations become reconstruction communication errors](0039-lost-invocations-become-reconstruction-communication-errors.md) |
| [0040](0040-built-in-scxml-event-processor-host-routed-external-events.md) | [Built-in SCXML event processor; host-routed external events](0040-built-in-scxml-event-processor-host-routed-external-events.md) |
| [0041](0041-one-authored-public-header-private-module-headers.md) | [One authored public header; private module headers](0041-one-authored-public-header-private-module-headers.md) |
| [0042](0042-definitions-are-inspectable-and-export-to-their-spec.md) | [Definitions are inspectable and export to their spec](0042-definitions-are-inspectable-and-export-to-their-spec.md) |
| [0043](0043-definitions-retire-and-definition-ids-are-generational.md) | [Definitions retire; definition IDs are generational](0043-definitions-retire-and-definition-ids-are-generational.md) |
| [0044](0044-core-owns-the-scxml-target-uri-grammar.md) | [Core owns the SCXML target URI grammar](0044-core-owns-the-scxml-target-uri-grammar.md) |
| [0045](0045-log-records-go-to-a-host-sink.md) | [`<log>` records go to a host sink](0045-log-records-go-to-a-host-sink.md) |
| [0046](0046-platform-specific-executable-content-is-a-send.md) | [Platform-specific executable content is a `<send>` to a host I/O processor](0046-platform-specific-executable-content-is-a-send.md) |
| [0047](0047-an-interrupted-macrostep-is-retryable.md) | [An allocation failure inside a macrostep is retryable](0047-an-interrupted-macrostep-is-retryable.md) |
| [0048](0048-core-owns-the-send-id-text.md) | [Core owns the send ID text, in both directions](0048-core-owns-the-send-id-text.md) |
| [0049](0049-interned-identities-are-readable-back.md) | [Identities core interns are readable back](0049-interned-identities-are-readable-back.md) |
| [0050](0050-envelope-data-is-shared-by-retain-not-cloned.md) | [Envelope data is shared by retain, not cloned](0050-envelope-data-is-shared-by-retain-not-cloned.md) |
| [0051](0051-the-only-public-restore-path-is-ta-actor-load.md) | [The only public restore path is `ta_actor_load`](0051-the-only-public-restore-path-is-ta-actor-load.md) |
| [0052](0052-reconstruction-is-public-and-route-neutral.md) | [Reconstruction is public and route-neutral](0052-reconstruction-is-public-and-route-neutral.md) |
| [0053](0053-a-chart-representation-is-a-codec-and-literal-content-is-bytes.md) | [A chart representation is a codec, and literal content reaches adapters as bytes](0053-a-chart-representation-is-a-codec-and-literal-content-is-bytes.md) |
| [0054](0054-delayed-sends-live-with-their-owner-the-global-index-orders-owners.md) | [Delayed sends live with their owner; the global index orders owners](0054-delayed-sends-live-with-their-owner-the-global-index-orders-owners.md) |
| [0055](0055-deliver-is-the-admission-test-and-reports-finished-actors.md) | [`ta_actor_deliver` is the admission test and reports finished actors](0055-deliver-is-the-admission-test-and-reports-finished-actors.md) |
| [0056](0056-a-finished-actor-reports-the-final-state-it-reached.md) | [A finished actor reports the final state it reached](0056-a-finished-actor-reports-the-final-state-it-reached.md) |
| [0057](0057-core-ships-the-null-data-model.md) | [Core ships the null data model](0057-core-ships-the-null-data-model.md) |
| [0058](0058-compile-errors-say-what-and-where.md) | [A compile error says what is wrong and where in the chart](0058-compile-errors-say-what-and-where.md) |
| [0059](0059-an-actor-id-is-a-public-slot-and-generation.md) | [An actor ID is a public slot and generation](0059-an-actor-id-is-a-public-slot-and-generation.md) |
| [0060](0060-no-recursion-in-core.md) | [No recursion in core](0060-no-recursion-in-core.md) |
| [0061](0061-a-macrostep-can-pause-inside-executable-content.md) | [A macrostep can pause inside executable content](0061-a-macrostep-can-pause-inside-executable-content.md) |
| [0062](0062-the-actor-walk-is-paged-by-position.md) | [The actor walk is paged by position](0062-the-actor-walk-is-paged-by-position.md) |
| [0063](0063-an-actor-is-one-allocation-in-a-paged-table.md) | [An actor is one allocation in a paged table](0063-an-actor-is-one-allocation-in-a-paged-table.md) |
| [0064](0064-an-internal-queue-is-lent-to-a-running-actor.md) | [An internal queue is lent to a running actor](0064-an-internal-queue-is-lent-to-a-running-actor.md) |
| [0065](0065-named-addresses-are-shared-reference-counted-handles.md) | [Named addresses are shared, reference-counted handles](0065-named-addresses-are-shared-reference-counted-handles.md) |
| [0066](0066-a-session-id-outlives-residency.md) | [A session ID outlives residency](0066-a-session-id-outlives-residency.md) |
| [0067](0067-core-traces-its-decisions-and-data-to-a-host-sink.md) | [Core traces its decisions and data to a host sink](0067-core-traces-its-decisions-and-data-to-a-host-sink.md) |
| [0068](0068-core-owns-the-io-processor-registry-and-every-origin-can-be-answered.md) | [Core owns the I/O processor registry, and every origin can be answered](0068-core-owns-the-io-processor-registry-and-every-origin-can-be-answered.md) |
| [0069](0069-an-idle-actor-carries-no-event.md) | [An idle actor carries no event](0069-an-idle-actor-carries-no-event.md) |
| [0070](0070-a-run-that-runs-out-of-memory-stops-where-it-stands.md) | [A run that runs out of memory stops where it stands](0070-a-run-that-runs-out-of-memory-stops-where-it-stands.md) |
| [0071](0071-transitions-actions-and-expressions-are-inspectable-by-id.md) | [Transitions, actions and expressions are inspectable by ID](0071-transitions-actions-and-expressions-are-inspectable-by-id.md) |
| [0072](0072-metrics-are-counters-core-keeps-logs-are-records-it-already-writes.md) | [Metrics are counters core keeps; logs are records it already writes](0072-metrics-are-counters-core-keeps-logs-are-records-it-already-writes.md) |
| [0073](0073-core-builds-generates-and-samples-trace-contexts.md) | [Core builds, generates and samples trace contexts](0073-core-builds-generates-and-samples-trace-contexts.md) |
| [0074](0074-an-unloaded-actor-is-offloaded-to-bytes-within-its-process.md) | [An unloaded actor is offloaded to bytes within its process](0074-an-unloaded-actor-is-offloaded-to-bytes-within-its-process.md) |
| [0075](0075-a-macrostep-that-runs-too-many-microsteps-stops-as-a-runaway.md) | [A macrostep that runs too many microsteps stops as a runaway](0075-a-macrostep-that-runs-too-many-microsteps-stops-as-a-runaway.md) |
| [0076](0076-an-imported-document-chooses-its-data-model-by-name.md) | [An imported document chooses its data model by name, and a data model learns how each expression is used](0076-an-imported-document-chooses-its-data-model-by-name.md) |
| [0077](0077-a-definition-is-reused-by-identity-and-freed-when-nothing-holds-it.md) | [A definition is reused by identity and freed when nothing holds it](0077-a-definition-is-reused-by-identity-and-freed-when-nothing-holds-it.md) |
| [0078](0078-the-definitions-of-a-system-are-walked-by-page.md) | [The definitions of a system are walked by page](0078-the-definitions-of-a-system-are-walked-by-page.md) |
| [0079](0079-core-tells-the-host-when-a-definition-is-registered-retired-or-freed.md) | [Core tells the host when a definition is registered, retired or freed](0079-core-tells-the-host-when-a-definition-is-registered-retired-or-freed.md) |
| [0080](0080-definition-events-are-lifecycle-records-too.md) | [Definition events are lifecycle records too](0080-definition-events-are-lifecycle-records-too.md) |
| [0081](0081-a-host-reports-an-invocation-error-with-its-own-reason.md) | [A host reports an invocation error with its own reason](0081-a-host-reports-an-invocation-error-with-its-own-reason.md) |
| [0082](0082-operator-transitions-and-abandoning-a-macrostep.md) | [An operator transition is a transition, and a macrostep that never ends can be abandoned](0082-operator-transitions-and-abandoning-a-macrostep.md) |
| [0083](0083-inspection-reports-where-a-paused-actor-stands.md) | [Inspection reports where a paused actor stands](0083-inspection-reports-where-a-paused-actor-stands.md) |
| [0084](0084-an-operator-can-withdraw-one-pending-delayed-send.md) | [An operator can withdraw one pending delayed send](0084-an-operator-can-withdraw-one-pending-delayed-send.md) |
| [0085](0085-a-spec-says-where-its-elements-were-written-and-an-encode-says-where-it-wrote-them.md) | [A spec says where its elements were written, and an encode says where it wrote them](0085-a-spec-says-where-its-elements-were-written-and-an-encode-says-where-it-wrote-them.md) |
| [0086](0086-a-resume-can-stop-before-a-microsteps-first-unit.md) | [A resume can stop before a microstep's first unit](0086-a-resume-can-stop-before-a-microsteps-first-unit.md) |
| [0087](0087-host-expressions-are-compared-by-handle.md) | [Host expressions are compared by handle](0087-host-expressions-are-compared-by-handle.md) |
| [0088](0088-a-session-id-names-its-family.md) | [A session ID names its family](0088-a-session-id-names-its-family.md) |
| [0089](0089-an-actor-is-rebuilt-by-replaying-its-inputs-before-it-is-published.md) | [An actor is rebuilt by replaying its inputs before it is published](0089-an-actor-is-rebuilt-by-replaying-its-inputs-before-it-is-published.md) |
| [0090](0090-core-defines-the-input-log-its-byte-layout-and-its-replay.md) | [Core defines the input log, its byte layout and its replay](0090-core-defines-the-input-log-its-byte-layout-and-its-replay.md) |
| [0091](0091-a-checkpoint-is-an-actors-state-at-a-record-of-its-log.md) | [A checkpoint is an actor's state at a record of its log](0091-a-checkpoint-is-an-actors-state-at-a-record-of-its-log.md) |
| [0092](0092-system-parameters-are-fixed-at-creation-and-bound-every-buffer.md) | [System parameters are fixed at creation and bound every buffer](0092-system-parameters-are-fixed-at-creation-and-bound-every-buffer.md) |
