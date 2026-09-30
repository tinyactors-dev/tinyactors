# frozen_string_literal: true

# Hosted mode (kernel ADR 0001 section 5): systems run by the host kernel
# (tinyactors-host) instead of by core calls a scenario makes one by one.
# The kernel queues, schedules and routes; the scenario runs turns and owns
# the clock.
TinyactorsTest.chart :hosted_ping do
  state :idle do
    transition on: 'go', target: :sent do
      send_event 'ping', to: '#_actor_hosted_pong/p'
    end
  end
  state :sent
end

TinyactorsTest.chart :hosted_pong do
  state :waiting do
    transition on: 'ping', target: :pinged
  end
  state :pinged
end

TinyactorsTest.chart :hosted_timer do
  state :idle do
    transition on: 'arm', target: :armed do
      send_event 'ring', delay: '5s'
    end
  end
  state :armed do
    transition on: 'ring', target: :rung
  end
  state :rung
end

TinyactorsTest.chart :hosted_box do
  data size: 0

  state :idle do
    transition on: 'put' do
      assign '.size', '$_event.data.blob | length'
    end
  end
end

TinyactorsTest.chart :hosted_seen do
  data seen: 0

  state :idle do
    transition on: 'e1', target: :waiting do
      assign '.seen', '.seen + 1'
      send_event 'timeout', delay: '5s'
    end
  end
  state :waiting do
    transition on: 'timeout', target: :timedout
  end
  state :timedout
end

TinyactorsTest.chart :hosted_stopper do
  state :running do
    transition on: 'stop', target: :stopped
    transition on: 'tick'
  end
  final :stopped
end

TinyactorsTest.chart :hosted_counter do
  data count: 0, seen: []

  state :counting do
    transition on: 'bump' do
      assign '.count', '.count + 1'
      assign '.seen', '.seen + [$_event.data.tag]'
    end
  end
end

# Core ADR 0089 section 2's timelines 2 and 3: a delayed send to another
# actor, which the input log records as FIRED when it fires.
TinyactorsTest.chart :hosted_relay do
  state :idle do
    transition on: 'relay', target: :waiting do
      send_event 'hello', to: '#_actor_hosted_sink/b', delay: '5s'
    end
  end
  state :waiting
end

TinyactorsTest.chart :hosted_sink do
  state :h0 do
    transition on: 'hello', target: :h1
  end
  state :h1 do
    transition on: 'hello', target: :h2
  end
  state :h2
end

module HostedRelay
  # A week, in milliseconds: how long a relay stays evicted before its restore.
  WEEK = 7 * 24 * 3600 * 1000

  # hosted_relay/p1 and hosted_sink/b, both spawned at 0; p1 is logged and
  # arms `hello` to b at 1000, due at 6000. Answers [hosted, relay
  # definition, relay, sink].
  def self.start(context, incarnation: 0)
    hosted = context.hosted_system(incarnation: incarnation)
    context.hosted_log(hosted, 'hosted_relay')
    sink = context.hosted_spawn(hosted, context.hosted_define(hosted, :hosted_sink), family: 'hosted_sink', name: 'b')
    definition = context.hosted_define(hosted, :hosted_relay)
    relay = context.hosted_spawn(hosted, definition, family: 'hosted_relay', name: 'p1')
    context.hosted_run(hosted)
    context.hosted_advance(hosted, 1000)
    context.hosted_post(hosted, relay, 'relay')
    context.hosted_run(hosted)
    context.assert_equal 6000, context.hosted_next_deadline(hosted)
    [hosted, definition, relay, sink]
  end
end

