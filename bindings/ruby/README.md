# Ruby binding rebuild

The previous Ruby extension and convenience implementation were removed.
This directory currently contains namespace scaffolding and implementation briefs.
It cannot create or run actors yet.

The replacement has two layers:

```ruby
# Procedural C mapping. Runtime failures remain C status results.
TinyActors::Raw.ta_context_send(raw_context, raw_send)

# Ruby convenience API. Failures carry their C status in TinyActors::Error.
context.reply("pong", data: { accepted: true })
```

These examples describe the planned API. They do not run in the current scaffold.
All opaque raw handles belong under `TinyActors::Raw::*`.
All convenience classes belong under `TinyActors::*`.

Start with [the brief index](briefs/README.md).
Read [the shared contract](briefs/CONTRACT.md) before delegating an individual class.
The index includes dependency order and a reusable assignment prompt for Luna at low reasoning effort.
No agents were delegated during this reset.

Check the scaffold and the coverage plan from the repository root:

```sh
ruby -Ibindings/ruby/lib -e 'require "tinyactors"; p TinyActors::Raw'
make ruby-interface
ruby tools/test_check_ruby_interface.rb
```

The coverage check reports planned and implemented procedures separately.
The following check intentionally fails until every raw procedure has implementation evidence:

```sh
ruby tools/check_ruby_interface.rb --require-complete
```

The native build will return under `ext/tinyactors_raw/` through the bootstrap brief.
The loader must never fall back to the retired `tinyactors.bundle`.

The [verification loop](verification.md) is implemented. It starts with hundreds
of failing checks for the missing binding and keeps agent-attributed history in
`bindings/ruby/.verification/checks.sqlite3`. Configure Ruby with
`python3 tools/verify_ruby.py init --ruby /path/to/ruby`, then use `run`, `next`,
`show`, and `gate`. A passing coverage plan alone does not pass this verifier.
