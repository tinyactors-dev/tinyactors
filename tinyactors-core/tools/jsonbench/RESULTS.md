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

## Second round: results as bytes, the check, appending

The same machine, idle, one program after another, after the lent-buffer ABI and call indexes (`json-bench 1000 20000 append`, `trampolines/go/run.sh`, `trampolines/kotlin/run.sh`).

```
== go
c	empty	writer	17.8 ns/call
c	hasItems	writer	195.7 ns/call
c	addItem	writer	559.9 ns/call
c	newItem	writer	167.2 ns/call
c	reemit(addItem result)	writer	581.4 ns/call
c	empty	bytes	24.7 ns/call
c	hasItems	bytes	191.9 ns/call
c	addItem	bytes	560.5 ns/call
c	newItem	bytes	165.5 ns/call
c	empty	bytes+check	31.8 ns/call
c	hasItems	bytes+check	212.7 ns/call
c	addItem	bytes+check	798.4 ns/call
c	newItem	bytes+check	248.8 ns/call
go	crossing	writer	94.0 ns/call	0.0 allocs/call	0 B/call
go	empty	writer	176.6 ns/call	0.0 allocs/call	0 B/call
go	hasItems	writer	1268.9 ns/call	5.0 allocs/call	118 B/call
go	addItem	writer	2866.2 ns/call	9.0 allocs/call	378 B/call
go	newItem	writer	1177.9 ns/call	4.0 allocs/call	83 B/call
go	crossing	bytes	88.1 ns/call	0.0 allocs/call	0 B/call
go	empty	bytes	159.8 ns/call	1.0 allocs/call	1 B/call
go	hasItems	bytes	1429.9 ns/call	6.0 allocs/call	120 B/call
go	addItem	bytes	2168.3 ns/call	9.0 allocs/call	378 B/call
go	newItem	bytes	789.5 ns/call	4.0 allocs/call	83 B/call
go	empty	bytes+check	163.7 ns/call	1.0 allocs/call	1 B/call
go	hasItems	bytes+check	1283.5 ns/call	6.0 allocs/call	120 B/call
go	addItem	bytes+check	2611.8 ns/call	9.0 allocs/call	378 B/call
go	newItem	bytes+check	895.2 ns/call	4.0 allocs/call	83 B/call
== kotlin
kotlin	empty	array	162.6 ns/call
kotlin	hasItems	array	579.3 ns/call
kotlin	addItem	array	2099.6 ns/call
kotlin	newItem	array	699.7 ns/call
kotlin	empty	buffer	146.3 ns/call
kotlin	hasItems	buffer	537.8 ns/call
kotlin	addItem	buffer	1990.7 ns/call
kotlin	newItem	buffer	686.3 ns/call
kotlin	empty	bytes	135.9 ns/call
kotlin	hasItems	bytes	597.1 ns/call
kotlin	addItem	bytes	1398.5 ns/call
kotlin	newItem	bytes	479.0 ns/call
kotlin	empty	bytes-direct	92.5 ns/call
kotlin	hasItems	bytes-direct	467.1 ns/call
kotlin	addItem	bytes-direct	1334.8 ns/call
kotlin	newItem	bytes-direct	439.1 ns/call
kotlin	empty	bytes+check	150.2 ns/call
kotlin	hasItems	bytes+check	558.0 ns/call
kotlin	addItem	bytes+check	1711.9 ns/call
kotlin	newItem	bytes+check	599.3 ns/call
== json-bench append (run 1)
to  10 items, ns/event: json addItem 4267 (trusted 3900), json items[length(items)] 2143 (trusted 2030); quickjs concat 4751, items[items.length] 2765
to  50 items, ns/event: json addItem 14484 (trusted 12186), json items[length(items)] 3251 (trusted 3121); quickjs concat 4171, items[items.length] 2587
to 200 items, ns/event: json addItem 51563 (trusted 48070), json items[length(items)] 7559 (trusted 7463); quickjs concat 7763, items[items.length] 4821
== json-bench append (run 2)
to  10 items, ns/event: json addItem 4361 (trusted 3807), json items[length(items)] 2241 (trusted 2089); quickjs concat 3453, items[items.length] 2596
to  50 items, ns/event: json addItem 13352 (trusted 11324), json items[length(items)] 3412 (trusted 3278); quickjs concat 4305, items[items.length] 2709
to 200 items, ns/event: json addItem 50515 (trusted 39971), json items[length(items)] 8493 (trusted 7326); quickjs concat 6972, items[items.length] 2527
```

## Third round: a staticjs datamodel beside json and quickjs

The same machine, idle, one program after another (`json-bench 10000 200000`, `json-bench 1000 20000 append`, `json-bench 1 1000000 convert`). "staticjs" runs the ECMAScript charts as written; "staticjs+natives" runs the json charts, calling the same functions as staticjs natives in C.