TinyactorsTest.scenarios 'hosted' do
  # Catches: the test host not linking the kernel, or a hosted system that
  # leaks what the kernel allocated for it.
  scenario 'a hosted system opens and closes without leaking' do
    system = hosted_system
    assert_equal 0, close_hosted(system), 'the kernel freed everything'
  end

  # Catches: a hosted system that takes its memory from libc whatever the
  # scenario hands it, so a budget or an armed fault never reaches the
  # kernel; or a close that gives back less than the kernel took.
  scenario "a hosted system's memory comes from the scenario's allocator" do
    before = allocator.stats[:live_bytes]
    hosted = hosted_system
    assert allocator.stats[:live_bytes] > before, 'the kernel allocated through the counting allocator'
    assert_equal 0, close_hosted(hosted)
    assert_equal before, allocator.stats[:live_bytes], 'closing gave back everything'
  end

  # Catches: a system half-created under a budget, or one that keeps what
  # it had allocated when a later allocation fails.
  scenario 'a hosted system that cannot get its memory is not created' do
    before = allocator.stats[:live_bytes]
    AllocationFailure.tighten(self, headroom: 64)
    assert_equal 'err_nomem', hosted_system
    allocator.budget(0)
    assert_equal before, allocator.stats[:live_bytes], 'the refused create kept nothing'
    assert_equal 0, close_hosted(hosted_system), 'with room, the same create works'
  end

  # Catches: mail between actors that bypasses the kernel's mailboxes and
  # router, or a turn that does not run what was posted.
  scenario 'mail between two actors goes through the kernel' do
    hosted = hosted_system
    ping = hosted_spawn(hosted, hosted_define(hosted, :hosted_ping), family: 'hosted_ping', name: 'a')
    pong = hosted_spawn(hosted, hosted_define(hosted, :hosted_pong), family: 'hosted_pong', name: 'p')
    hosted_run(hosted)
    assert_equal 'ok', hosted_post(hosted, ping, 'go')
    assert hosted_run(hosted)[:quiet], 'the system is quiet after the mail was delivered'
    assert_equal [:sent], hosted_states(hosted, ping)
    assert_equal [:pinged], hosted_states(hosted, pong)
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a post delivered at once instead of queued, a mailbox that
  # loses its order, or a turn that leaves delivered mail in it.
  scenario "mail waits in the kernel's mailbox, in order, until a turn" do
    hosted = hosted_system
    actor = hosted_spawn(hosted, hosted_define(hosted, :hosted_stopper))
    hosted_run(hosted)
    3.times { assert_equal 'ok', hosted_post(hosted, actor, 'tick') }
    assert_equal 'ok', hosted_post(hosted, actor, 'stop')
    boxes = hosted_mailboxes(hosted, actor)
    assert_equal({ actor: actor, state: 'ready', depth: 4, events: %w[tick tick tick stop] }, boxes[:mailboxes].first)
    assert_equal 4, boxes[:queued]
    delivered = boxes[:delivered]
    assert hosted_run(hosted)[:quiet]
    assert_equal [], hosted_mailbox(hosted, actor)
    assert_equal delivered + 4, hosted_mailboxes(hosted)[:delivered]
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: mail for a finished actor delivered, dropped silently, or
  # reported without its data; dead-letter counters that miss it.
  scenario 'mail for an actor that finished is a dead letter, with its data' do
    hosted = hosted_system
    actor = hosted_spawn(hosted, hosted_define(hosted, :hosted_stopper))
    hosted_run(hosted)
    hosted_post(hosted, actor, 'stop')
    hosted_post_data(hosted, actor, 'tick', { n: 1 })
    dead = hosted_run(hosted)[:dead_letters]
    assert_equal [{ actor: actor, event: 'tick', reason: 'done', data: { n: 1 } }], dead
    assert_equal({ total: 1, by_reason: { done: 1 } }, hosted_mailboxes(hosted)[:dead_letters])
    assert_equal [], hosted_run(hosted)[:dead_letters], 'a dead letter is reported once'
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: mail that vanishes with the actor it waited for, or mail to
  # a stale ID that joins the mailbox of whatever comes next.
  scenario "an evicted actor's waiting mail, and mail to its old ID, are dead letters" do
    hosted = hosted_system
    actor = hosted_spawn(hosted, hosted_define(hosted, :hosted_stopper), family: 'hosted_stopper', name: 's')
    hosted_run(hosted)
    hosted_post(hosted, actor, 'tick')
    assert_equal 'ok', hosted_evict(hosted, actor)
    assert_equal 'ok', hosted_post(hosted, actor, 'stop')
    dead = hosted_run(hosted)[:dead_letters]
    assert_equal [{ actor: actor, event: 'tick', reason: 'destroyed' },
                  { actor: actor, event: 'stop', reason: 'not-found' }], dead
    assert_equal({ total: 2, by_reason: { destroyed: 1, 'not-found': 1 } }, hosted_mailboxes(hosted)[:dead_letters])
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a delayed send that the kernel's clock never makes due.
  scenario 'a delayed send fires when the scenario moves the clock' do
    hosted = hosted_system
    timer = hosted_spawn(hosted, hosted_define(hosted, :hosted_timer), family: 'hosted_timer', name: 't')
    hosted_run(hosted)
    hosted_post(hosted, timer, 'arm')
    hosted_run(hosted)
    assert_equal [:armed], hosted_states(hosted, timer)
    assert_equal 'ok', hosted_advance(hosted, 5000)
    hosted_run(hosted)
    assert_equal [:rung], hosted_states(hosted, timer)
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a log that misses an input, or a replay that loses the pending
  # timer: the rebuilt actor must be where the live one was, and its timeout
  # must still fire.
  scenario 'an evicted actor is rebuilt from its input log, with its timer' do
    hosted = hosted_system
    hosted_log(hosted, 'hosted_timer')
    definition = hosted_define(hosted, :hosted_timer)
    timer = hosted_spawn(hosted, definition, family: 'hosted_timer', name: 't')
    hosted_run(hosted)
    hosted_post(hosted, timer, 'arm')
    hosted_run(hosted)
    assert_equal 'ok', hosted_evict(hosted, timer)
    store = hosted_store(hosted)
    assert !store['hosted_timer/t'].empty?, 'the log holds records for the actor'
    rebuilt = hosted_restore(hosted, definition, 'hosted_timer', 't', store['hosted_timer/t'])
    assert_equal [:armed], hosted_states(hosted, rebuilt)
    hosted_advance(hosted, 5000)
    hosted_run(hosted)
    assert_equal [:rung], hosted_states(hosted, rebuilt)
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a value lost or cut on its way through the kernel's log and the
  # jq model's streaming codec.
  scenario 'a large value is logged and replayed through the kernel' do
    hosted = hosted_system
    hosted_log(hosted, 'hosted_box')
    definition = hosted_define(hosted, :hosted_box)
    box = hosted_spawn(hosted, definition, family: 'hosted_box', name: 'big')
    hosted_run(hosted)
    blob = 'v' * (1024 * 1024)
    assert_equal 'ok', hosted_post_data(hosted, box, 'put', { blob: blob })
    hosted_run(hosted)
    assert_equal blob.size, hosted_data(hosted, box, '.size')
    hosted_evict(hosted, box)
    log = hosted_store(hosted)['hosted_box/big']
    assert log.size / 2 > blob.size, 'the value is in the log'
    rebuilt = hosted_restore(hosted, definition, 'hosted_box', 'big', log)
    assert_equal blob.size, hosted_data(hosted, rebuilt, '.size')
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a data model or kernel framing that holds a whole value while
  # it is logged or replayed. The jq model writes in pieces (core ADR 0090's
  # notes); its heap is jq's, so the kernel's core counters show core's
  # side: no copy of the value, whatever its size. What the core log's
  # 'a large value is logged and replayed in pieces' checked.
  scenario 'a large value is written to the log in pieces, and core holds no copy of it' do
    hosted = hosted_system
    hosted_log(hosted, 'hosted_box')
    definition = hosted_define(hosted, :hosted_box)
    box = hosted_spawn(hosted, definition, family: 'hosted_box', name: 'big')
    hosted_run(hosted)
    blob = 'v' * (4 * 1024 * 1024)
    assert_equal 'ok', hosted_post_data(hosted, box, 'put', { blob: blob })
    hosted_run(hosted)
    assert_equal blob.size, hosted_data(hosted, box, '.size')
    written = hosted_stats(hosted)[:values]
    assert written[:bytes_written] > blob.size, 'the value went through the writer'
    assert written[:largest_write] <= 8192, "the value reached the log in pieces, the largest #{written[:largest_write]} bytes"
    hosted_evict(hosted, box)
    rebuilt = hosted_restore(hosted, definition, 'hosted_box', 'big', hosted_store(hosted)['hosted_box/big'])
    assert_equal blob.size, hosted_data(hosted, rebuilt, '.size')
    peak = hosted_stats(hosted).dig(:memory, :core, :peak_bytes)
    assert peak < blob.size / 4, "core's allocations peaked at #{peak} bytes for a #{blob.size}-byte value"
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a checkpoint that does not hold the actor's state, or a log
  # compacted to it that loses its pending timer.
  scenario 'a log compacted to its checkpoint rebuilds the actor' do
    hosted = hosted_system
    hosted_log(hosted, 'hosted_seen')
    definition = hosted_define(hosted, :hosted_seen)
    live = hosted_spawn(hosted, definition, family: 'hosted_seen', name: 'k')
    hosted_run(hosted)
    hosted_advance(hosted, 1000)
    hosted_post(hosted, live, 'e1')
    hosted_run(hosted) # waiting; the timeout is due at 6000
    checkpoint = hosted_checkpoint(hosted, live)
    assert_equal 'ok', checkpoint[:result]
    hosted_evict(hosted, live)
    rebuilt = hosted_restore(hosted, definition, 'hosted_seen', 'k', checkpoint[:bytes]) # the checkpoint alone
    assert_equal [:waiting], hosted_states(hosted, rebuilt)
    assert_equal 1, hosted_data(hosted, rebuilt, '.seen'), 'the data came from the checkpoint'
    hosted_advance(hosted, 6000)
    hosted_run(hosted)
    assert_equal [:timedout], hosted_states(hosted, rebuilt)
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a checkpoint of an actor the log does not cover.
  scenario 'a checkpoint needs a logged, named actor' do
    hosted = hosted_system
    hosted_log(hosted, 'hosted_seen')
    unnamed = hosted_spawn(hosted, hosted_define(hosted, :hosted_seen))
    hosted_run(hosted)
    assert_equal 'err_invalid', hosted_checkpoint(hosted, unnamed)[:result]
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a log that cannot be replayed by another system, or a crash
  # that loses the actor's pending timer. The first system is dropped after
  # its batches were stored, as a crash would leave them.
  scenario 'after a crash, another system rebuilds the actor from the stored log' do
    first = hosted_system(incarnation: 1)
    hosted_log(first, 'hosted_seen')
    live = hosted_spawn(first, hosted_define(first, :hosted_seen), family: 'hosted_seen', name: 'c')
    hosted_run(first)
    hosted_advance(first, 1000)
    hosted_post(first, live, 'e1')
    hosted_run(first) # waiting; the timeout is due at 6000
    store = hosted_store(first)
    close_hosted(first)

    second = hosted_system(incarnation: 2)
    hosted_log(second, 'hosted_seen')
    definition = hosted_define(second, :hosted_seen)
    hosted_advance(second, 2000)
    rebuilt = hosted_restore(second, definition, 'hosted_seen', 'c', store['hosted_seen/c'])
    assert_equal [:waiting], hosted_states(second, rebuilt)
    assert_equal 1, hosted_data(second, rebuilt, '.seen')
    hosted_advance(second, 6000)
    hosted_run(second)
    assert_equal [:timedout], hosted_states(second, rebuilt)
    assert_equal 0, close_hosted(second)
  end

  # Timeline 2 (E >= d). Catches: a replay that sends again what the live
  # actor sent before it stopped: FIRED in the log drops the held send at
  # publish, so the rebuilt relay holds nothing and b hears it once.
  scenario 'a send to another actor that fired before the eviction does not fire again' do
    hosted, definition, relay, sink = HostedRelay.start(self)
    hosted_advance(hosted, 6000)
    hosted_run(hosted)
    assert_equal [:h1], hosted_states(hosted, sink)
    hosted_advance(hosted, 7000)
    assert_equal 'ok', hosted_evict(hosted, relay)
    log = hosted_store(hosted)['hosted_relay/p1']

    hosted_advance(hosted, 7000 + HostedRelay::WEEK)
    rebuilt = hosted_restore(hosted, definition, 'hosted_relay', 'p1', log)
    hosted_run(hosted)
    assert_equal [:waiting], hosted_states(hosted, rebuilt)
    assert_equal nil, hosted_next_deadline(hosted), 'the rebuilt relay holds no send'
    assert_equal [:h1], hosted_states(hosted, sink), 'b heard it once'
    assert_equal({}, hosted_store(hosted), 'the rebuilt relay recorded nothing')
    assert_equal 0, close_hosted(hosted)
  end

  # Timeline 2 after a crash: the log ends at FIRED, whose time alone is the
  # publish time and covers the send. Catches: a send fired again by the
  # process that restores the actor.
  scenario 'a send to another actor that fired before a crash does not fire after it' do
    first, _definition, _relay, sink = HostedRelay.start(self, incarnation: 1)
    hosted_advance(first, 6000)
    hosted_run(first)
    assert_equal [:h1], hosted_states(first, sink)
    log = hosted_store(first)['hosted_relay/p1']
    close_hosted(first)

    second = hosted_system(incarnation: 2)
    hosted_log(second, 'hosted_relay')
    sink = hosted_spawn(second, hosted_define(second, :hosted_sink), family: 'hosted_sink', name: 'b')
    definition = hosted_define(second, :hosted_relay)
    hosted_advance(second, 6000 + HostedRelay::WEEK)
    rebuilt = hosted_restore(second, definition, 'hosted_relay', 'p1', log)
    hosted_run(second)
    assert_equal [:waiting], hosted_states(second, rebuilt)
    assert_equal nil, hosted_next_deadline(second), 'the rebuilt relay holds no send'
    assert_equal [:h0], hosted_states(second, sink), 'the send is not made again'
    assert_equal 0, close_hosted(second)
  end

  # Timeline 3 (E < d). Catches: a send still pending at the eviction that
  # the replay loses, or fires more than once: it is held at publish, fires
  # at the next dispatch, and the rebuilt relay records its FIRED.
  scenario 'a send to another actor still pending at the eviction fires once after the restore' do
    hosted, definition, relay, sink = HostedRelay.start(self)
    hosted_advance(hosted, 3000)
    assert_equal 'ok', hosted_evict(hosted, relay)
    hosted_run(hosted)
    assert_equal [:h0], hosted_states(hosted, sink)
    log = hosted_store(hosted)['hosted_relay/p1']
    assert_equal nil, hosted_next_deadline(hosted), 'the send left with the evicted relay'

    hosted_advance(hosted, HostedRelay::WEEK)
    rebuilt = hosted_restore(hosted, definition, 'hosted_relay', 'p1', log)
    hosted_run(hosted)
    assert_equal [:h1], hosted_states(hosted, sink), 'b heard it after the restore'
    assert_equal nil, hosted_next_deadline(hosted)
    assert !hosted_store(hosted)['hosted_relay/p1'].empty?, 'the rebuilt relay recorded that it fired'
    hosted_advance(hosted, 2 * HostedRelay::WEEK)
    hosted_run(hosted)
    assert_equal [:h1], hosted_states(hosted, sink), 'once'
    assert_equal [:waiting], hosted_states(hosted, rebuilt)
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: mail for an evicted actor that is lost or a dead letter although
  # its family has a loader; a load whose waiting mail is not delivered, or
  # delivered out of order; a loaded actor that did not get its state back.
  scenario 'mail to an evicted actor loads it from its log and delivers the mail' do
    hosted = hosted_system
    hosted_log(hosted, 'hosted_counter')
    definition = hosted_define(hosted, :hosted_counter)
    counter = hosted_spawn(hosted, definition, family: 'hosted_counter', name: 'c')
    hosted_run(hosted)
    hosted_post_to(hosted, 'hosted_counter', 'c', 'bump', data: { tag: 'a' })
    hosted_run(hosted)
    assert_equal 'ok', hosted_evict(hosted, counter)
    store = hosted_store(hosted)

    assert_equal 'ok', hosted_set_loader(hosted, 'hosted_counter')
    assert_equal 'ok', hosted_post_to(hosted, 'hosted_counter', 'c', 'bump', data: { tag: 'b' })
    assert_equal 'ok', hosted_post_to(hosted, 'hosted_counter', 'c', 'bump', data: { tag: 'c' })
    run = hosted_run(hosted)
    assert_equal [{ family: 'hosted_counter', name: 'c', why: 'mail' }],
                 run[:loads].map { |load| load.slice(:family, :name, :why) }
    assert_equal [], run[:dead_letters]
    loaded = hosted_load(hosted, run[:loads].first, definition: definition, log: store['hosted_counter/c'])
    assert_equal 'ok', loaded[:result]
    assert_equal 'loaded', loaded[:outcome]
    assert hosted_run(hosted)[:quiet]
    assert_equal 3, hosted_data(hosted, loaded[:actor], '.count')
    assert_equal %w[a b c], hosted_data(hosted, loaded[:actor], '.seen'), 'the waiting mail came after the log, in order'
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a load for a name the store has never seen that cannot start an
  # actor, or a loader-backed run that loses the mail that started it.
  scenario 'mail to a name the store has never seen spawns it through the loader' do
    hosted = hosted_system
    hosted_log(hosted, 'hosted_counter')
    definition = hosted_define(hosted, :hosted_counter)
    hosted_set_loader(hosted, 'hosted_counter')
    hosted_post_to(hosted, 'hosted_counter', 'fresh', 'bump', data: { tag: 'x' })
    answers = hosted_run_loading(hosted, {}, { 'hosted_counter' => definition })
    assert_equal [%w[ok spawned]], answers.map { |answer| [answer[:result], answer[:outcome]] }
    assert_equal %w[x], hosted_data(hosted, answers.first[:actor], '.seen')
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a load that cannot be answered keeping its mail forever, or
  # dropping it silently; a load's mailbox that grows past max_waiting.
  scenario 'a load the loader cannot answer turns its waiting mail into dead letters' do
    hosted = hosted_system
    hosted_define(hosted, :hosted_counter)
    hosted_set_loader(hosted, 'hosted_counter', max_waiting: 1)
    hosted_post_to(hosted, 'hosted_counter', 'gone', 'bump', data: { tag: 'kept' })
    hosted_post_to(hosted, 'hosted_counter', 'gone', 'bump', data: { tag: 'over' })
    run = hosted_run(hosted)
    assert_equal [['waiting-full', { tag: 'over' }]], run[:dead_letters].map { |dead| [dead[:reason], dead[:data]] }
    assert_equal 'not-found', hosted_load(hosted, run[:loads].first, outcome: 'not-found')[:outcome]
    dead = hosted_run(hosted)[:dead_letters]
    assert_equal [['not-found', { tag: 'kept' }]], dead.map { |letter| [letter[:reason], letter[:data]] }
    assert_equal 'err_stale', hosted_load(hosted, run[:loads].first, outcome: 'not-found')[:result],
                 'a load is answered once'
    assert_equal 0, close_hosted(hosted)
  end
