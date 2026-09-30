# Architecture decision records: tinyactors-host

These ADRs record the design of the host kernel: the C that every host
keeping actors in memory needs, free of any language's API (see the
repository README, "Layers and responsibilities").

Much of the kernel's code comes from `tinyactors-napi`, whose ADRs
recorded how it was decided. Those stay in `tinyactors-napi/docs/adr` and
are cited from here. Core decisions live in `tinyactors-core/docs/adr`.

| ADR | Title |
|---|---|
| [0001](0001-the-host-kernel-holds-what-every-host-needs.md) | [The host kernel holds what every host needs](0001-the-host-kernel-holds-what-every-host-needs.md) |
| [0002](0002-the-kernel-keeps-the-write-ahead-log.md) | [The kernel keeps the write-ahead log](0002-the-kernel-keeps-the-write-ahead-log.md) |
| [0003](0003-a-full-mailbox-overflows-instead-of-refusing-mail-between-actors.md) | [A full mailbox overflows instead of refusing mail between actors](0003-a-full-mailbox-overflows-instead-of-refusing-mail-between-actors.md) |
