# frozen_string_literal: true

# An invoked child that reaches its final state has to tell its parent
# (`done.invoke.<id>`). Core admits that event like any other (ADR 0099), so
# a parent inside its own macrostep does not refuse it, and nothing is held
# for a retry; the child keeps its completion until its parent has taken the
# event. `docs/internal/actor-lifecycle.md` describes the protocol:
#
#   "An invoked child retains a pending completion until the parent takes
#   it; destruction is busy while the parent invocation remains active. If
#   that invocation was cancelled, reclamation is allowed without
#   manufacturing a done event."
#
# A scenario reaches that window by driving ta_run itself, one microstep at
# a time: the parent takes `finish` (which forwards it to the child), then
# the child's input runs to its end, and the parent's done.invoke waits in
# the run queue.
module InvokedCompletion
  # [parent, child] with the child finished and its completion pending.
  def self.park(context)
    context.instance_exec do
      parent = spawn :caller
      child = invoked_actors.fetch(0)
      assert_equal 'ok', parent.post('finish')
      5.times { break unless parent.resume(budget: 1) == 'paused' }
      5.times do
        break if child.inspect_actor.fetch(:residency) == 'done'

        child.resume(budget: 1)
      end
      assert_equal 'done', child.inspect_actor.fetch(:residency)
      assert child.inspect_actor.fetch(:has_completion), 'the child holds its done data'
      assert_equal [], parent.data('.log').grep(/done/), 'the parent has not taken the completion yet'
      [parent, child]
    end
  end
end

TinyactorsTest.scenarios 'invoked completion' do
  # Catches: a completion dropped before the parent took it, or one held
  # for a retry that core no longer needs. The parent gets its done.invoke
  # from the run queue, and the child is reclaimed after.
  scenario 'a child that finishes before its parent takes the completion is reclaimed after' do
    parent, child = InvokedCompletion.park(self)

    assert_equal [], Array(child.inspect_actor[:delayed_sends]), 'the completion is queued, not held'

    run_system('let the parent take the completion')
    assert_equal ['progress 21', 'done 42'], parent.data('.log')
    assert_equal [], invoked_actors, 'the child is reclaimed once its parent has been told'
  end

  # Catches: reclamation of a child whose parent still expects its
  # completion.
  scenario 'a child with a pending completion cannot be destroyed' do
    _parent, child = InvokedCompletion.park(self)

    assert_equal 'err_busy',
                 ta.actor.destroy(system: system_handle, actor: child.handle)[:result],
                 'destroying a child with a pending completion is busy'
  end
end