end

# Core ADR 0094 section 2: the kernel records an input before core takes
# it, so an input whose record cannot be written is refused, and nothing
# changes: not the log, not the actor, not its mailbox. What was the core
# log's 'an input whose record the sink refuses changes nothing'.
module HostedRefusal
  # A logged hosted_counter at hosted_counter/q that took one bump, its
  # batches stored. Answers [hosted, definition, counter, store].
  def self.start(context)
    hosted = context.hosted_system
    context.hosted_log(hosted, 'hosted_counter')
    definition = context.hosted_define(hosted, :hosted_counter)
    counter = context.hosted_spawn(hosted, definition, family: 'hosted_counter', name: 'q')
    context.hosted_run(hosted)
    context.hosted_post_data(hosted, counter, 'bump', { tag: 'first' })
    context.hosted_run(hosted)
    [hosted, definition, counter, context.hosted_store(hosted)]
  end

  # The actor, its mailbox and its stored log say it took `tags`, and
  # replaying the log rebuilds the same actor.
  def self.check(context, hosted, definition, counter, store, tags)
    context.hosted_run(hosted)
    context.hosted_store(hosted, store)
    context.assert_equal tags, context.hosted_data(hosted, counter, '.seen')
    context.assert_equal [], context.hosted_mailbox(hosted, counter)
    context.assert_equal 'ok', context.hosted_evict(hosted, counter)
    context.hosted_store(hosted, store)
    rebuilt = context.hosted_restore(hosted, definition, 'hosted_counter', 'q', store['hosted_counter/q'])
    context.assert_equal tags, context.hosted_data(hosted, rebuilt, '.seen'), 'the log holds exactly what the actor took'
  end
