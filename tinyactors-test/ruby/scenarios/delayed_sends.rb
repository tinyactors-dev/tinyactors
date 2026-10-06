# frozen_string_literal: true

# Delayed sends live with their owner (ADR 0054), as envelopes of core's
# pool on the owner's list, ordered by core's deadline index (core ADR
# 0101). So:
#
# - inspection lists an actor's own sends, with where each one goes (D4, D5);
# - due sends still go out in one global (due, send order), across owners,
#   and cancelling an owner's front send moves the deadline to its next
#   (D2); one to a resident actor goes into core's pool (core ADR 0099);
# - a send its I/O processor refuses (out of memory) when it falls due
#   stays with its owner, marked submitted, and stays indexed when the
#   owner is destroyed or unloaded (D3);
# - destroy and unload need no memory to keep it, so running out of memory
#   cannot stop them.
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

  # The I/O processor the refused send goes through.
  IO_TYPE = 'x-slow'

  # [io, sender] with the sender's one send to IO_TYPE refused once at 10s:
  # the processor ran out of memory, so the send is back in the sender's
  # list, submitted, and still due.
  def self.refused(context, name: nil)
    context.instance_exec do
      io = recording_io(DelayedSendsAtOwner::IO_TYPE)
      io.arm_fault(:send, status: 'nomem')
      chart = DelayedSendsAtOwner.sender(:refused, [['tick.one', '10s']], to: 'somewhere',
                                                                           type: DelayedSendsAtOwner::IO_TYPE)
      sender = name ? spawn(chart, name: name) : spawn(chart)
      core('ta_system_set_time', ta.system.set_time(system: system_handle, now: 10_000))
      due = dispatch_due(100)
      assert_equal [0, true], due.values_at(:dispatched, :more_due),
                   'the processor refused the one due send, which waits to be offered again'
      [io, sender]
    end
  end

  # The events IO_TYPE took, refusals left out.
  def self.delivered(io)
    io.entries('send').reject { |entry| entry[:injected] }.map { |entry| entry[:event_text] }
  end

  # The sender's pending sends as [event name, due, submitted].
  def self.pending(context, actor)
    Array(actor.inspect_actor[:delayed_sends]).map do |send|
      [send[:event].to_i.zero? ? send[:event_text] : context.event_text(send[:event]), send[:due], send[:submitted]]
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
    assert_equal 'elsewhere', send_target(to_io.inspect_actor[:definition_id], custom),
                 'a custom processor keeps the literal target the chart wrote'
    assert_equal nil, custom[:target], 'a literal target is text, not a value of the data model'
    assert_equal({ kind: 'runtime', runtime: 0 }, custom[:destination],
                 'a custom processor resolves no destination: the field is zero, which names no actor')
  end

  # Catches: a deadline index keyed by something other than each send's due
  # time and order (order would follow the owners, not the sends), a tie
  # broken by owner instead of by send order, or a cancel that leaves the
  # cancelled send indexed (the owner's other send would go out at the
  # cancelled send's time).
  scenario 'due sends go out in one global order across owners, after a cancel too' do
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
    assert_equal [], load_requests,
                 'sends to a resident actor ask the host for nothing: core queues them in that order'
    assert_equal nil, next_deadline, 'nothing is pending'
  end

  # Catches: a refused send dropped, rescheduled as a new send (it would
  # lose its place in deadline order), or not marked as waiting to be
  # offered again (D3, ADR 0026).
  scenario 'a refused send goes back to its owner, submitted' do
    io, sender = DelayedSendsAtOwner.refused(self)
    assert_equal [['tick.one', 10_000, true]], DelayedSendsAtOwner.pending(self, sender),
                 'the refused send is back in its owner\'s list, submitted, at its own time'
    assert_equal 10_000, next_deadline, 'it is still the next deadline'

    run_system('offer it again')
    assert_equal ['tick.one'], DelayedSendsAtOwner.delivered(io), 'the second offer delivered it, once'
    assert_equal [], DelayedSendsAtOwner.pending(self, sender)
  end

  # Catches: destroy discarding a submitted send (it is already sent, so it
  # belongs to the receiver now), or keeping it in the destroyed actor's
  # storage. It stays indexed with no owner, stays due, and goes out.
  scenario 'a refused send outlives its owner\'s destroy' do
    io, sender = DelayedSendsAtOwner.refused(self)
    core('ta_actor_destroy', ta.actor.destroy(system: system_handle, actor: sender.handle))
    forget(sender)
    assert !sender.resident?, 'the owner is gone'
    assert_equal 10_000, next_deadline, 'its submitted send is still pending, owned by the system'

    run_system('offer the orphaned send again')
    assert_equal ['tick.one'], DelayedSendsAtOwner.delivered(io)
    assert_equal nil, next_deadline, 'nothing is pending'
  end

  # Catches: the same for unload, which ends residency the same way: the
  # image keeps the unsent sends, and the submitted one stays with the
  # system.
  scenario 'a refused send outlives its owner\'s unload' do
    io, sender = DelayedSendsAtOwner.refused(self, name: 'u1')
    image = capture(sender)
    assert_equal 'ok', unload(sender, image)
    assert_equal nil, image_deadline(image), 'the image holds no submitted send; it is not the actor\'s any more'
    assert_equal 10_000, next_deadline, 'the submitted send is still pending, owned by the system'

    run_system('offer the orphaned send again')
    assert_equal ['tick.one'], DelayedSendsAtOwner.delivered(io)
    destroy_image(image)
  end

  # Catches: a destroy that needs memory to keep the submitted send, and so
  # can fail, or leave the actor half destroyed, when memory runs out. The
  # send is already an envelope of core's pool (core ADR 0101): destroy
  # allocates nothing for it, and succeeds with every allocation refused.
  scenario 'a destroy keeps a submitted send without allocating' do
    io, sender = DelayedSendsAtOwner.refused(self)
    allocator.arm_fault(after: 0, count: 1_000_000)
    result = ta.actor.destroy(system: system_handle, actor: sender.handle)[:result]
    allocator.clear_faults
    assert_equal 'ok', result, 'the destroy needed no memory'
    forget(sender)
    assert_equal 10_000, next_deadline, 'its submitted send is still pending'
    run_system('offer the orphaned send again')
    assert_equal ['tick.one'], DelayedSendsAtOwner.delivered(io), 'the send went out'
  end

  # Catches: the same for unload.
  scenario 'an unload keeps a submitted send without allocating for it' do
    io, sender = DelayedSendsAtOwner.refused(self, name: 'u1')
    image = capture(sender)
    assert_equal 'ok', unload(sender, image), 'the unload succeeds'
    assert_equal 10_000, next_deadline, 'the submitted send is still pending, owned by no session'
    run_system('offer the orphaned send again')
    assert_equal ['tick.one'], DelayedSendsAtOwner.delivered(io)
    destroy_image(image)
  end

  # Catches: an operator's withdrawal (ADR 0084) that leaves the send in the
  # global index (it would still fire), removes another owner's send with
  # the same ID, or writes no record. It names the owner and a send ID from
  # inspection, and the rest go out as before.
  scenario 'an operator withdraws one pending send of the actor it names' do
    recording_trace(kinds: %i[withdrawn])
    receiver = spawn DelayedSendsAtOwner.receiver
    to = "#_scxml_#{receiver.handle}"
    a = spawn DelayedSendsAtOwner.sender(:a, [['tick.one', '10s'], ['tick.two', '40s']], to: to)
    b = spawn DelayedSendsAtOwner.sender(:b, [['tick.one', '20s']], to: to)
    one = a.inspect_actor[:delayed_sends].first[:send_id]

    assert_equal 'ok', ta.actor.withdraw_send(system: system_handle, actor: a.handle, send: one)[:result]
    assert_equal [['tick.two', 40_000, false]], DelayedSendsAtOwner.pending(self, a), 'a\'s tick.one is gone'
    assert_equal [['tick.one', 20_000, false]], DelayedSendsAtOwner.pending(self, b), 'b\'s send of that ID stays'
    assert_equal 20_000, next_deadline, 'the owner is re-keyed to its next send'
    assert_equal ["withdrawn send #{one}"], trace_lines(actor: a)
    assert_equal 'err_not_found', ta.actor.withdraw_send(system: system_handle, actor: a.handle, send: one)[:result],
                 'a send withdrawn once is not found again'

    advance_time 60_000
    assert_equal %w[tick.one tick.two], receiver.data('.inbox'), 'b\'s tick.one and a\'s tick.two'
  end

  # Catches: a withdrawal that takes back a send its processor already
  # refused once: it was sent and belongs to its receiver (ADR 0054), so it
  # stays.
  scenario 'a refused send is not withdrawn' do
    io, sender = DelayedSendsAtOwner.refused(self)
    send_id = sender.inspect_actor[:delayed_sends].first[:send_id]
    result = ta.actor.withdraw_send(system: system_handle, actor: sender.handle, send: send_id)[:result]
    assert_equal 'err_done', result, 'a submitted send is done'
    run_system('offer it again')
    assert_equal ['tick.one'], DelayedSendsAtOwner.delivered(io)
  end

  # Catches: a send that fires without advancing its owner's session epoch.
  # The image taken before still holds it, so an unload with that image
  # would be accepted and the load would fire the send a second time. The
  # image is stale (ADR 0014); one taken after holds only the later send.
  scenario 'an image from before a send fired is stale' do
    receiver = spawn DelayedSendsAtOwner.receiver
    to = "#_scxml_#{receiver.handle}"
    chart = DelayedSendsAtOwner.sender(:twice, [['tick.one', '10s'], ['tick.two', '40s']], to: to)
    sender = spawn(chart, name: 'u1')
    before = capture(sender)
    advance_time 10_000
    assert_equal ['tick.one'], receiver.data('.inbox')
    assert_equal 'err_stale', unload(sender, before), 'the image still holds tick.one, which has fired'
    destroy_image(before)

    after = capture(sender)
    assert_equal 40_000, image_deadline(after), 'an image from after holds only tick.two'
    assert_equal 'ok', unload(sender, after)
    assert_equal 'ok', load(sender, after)
    destroy_image(after)
    advance_time 50_000
    assert_equal %w[tick.one tick.two], receiver.data('.inbox'), 'each send fired once'
  end
end
