# frozen_string_literal: true

$LOAD_PATH.unshift File.expand_path("../lib", __dir__)
require "tinyactors"

system = TinyActors::System.new
definition = system.definition_build(
  name: "counter",
  initial: :ready,
  states: {
    ready: {
      on: [{ event: "increment", actions: [TinyActors.raise_event("changed")] }]
    }
  }
)

actor = system.actor_spawn(definition, name: "counter")
actor.send("increment")
system.run while system.runnable?

puts actor.snapshot.inspect
actor.stop
system.run while system.runnable?
actor.release
system.close
