# tinyactors-host: the host kernel

The C that every host keeping actors in memory needs, free of any
language's API: scheduling, mailboxes, routing, the directory, turns,
effects, the OTLP encoder and the write-ahead log. Language bindings
(`tinyactors-napi`, the test host in `tinyactors-test`) link it and
implement its host interface: values and the data model, a pump driver,
and hooks into their language. See the repository README, "Layers and
responsibilities", and `docs/adr/`.

It is libc-free C, like core, and builds for `wasm32-freestanding`.

It is being extracted from napi; the steps are in the repository's
`ROADMAP.md` ("Migrate to the host kernel").

```
include/tinyactors_host.h   the public API and the host interface
src/                        the kernel (freestanding)
tests/                      test programs
mise/tasks/                 build, test, asan, check-recursion, check-freestanding
```

`mise run build` builds `build/libtinyactors_host.a` and the tests;
`mise run test` checks recursion and freestanding builds, runs the tests
under the sanitizers, then runs them.