end

TinyactorsTest.scenarios 'hosted, when the log cannot grow' do
  # Catches: an input taken although its record could not be written, so
  # the actor moves on and its log does not say so; or a refusal that
  # leaves the message queued anyway.
  scenario 'an input is refused when the log cannot grow, and nothing changes' do
    hosted, definition, counter, store = HostedRefusal.start(self)
    logged = store['hosted_counter/q'].dup
    # Room for the envelope and the small allocations, not for a 64 KiB record.
    AllocationFailure.tighten(self, headroom: 16 * 1024)
    assert_equal 'err_nomem', hosted_post_data(hosted, counter, 'bump', { tag: 'refused', pad: 'p' * 65_536 })
    allocator.budget(0)
    refused = allocator.entries('alloc_failed')
    assert !refused.empty? && refused.all? { |entry| entry[:alignment] == 1 }, "the log's byte buffer is what failed"
    assert_equal [], hosted_mailbox(hosted, counter), 'nothing was queued'
    hosted_run(hosted)
    assert_equal logged, hosted_store(hosted, store)['hosted_counter/q'], 'nothing was logged'
    assert_equal %w[first], hosted_data(hosted, counter, '.seen'), 'the actor did not take it'
    assert_equal 'ok', hosted_post_data(hosted, counter, 'bump', { tag: 'second', pad: 'p' * 65_536 })
    HostedRefusal.check(self, hosted, definition, counter, store, %w[first second])
    assert_equal 0, close_hosted(hosted)
  end

  # The same property at every allocation a post makes, for values that
  # fit the log's buffer and values that make it grow. Catches: a failure
  # path of the kernel's post or record writer that keeps half of an input.
  scenario 'a post refused at any allocation changes nothing', exhaustive: true do
    hosted, definition, counter, store = HostedRefusal.start(self)
    tags = %w[first]
    [0, 1024, 65_536, 1024 * 1024].each do |size|
      data = { tag: "s#{size}", pad: 'p' * size }
      before = allocator.stats[:allocations]
      assert_equal 'ok', hosted_post_data(hosted, counter, 'bump', data)
      allocations = allocator.stats[:allocations] - before
      tags << "s#{size}"
      hosted_run(hosted)
      allocations.times do |failing|
        allocator.arm_fault(after: failing, count: 1)
        result = hosted_post_data(hosted, counter, 'bump', data.merge(tag: "s#{size}f#{failing}"))
        allocator.clear_faults
        assert %w[ok err_nomem].include?(result), "post #{size}/#{failing}: #{result}"
        tags << "s#{size}f#{failing}" if result == 'ok'
        assert_equal(result == 'ok' ? 1 : 0, hosted_mailbox(hosted, counter).size, "post #{size}/#{failing}")
        hosted_run(hosted)
      end
    end
    HostedRefusal.check(self, hosted, definition, counter, store, tags)
    assert_equal 0, close_hosted(hosted)
  end
end
