# frozen_string_literal: true

# Delayed sends live with their owner, and a global index orders the owners
# (ADR 0054). Each actor keeps its pending sends in one list sorted by
# (due, send order); the system keeps a heap of the owners that have any,
# keyed by each owner's front send. So:
#
# - inspection lists an actor's own sends, with where each one goes (D4, D5);
# - due sends still go out in one global (due, send order), across owners,
#   and cancelling an owner's front send re-keys it (D2);
# - a send the router refuses goes back to its owner, marked submitted, and
#   to a system-owned list when the owner is destroyed or unloaded (D3);
# - destroy and unload reserve that room first, so running out of memory
#   leaves the actor exactly as it was (D3).
module DelayedSendsAtOwner
  # An actor that arms one delayed event per [event, delay] pair on entry,
  # each with the send ID of its event, to `to` through `type` (the built-in
  # SCXML processor when nil). `drop` cancels the first one.
  def self.sender(name, sends, to:, type: nil)
    first = sends.first.first
    TinyactorsTest::Chart.define(name) do
      state :waiting do
        on_entry do
          sends.each { |event, delay| send_event event, to: to, type: type, delay: delay, id: event.tr('.', '_') }
        end
        transition on: 'drop', type: :internal do
          cancel first.tr('.', '_')
        end
      end
    end
  end

  # Records the name of every `tick` event it receives, in arrival order.
  def self.receiver
    TinyactorsTest::Chart.define(:receiver) do
      data inbox: { expr: '[]' }
      state :listening do
        transition on: 'tick', type: :internal do
          assign '.inbox', '.inbox + [$_event.name]'
        end
      end
    end
  end

  # [receiver, sender] with the sender's one send refused once by the router
  # at 10s: it is back in the sender's list, submitted, and still due.
  def self.refused(context, name: nil)
    context.instance_exec do
      router = recording_router
      receiver = spawn DelayedSendsAtOwner.receiver
      chart = DelayedSendsAtOwner.sender(:refused, [['tick.one', '10s']], to: "#_scxml_#{receiver.handle}")
      sender = name ? spawn(chart, name: name) : spawn(chart)
      router.arm_fault(:submit, status: 'retry', count: 1)
      core('ta_system_set_time', ta.system.set_time(system: system_handle, now: 10_000))
      due = core('ta_system_dispatch_due', ta.system.dispatch_due(system: system_handle, budget: 1))
      assert_equal [0, true], due.values_at(:dispatched, :more_due), 'the router refused the one due send'
      [receiver, sender]
    end
  end

  # The sender's pending sends as [event name, due, submitted].
  def self.pending(context, actor)
    Array(actor.inspect_actor[:delayed_sends]).map do |send|
      [context.event_text(send[:event]), send[:due], send[:submitted]]
    end
  end
end

