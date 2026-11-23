# json-bench and the trampoline spikes: results

Run 2026-11-23 on one machine, one program after another, nothing else running:
Linux 6.18.44-fc-v726 x86_64, Intel(R) Xeon(R) Processor @ 2.30GHz, 4 CPUs, 15 GiB.
Core at -O2 -DNDEBUG (build/release). Go 1.24.7 with fxamacker/cbor v2.7.0; OpenJDK 21.0.13, Kotlin 2.4.21, kotlinx-serialization-cbor 1.13.0; Swift 6.1.2. The CPUs are shared with the container's host: timings varied up to 1.7x between runs, bytes not at all.

```
== go (with C baselines)
c	empty	22.2 ns/call
c	hasItems	201.0 ns/call
c	addItem	576.7 ns/call
c	reemit(addItem result)	565.5 ns/call
go	crossing	90.4 ns/call	0.0 allocs/call	0 B/call
go	empty	161.1 ns/call	0.0 allocs/call	0 B/call
go	hasItems	1341.1 ns/call	5.0 allocs/call	118 B/call
go	addItem	3170.3 ns/call	9.0 allocs/call	378 B/call
== kotlin
kotlin	empty	175.3 ns/call
kotlin	hasItems	675.5 ns/call
kotlin	addItem	2072.4 ns/call
== swift
[0/2] Write swift-version--6697F7667C86F10B.txt
Build complete! (0.65s)
swift	empty	34.4 ns/call
swift	hasItems	2403.5 ns/call
swift	addItem	4891.7 ns/call
== swift+SwiftCBOR (N=20000)
Build complete! (0.56s)
swift+SwiftCBOR	hasItems	71650.9 ns/call
swift+SwiftCBOR	addItem	110423.3 ns/call
json     actors 10000: bytes/actor idle 679 (datamodel 361), after the order's 3 events 874 (datamodel 515)
json     ns/event: order's add/address 1767, a guard alone 505, a guard and an assignment 1213
quickjs  actors 10000: bytes/actor idle 1127 (datamodel 809), after the order's 3 events 2260 (datamodel 1771)
quickjs  ns/event: order's add/address 3119, a guard alone 1389, a guard and an assignment 2137
json     document of     64 bytes: an assignment 636 ns/event
quickjs  document of     64 bytes: an assignment 1602 ns/event
json     document of   1024 bytes: an assignment 1307 ns/event
quickjs  document of   1024 bytes: an assignment 2409 ns/event
json     document of  16384 bytes: an assignment 2008 ns/event
quickjs  document of  16384 bytes: an assignment 1937 ns/event
json     document of  60000 bytes: an assignment 5559 ns/event
quickjs  document of  60000 bytes: an assignment 2368 ns/event
---
json     actors 10000: bytes/actor idle 679 (datamodel 361), after the order's 3 events 874 (datamodel 515)
json     ns/event: order's add/address 1723, a guard alone 494, a guard and an assignment 1221
quickjs  actors 10000: bytes/actor idle 1127 (datamodel 809), after the order's 3 events 2260 (datamodel 1771)
quickjs  ns/event: order's add/address 3161, a guard alone 1796, a guard and an assignment 2027
json     document of     64 bytes: an assignment 640 ns/event
quickjs  document of     64 bytes: an assignment 1320 ns/event
json     document of   1024 bytes: an assignment 716 ns/event
quickjs  document of   1024 bytes: an assignment 1281 ns/event
json     document of  16384 bytes: an assignment 1136 ns/event
quickjs  document of  16384 bytes: an assignment 1257 ns/event
json     document of  60000 bytes: an assignment 5106 ns/event
quickjs  document of  60000 bytes: an assignment 2500 ns/event
---
json     actors 10000: bytes/actor idle 679 (datamodel 361), after the order's 3 events 874 (datamodel 515)
json     ns/event: order's add/address 2489, a guard alone 494, a guard and an assignment 1227
quickjs  actors 10000: bytes/actor idle 1127 (datamodel 809), after the order's 3 events 2260 (datamodel 1771)
quickjs  ns/event: order's add/address 3256, a guard alone 1452, a guard and an assignment 2339
json     document of     64 bytes: an assignment 640 ns/event
quickjs  document of     64 bytes: an assignment 1271 ns/event
json     document of   1024 bytes: an assignment 731 ns/event
quickjs  document of   1024 bytes: an assignment 1388 ns/event
json     document of  16384 bytes: an assignment 1548 ns/event
quickjs  document of  16384 bytes: an assignment 1262 ns/event
json     document of  60000 bytes: an assignment 5226 ns/event
quickjs  document of  60000 bytes: an assignment 1394 ns/event
---
```

Commands: `build/release/bin/json-bench 10000 200000`; `trampolines/go/run.sh`; `trampolines/kotlin/run.sh 1000000 200000 5`; `JSONBENCH_ONLY=swift/ trampolines/swift/run.sh 1000000 5`; `JSONBENCH_ONLY=SwiftCBOR trampolines/swift/run.sh 20000 3`.
