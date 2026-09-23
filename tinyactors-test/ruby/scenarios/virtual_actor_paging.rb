# frozen_string_literal: true

# Virtual actors paging in and out of memory (docs/manual/virtual-actors.md):
# a host holds N named actors but keeps at most M resident. Messages arrive
# for any of the N at random. A message for an actor that is not resident
# pages it in: when M are already resident, the host evicts one (capture,
# keep the image and its data-model binding, unload), then loads the target
# from its image, or spawns it on its first message, and delivers.
#
# Each actor folds every message it receives into a running value, in order,
# so a lost, repeated or reordered message changes its final state. After
# every message, core's own paged walk (ADR 0062) must count at most M
# resident actors. At the end, every actor's state must equal a reference
# run of the same stream with all N resident and nothing ever evicted.
#
# The stream and the evictions come from one seed. A failure names the seed,
# the message index and the actor; set TAT_PAGING_SEED to replay it.
#
# Catches: an image or binding that loses or repeats a message across a
# residency change; a load that does not continue where the unload left off;
# an unload that leaves the actor resident; a directory that routes to a
# stale runtime ID after a load.
module VirtualActorPagingScenarios
  # Keeps the running value exact in the jq data model's doubles.
  MODULUS = 1_000_000_007

  def self.seed
    Integer(ENV.fetch('TAT_PAGING_SEED', '20260923'))
  end

  # A host that keeps at most `limit` of its named actors resident.
  class Host
    attr_reader :page_ins, :page_outs, :spawns

    def initialize(context, limit:, random:, prefix:, chart: :pager, keep_binding: true)
      @context = context
      @chart = chart
      @limit = limit
      @random = random
      @prefix = prefix
      @resident = []      # names, in the order they became resident
      @actors = {}        # name => the scenario's Actor, resident or not
      @parked = {}        # name => [image, binding] of an unloaded actor
      @page_ins = 0
      @page_outs = 0
      @spawns = 0
      @keep_binding = keep_binding
      @unbound = []       # names loaded without their data binding
    end

    attr_reader :unbound

    def name(index) = "#{@prefix}#{index}"

    # Routes one message to actor `index`, paging it in first if needed.
    def deliver(index, payload)
      actor = page_in(name(index))
      actor.deliver('msg', data: { m: payload })
    end

    # The actor `name`, resident: found, loaded from its image, or spawned.
    def page_in(name)
      actor = @actors[name]
      unless actor.nil?
        result, id = @context.find(actor.address)
        if result == 'ok'
          @context.assert_equal actor.handle, id, "#{name}: the directory routes to its current runtime ID"
          return actor
        end
        @context.assert_equal 'err_unloaded', result, "#{name}: a parked actor is unloaded"
      end

      evict_one while @resident.size >= @limit
      if actor.nil?
        actor = @context.spawn(@chart, name: name)
        @actors[name] = actor
        @spawns += 1
      else
        image, binding = @parked.delete(name)
        loaded = @keep_binding ? @context.load(actor, image, binding: binding) : @context.load(actor, image)
        @context.core('ta_actor_load', { result: loaded })
        @unbound << name unless @keep_binding
        @context.destroy_image(image)
        @page_ins += 1
      end
      @resident << name
      actor
    end

    # Pages out one resident actor, chosen by the seeded random.
    def evict_one
      name = @resident.delete_at(@random.rand(@resident.size))
      actor = @actors.fetch(name)
      image = @context.capture(actor)
      binding = actor.data
      @context.core('ta_actor_unload', { result: @context.unload(actor, image) })
      @parked[name] = [image, binding]
      @page_outs += 1
    end

    # The final data of every actor that received a message, paging each in.
    def final_states
      @actors.keys.sort.to_h { |name| [name, page_in(name).data] }
    end

    # Destroys the images of the actors still paged out: the host owns them.
    def release_parked
      @parked.each_value { |image, _| @context.destroy_image(image) }
      @parked.clear
    end
  end

  # Counts resident actors with core's own paged walk (ADR 0062), not the
  # host's bookkeeping. It reads only the journal entries the walk added, so
  # the count costs the same at the last message as at the first.
  class ResidentCounter
    def initialize(context)
      @context = context
      @inspector = context.recording_inspector
      @seen = nil
    end

    def count
      cursor = 0
      until cursor == @context.actor_visit_end
        result, cursor = @context.visit_actors(@inspector, from: cursor, limit: 64)
        @context.assert_equal 'ok', result, 'the actor walk'
      end
      entries = @inspector.journal(since: @seen)
      @seen = entries.last[:seq] unless entries.empty?
      entries.count { |entry| entry[:op] == 'visit' }
    end
  end

  def self.run(context, population:, limit:, messages:, chart: :pager, keep_binding: true)
    seed = self.seed
    stream = Random.new(seed)
    order = Array.new(messages) { [stream.rand(population), stream.rand(1_000)] }

    paged = Host.new(context, limit: limit, random: Random.new(seed + 1), prefix: 'paged', chart: chart,
                              keep_binding: keep_binding)
    counter = ResidentCounter.new(context)
    order.each_with_index do |(index, payload), at|
      paged.deliver(index, payload)
      count = counter.count
      context.assert count <= limit,
                     "seed #{seed}, message #{at} to #{paged.name(index)}: #{count} actors resident, " \
                     "limit #{limit} (replay with TAT_PAGING_SEED=#{seed})"
    end
    unbound = paged.unbound.map { |name| name.delete_prefix('paged') }
    got = paged.final_states.transform_keys { |name| name.delete_prefix('paged') }
    paged.release_parked

    # The reference: the same stream to a fresh set of actors, all resident.
    reference = Host.new(context, limit: population * 2, random: Random.new(0), prefix: 'all', chart: chart)
    order.each { |index, payload| reference.deliver(index, payload) }
    want = reference.final_states.transform_keys { |name| name.delete_prefix('all') }
    context.assert_equal 0, reference.page_outs, 'the reference run never evicts'

    want.each do |name, state|
      # Loaded without its binding, an actor's data is gone, and every later
      # message fails its first <assign> (error.execution), which skips the
      # rest of that block (SCXML 5.4): its root stays empty.
      state = {} if unbound.include?(name)
      context.assert_equal state, got[name],
                           "seed #{seed}, actor #{name}: paged state differs from the all-resident run " \
                           "(replay with TAT_PAGING_SEED=#{seed})"
    end
    context.assert_equal want.keys, got.keys, "seed #{seed}: the same actors received messages"
    { spawns: paged.spawns, page_ins: paged.page_ins, page_outs: paged.page_outs, unbound: unbound.uniq.size }
  end
