# Ruby binding architecture

The Ruby binding has two deliberately separate layers.

## 1. Low-level extension layer

The low-level layer is the Ruby-facing representation of `src/tinyactors.h`.
It must stay close to the C surface and must not silently change runtime
semantics.

Its responsibilities are:

- Wrap opaque C handles such as `ta_system_t`, `ta_definition_t`,
  `ta_actor_ref_t`, `ta_request_ref_t`, and `ta_invocation_ref_t`.
- Represent public C value structures such as events, messages, targets, send
  descriptions, charts, states, transitions, I/O requests, and run options.
- Convert Ruby values to `ta_value_t` native handles or explicitly requested
  byte payloads.
- Convert `ta_status_t` failures into Ruby exceptions without dropping status
  information.
- Root every Ruby object retained by a definition, actor, callback, queued
  message, invocation, or leased I/O request.
- Enforce the lifetime rules of `ta_context_t`, `ta_io_request_t`, and other
  borrowed views. Temporary contexts and views must not remain usable after a
  callback or lease ends.
- Expose the I/O request/complete and invocation emit/done/fail protocol
  without performing transport I/O inside the runtime.

The extension bootstrap currently contains stubs. A stub must be replaced by
an actual wrapper only after its ownership, GC marking, error behavior, and
borrowed/owned lifetime are understood.

The low-level layer is the compatibility boundary. New C API functions must
be represented there before adding DSL features.

## 2. Ruby convenience layer

The convenience layer is ordinary Ruby code built on top of the low-level
objects. It may provide APIs such as:

```ruby
WebhookDelivery = TinyActors.actor(:webhook_delivery) do
  initial :pending

  state :pending do
    on "delivery.start", target: :sending
  end

  state :sending do
    entry do |ctx|
      ctx.http.post(
        ctx[:endpoint],
        headers: {
          "content-type" => "application/json",
          "x-delivery-id" => ctx[:delivery_id]
        },
        body: ctx[:payload],
        reply_to: "delivery.response"
      )
    end

    on "delivery.response", target: :delivered
    on "http.failure", target: :retrying
  end

  state :retrying do
    on "delivery.retry", target: :sending
    on "delivery.exhausted", target: :failed
  end

  state :delivered do
    entry { |ctx| ctx.report(:delivered) }
    final
  end

  state :failed do
    entry { |ctx| ctx.report(:failed) }
    final
  end
end
```

The low-level host registers the processor and services its request leases:

```ruby
system = TinyActors::System.new
system.register_io_processor(
  type: "https://example.test/http",
  aliases: ["http"]
)

actor = system.actor_spawn(
  definition: WebhookDelivery,
  input: {
    endpoint: "https://hooks.example.test/orders",
    delivery_id: "delivery-123",
    payload: { order_id: 42 }
  }
)

actor.send("delivery.start")

loop do
  system.run(work_units: 1_000)

  while (request = system.io_next)
    begin
      response = http_client.perform(request)
      system.io_complete(request, :ok)
      system.io_receive(
        processor: "https://example.test/http",
        destination: request.actor,
        event: response.to_event
      )
    rescue StandardError
      system.io_complete(request, :communication_error)
    end
  end

  break unless system.runnable?
end
```

The processor registration only establishes routing. The host owns the HTTP
client, sockets, retries outside the actor protocol, and response decoding.
`io_next` leases a native request; `io_complete` closes that lease; and
`io_receive` admits the decoded response as an actor event. The convenience
`ctx.http.post` call in the example is shorthand for producing the same
low-level I/O request.

## End-to-end host workflow

The binding documentation should eventually demonstrate the complete host
workflow:

```text
define actor
→ register processor
→ spawn actor
→ post event
→ run bounded work
→ service I/O lease
→ receive response
→ inspect actor
→ observe delivered/failed result
→ release actor/system
```

Inspection is part of the low-level API, not merely debugging output. A host
should be able to inspect one actor after a run boundary:

```ruby
record = system.actor_inspect(actor.ref)

record.state
record.stable?
record.data
record.last_event
record.status
```

For a larger test run, inspection is resumable and budgeted:

```ruby
cursor = nil

loop do
  page = system.inspect(cursor: cursor, work_units: 1_000)

  page.records.each do |record|
    puts "#{record.actor.id}: #{record.state}"
  end

  break if page.done?
  cursor = page.cursor
end
```

Examples should also cover exact target routing (`self`, `internal`, `parent`,
named actor, instance, and processor URI), invocation start/emit/done/cancel,
actor data-model access and GC ownership, timers and `next_deadline`, bounded
run results such as `:again`, error/status handling, graceful shutdown, and
actor release.

Convenience code compiles declarations into low-level `Chart`, `ChartState`,
`ChartTransition`, `Code`, `Send`, and `Invoke` objects. It must not create a
second execution engine or bypass the low-level routing, queue, invocation,
budget, and lifetime rules.

The convenience layer may improve names and defaults, but unusual or advanced
low-level features must remain accessible: explicit target kinds, processor
URIs, delayed sends, ordered parameters, invocation handles, byte payloads,
inspection cursors, upgrades, and I/O leases.

## Interface coverage

The public C interface is audited by:

```sh
make ruby-interface
```

`tools/check_ruby_interface.rb` invokes Clang to extract all public `ta_*`
functions from `src/tinyactors.h` and compares them with
`interface_symbols.h`. Update that manifest whenever a low-level Ruby wrapper
is added for a new C symbol. The check must fail for both missing and stale
entries.

## Naming and semantics

The binding preserves C semantics first. Ruby method names may be idiomatic,
but the mapping should be obvious and documented. Event names are opaque
strings and retain hierarchical names such as `user.session.start`; they must
not be normalized to uppercase or otherwise rewritten.

Actor callbacks execute synchronously from the runtime's perspective. Ruby
Fibers or `Fiber.scheduler` may drive `System#run` from outside the callback,
but callbacks must not recursively run the system or retain a context after it
returns.
