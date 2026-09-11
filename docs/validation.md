# Validation record

Measured on 2026-09-11, macOS arm64, Apple clang, C99 `-O2`. These results describe
this checkout and these workloads, not general throughput or conformance claims.

## Checks

- Strict desktop C99, C++11 public-header and `arm-none-eabi` syntax checks pass.
- All 54 externally declared API functions have implementations. All archive
  references resolve internally; there is no unresolved libc dependency.
- Every implemented C function in the library, examples, tests and benchmark has
  at most three parameters.
- 25 test executables pass, including host chunk-descriptor checks and injected
  allocation failure. The suite also passes with macOS malloc scribbling and
  guard edges enabled.
- The hosted UndefinedBehaviorSanitizer suite passes with recovery disabled.
- Apple AddressSanitizer hangs before `main` on this machine. Sampling shows
  recursive initialization through dyld shared-cache enumeration and intercepted
  malloc. The same problem occurs with the library's compiler primitives omitted.
  Those runs were terminated; they are **not** sanitizer passes.

Semantic cases include eventless-before-internal-before-next-external ordering,
microstep yields, internal/external parent re-entry, simultaneous orthogonal
transitions, shallow/deep history, nested parallel completion, block error
boundaries, resource-error reservation, and cross-yield livelock ceilings.

Integration cases include external invoke payload snapshots and completion,
START/CANCEL races before and after leasing, inline child final data and automatic
retirement, byte/native upgrade ownership, binding/allocation rollback, retained
history and FIFO, repeated-upgrade memory plateaus, current-incarnation routing,
definition publication, simulation-to-renderer streaming, WebSocket heartbeat
and timeout, and process/send span parent/link/end relationships.

## 200,000 entities

`make benchmark` creates 200,000 actors sharing a single-state definition. Each
owns a 24-byte `{ x, y, vx, vy, sprite, entity }` C value. It initializes them,
posts one tick each, processes the tick, streams a checked frame with 1,024-record
budgets, stops/releases all actors, and repeats with fresh incarnation IDs.

| Measurement | First round | Second round |
| --- | ---: | ---: |
| Idle acquired capacity, actor delta | 100,600,000 B | 100,600,000 B |
| Acquired bytes per actor, including state | 503 B | 503 B |
| Delta with one queued event per actor | 203,000,000 B | 203,000,000 B |
| Spawn CPU time | 51.4 ms | 23.2 ms |
| Initialize CPU time | 49.2 ms | 43.5 ms |
| Post CPU time | 36.3 ms | 34.5 ms |
| Process CPU time | 47.6 ms | 46.0 ms |
| Stream inspection CPU time | 18.9 ms | 17.6 ms |
| Stop/run/release CPU time | 55.4 ms | 46.5 ms |

Inspection acquired zero chunks. Both rounds restored the pre-actor byte
baseline. These figures exclude host allocator overhead, the benchmark's ref
array, VM heaps, rendering, payload serialization and export buffers. `_event`
retains its last dequeued message until replacement/release, so processed actors
with a last event have a higher footprint than never-messaged idle actors.

The initial scale run exposed a linear tail scan on each actor insertion. Root
review replaced it with constant-time live-list insertion; the ID index remains
an intrusive mixed-ID treap. Ready scheduling and timer selection do not scan
idle actors. Inspection walks live actors, not historical IDs.

## Boundaries

This is not a claim of full W3C conformance certification: the complete W3C test
corpus, exhaustive allocation-failure combinations and randomized model checking
have not been run. No ESP32 hardware, real network backend or real OTel collector
was exercised. The [Node binding validation](../bindings/napi/README.md#limits-and-validation)
records separate JS-object measurements and native-addon tests. ARM syntax checking is not an ESP32 memory,
latency, linking or hardware test. Hosts must validate their compiler runtime,
memory policy, native rooting, transports and thread ownership on target.

Arena chunk sizes, feature quotas, actor limits and work budgets are tunable.
The benchmark is not a promise of a 60 Hz full-entity frame budget; measure the
actual charts and message/inspection workload. The public header specifies the
resource-exhaustion extensions and lifetime rules.
