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
      return @actors.fetch(name) if @resident.include?(name) # a load answered while evicting brought it in

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

    # Pages out one resident actor, chosen by the seeded random. Core refuses
    # to unload an actor that has input queued (err_busy), as a host that
    # pages out between turns does, so the system runs first: what is queued
    # runs, and the loads it asks for are answered.
    def evict_one
      @context.run_system('settle before paging out')
      name = @resident.delete_at(@random.rand(@resident.size))
      actor = @actors.fetch(name)
      before_evict(actor)
      image = @context.capture(actor)
      binding = actor.data
      @context.core('ta_actor_unload', { result: @context.unload(actor, image) })
      @parked[name] = [image, binding]
      @page_outs += 1
      name
    end

    # Called with the actor about to be paged out, while it is resident.
    def before_evict(actor); end

    # The final data of every actor that received a message, paging each in.
    def final_states
      @actors.keys.sort.to_h { |name| [name, page_in(name).data] }
    end

    # The name of the actor whose current runtime ID is `handle`, or nil.
    def name_of(handle) = @actors.find { |_, actor| actor.handle == handle }&.first

    def resident?(name) = @resident.include?(name)

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
  # The small runs are the fast pass's; the large ones run in test:full.
  [[20, 3, 120, false], [20, 20, 60, false],
   [100, 3, 1000, true], [100, 1, 400, true], [100, 100, 300, true]].each do |population, limit, messages, exhaustive|
    scenario "#{population} actors, at most #{limit} resident, #{messages} random messages", exhaustive: exhaustive do
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
  [[3, 2, 100, false], [10, 3, 200, true]].each do |population, limit, messages, exhaustive|
    scenario "#{population} actors, at most #{limit} resident, #{messages} messages, loaded without their binding",
             exhaustive: exhaustive do
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
  scenario '20 actors, at most 3 resident, 120 random messages' do
    counts = VirtualActorPagingScenarios.run(self, population: 20, limit: 3, messages: 120, chart: :pager_js)
    assert counts[:page_ins] >= 20, "every actor pages in again: #{counts}"
  end

  scenario '100 actors, at most 3 resident, 1000 random messages', exhaustive: true do
    counts = VirtualActorPagingScenarios.run(self, population: 100, limit: 3, messages: 1000, chart: :pager_js)
    assert counts[:page_ins] >= 100, "every actor pages in again: #{counts}"
  end

  scenario '10 actors, at most 3 resident, 200 messages, loaded without their binding', exhaustive: true do
    counts = VirtualActorPagingScenarios.run(self, population: 10, limit: 3, messages: 200, chart: :pager_js,
                                                   keep_binding: false)
    assert counts[:unbound].positive?, "some actor was loaded without its binding: #{counts}"
  end
end

