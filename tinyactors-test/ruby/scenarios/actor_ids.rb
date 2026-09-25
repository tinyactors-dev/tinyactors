# frozen_string_literal: true

# An actor ID is a public slot and generation (ADR 0059): the slot in the
# low 32 bits, the generation in the high 32. A host splits and joins IDs
# with ta_actor_id_slot, ta_actor_id_generation and ta_actor_id_make, and
# can carry an actor as two small integers without losing stale-ID
# detection. A made ID is checked where it is used, like any other.

TinyactorsTest.chart :actor_ids do
  state :idle
end

# The three pure functions over the protocol, and a check of a raw ID.
module ActorIDs
  def self.slot(context, id)
    context.ta.actor.id_slot(id: id)[:value]
  end

  def self.generation(context, id)
    context.ta.actor.id_generation(id: id)[:value]
  end

  def self.make(context, slot, generation)
    context.ta.actor.id_make(slot: slot, generation: generation)[:value]
  end

  # What inspecting a raw ID answers: ok, or err_not_found for a stale one.
  def self.inspect(context, id)
    context.ta.actor.inspect(system: context.system_handle, actor: id)[:result]
  end
end

TinyactorsTest.scenarios 'actor IDs', chart: :actor_ids do
  # Catches: a split or join that disagrees with the documented layout, or
  # with the IDs core actually issues.
  scenario 'an ID splits into its slot and generation and joins back' do
    actors = Array.new(3) { spawn }
    actors.each do |actor|
      id = actor.handle
      slot = ActorIDs.slot(self, id)
      generation = ActorIDs.generation(self, id)
      assert_equal id & 0xffff_ffff, slot, 'the slot is the low 32 bits'
      assert_equal id >> 32, generation, 'the generation is the high 32 bits'
      assert generation.positive?, 'no ID has generation 0'
      assert_equal id, ActorIDs.make(self, slot, generation), 'make joins the halves back into the ID'
      assert_equal 'ok', ActorIDs.inspect(self, ActorIDs.make(self, slot, generation))
    end
    slots = actors.map { |actor| ActorIDs.slot(self, actor.handle) }
    assert_equal slots.uniq.size, slots.size, 'resident actors have distinct slots'
  end

  # Catches: a made ID accepted with the wrong generation, which would let a
  # host that kept a stale pair reach whatever actor now holds the slot.
  scenario 'a made ID with the wrong generation is not found' do
    actor = spawn
    slot = ActorIDs.slot(self, actor.handle)
    generation = ActorIDs.generation(self, actor.handle)
    assert_equal 'err_not_found', ActorIDs.inspect(self, ActorIDs.make(self, slot, generation + 1))
    assert_equal 'err_not_found', ActorIDs.inspect(self, ActorIDs.make(self, slot, 0)),
                 'generation 0 is never valid'
    assert_equal 'err_not_found', ActorIDs.inspect(self, ActorIDs.make(self, 0, 0)), 'the zero ID is never valid'
    assert_equal 'ok', ActorIDs.inspect(self, actor.handle), 'the right pair still names the actor'
  end

  # Catches: a slot reused without a new generation, so a destroyed actor's
  # ID names its successor.
  scenario "a destroyed actor's ID goes stale, and the slot's next occupant is newer" do
    first = spawn
    slot = ActorIDs.slot(self, first.handle)
    generation = ActorIDs.generation(self, first.handle)
    first.destroy
    assert_equal 'err_not_found', ActorIDs.inspect(self, first.handle), 'the old ID is stale'
    second = spawn
    assert_equal slot, ActorIDs.slot(self, second.handle), 'the free slot is reused'
    assert ActorIDs.generation(self, second.handle) > generation, 'with a newer generation'
    assert_equal 'err_not_found', ActorIDs.inspect(self, first.handle), 'the old ID still names nothing'
    assert_equal 'ok', ActorIDs.inspect(self, second.handle)
  end
end
