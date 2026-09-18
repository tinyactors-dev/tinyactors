# `clock.h`

The clock stores caller-supplied monotonic logical time. `ta_time` is in
milliseconds; delays in a statechart are converted to milliseconds before they
are added to it.

The core must never read an OS wall clock. `ta_system_set_time()` should reject backwards movement unless a future explicit simulation API defines otherwise.

Logical time is used to determine eligibility of delayed sends. Time advancement alone should not perform arbitrary external I/O during deserialization/load; it makes work due, and ordinary execution/scheduler integration dispatches it according to host policy.
