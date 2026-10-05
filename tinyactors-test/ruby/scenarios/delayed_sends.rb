# frozen_string_literal: true

# Delayed sends live with their owner, and a global index orders the owners
# (ADR 0054). Each actor keeps its pending sends in one list sorted by
# (due, send order); the system keeps a heap of the owners that have any,
# keyed by each owner's front send. So:
#
# - inspection lists an actor's own sends, with where each one goes (D4, D5);
# - due sends still go out in one global (due, send order), across owners,
#   and cancelling an owner's front send re-keys it (D2);
# - a send that has fallen due leaves its owner for core's mail (ADR 0099):
#   it is no longer the owner's to withdraw, and destroying or unloading the
#   owner does not take it back.
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

  # The sender's pending sends as [event name, due].
  def self.pending(context, actor)
    Array(actor.inspect_actor[:delayed_sends]).map do |send|
      [context.event_text(send[:event]), send[:due]]
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

  # Catches: an owner heap keyed by something other than each owner's front
  # send (order would follow the owners, not the sends), a tie broken by
  # owner instead of by send order, or a cancel of an owner's front send
  # that leaves the heap keyed by the cancelled send (the owner's other send
  # would go out at the cancelled send's time).
  scenario 'due sends go out in one global order across owners, after a cancel too' do
    receiver = spawn DelayedSendsAtOwner.receiver
    to = "#_scxml_#{receiver.handle}"
    a = spawn DelayedSendsAtOwner.sender(:a, [['tick.a1', '10s'], ['tick.a2', '40s']], to: to)
    spawn DelayedSendsAtOwner.sender(:b, [['tick.b1', '20s'], ['tick.b2', '30s']], to: to)
    spawn DelayedSendsAtOwner.sender(:c, [['tick.c1', '20s'], ['tick.c2', '50s']], to: to)
    assert_equal 10_000, next_deadline, 'a1 is due first'

    a.deliver 'drop'
    assert_equal [['tick.a2', 40_000]], DelayedSendsAtOwner.pending(self, a), 'a1 is cancelled'
    assert_equal 20_000, next_deadline, 'the owner of a1 is re-keyed to a2; b1 is due first now'

    advance_time 60_000
    expected = %w[tick.b1 tick.c1 tick.b2 tick.a2 tick.c2]
    assert_equal expected, receiver.data('.inbox'),
                 'by due time, and b1 before c1 at 20s because it was sent first'
    assert_equal nil, next_deadline, 'nothing is pending'
  end

  # Catches: a send that fell due and was handed to core's mail but stays on
  # its owner's list (it would go out twice, or be withdrawable after it was
  # sent), or one that is lost with its owner: destroy and unload do not take
  # back what was sent.
  scenario 'a send that fell due is the receiver\'s: destroying or unloading its owner does not take it back' do
    receiver = spawn DelayedSendsAtOwner.receiver
    to = "#_scxml_#{receiver.handle}"
    sender = spawn DelayedSendsAtOwner.sender(:s, [['tick.one', '10s'], ['tick.two', '20s']], to: to)
    advance_time 10_000
    assert_equal ['tick.one'], receiver.data('.inbox'), 'the due send was delivered once'
    assert_equal [['tick.two', 20_000]], DelayedSendsAtOwner.pending(self, sender), 'it left its owner\'s list'
    sender.destroy
    assert_equal nil, next_deadline, 'destroying the owner cancels what it still owned'
    advance_time 20_000
    assert_equal ['tick.one'], receiver.data('.inbox')
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
    assert_equal [['tick.two', 40_000]], DelayedSendsAtOwner.pending(self, a), 'a\'s tick.one is gone'
    assert_equal [['tick.one', 20_000]], DelayedSendsAtOwner.pending(self, b), 'b\'s send of that ID stays'
    assert_equal 20_000, next_deadline, 'the owner is re-keyed to its next send'
    # The record names the send by its text, the id attribute the sender gave it (core ADR 0098).
    assert_equal ['withdrawn send tick_one'], trace_lines(actor: a)
    assert_equal 'err_not_found', ta.actor.withdraw_send(system: system_handle, actor: a.handle, send: one)[:result],
                 'a send withdrawn once is not found again'

    advance_time 60_000
    assert_equal %w[tick.one tick.two], receiver.data('.inbox'), 'b\'s tick.one and a\'s tick.two'
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