end

TinyactorsTest.chart :pager do
  data v: 0
  data n: 0

  state :listening do
    transition on: 'msg', type: :internal do
      assign '.v', "(.v * 31 + $_event.data.m) % #{VirtualActorPagingScenarios::MODULUS}"
      assign '.n', '.n + 1'
    end
  end
end

TinyactorsTest.scenarios 'virtual actor paging', chart: :pager do
  [[100, 3, 1000], [100, 1, 400], [100, 100, 300]].each do |population, limit, messages|
    scenario "#{population} actors, at most #{limit} resident, #{messages} random messages" do
      counts = VirtualActorPagingScenarios.run(self, population: population, limit: limit, messages: messages)
      if limit >= population
        assert_equal 0, counts[:page_outs], 'with room for every actor nothing is evicted'
      else
        assert counts[:page_ins] >= population, "every actor pages in again: #{counts}"
      end
    end
  end

  # Loading without the data binding is allowed (it is TA_OPTIONAL): the
  # actor's data is lost, and it fails cleanly. This used to crash the host:
  # capture retained the external event a raised error.execution had
  # replaced and released, and the image released it again (a double free,
  # tinyactors-core/tests/session_image_test.c).
  [[3, 2, 100], [10, 3, 200]].each do |population, limit, messages|
    scenario "#{population} actors, at most #{limit} resident, #{messages} messages, loaded without their binding" do
      counts = VirtualActorPagingScenarios.run(self, population: population, limit: limit, messages: messages,
                                                     keep_binding: false)
      assert counts[:unbound].positive?, "some actor was loaded without its binding: #{counts}"
    end
  end
end

# The same paging on the ECMAScript data model (SCXML B.2). A binding is a
# value of the data model the image's definition uses, so the test host
# decodes it with that model (ta_session_image_definition), not with jq.
TinyactorsTest.chart :pager_js, datamodel: 'ecmascript' do
  data v: 0
  data n: 0

  state :listening do
    transition on: 'msg', type: :internal do
      assign 'v', "(v * 31 + _event.data.m) % #{VirtualActorPagingScenarios::MODULUS}"
      assign 'n', 'n + 1'
    end
  end
end

TinyactorsTest.scenarios 'virtual actor paging, ECMAScript', chart: :pager_js do
  scenario '100 actors, at most 3 resident, 1000 random messages' do
    counts = VirtualActorPagingScenarios.run(self, population: 100, limit: 3, messages: 1000, chart: :pager_js)
    assert counts[:page_ins] >= 100, "every actor pages in again: #{counts}"
  end

  scenario '10 actors, at most 3 resident, 200 messages, loaded without their binding' do
    counts = VirtualActorPagingScenarios.run(self, population: 10, limit: 3, messages: 200, chart: :pager_js,
                                                   keep_binding: false)
    assert counts[:unbound].positive?, "some actor was loaded without its binding: #{counts}"
  end
end
