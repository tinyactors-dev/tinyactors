# frozen_string_literal: true

# Share everything at definition level (ADRs 0003 and 0018): immutable
# topology, transition and action tables, event patterns, and the
# configuration and history layouts live in the definition, so an actor
# holds only its own session state. `ta_definition_inspect` reports that as
# `actor_fixed_bytes`, and the counting allocator says what core really
# took, so the claim is measurable rather than asserted.
#
# `actor_fixed_bytes` covers the actor record, session storage, and the
# data-model instance before the actor processes events; it deliberately
# excludes queue capacities and the sparse state of invoked actors. The
# charts here arm no timers and invoke nothing before they are measured, so
# the two numbers must agree exactly.
module ActorMemory
  def self.live(context) = context.allocator.stats.fetch(:live_bytes)

  # A chart of `count` states, each with two transitions: a definition that
  # grows with `count` while a session stays a configuration layout.
  def self.chart(count)
    TinyactorsTest::Chart.define(:"sized_#{count}") do
      data n: 0
      count.times do |index|
        state :"s#{index}" do
          transition on: 'go', target: :"s#{(index + 1) % count}" do
            assign '.n', '.n + 1'
          end
          transition on: 'other', target: :"s#{(index + 2) % count}"
        end
      end
    end
  end

  # Bytes the allocator hands out for one more actor of `chart`. The first
  # spawn also pays one-off costs (the definition, the actor index), so the
  # caller spawns one first.
  def self.marginal(context, chart)
    before = live(context)
    context.spawn(chart)
    live(context) - before
  end
end

TinyactorsTest.scenarios 'actor memory' do
  # Catches: per-actor copies of what the definition owns, and an
  # `actor_fixed_bytes` that does not describe what core allocates. A
  # traffic light has 8 states, a history pseudo-state and three timers;
  # copying its tables into each actor would cost far more than its
  # reported fixed size.
  scenario 'an actor costs exactly the fixed size its definition reports' do
    %i[counter_v1 relay traffic_light].each do |chart|
      fixed = definition_facts(chart).fetch(:actor_fixed_bytes)
      first = spawn(chart)
      assert_equal fixed, first.inspect_actor.fetch(:allocated_bytes),
                   "#{chart}: inspection agrees with the definition's fixed size"
      assert_equal fixed, ActorMemory.marginal(self, chart),
                   "#{chart}: the allocator handed out exactly the fixed size for one more actor"
    end
  end

  # Catches: a definition compiled, copied, or indexed per actor. A chart
  # 200 times bigger costs its actors a little more configuration layout
  # and nothing else: here 96 bytes more per actor against 152 KB more of
  # definition.
  scenario 'a bigger chart costs its definition, not its actors' do
    small = ActorMemory.chart(2)
    large = ActorMemory.chart(400)
    spawn(small)
    spawn(large)

    small_facts = definition_facts(small)
    large_facts = definition_facts(large)
    per_actor = { small: ActorMemory.marginal(self, small), large: ActorMemory.marginal(self, large) }

    assert_equal small_facts.fetch(:actor_fixed_bytes), per_actor[:small]
    assert_equal large_facts.fetch(:actor_fixed_bytes), per_actor[:large]
    definition_growth = large_facts.fetch(:definition_bytes) - small_facts.fetch(:definition_bytes)
    actor_growth = per_actor[:large] - per_actor[:small]
    assert definition_growth > 100_000,
           "200x the states is a much bigger definition: #{definition_growth} bytes more"
    assert actor_growth < 256,
           "but only a little more per actor: #{actor_growth} bytes more, for #{large_facts[:state_count]} states"
  end

  # Catches: session state that outlives its actor - a leaked configuration,
  # history, data-model instance, or queue. The teardown leak check catches
  # what survives the system; this catches what survives the actor. A first
  # round may grow a shared index (the actor table, the timer heap); once
  # grown, a round of spawns and destroys must net to nothing.
  scenario 'spawning and destroying actors reaches a steady state' do
    round = lambda do
      before = ActorMemory.live(self)
      batch = Array.new(5) { spawn :traffic_light }
      advance_time 30_000
      batch.each(&:destroy)
      ActorMemory.live(self) - before
    end

    round.call # warm up: compile the definition and grow the shared indexes
    assert_equal [0, 0, 0], Array.new(3) { round.call },
                 'a warmed-up round of spawns and destroys releases everything it took'
  end

  # Catches: memory that grows with what an actor has done rather than with
  # what it holds - a history list that appends instead of replacing, a
  # queue that never shrinks, a data model that accumulates.
  scenario 'an actor that has run for an hour holds what a fresh one holds' do
    veteran = spawn :traffic_light
    before = veteran.inspect_actor.fetch(:allocated_bytes)
    120.times { advance_time 30_000 }

    assert veteran.resident?, 'the veteran is still running'
    assert_equal before, veteran.inspect_actor.fetch(:allocated_bytes),
                 'an hour of cycling changed nothing it holds'
    fresh = spawn :traffic_light
    assert_equal fresh.inspect_actor.fetch(:allocated_bytes), veteran.inspect_actor.fetch(:allocated_bytes)
  end
end
