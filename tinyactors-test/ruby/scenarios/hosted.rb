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

# A delayed send to another actor: its firing is not an event of the
# sender (kernel ADR 0006, notes of 2026-10-01).
TinyactorsTest.chart :hosted_relay do
  state :idle do
    transition on: 'relay', target: :waiting do
      send_event 'hello', to: '#_actor_hosted_sink/b', delay: '5s'
    end
  end
  state :waiting do
    transition on: 'note'
  end
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

  # hosted_relay/p1 and hosted_sink/b, both spawned at 0; p1 is recorded
  # and arms `hello` to b at 1000, due at 6000. Answers [hosted, relay
  # definition, relay, sink, store].
  def self.start(context, incarnation: 0)
    hosted = context.hosted_system(incarnation: incarnation)
    context.hosted_record(hosted, 'hosted_relay')
    sink = context.hosted_spawn(hosted, context.hosted_define(hosted, :hosted_sink), family: 'hosted_sink', name: 'b')
    definition = context.hosted_define(hosted, :hosted_relay)
    relay = context.hosted_spawn(hosted, definition, family: 'hosted_relay', name: 'p1')
    context.hosted_run(hosted)
    context.hosted_advance(hosted, 1000)
    context.hosted_post(hosted, relay, 'relay')
    context.hosted_run(hosted)
    context.assert_equal 6000, context.hosted_next_deadline(hosted)
    store = context.hosted_store(hosted)
    context.assert_equal 6000, store.wakes['hosted_relay/p1']
    [hosted, definition, relay, sink, store]
  end
end

