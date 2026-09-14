# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "tinyactors"

actors_count = Integer(ARGV.fetch(0, 64))
hops = Integer(ARGV.fetch(1, 1_000_000))
raise ArgumentError, "actors and hops must be positive" unless actors_count.positive? && hops.positive?

actors = []
seen = 0
system = TinyActors::System.new(
  run: lambda do |context|
    seen += 1
    next if seen > hops

    current = actors.index { |actor| actor.id == context.actor }
    context.send(event: "hop", target: TinyActors::Target.instance(actors[(current + 1) % actors_count].id))
  end
)
definition = system.definition_build(
  name: "ring",
  initial: :ready,
  states: { ready: { on: [{ event: "hop", actions: [{ type: :custom, code: 1 }] }] } }
)
actors.concat(actors_count.times.map do |i|
  system.actor_spawn(definition, name: "ring-#{i}")
end)
system.run while system.runnable?

start = Process.clock_gettime(Process::CLOCK_MONOTONIC)
actors[0].send("hop")
system.run while system.runnable?
elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - start

stats = system.stats
puts format(
  "actors=%d hops=%d elapsed_ns=%d ns_per_hop=%.1f hops_per_sec=%.3f",
  actors_count, hops, (elapsed * 1e9).to_i, elapsed * 1e9 / hops,
  hops / elapsed
)

actors.each(&:stop)
system.run while system.runnable?
actors.each(&:release)
system.close
