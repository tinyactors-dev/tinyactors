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
      name
    end

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
# - its target is paged out when it comes due: the router holds the named
#   delivery, and the host pages the target in and releases it.
#
# Sends between actors go to named addresses (#_actor_<family>/<name>),
# which stay valid across a reload; a runtime ID does not. A send to self
# (no target, or #_scxml_<_sessionid>) names the sender's runtime ID, and
# an image rebinds it to the reloaded session (ta_image_rebind_self); one
# the router refused names its owner's address once the owner unloads.
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

    def evict_one
      name = super
      deadline = @context.image_deadline(@parked.fetch(name).first)
      deadline.nil? ? @deadlines.delete(name) : @deadlines[name] = deadline
      @on_evict&.call(name, deadline)
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

  # What every delayed send must do: go out once, not before it is due.
  class SendLedger
    attr_reader :sends

    def initialize(context, label)
      @context = context
      @label = label
      @sends = {} # k => {owner:, target:, due:, delivered_at:}
    end

    def armed(k, owner:, target:, due:, route: :named)
      @sends[k] = { owner: owner, target: target, due: due, route: route, delivered_at: nil }
    end

    def delivered(k, target)
      send = @sends.fetch(k) { raise TinyactorsTest::AssertionFailed, "#{@label}: delivery of unknown send #{k}" }
      @context.assert send[:delivered_at].nil?, "#{@label}: send #{k} delivered twice"
      @context.assert_equal send[:target], target, "#{@label}: send #{k} goes to its own target"
      @context.assert @context.now >= send[:due],
                      "#{@label}: send #{k} delivered at t=#{@context.now}ms, before it is due at #{send[:due]}ms"
      # Across senders the host decides the order (docs/internal/concurrency.md);
      # from one owner to one target by one route, it is (due, send).
      earlier = @sends.select do |other_k, other|
        other_k != k && other[:delivered_at].nil? && other.values_at(:owner, :target, :route) ==
          send.values_at(:owner, :target, :route) && ([other[:due], other_k] <=> [send[:due], k]).negative?
      end
      @context.assert earlier.empty?, "#{@label}: send #{k} delivered before #{earlier.keys} of the same owner, target and route"
      send[:delivered_at] = @context.now
    end

    # The earliest due time of `owner`'s sends that are neither delivered nor
    # held by the router: what the owner's image must report.
    def owner_deadline(owner, held)
      @sends.filter_map { |k, send| send[:due] if send[:owner] == owner && send[:delivered_at].nil? && !held.include?(k) }.min
    end

    def undelivered = @sends.reject { |_, send| send[:delivered_at] }.keys
  end

  # Releases every delivery the router holds, paging each named destination
  # in first; runs the system after each, which may hold more. Returns the
  # number of releases that had to page their target in.
  def self.pump(context, router, host, ledger)
    target_page_ins = 0
    loop do
      held = router.held
      break if held.empty?

      # A send to self names its owner's runtime ID. Its owner was resident
      # when it went out, so release it before a named delivery pages
      # something in and perhaps evicts that owner.
      delivery = held.find { |held_delivery| held_delivery.dig(:destination, :kind) == 'runtime' } || held.first
      if delivery.dig(:destination, :kind) == 'runtime'
        id = delivery.dig(:destination, :runtime)
        name = host.name_of(id)
        context.assert !name.nil? && host.resident?(name),
                       "a send to self goes to its owner's current runtime ID, not a stale one (#{id})"
      else
        name = delivery.dig(:destination, :name)
        context.assert host.owns?(name), "a held delivery for #{name.inspect} belongs to this run"
        before = host.page_ins
        host.page_in(name)
        target_page_ins += 1 if host.page_ins > before
      end
      released = router.release(delivery.fetch(:id))
      context.assert_equal 'ok', released[:result], "#{name}: a released delivery reaches the resident actor"
      ledger.delivered(delivery.dig(:event, :data, :k), name)
      context.run_system("pump #{name}")
    end
    target_page_ins
  end

  # The ks of the tick deliveries the router holds.
  def self.held_ks(router) = router.held.map { |delivery| delivery.dig(:event, :data, :k) }

  # A seeded schedule of messages, delayed sends and time steps, run once
  # with at most `limit` resident and once with everything resident.
  # Messages fold into the state in order; delayed deliveries add up without
  # order, because across senders the host decides it. The ledger checks
  # the order that is specified: one owner to one target by one route.
  #
  # With `self_sends:` the schedule also arms sends to self: with no target,
  # or to #_scxml_<_sessionid>. Refused submissions are mixed in as for the
  # other sends: a refused send to self whose owner pages out before the
  # retry names its owner's address from then on (ADR 0054), so the retry
  # pages the owner in like any send to a paged-out actor.
  def self.run_timed(context, population:, limit:, steps:, chart:, self_sends: false)
    seed = self.seed
    stream = Random.new(seed)
    schedule = Array.new(steps) do
      if self_sends
        case stream.rand(10)
        when 0..2 then [:msg, stream.rand(population), stream.rand(1_000)]
        when 3..4 then [:arm, stream.rand(population), stream.rand(population), 100 * (1 + stream.rand(40))]
        when 5..6 then [:self, stream.rand(population), stream.rand(2), 100 * (1 + stream.rand(40))]
        else [:advance, 100 * (1 + stream.rand(20)), stream.rand(4).zero?]
        end
      else
        case stream.rand(10)
        when 0..3 then [:msg, stream.rand(population), stream.rand(1_000)]
        when 4..6 then [:arm, stream.rand(population), stream.rand(population), 100 * (1 + stream.rand(40))]
        else [:advance, 100 * (1 + stream.rand(20)), stream.rand(4).zero?]
        end
      end
    end
    router = context.recording_router(mode: :hold, directory: true)
    counter = ResidentCounter.new(context)

    execute = lambda do |host, ledger, label, bound|
      counts = { owner_deadlines: 0, target_page_ins: 0, refused: 0 }
      next_k = 0
      after = lambda do |at, what|
        counts[:target_page_ins] += pump(context, router, host, ledger)
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
          ledger.armed(next_k, owner: owner, target: target, due: context.now + c)
        when :self
          next_k += 1
          owner = host.name(a)
          host.page_in(owner).deliver(b.zero? ? 'self_arm' : 'session_arm', data: { delay: "#{c}ms", k: next_k })
          ledger.armed(next_k, owner: owner, target: owner, due: context.now + c, route: :self)
        when :advance
          if b
            router.arm_fault(:submit, status: 'retry', count: 1)
            counts[:refused] += 1
          end
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
      router.clear_faults
      context.assert_equal [], ledger.undelivered,
                           "#{label}: seed #{seed}: every delayed send is delivered (replay with TAT_PAGING_SEED=#{seed})"
      counts
    end

    ledger = SendLedger.new(context, 'paged')
    on_evict = lambda do |name, deadline|
      expected = ledger.owner_deadline(name, held_ks(router))
      context.assert_equal expected, deadline,
                           "seed #{seed}: #{name}'s image holds its pending sends (replay with TAT_PAGING_SEED=#{seed})"
    end
    paged = TimedHost.new(context, limit: limit, random: Random.new(seed + 1), prefix: 'paged', chart: chart,
                                   on_evict: on_evict)
    counts = execute.call(paged, ledger, 'paged', limit)
    got = paged.final_states.transform_keys { |name| name.delete_prefix('paged') }
    paged.release_parked

    reference_ledger = SendLedger.new(context, 'reference')
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
                 self_sends: ledger.sends.count { |_, send| send[:owner] == send[:target] },
                 refusals: router.journal.count { |entry| entry[:op] == 'submit' && entry[:status] == 'retry' })
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
      assert counts[:refusals].positive?, "the router refused some submissions once: #{counts}"
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

  # ADR 0054's retry path for an unloaded target. The router refuses the
  # due send once: it goes back to its owner, submitted, and stays due. The
  # host pages the target in, evicting the owner; the owner's image does not
  # hold the submitted send (it moved to the system), and the retry reaches
  # the paged-in target exactly once.
  scenario 'a send refused for an unloaded target is retried after the target pages in' do
    router = recording_router(mode: :hold, directory: true)
    host = VirtualActorPagingScenarios::TimedHost.new(self, limit: 1, random: Random.new(0), prefix: 'r',
                                                            chart: :timed_pager)
    host.page_in('r1')
    owner = host.page_in('r0') # evicts r1: the target is unloaded
    owner.deliver('arm', data: { to: "#_actor_#{family(:timed_pager)}/r1", delay: '1000ms', k: 1 })
    router.arm_fault(:submit, status: 'retry', count: 1)
    set_time(now + 1_000)
    due = core('ta_system_dispatch_due', ta.system.dispatch_due(system: system_handle, budget: 1))
    assert_equal [0, true], due.values_at(:dispatched, :more_due), 'the router refused the due send'
    pending = Array(owner.inspect_actor[:delayed_sends])
    assert_equal [true], pending.map { |send| send[:submitted] }, 'the refused send is back with its owner, submitted'

    host.page_in('r1') # evicts the owner while its send is submitted
    assert_equal nil, host.earliest_deadline, 'the owner\'s image holds no submitted send: it moved to the system'
    run_system('retry the refused send')
    assert_equal 1, router.held.size, 'the retry reached the router'
    ledger = VirtualActorPagingScenarios::SendLedger.new(self, 'refused')
    ledger.armed(1, owner: 'r0', target: 'r1', due: now)
    VirtualActorPagingScenarios.pump(self, router, host, ledger)
    assert_equal 1, host.page_in('r1').data('.t'), 'the target received the send exactly once'
    submits = router.journal.select { |entry| entry[:op] == 'submit' }.map { |entry| entry[:status] }
    assert_equal %w[retry accepted], submits, 'refused once, then held for the host'
    host.release_parked
  end