module HostedOverflow
  # hosted_ping/a sends `ping` to hosted_pong/p while the queue is full
  # (maxQueuedMessages 1, taken by the application's `noise` for p): the
  # router accepts it into p's overflow (kernel ADR 0003). Both families
  # are recorded. Answers [hosted, ping, pong].
  def self.start(context, incarnation: 1)
    hosted = context.hosted_system(incarnation: incarnation, max_queued_messages: 1)
    context.hosted_record(hosted, 'hosted_ping', 'hosted_pong')
    ping = context.hosted_spawn(hosted, context.hosted_define(hosted, :hosted_ping), family: 'hosted_ping', name: 'a')
    pong = context.hosted_spawn(hosted, context.hosted_define(hosted, :hosted_pong), family: 'hosted_pong', name: 'p')
    context.hosted_run(hosted)
    context.hosted_post(hosted, ping, 'go')
    context.hosted_run(hosted, limit: 1) # `go` delivered; a runs it next turn
    context.assert_equal 'ok', context.hosted_post(hosted, pong, 'noise')
    context.assert_equal 'err_busy', context.hosted_post(hosted, pong, 'noise'), "the application's mail is refused"
    context.hosted_run(hosted, limit: 1) # a sends `ping`; p takes `noise`
    boxes = context.hosted_mailboxes(hosted, pong)
    context.assert_equal 1, boxes[:overflow]
    context.assert_equal %w[ping], boxes[:mailboxes].first[:events]
    [hosted, ping, pong]
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

  # Catches: a full queue that refuses mail between actors (kernel ADR
  # 0003); overflow that is delivered before there is room, or never.
  scenario 'mail between actors waits in the overflow of a full queue, and is delivered once' do
    hosted, _ping, pong = HostedOverflow.start(self)
    assert_equal [:waiting], hosted_states(hosted, pong)
    assert hosted_run(hosted)[:quiet]
    assert_equal [:pinged], hosted_states(hosted, pong)
    boxes = hosted_mailboxes(hosted, pong)
    assert_equal [0, 0], [boxes[:queued], boxes[:overflow]]
    assert_equal 0, close_hosted(hosted)
  end

  # What a crash costs is explicit (kernel ADR 0006 section 5): mail p had
  # not taken is in no journal, and a's rebuild does not send it again, so
  # delivery is at most once where nothing retries. Catches: a rebuild that
  # resends what a replayed macrostep sent.
  scenario 'mail in the overflow at a crash is lost unless its sender retries' do
    hosted, = HostedOverflow.start(self)
    store = hosted_store(hosted)
    close_hosted(hosted) # a crash: nothing more is recorded
    assert_equal %w[go], store.journals['hosted_ping/a'].map { |entry| entry[:event][:name] }
    assert_equal [], store.journals['hosted_pong/p'], "p's `noise` was delivered, but its macrostep had not ended"

    again = hosted_system(incarnation: 2)
    ping = hosted_restore(again, hosted_define(again, :hosted_ping), 'hosted_ping', 'a', store.journals['hosted_ping/a'])
    pong = hosted_restore(again, hosted_define(again, :hosted_pong), 'hosted_pong', 'p', store.journals['hosted_pong/p'])
    hosted_run(again)
    assert_equal [:sent], hosted_states(again, ping)
    assert_equal [:waiting], hosted_states(again, pong), 'the ping is not sent again'
    assert_equal 0, close_hosted(again)
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

  # Catches: a journal that misses an event, or a replay that loses the
  # pending timer: the rebuilt actor must be where the live one was, and its
  # timeout must still fire; an eviction that stores no wake.
  scenario 'an evicted actor is rebuilt from its journal, with its timer' do
    hosted = hosted_system
    hosted_record(hosted, 'hosted_timer')
    definition = hosted_define(hosted, :hosted_timer)
    timer = hosted_spawn(hosted, definition, family: 'hosted_timer', name: 't')
    hosted_run(hosted)
    hosted_post(hosted, timer, 'arm')
    hosted_run(hosted)
    assert_equal 'ok', hosted_evict(hosted, timer)
    store = hosted_store(hosted)
    assert_equal [{ event: { name: 'arm', type: 'external' }, at: 0 }], store.journals['hosted_timer/t']
    assert_equal 5000, store.wakes['hosted_timer/t'], 'the eviction stored its timer as the wake'
    rebuilt = hosted_restore(hosted, definition, 'hosted_timer', 't', store.journals['hosted_timer/t'])
    assert_equal [:armed], hosted_states(hosted, rebuilt)
    hosted_advance(hosted, 5000)
    hosted_run(hosted)
    assert_equal [:rung], hosted_states(hosted, rebuilt)
    ring = hosted_store(hosted).journals['hosted_timer/t']
    assert_equal [['ring', 5000, '#_actor_hosted_timer/t']], ring.map { |entry| [entry.dig(:event, :name), entry[:at], entry.dig(:event, :origin)] },
                 'the timeout it sent itself is in its journal'
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a value lost or cut on its way through the recorder and the
  # replay, or a kernel or core that holds a copy of it.
  scenario 'a large value is recorded and replayed, and core holds no copy of it' do
    hosted = hosted_system
    hosted_record(hosted, 'hosted_box')
    definition = hosted_define(hosted, :hosted_box)
    box = hosted_spawn(hosted, definition, family: 'hosted_box', name: 'big')
    hosted_run(hosted)
    blob = 'v' * (1024 * 1024)
    assert_equal 'ok', hosted_post_data(hosted, box, 'put', { blob: blob })
    hosted_run(hosted)
    assert_equal blob.size, hosted_data(hosted, box, '.size')
    hosted_evict(hosted, box)
    journal = hosted_store(hosted).journals['hosted_box/big']
    assert_equal blob.size, journal.first.dig(:event, :data, :blob).size, 'the value is in the journal'
    rebuilt = hosted_restore(hosted, definition, 'hosted_box', 'big', journal)
    assert_equal blob.size, hosted_data(hosted, rebuilt, '.size')
    peak = hosted_stats(hosted).dig(:memory, :core, :peak_bytes)
    # The envelope pool's reserve (core ADR 0099) is a fixed part of the peak; a copy of the value would add all of it.
    assert peak < blob.size / 2, "core's allocations peaked at #{peak} bytes for a #{blob.size}-byte value"
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a journal that cannot be replayed by another system, or a
  # crash that loses the actor's pending timer. The first system is dropped
  # after its records were stored, as a crash would leave them.
  scenario 'after a crash, another system rebuilds the actor from the stored journal' do
    first = hosted_system(incarnation: 1)
    hosted_record(first, 'hosted_seen')
    live = hosted_spawn(first, hosted_define(first, :hosted_seen), family: 'hosted_seen', name: 'c')
    hosted_run(first)
    hosted_advance(first, 1000)
    hosted_post(first, live, 'e1')
    hosted_run(first) # waiting; the timeout is due at 6000
    store = hosted_store(first)
    close_hosted(first)

    second = hosted_system(incarnation: 2)
    definition = hosted_define(second, :hosted_seen)
    hosted_advance(second, 2000)
    rebuilt = hosted_restore(second, definition, 'hosted_seen', 'c', store.journals['hosted_seen/c'])
    assert_equal [:waiting], hosted_states(second, rebuilt)
    assert_equal 1, hosted_data(second, rebuilt, '.seen')
    hosted_advance(second, 6000)
    hosted_run(second)
    assert_equal [:timedout], hosted_states(second, rebuilt)
    assert_equal 0, close_hosted(second)
  end

  # Kernel ADR 0006, notes of 2026-10-01. Catches: a firing that leaves the
  # sender's stored wake naming it, so every restart reloads the sender and
  # repeats the send. A rebuild before the relay takes another event repeats
  # the send once (at least once, accepted); none after another event.
  scenario 'a send to another actor that fired repeats at most once on a rebuild' do
    hosted, definition, relay, sink, store = HostedRelay.start(self)
    hosted_advance(hosted, 6000)
    hosted_run(hosted)
    assert_equal [:h1], hosted_states(hosted, sink)
    hosted_store(hosted, store)
    assert_equal nil, store.wakes['hosted_relay/p1'], 'the firing moved the wake'
    hosted_advance(hosted, 7000)
    assert_equal 'ok', hosted_evict(hosted, relay)
    hosted_store(hosted, store)
    assert_equal nil, store.wakes['hosted_relay/p1'], 'nothing reloads the evicted relay'

    hosted_advance(hosted, 7000 + HostedRelay::WEEK)
    rebuilt = hosted_restore(hosted, definition, 'hosted_relay', 'p1', store.journals['hosted_relay/p1'])
    hosted_run(hosted)
    assert_equal [:h2], hosted_states(hosted, sink), 'repeated once'
    assert_equal nil, hosted_next_deadline(hosted)
    hosted_post(hosted, rebuilt, 'note')
    hosted_run(hosted)
    assert_equal 'ok', hosted_evict(hosted, rebuilt)
    hosted_store(hosted, store)
    hosted_restore(hosted, definition, 'hosted_relay', 'p1', store.journals['hosted_relay/p1'])
    hosted_run(hosted)
    assert_equal [:h2], hosted_states(hosted, sink), 'not repeated after another event'
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a send still pending at the eviction that the rebuild loses,
  # or fires more than once.
  scenario 'a send to another actor still pending at the eviction fires once after the restore' do
    hosted, definition, relay, sink, store = HostedRelay.start(self)
    hosted_advance(hosted, 3000)
    assert_equal 'ok', hosted_evict(hosted, relay)
    hosted_run(hosted)
    assert_equal [:h0], hosted_states(hosted, sink)
    hosted_store(hosted, store)
    assert_equal 6000, store.wakes['hosted_relay/p1'], 'the wake still names the pending send'
    assert_equal nil, hosted_next_deadline(hosted), 'the send left with the evicted relay'

    hosted_advance(hosted, HostedRelay::WEEK)
    rebuilt = hosted_restore(hosted, definition, 'hosted_relay', 'p1', store.journals['hosted_relay/p1'])
    hosted_run(hosted)
    assert_equal [:h1], hosted_states(hosted, sink), 'b heard it after the restore'
    hosted_advance(hosted, 2 * HostedRelay::WEEK)
    hosted_run(hosted)
    assert_equal [:h1], hosted_states(hosted, sink), 'once'
    assert_equal [:waiting], hosted_states(hosted, rebuilt)
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: mail for an evicted actor that is lost or a dead letter although
  # its family has a loader; a load whose waiting mail is not delivered, or
  # delivered out of order; a loaded actor that did not get its state back.
  scenario 'mail to an evicted actor loads it from its journal and delivers the mail' do
    hosted = hosted_system
    hosted_record(hosted, 'hosted_counter')
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
    loaded = hosted_load(hosted, run[:loads].first, definition: definition, journal: store.journals['hosted_counter/c'])
    assert_equal 'ok', loaded[:result]
    assert_equal 'loaded', loaded[:outcome]
    assert hosted_run(hosted)[:quiet]
    assert_equal 3, hosted_data(hosted, loaded[:actor], '.count')
    assert_equal %w[a b c], hosted_data(hosted, loaded[:actor], '.seen'), 'the waiting mail came after the journal, in order'
    assert_equal %w[a b c], hosted_store(hosted, store).journals['hosted_counter/c'].map { |entry| entry.dig(:event, :data, :tag) }
    assert_equal 0, close_hosted(hosted)
  end

  # Catches: a load for a name the store has never seen that cannot start an
  # actor, or a loader-backed run that loses the mail that started it.
  scenario 'mail to a name the store has never seen spawns it through the loader' do
    hosted = hosted_system
    hosted_record(hosted, 'hosted_counter')
    definition = hosted_define(hosted, :hosted_counter)
    hosted_set_loader(hosted, 'hosted_counter')
    hosted_post_to(hosted, 'hosted_counter', 'fresh', 'bump', data: { tag: 'x' })
    answers = hosted_run_loading(hosted, empty_hosted_store, { 'hosted_counter' => definition })
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