# Delayed sends across paging (docs/manual/virtual-actors.md, "Loading for a
# new message" and the deadline index; ADR 0054; ADR 0038). A delayed send
# is delivered, exactly once and not before it is due, to an actor that was
# paged out and is paged in again:
#
# - its owner is paged out before it comes due: unloading takes the send out
#   of the resident timers and into the image, so it cannot fire while the
#   owner is out. The host indexes each held image's earliest deadline
#   (ta_session_image_next_deadline) and, when logical time reaches it,
#   pages the owner in and lets due sends go out (ta_system_dispatch_due);
# - its target is paged out when it comes due: core keeps the mail on the
#   target's address and reports LOAD_NEEDED (ADR 0099); the host pages the
#   target in, and core moves the mail to it.
#
# Sends between actors go to named addresses (#_actor_<family>/<name>),
# which stay valid across a reload; a runtime ID does not. A send to self
# (no target, or #_scxml_<_sessionid>) names the sender's session, which an
# image keeps (ta_image_rebind_self), so it finds its owner after a reload.
module VirtualActorPagingScenarios
  # A Host that also indexes the deadlines of the images it holds.
  class TimedHost < Host
    attr_reader :deadline_page_ins

    def initialize(context, on_evict: nil, **options)
      super(context, **options)
      @deadlines = {} # name => earliest deadline its held image reports
      @on_evict = on_evict
      @deadline_page_ins = 0
    end

    def owns?(name) = name.start_with?(@prefix) && name.delete_prefix(@prefix).match?(/\A\d+\z/)

    # The earliest pending send of the actor about to be paged out: what its image must report.
    def before_evict(actor)
      @expected_deadline = Array(actor.inspect_actor[:delayed_sends]).map { |send| send[:due] }.min
    end

    def evict_one
      name = super
      deadline = @context.image_deadline(@parked.fetch(name).first)
      deadline.nil? ? @deadlines.delete(name) : @deadlines[name] = deadline
      @on_evict&.call(name, deadline, @expected_deadline)
      name
    end

    def page_in(name)
      @deadlines.delete(name)
      super
    end

    # [name, deadline] of the held image that comes due first, or nil.
    def earliest_deadline = @deadlines.min_by { |_, due| due }

    # Pages in the owner of the earliest held deadline and runs the system at
    # that time, so its due sends go out. False when none is due by `time`.
    def page_in_due(time)
      name, due = earliest_deadline
      return false if due.nil? || due > time

      page_in(name)
      @deadline_page_ins += 1
      @context.advance_time([due - @context.now, 0].max)
      true
    end
  end

  # What every delayed send must do: go out exactly once. The ledger keeps,
  # per target, how many sends were armed for it and the sum of their keys;
  # the actor folds each tick it receives into the same two numbers (`t`,
  # `s`), so a lost or repeated send shows in the final comparison. When a
  # send goes out is core's timer, which the scenarios of delayed_sends.rb
  # and the timer tests check; here it is the paging that is under test.
  class SendLedger
    attr_reader :sends

    def initialize(label)
      @label = label
      @sends = {} # k => {owner:, target:}
    end

    def armed(k, owner:, target:)
      @sends[k] = { owner: owner, target: target }
    end

    # {target name => [ticks, sum of keys]} that every armed send adds up to.
    def expected
      @sends.group_by { |_, send| send[:target] }
            .to_h { |name, pairs| [name, [pairs.size, pairs.sum { |k, _| k }]] }
    end
  end

  # A seeded schedule of messages, delayed sends and time steps, run once
  # with at most `limit` resident and once with everything resident.
  # Messages fold into the state in order; delayed deliveries add up without
  # order, because across senders the host decides it. The host answers
  # core's LOAD_NEEDED for a paged-out target by paging it in, as a host with
  # a loader does, and the ledger checks that every send arrived once.
  #
  # With `self_sends:` the schedule also arms sends to self: with no target,
  # or to #_scxml_<_sessionid>. A send to self whose owner paged out before
  # it came due is in the owner's image; paging the owner in for its
  # deadline sends it.
  def self.run_timed(context, population:, limit:, steps:, chart:, self_sends: false)
    seed = self.seed
    stream = Random.new(seed)
    schedule = Array.new(steps) do
      if self_sends
        case stream.rand(10)
        when 0..2 then [:msg, stream.rand(population), stream.rand(1_000)]
        when 3..4 then [:arm, stream.rand(population), stream.rand(population), 100 * (1 + stream.rand(40))]
        when 5..6 then [:self, stream.rand(population), stream.rand(2), 100 * (1 + stream.rand(40))]
        else [:advance, 100 * (1 + stream.rand(20))]
        end
      else
        case stream.rand(10)
        when 0..3 then [:msg, stream.rand(population), stream.rand(1_000)]
        when 4..6 then [:arm, stream.rand(population), stream.rand(population), 100 * (1 + stream.rand(40))]
        else [:advance, 100 * (1 + stream.rand(20))]
        end
      end
    end
    counter = ResidentCounter.new(context)

    execute = lambda do |host, ledger, label, bound|
      counts = { owner_deadlines: 0, target_page_ins: 0 }
      next_k = 0
      # The host's loader: a paged-out target comes in when core asks for it.
      context.on_load_needed do |_, notice|
        name = notice[:address][:name]
        context.assert host.owns?(name), "a LOAD_NEEDED for #{name.inspect} belongs to this run"
        before = host.page_ins
        host.page_in(name)
        counts[:target_page_ins] += 1 if host.page_ins > before
      end
      after = lambda do |at, what|
        resident = counter.count
        context.assert bound.nil? || resident <= bound,
                       "#{label}: seed #{seed}, step #{at} (#{what}): #{resident} actors resident, limit #{bound} " \
                       "(replay with TAT_PAGING_SEED=#{seed})"
      end
      advance_to = lambda do |time, at|
        while host.respond_to?(:page_in_due) && host.page_in_due(time)
          counts[:owner_deadlines] += 1
          after.call(at, 'a held deadline came due')
        end
        context.advance_time(time - context.now) if time > context.now
      end
      schedule.each_with_index do |(op, a, b, c), at|
        case op
        when :msg
          host.deliver(a, b)
        when :arm
          next_k += 1
          owner = host.name(a)
          target = host.name(b)
          host.page_in(owner).deliver('arm', data: { to: "#_actor_#{context.family(chart)}/#{target}",
                                                     delay: "#{c}ms", k: next_k })
          ledger.armed(next_k, owner: owner, target: target)
        when :self
          next_k += 1
          owner = host.name(a)
          host.page_in(owner).deliver(b.zero? ? 'self_arm' : 'session_arm', data: { delay: "#{c}ms", k: next_k })
          ledger.armed(next_k, owner: owner, target: owner)
        when :advance
          advance_to.call(context.now + a, at)
        end
        after.call(at, op)
      end
      # Drain: every send still pending, resident or in a held image, comes due.
      10_000.times do
        due = [host.respond_to?(:earliest_deadline) ? host.earliest_deadline&.last : nil, context.next_deadline].compact.min
        break if due.nil?

        advance_to.call([due, context.now].max, steps)
        after.call(steps, 'drain')
      end
      counts
    end

    ledger = SendLedger.new('paged')
    on_evict = lambda do |name, deadline, expected|
      context.assert_equal expected, deadline,
                           "seed #{seed}: #{name}'s image holds its pending sends (replay with TAT_PAGING_SEED=#{seed})"
    end
    paged = TimedHost.new(context, limit: limit, random: Random.new(seed + 1), prefix: 'paged', chart: chart,
                                   on_evict: on_evict)
    counts = execute.call(paged, ledger, 'paged', limit)
    got = paged.final_states.transform_keys { |name| name.delete_prefix('paged') }
    ledger.expected.each do |name, (ticks, sum)|
      state = got[name.delete_prefix('paged')]
      context.assert_equal [ticks, sum], [state[:t], state[:s]],
                           "seed #{seed}, actor #{name}: every send arrived once (replay with TAT_PAGING_SEED=#{seed})"
    end
    paged.release_parked

    reference_ledger = SendLedger.new('reference')
    reference = TimedHost.new(context, limit: population * 2, random: Random.new(0), prefix: 'all', chart: chart)
    execute.call(reference, reference_ledger, 'reference', nil)
    want = reference.final_states.transform_keys { |name| name.delete_prefix('all') }
    context.assert_equal 0, reference.page_outs, 'the reference run never evicts'

    context.assert_equal want.keys, got.keys, "seed #{seed}: the same actors were used"
    want.each do |name, state|
      context.assert_equal state, got[name],
                           "seed #{seed}, actor #{name}: paged state differs from the all-resident run " \
                           "(replay with TAT_PAGING_SEED=#{seed})"
    end
    counts.merge(page_ins: paged.page_ins, page_outs: paged.page_outs, sends: ledger.sends.size,
                 self_sends: ledger.sends.count { |_, send| send[:owner] == send[:target] })
  end