TinyactorsTest.scenarios 'delayed sends at their owner' do
  # Catches: an inspection that shows a count instead of the sends, or that
  # loses where a send goes. A send through the built-in SCXML processor is
  # resolved when it runs, so its destination is the receiving actor; a send
  # through a custom I/O processor keeps its target text and resolves
  # nothing (D5).
  scenario 'inspection lists each pending send and where it goes' do
    receiver = spawn DelayedSendsAtOwner.receiver
    to_actor = spawn DelayedSendsAtOwner.sender(:to_actor, [['tick.one', '10s']], to: "#_scxml_#{receiver.handle}")
    recording_io('x-test')
    to_io = spawn DelayedSendsAtOwner.sender(:to_io, [['tick.two', '20s']], to: 'elsewhere', type: 'x-test')

    [to_actor, to_io].each do |actor|
      sends = Array(actor.inspect_actor[:delayed_sends])
      assert_equal 1, sends.size, "#{actor} lists its own send and no one else's"
      assert_equal false, sends.first[:submitted], 'a send the router has not seen is not submitted'
    end
    scxml = to_actor.inspect_actor[:delayed_sends].first
    assert_equal 10_000, scxml[:due]
    assert_equal({ kind: 'runtime', runtime: receiver.handle }, scxml[:destination],
                 'a send through the SCXML processor names the actor it resolved to')
    custom = to_io.inspect_actor[:delayed_sends].first
    assert_equal 20_000, custom[:due]
    assert_equal 'elsewhere', custom[:target], 'a custom processor keeps the target it was given'
    assert_equal({ kind: 'runtime', runtime: 0 }, custom[:destination],
                 'a custom processor resolves no destination: the field is zero, which names no actor')
  end

  # Catches: an owner heap keyed by something other than each owner's front
  # send (order would follow the owners, not the sends), a tie broken by
  # owner instead of by send order, or a cancel of an owner's front send
  # that leaves the heap keyed by the cancelled send (the owner's other send
  # would go out at the cancelled send's time).
  scenario 'due sends go out in one global order across owners, after a cancel too' do
    router = recording_router
    receiver = spawn DelayedSendsAtOwner.receiver
    to = "#_scxml_#{receiver.handle}"
    a = spawn DelayedSendsAtOwner.sender(:a, [['tick.a1', '10s'], ['tick.a2', '40s']], to: to)
    spawn DelayedSendsAtOwner.sender(:b, [['tick.b1', '20s'], ['tick.b2', '30s']], to: to)
    spawn DelayedSendsAtOwner.sender(:c, [['tick.c1', '20s'], ['tick.c2', '50s']], to: to)
    assert_equal 10_000, next_deadline, 'a1 is due first'

    a.deliver 'drop'
    assert_equal [['tick.a2', 40_000, false]], DelayedSendsAtOwner.pending(self, a), 'a1 is cancelled'
    assert_equal 20_000, next_deadline, 'the owner of a1 is re-keyed to a2; b1 is due first now'

    advance_time 60_000
    expected = %w[tick.b1 tick.c1 tick.b2 tick.a2 tick.c2]
    assert_equal expected, receiver.data('.inbox'),
                 'by due time, and b1 before c1 at 20s because it was sent first'
    accepted = router.entries('submit').select { |entry| entry[:status] == 'accepted' }
    assert_equal expected, accepted.map { |entry| entry.dig(:event, :name_text) },
                 'the router accepted them in the same order; a refused one kept its place'
    assert_equal nil, next_deadline, 'nothing is pending'
  end

  # Catches: a refused send dropped, rescheduled as a new send (it would
  # lose its place in deadline order), or not marked as waiting to be
  # offered again (D3, ADR 0026).
  scenario 'a refused send goes back to its owner, submitted' do
    receiver, sender = DelayedSendsAtOwner.refused(self)
    assert_equal [['tick.one', 10_000, true]], DelayedSendsAtOwner.pending(self, sender),
                 'the refused send is back in its owner\'s list, submitted, at its own time'
    assert_equal 10_000, next_deadline, 'it is still the next deadline'

    run_system('offer it again')
    assert_equal ['tick.one'], receiver.data('.inbox'), 'the second offer delivered it, once'
    assert_equal [], DelayedSendsAtOwner.pending(self, sender)
  end

  # Catches: destroy discarding a submitted send (it is already sent, so it
  # belongs to the receiver now), or keeping it in the destroyed actor's
  # storage. It moves to the system-owned list, stays due, and goes out.
  scenario 'a refused send outlives its owner\'s destroy' do
    receiver, sender = DelayedSendsAtOwner.refused(self)
    core('ta_actor_destroy', ta.actor.destroy(system: system_handle, actor: sender.handle))
    forget(sender)
    assert !sender.resident?, 'the owner is gone'
    assert_equal 10_000, next_deadline, 'its submitted send is still pending, owned by the system'

    run_system('offer the orphaned send again')
    assert_equal ['tick.one'], receiver.data('.inbox')
    assert_equal nil, next_deadline, 'nothing is pending'
  end

  # Catches: the same for unload, which ends residency the same way: the
  # image keeps the unsent sends, and the submitted one stays with the
  # system.
  scenario 'a refused send outlives its owner\'s unload' do
    receiver, sender = DelayedSendsAtOwner.refused(self, name: 'u1')
    image = capture(sender)
    assert_equal 'ok', unload(sender, image)
    assert_equal nil, image_deadline(image), 'the image holds no submitted send; it is not the actor\'s any more'
    assert_equal 10_000, next_deadline, 'the submitted send is still pending, owned by the system'

    run_system('offer the orphaned send again')
    assert_equal ['tick.one'], receiver.data('.inbox')
    destroy_image(image)
  end

  # Catches: destroy that moves the submitted send before it has room for
  # it, and then either loses it or leaves the actor half destroyed. Room is
  # reserved first, so a refused allocation is TA_ERR_NOMEM with nothing
  # changed, and a retry succeeds (D3, ADR 0047).
  scenario 'a destroy that cannot reserve room changes nothing' do
    receiver, sender = DelayedSendsAtOwner.refused(self)
    before = DelayedSendsAtOwner.pending(self, sender)

    allocator.arm_fault(after: 0, count: 1)
    result = ta.actor.destroy(system: system_handle, actor: sender.handle)[:result]
    allocator.clear_faults
    assert_equal 'err_nomem', result, 'the destroy could not reserve room for the submitted send'
    assert sender.resident?, 'the actor is still resident'
    assert_equal before, DelayedSendsAtOwner.pending(self, sender), 'its sends are unchanged'
    assert_equal 10_000, next_deadline, 'and so is the global order'

    sender.destroy
    assert_equal ['tick.one'], receiver.data('.inbox'), 'the retried destroy kept the send, which went out'
  end

  # Catches: the same for unload.
  scenario 'an unload that cannot reserve room changes nothing' do
    receiver, sender = DelayedSendsAtOwner.refused(self, name: 'u1')
    before = DelayedSendsAtOwner.pending(self, sender)
    image = capture(sender)

    allocator.arm_fault(after: 0, count: 1)
    result = unload(sender, image)
    allocator.clear_faults
    assert_equal 'err_nomem', result, 'the unload could not reserve room for the submitted send'
    assert sender.resident?, 'the actor is still resident'
    assert_equal before, DelayedSendsAtOwner.pending(self, sender), 'its sends are unchanged'
    assert_equal 10_000, next_deadline

    assert_equal 'ok', unload(sender, image), 'a retry succeeds'
    run_system('offer the orphaned send again')
    assert_equal ['tick.one'], receiver.data('.inbox')
    destroy_image(image)
  end
end
