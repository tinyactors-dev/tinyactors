# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "tinyactors"

count = Integer(ARGV.fetch(0, 1_000))
raise ArgumentError, "count must be positive" unless count.positive?

system = TinyActors::System.new
definition = system.definition_build(
  name: "entity",
  initial: :alive,
  states: { alive: { on: [{ event: "tick" }] } }
)
actors = count.times.map { |i| system.actor_spawn(definition, name: "entity-#{i}") }
system.run while system.runnable?

start = Process.clock_gettime(Process::CLOCK_MONOTONIC)
actors.each { |actor| actor.send("tick") }
system.run while system.runnable?
elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - start

puts({ actors: count, elapsed_seconds: elapsed, stats: system.stats }.inspect)
actors.each { |actor| actor.stop }
system.run while system.runnable?
actors.each(&:release)
system.close