end

TinyactorsTest.chart :timed_pager do
  data v: 0
  data n: 0
  data t: 0
  data s: 0

  state :listening do
    transition on: 'msg', type: :internal do
      assign '.v', "(.v * 31 + $_event.data.m) % #{VirtualActorPagingScenarios::MODULUS}"
      assign '.n', '.n + 1'
    end
    transition on: 'arm', type: :internal do
      send_event 'tick', to_expr: '$_event.data.to', delay_expr: '$_event.data.delay', data: '{k: $_event.data.k}'
    end
    transition on: 'self_arm', type: :internal do
      send_event 'tick', delay_expr: '$_event.data.delay', data: '{k: $_event.data.k}'
    end
    transition on: 'session_arm', type: :internal do
      send_event 'tick', to_expr: '"#_scxml_\($_sessionid)"', delay_expr: '$_event.data.delay',
                         data: '{k: $_event.data.k}'
    end
    transition on: 'tick', type: :internal do
      assign '.s', "(.s + $_event.data.k) % #{VirtualActorPagingScenarios::MODULUS}"
      assign '.t', '.t + 1'
    end
  end
end

TinyactorsTest.chart :timed_pager_js, datamodel: 'ecmascript' do
  data v: 0
  data n: 0
  data t: 0
  data s: 0

  state :listening do
    transition on: 'msg', type: :internal do
      assign 'v', "(v * 31 + _event.data.m) % #{VirtualActorPagingScenarios::MODULUS}"
      assign 'n', 'n + 1'
    end
    transition on: 'arm', type: :internal do
      send_event 'tick', to_expr: '_event.data.to', delay_expr: '_event.data.delay', data: '({k: _event.data.k})'
    end
    transition on: 'self_arm', type: :internal do
      send_event 'tick', delay_expr: '_event.data.delay', data: '({k: _event.data.k})'
    end
    transition on: 'session_arm', type: :internal do
      send_event 'tick', to_expr: "'#_scxml_' + _sessionid", delay_expr: '_event.data.delay',
                         data: '({k: _event.data.k})'
    end
    transition on: 'tick', type: :internal do
      assign 's', "(s + _event.data.k) % #{VirtualActorPagingScenarios::MODULUS}"
      assign 't', 't + 1'
    end
  end
