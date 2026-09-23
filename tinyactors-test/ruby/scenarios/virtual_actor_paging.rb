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
# Sends go to named addresses (#_actor_<family>/<name>), which stay valid
# across a reload; a runtime ID does not.
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

    def armed(k, owner:, target:, due:) = @sends[k] = { owner: owner, target: target, due: due, delivered_at: nil }

    def delivered(k, target)
      send = @sends.fetch(k) { raise TinyactorsTest::AssertionFailed, "#{@label}: delivery of unknown send #{k}" }
      @context.assert send[:delivered_at].nil?, "#{@label}: send #{k} delivered twice"
      @context.assert_equal send[:target], target, "#{@label}: send #{k} goes to its own target"
      @context.assert @context.now >= send[:due],
                      "#{@label}: send #{k} delivered at t=#{@context.now}ms, before it is due at #{send[:due]}ms"
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

      delivery = held.first
      name = delivery.dig(:destination, :name)
      context.assert host.owns?(name), "a held delivery for #{name.inspect} belongs to this run"
      before = host.page_ins
      host.page_in(name)
      target_page_ins += 1 if host.page_ins > before
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
  def self.run_timed(context, population:, limit:, steps:, chart:)
    seed = self.seed
    stream = Random.new(seed)
    schedule = Array.new(steps) do
      case stream.rand(10)
      when 0..3 then [:msg, stream.rand(population), stream.rand(1_000)]
      when 4..6 then [:arm, stream.rand(population), stream.rand(population), 100 * (1 + stream.rand(40))]
      else [:advance, 100 * (1 + stream.rand(20)), stream.rand(4).zero?]
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
                 refusals: router.journal.count { |entry| entry[:op] == 'submit' && entry[:status] == 'retry' })
  end
end

TinyactorsTest.chart :timed_pager do
  data v: 0
  data n: 0
  data t: 0

  state :listening do
    transition on: 'msg', type: :internal do
      assign '.v', "(.v * 31 + $_event.data.m) % #{VirtualActorPagingScenarios::MODULUS}"
      assign '.n', '.n + 1'
    end
    transition on: 'arm', type: :internal do
      send_event 'tick', to_expr: '$_event.data.to', delay_expr: '$_event.data.delay', data: '{k: $_event.data.k}'
    end
    transition on: 'tick', type: :internal do
      assign '.v', "(.v * 37 + $_event.data.k + 7) % #{VirtualActorPagingScenarios::MODULUS}"
      assign '.t', '.t + 1'
    end
  end
end

TinyactorsTest.chart :timed_pager_js, datamodel: 'ecmascript' do
  data v: 0
  data n: 0
  data t: 0

  state :listening do
    transition on: 'msg', type: :internal do
      assign 'v', "(v * 31 + _event.data.m) % #{VirtualActorPagingScenarios::MODULUS}"
      assign 'n', 'n + 1'
    end
    transition on: 'arm', type: :internal do
      send_event 'tick', to_expr: '_event.data.to', delay_expr: '_event.data.delay', data: '({k: _event.data.k})'
    end
    transition on: 'tick', type: :internal do
      assign 'v', "(v * 37 + _event.data.k + 7) % #{VirtualActorPagingScenarios::MODULUS}"
      assign 't', 't + 1'
    end
  end
end

TinyactorsTest.scenarios 'virtual actor paging, delayed sends', chart: :timed_pager do
  [[:timed_pager, 20, 3, 300], [:timed_pager, 20, 1, 200], [:timed_pager, 20, 20, 200],
   [:timed_pager_js, 20, 3, 300]].each do |chart, population, limit, steps|
    scenario "#{chart}: #{population} actors, at most #{limit} resident, #{steps} steps with delayed sends" do
      counts = VirtualActorPagingScenarios.run_timed(self, population: population, limit: limit, steps: steps,
                                                           chart: chart)
      warn "  #{counts}" if ENV["TAT_PAGING_VERBOSE"]
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
