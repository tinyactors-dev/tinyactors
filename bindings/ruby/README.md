# TinyActors Ruby binding

This directory is the bootstrap for a low-level, 1-to-1 Ruby binding of
`src/tinyactors.h`. The Ruby convenience DSL belongs above this layer and is
not part of the initial extension.

The intended boundary is:

```text
Ruby value objects and callback handles
        |
        +-- bindings/ruby/tinyactors.c
        |
        +-- tinyactors.h / libtinyactors
        |
Ruby System, Definition, Actor, Event, Send, Invoke, and I/O leases
```

The C extension currently contains placeholders. Methods deliberately raise
`TinyActors::NotImplementedError` until their corresponding ownership,
GC-rooting, status, and lifetime rules are implemented.

## Local build

```sh
cd bindings/ruby
ruby extconf.rb
make
ruby -Ilib -I. -e 'require "tinyactors"; p TinyActors::VERSION'
```

The extension is intentionally not linked against the runtime yet. Linking
the runtime will happen after the low-level object and callback mapping is
settled.