end

TinyactorsTest.scenarios 'virtual actor paging, delayed sends', chart: :timed_pager do
  # The small runs are the fast pass's; the large ones run in test:full.
  [[:timed_pager, 8, 2, 80, false], [:timed_pager, 8, 8, 60, false], [:timed_pager_js, 8, 2, 80, false],
   [:timed_pager, 20, 3, 300, true], [:timed_pager, 20, 1, 200, true], [:timed_pager, 20, 20, 200, true],
   [:timed_pager_js, 20, 3, 300, true]].each do |chart, population, limit, steps, exhaustive|
    scenario "#{chart}: #{population} actors, at most #{limit} resident, #{steps} steps with delayed sends",
             exhaustive: exhaustive do
      counts = VirtualActorPagingScenarios.run_timed(self, population: population, limit: limit, steps: steps,
                                                           chart: chart)
      warn "  #{counts}" if ENV['TAT_PAGING_VERBOSE']
      assert counts[:sends].positive?, "the schedule armed delayed sends: #{counts}"
      if limit >= population
        assert_equal 0, counts[:page_outs], 'with room for every actor nothing is evicted'
      else
        assert counts[:owner_deadlines].positive?, "an owner was paged in for its own deadline: #{counts}"
        assert counts[:target_page_ins].positive?, "a target was paged in to receive a send: #{counts}"
      end
    end
  end

  [[:timed_pager, 8, 2, 80, false], [:timed_pager_js, 8, 2, 80, false],
   [:timed_pager, 20, 3, 300, true], [:timed_pager, 20, 1, 200, true],
   [:timed_pager_js, 20, 3, 300, true]].each do |chart, population, limit, steps, exhaustive|
    scenario "#{chart}: #{population} actors, at most #{limit} resident, #{steps} steps with sends to self",
             exhaustive: exhaustive do
      counts = VirtualActorPagingScenarios.run_timed(self, population: population, limit: limit, steps: steps,
                                                           chart: chart, self_sends: true)
      warn "  #{counts}" if ENV['TAT_PAGING_VERBOSE']
      assert counts[:self_sends].positive?, "the schedule armed sends to self: #{counts}"
      assert counts[:owner_deadlines].positive?, "an owner was paged in for its own deadline: #{counts}"
    end
  end

  # A due send to an unloaded target. Core keeps it on the target's address
  # and tells the host (LOAD_NEEDED) once; the host here answers later. The
  # send has left its owner: paging the owner out meanwhile leaves nothing
  # of it in the owner's image. Paging the target in moves the mail, and it
  # arrives exactly once.
  scenario 'a send to an unloaded target waits in core until the target pages in' do
    notices = []
    on_load_needed { |_, notice| notices << notice }
    host = VirtualActorPagingScenarios::TimedHost.new(self, limit: 1, random: Random.new(0), prefix: 'r',
                                                            chart: :timed_pager)
    host.page_in('r1')
    owner = host.page_in('r0') # evicts r1: the target is unloaded
    owner.deliver('arm', data: { to: "#_actor_#{family(:timed_pager)}/r1", delay: '1000ms', k: 1 })
    advance_time(1_000)
    assert_equal ['r1'], notices.map { |notice| notice[:address][:name] }, 'core asked the host once'
    assert_equal 1, address_waiting(notices.first[:address]), 'the send waits on the target\'s address'
    assert_equal [], Array(owner.inspect_actor[:delayed_sends]), 'it is not the owner\'s any more'

    host.page_in('r1') # evicts the owner while the send waits
    assert_equal nil, host.earliest_deadline, 'the owner\'s image holds no send: it was sent'
    run_system('the target is resident')
    assert_equal 0, address_waiting(notices.first[:address]), 'core moved the mail when the target loaded'
    assert_equal 1, host.page_in('r1').data('.t'), 'the target received the send exactly once'
    assert_equal ['r1'], notices.map { |notice| notice[:address][:name] }, 'and was asked about once'
    host.release_parked
  end