end

# Delayed sends to a session across paging, one case at a time (ADR 0066).
# A session's ID is the runtime ID it was spawned with, and it outlives
# residency: loading keeps it, so _sessionid is constant for the life of the
# session. A send to self (no target) and a send to #_scxml_<_sessionid>
# both name the session by that ID, which no reload changes. When the send
# is submitted, core resolves a session ID that is not a resident runtime
# ID through its session table: to the reloaded session's runtime ID, or,
# while the session is unloaded, to its address, so the host pages it in.
# A session that no longer exists stays unreachable: error.communication.
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
      router = recording_router(mode: :direct, directory: true)
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
      assert_equal [], router.journal.select { |entry| entry[:result] == 'err_not_found' }, 'no delivery went stale'
    end
  end

  %i[self_pager self_pager_js].each do |chart|
    scenario "#{chart}: _sessionid is constant across reloads" do
      recording_router(mode: :direct, directory: true)
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
    recording_router(mode: :direct, directory: true)
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
    router = recording_router(mode: :direct, directory: true)
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
    submits = router.journal.select { |entry| entry[:op] == 'submit' }
    assert_equal [{ kind: 'runtime', runtime: target.handle }], submits.map { |entry| entry[:destination] },
                 'core resolved the session to its current runtime ID'
  end

  scenario 'a send to an unloaded session goes by its address, so the host pages it in' do
    router = recording_router(mode: :hold, directory: true)
    sender = spawn :self_pager, name: 'sender'
    target = spawn :self_pager, name: 'target'
    session = target.inspect_actor[:session_id]
    sender.deliver 'to_arm', data: { to: "#_scxml_#{session}", delay: '100ms', k: 6 }
    image = capture(target)
    binding = target.data
    core('ta_actor_unload', { result: unload(target, image) })
    set_time(now + 100)
    core('ta_system_dispatch_due', ta.system.dispatch_due(system: system_handle, budget: 1))
    submit = router.journal.find { |entry| entry[:op] == 'submit' }
    assert_equal 'named', submit.dig(:destination, :kind), 'the unloaded session is addressed by name'
    assert_equal 'target', submit.dig(:destination, :name)
    core('ta_actor_load', { result: load(target, image, binding: binding) })
    destroy_image(image)
    assert_equal session, target.inspect_actor[:session_id], 'the paged-in actor continues the session'
    router.held.each { |delivery| router.release(delivery.fetch(:id)) }
    run_system('after paging the target in')
    assert_equal [6], target.data('.k'), 'the paged-in session receives the send once'
  end

  scenario 'a send to a session that ended is error.communication' do
    router = recording_router(mode: :direct, directory: true)
    sender = spawn :self_pager, name: 'sender'
    target = spawn :self_pager, name: 'target'
    session = target.inspect_actor[:session_id]
    sender.deliver 'to_arm', data: { to: "#_scxml_#{session}", delay: '100ms', k: 4 }
    target.destroy
    advance_time(100)
    assert_equal ['error.communication'], sender.data('.errs'), 'the sender learns the session is gone'
    assert_equal %w[err_not_found], router.journal.select { |entry| entry[:op] == 'submit' }.map { |entry| entry[:result] }
  end

  scenario 'a send to self captured while paused mid-microstep reaches the reloaded actor' do
    recording_router(mode: :direct, directory: true)
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

  # A send to self that the router refuses is submitted: it stays with its
  # owner, not in the owner's image (ADR 0054). When the owner pages out
  # before the retry, the send moves to the system's list. It names the
  # session by its session ID, so its retry reaches the reloaded owner
  # (ADR 0066).
  scenario 'a refused send to self survives its owner paging out' do
    router = recording_router(mode: :hold, directory: true)
    owner = spawn :self_pager, name: 'a'
    owner.deliver 'self_arm', data: { delay: '100ms', k: 7 }
    router.arm_fault(:submit, status: 'retry', count: 1)
    set_time(now + 100)
    core('ta_system_dispatch_due', ta.system.dispatch_due(system: system_handle, budget: 1))
    cycle_residency(owner)
    run_system('retry the refused send')
    router.held.each { |delivery| router.release(delivery.fetch(:id)) }
    run_system('after the retry')
    assert_equal [7], owner.data('.k'), 'the reloaded owner receives its refused send to self'
  end
end