```
== json-bench 10000 200000 (run 1)
json             actors 10000: bytes/actor idle 679 (datamodel 361), after the order's 3 events 874 (datamodel 515)
json             ns/event: order's add/address 2295, a guard alone 506, a guard and an assignment 1404
staticjs         actors 10000: bytes/actor idle 1226 (datamodel 908), after the order's 3 events 2184 (datamodel 1826)
staticjs         ns/event: order's add/address 1629, a guard alone 758, a guard and an assignment 884
staticjs+natives actors 10000: bytes/actor idle 1226 (datamodel 908), after the order's 3 events 2152 (datamodel 1794)
staticjs+natives ns/event: order's add/address 1352, a guard alone 776, a guard and an assignment 898
quickjs          actors 10000: bytes/actor idle 1127 (datamodel 809), after the order's 3 events 2260 (datamodel 1771)
quickjs          ns/event: order's add/address 2687, a guard alone 1132, a guard and an assignment 1716
json             document of     64 bytes: an assignment 644 ns/event
staticjs         document of     64 bytes: an assignment 721 ns/event
staticjs+natives document of     64 bytes: an assignment 750 ns/event
quickjs          document of     64 bytes: an assignment 1234 ns/event
json             document of   1024 bytes: an assignment 734 ns/event
staticjs         document of   1024 bytes: an assignment 737 ns/event
staticjs+natives document of   1024 bytes: an assignment 753 ns/event
quickjs          document of   1024 bytes: an assignment 1263 ns/event
json             document of  16384 bytes: an assignment 1179 ns/event
staticjs         document of  16384 bytes: an assignment 731 ns/event
staticjs+natives document of  16384 bytes: an assignment 777 ns/event
quickjs          document of  16384 bytes: an assignment 1238 ns/event
json             document of  60000 bytes: an assignment 4342 ns/event
staticjs         document of  60000 bytes: an assignment 727 ns/event
staticjs+natives document of  60000 bytes: an assignment 752 ns/event
quickjs          document of  60000 bytes: an assignment 1233 ns/event
== json-bench 10000 200000 (run 2)
json             actors 10000: bytes/actor idle 679 (datamodel 361), after the order's 3 events 874 (datamodel 515)
json             ns/event: order's add/address 1787, a guard alone 524, a guard and an assignment 1225
staticjs         actors 10000: bytes/actor idle 1226 (datamodel 908), after the order's 3 events 2184 (datamodel 1826)
staticjs         ns/event: order's add/address 1638, a guard alone 770, a guard and an assignment 899
staticjs+natives actors 10000: bytes/actor idle 1226 (datamodel 908), after the order's 3 events 2152 (datamodel 1794)
staticjs+natives ns/event: order's add/address 1530, a guard alone 945, a guard and an assignment 907
quickjs          actors 10000: bytes/actor idle 1127 (datamodel 809), after the order's 3 events 2260 (datamodel 1771)
quickjs          ns/event: order's add/address 2694, a guard alone 1133, a guard and an assignment 1715
json             document of     64 bytes: an assignment 654 ns/event
staticjs         document of     64 bytes: an assignment 727 ns/event
staticjs+natives document of     64 bytes: an assignment 757 ns/event
quickjs          document of     64 bytes: an assignment 1251 ns/event
json             document of   1024 bytes: an assignment 739 ns/event
staticjs         document of   1024 bytes: an assignment 723 ns/event
staticjs+natives document of   1024 bytes: an assignment 752 ns/event
quickjs          document of   1024 bytes: an assignment 1345 ns/event
json             document of  16384 bytes: an assignment 1213 ns/event
staticjs         document of  16384 bytes: an assignment 750 ns/event
staticjs+natives document of  16384 bytes: an assignment 758 ns/event
quickjs          document of  16384 bytes: an assignment 1280 ns/event
json             document of  60000 bytes: an assignment 4480 ns/event
staticjs         document of  60000 bytes: an assignment 722 ns/event
staticjs+natives document of  60000 bytes: an assignment 756 ns/event
quickjs          document of  60000 bytes: an assignment 1235 ns/event
== json-bench append (run 1)
to  10 items, ns/event: json addItem 4098 (trusted 3494), json items[length(items)] 2059 (trusted 1963); quickjs concat 3542, items[items.length] 2588; staticjs concat 1638, items[items.length] 1886; staticjs+natives addItem 1993, newItem 1593
to  50 items, ns/event: json addItem 13616 (trusted 11481), json items[length(items)] 3303 (trusted 3140); quickjs concat 4356, items[items.length] 2713; staticjs concat 1937, items[items.length] 2197; staticjs+natives addItem 1997, newItem 2352
to 200 items, ns/event: json addItem 49411 (trusted 43582), json items[length(items)] 8804 (trusted 7997); quickjs concat 7397, items[items.length] 2895; staticjs concat 2381, items[items.length] 2780; staticjs+natives addItem 2512, newItem 2625
== json-bench append (run 2)
to  10 items, ns/event: json addItem 4191 (trusted 3794), json items[length(items)] 2141 (trusted 1996); quickjs concat 3491, items[items.length] 2607; staticjs concat 1726, items[items.length] 1665; staticjs+natives addItem 1487, newItem 1617
to  50 items, ns/event: json addItem 13354 (trusted 11431), json items[length(items)] 3300 (trusted 3154); quickjs concat 4462, items[items.length] 2614; staticjs concat 1859, items[items.length] 1889; staticjs+natives addItem 1763, newItem 1870
to 200 items, ns/event: json addItem 47884 (trusted 42705), json items[length(items)] 7617 (trusted 7447); quickjs concat 7324, items[items.length] 2566; staticjs concat 2392, items[items.length] 2642; staticjs+natives addItem 4380, newItem 3966
== convert (run 1)
staticjs natives in another language, per addItem call: arguments to CBOR and the result back 1601 ns (writing the arguments 725, reading the result 876)
== convert (run 2)
staticjs natives in another language, per addItem call: arguments to CBOR and the result back 1370 ns (writing the arguments 734, reading the result 636)
```