end

# Delayed sends to a session across paging, one case at a time (ADR 0066).
# A session's ID is the runtime ID it was spawned with, and it outlives
# residency: loading keeps it, so _sessionid is constant for the life of the
# session. A send to self (no target) and a send to #_scxml_<_sessionid>
# both name the session by that ID, which no reload changes. When the send
# falls due, core resolves a session ID that is not a resident runtime ID
# through its session table: to the reloaded session's runtime ID, or, while
# the session is unloaded, to its address, so the host is asked to page it
# in (LOAD_NEEDED). A session that no longer exists stays unreachable:
# error.communication.
TinyactorsTest.chart :self_pager do
  data t: 0
  data k: []
  data origin: ''
  data sid: ''
  data errs: []
  data after: 0

  state :listening do
    transition on: 'self_arm', type: :internal do
      send_event 'tick', delay_expr: '$_event.data.delay', data: '{k: $_event.data.k}'
    end
    transition on: 'session_arm', type: :internal do
      send_event 'tick', to_expr: '"#_scxml_\($_sessionid)"', delay_expr: '$_event.data.delay',
                         data: '{k: $_event.data.k}'
    end
    transition on: 'to_arm', type: :internal do
      send_event 'tick', to_expr: '$_event.data.to', delay_expr: '$_event.data.delay', data: '{k: $_event.data.k}'
    end
    # The send executes, then the assign: an action budget of 1 pauses between.
    transition on: 'self_arm_then', type: :internal do
      send_event 'tick', delay_expr: '$_event.data.delay', data: '{k: $_event.data.k}'
      assign '.after', '.after + 1'
    end
    transition on: 'internal_arm', type: :internal do
      send_event 'tick', to: '#_internal', delay: 100, data: '{k: 99}'
    end
    transition on: 'remember', type: :internal do
      assign '.sid', '$_sessionid'
    end
    transition on: 'tick', type: :internal do
      assign '.t', '.t + 1'
      assign '.k', '.k + [$_event.data.k]'
      assign '.origin', '$_event.origin'
    end
    transition on: 'error.*', type: :internal do
      assign '.errs', '.errs + [$_event.name]'
    end
  end
