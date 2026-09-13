# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "tinyactors"

actors_count = Integer(ARGV.fetch(0, 64))
hops = Integer(ARGV.fetch(1, 1_000_000))
raise ArgumentError, "actors and hops must be positive" unless actors_count.positive? && hops.positive?

system = TinyActors::System.new
definition = system.definition_build(
  name: "ring",
  initial: :ready,
  states: { ready: { on: [{ event: "hop" }] } }
)
actors = actors_count.times.map do |i|
  system.actor_spawn(definition, name: "ring-#{i}")
end
system.run while system.runnable?

start = Process.clock_gettime(Process::CLOCK_MONOTONIC)
next_actor = 0
hops.times do
  actors[next_actor].send("hop")
  system.run
  next_actor = (next_actor + 1) % actors_count
end
elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - start

stats = system.stats
puts format(
  "actors=%d hops=%d elapsed_ns=%d ns_per_hop=%.1f hops_per_sec=%.3f microsteps=%d",
  actors_count, hops, (elapsed * 1e9).to_i, elapsed * 1e9 / stats[:microsteps],
  stats[:microsteps] / elapsed, stats[:microsteps]
)

actors.each(&:stop)
system.run while system.runnable?
actors.each(&:release)
system.close