end

TinyactorsTest.chart :self_pager_js, datamodel: 'ecmascript' do
  data t: 0
  data k: []
  data origin: ''

  state :listening do
    transition on: 'self_arm', type: :internal do
      send_event 'tick', delay_expr: '_event.data.delay', data: '({k: _event.data.k})'
    end
    transition on: 'session_arm', type: :internal do
      send_event 'tick', to_expr: "'#_scxml_' + _sessionid", delay_expr: '_event.data.delay',
                         data: '({k: _event.data.k})'
    end
    transition on: 'tick', type: :internal do
      assign 't', 't + 1'
      assign 'k', 'k.concat([_event.data.k])'
      assign 'origin', '_event.origin'
    end
  end
end

module VirtualActorPagingScenarios
  # The runtime IDs an actor's pending delayed sends go to.
  def self.send_destinations(actor)
    Array(actor.inspect_actor[:delayed_sends]).map { |send| send.dig(:destination, :runtime) }
  end
end

TinyactorsTest.scenarios 'virtual actor paging, sends to self', chart: :self_pager do
  %i[self_pager self_pager_js].each do |chart|
    scenario "#{chart}: a send to self reaches its owner after three reloads" do
      owner = spawn chart, name: 'owner'
      owner.deliver 'self_arm', data: { delay: '1000ms', k: 1 }
      owner.deliver 'session_arm', data: { delay: '1000ms', k: 2 }
      ids = [owner.handle]
      session = owner.inspect_actor[:session_id]
      assert_equal owner.handle, session, 'a fresh session is named by its runtime ID'
      3.times do
        cycle_residency(owner)
        ids << owner.handle
        assert_equal session, owner.inspect_actor[:session_id], 'a reload keeps the session ID'
        assert_equal [session] * 2, VirtualActorPagingScenarios.send_destinations(owner),
                     'both sends to self name the session, which a reload does not change'
      end
      assert_equal ids.uniq, ids, "every reload gives a new runtime ID: #{ids}"
      advance_time(1000)
      assert_equal 2, owner.data('.t'), 'each send to self is delivered once'
      assert_equal [1, 2], owner.data('.k')
      assert_equal "#_actor_#{family(chart)}/owner", owner.data('.origin'),
                   'the origin is the owner\'s address'
      assert_equal [], load_notices, 'no delivery went stale: core always found the owner'
    end
  end

  %i[self_pager self_pager_js].each do |chart|
    scenario "#{chart}: _sessionid is constant across reloads" do
      actor = spawn chart, name: 'a'
      first = actor.handle
      session = actor.inspect_actor[:session_id]
      assert_equal first, session, 'a fresh session is named by its runtime ID'
      2.times do
        cycle_residency(actor)
        assert first != actor.handle, 'a reload gives a new runtime ID'
        assert_equal session, actor.inspect_actor[:session_id], 'but keeps the session ID'
      end
      actor.deliver 'session_arm', data: { delay: '10ms', k: 3 }
      assert_equal [session], VirtualActorPagingScenarios.send_destinations(actor),
                   '#_scxml_<_sessionid> names the first runtime ID: the session'
      advance_time(10)
      assert_equal [3], actor.data('.k'), '#_scxml_<_sessionid> reaches the reloaded session'
    end
  end

  scenario '_sessionid reads the same text before and after a reload' do
    actor = spawn :self_pager, name: 'a'
    actor.deliver 'remember'
    before = actor.data('.sid').to_s
    assert_equal actor.session_text, before
    cycle_residency(actor)
    actor.deliver 'remember'
    assert_equal before, actor.data('.sid').to_s, 'after a reload _sessionid is unchanged'
  end

  scenario 'a delayed send to #_internal is error.execution and schedules nothing' do
    actor = spawn :self_pager, name: 'a'
    actor.deliver 'internal_arm'
    assert_equal ['error.execution'], actor.data('.errs')
    assert_equal [], VirtualActorPagingScenarios.send_destinations(actor)
    advance_time(1000)
    assert_equal 0, actor.data('.t')
  end

  scenario 'a send to another session reaches it after it reloads' do
    sender = spawn :self_pager, name: 'sender'
    target = spawn :self_pager, name: 'target'
    session = target.inspect_actor[:session_id]
    sender.deliver 'to_arm', data: { to: "#_scxml_#{session}", delay: '100ms', k: 4 }
    cycle_residency(target)
    cycle_residency(sender)
    assert_equal [session], VirtualActorPagingScenarios.send_destinations(sender),
                 'the send names the target session, which its reload did not change'
    advance_time(100)
    assert_equal 1, target.data('.t'), 'the reloaded target session receives the send once'
    assert_equal [], sender.data('.errs')
    assert_equal [], load_notices, 'core resolved the session to its current runtime ID: no load was needed'
  end

  scenario 'a send to an unloaded session goes by its address, so the host pages it in' do
    notices = []
    on_load_needed { |_, notice| notices << notice }
    sender = spawn :self_pager, name: 'sender'
    target = spawn :self_pager, name: 'target'
    session = target.inspect_actor[:session_id]
    sender.deliver 'to_arm', data: { to: "#_scxml_#{session}", delay: '100ms', k: 6 }
    image = capture(target)
    binding = target.data
    core('ta_actor_unload', { result: unload(target, image) })
    advance_time(100)
    assert_equal [['self_pager', 'target']], notices.map { |notice| [notice[:address].dig(:family, :name), notice[:address][:name]] },
                 'the unloaded session is addressed by name, and the host is asked once'
    assert_equal 1, address_waiting(notices.first[:address])
    core('ta_actor_load', { result: load(target, image, binding: binding) })
    destroy_image(image)
    assert_equal session, target.inspect_actor[:session_id], 'the paged-in actor continues the session'
    run_system('after paging the target in')
    assert_equal [6], target.data('.k'), 'the paged-in session receives the send once'
  end

  scenario 'a send to a session that ended is error.communication' do
    sender = spawn :self_pager, name: 'sender'
    target = spawn :self_pager, name: 'target'
    session = target.inspect_actor[:session_id]
    sender.deliver 'to_arm', data: { to: "#_scxml_#{session}", delay: '100ms', k: 4 }
    target.destroy
    advance_time(100)
    assert_equal ['error.communication'], sender.data('.errs'), 'the sender learns the session is gone'
    assert_equal [], Array(load_notices.reject { |notice| notice[:session] }), 'only the gone session\'s key was asked about'
  end

  scenario 'a send to self captured while paused mid-microstep reaches the reloaded actor' do
    actor = spawn :self_pager, name: 'a'
    assert_equal 'ok', actor.post('self_arm_then', data: { delay: '100ms', k: 5 })
    assert_equal 'paused', actor.resume(budget: 1000, action_budget: 1)
    assert actor.inspect_actor[:microstep_in_progress], 'paused between the send and the assign'
    old = actor.handle
    cycle_residency(actor)
    assert old != actor.handle, 'a reload gives a new runtime ID'
    assert_equal [old], VirtualActorPagingScenarios.send_destinations(actor), 'the send to self names the session'
    nil while actor.resume(budget: 1000, action_budget: 1) == 'paused'
    assert_equal 1, actor.data('.after'), 'the rest of the microstep ran once'
    advance_time(100)
    assert_equal [5], actor.data('.k'), 'the send to self is delivered once'
  end
end
