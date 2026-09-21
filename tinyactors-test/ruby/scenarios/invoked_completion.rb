# frozen_string_literal: true

# An invoked child that reaches its final state has to tell its parent
# (`done.invoke.<id>`), and the parent may be busy with its own macrostep
# when that happens. `docs/internal/actor-lifecycle.md` describes the
# protocol:
#
#   "An invoked child retains a pending completion while its parent is
#   busy; ta_actor_publish_completion() retries publication, and
#   destruction is busy while the parent invocation remains active. If that
#   invocation was cancelled, publication returns TA_ERR_NOT_FOUND, retires
#   the pending completion, and allows reclamation without manufacturing a
#   done event."
#
# The header adds: "Retries an invoked child's completion after
# TA_ERR_BUSY."
#
# A scenario reaches that window by driving the macrosteps itself: the
# parent accepts `finish` and runs one microstep (which forwards it to the
# child), then the child runs to its end while the parent is still inside
# its own macrostep.
module InvokedCompletion
  # [parent, child] with the child finished and its completion pending.
  def self.park(context)
    context.instance_exec do
      parent = spawn :caller
      child = invoked_actors.fetch(0)
      assert_equal 'ok', parent.post('finish')
      assert_equal 'paused', parent.resume(budget: 1)
      child.resume(budget: 64)
      assert_equal 'done', child.inspect_actor.fetch(:residency)
      assert child.inspect_actor.fetch(:has_completion), 'the child holds its done data'
      assert parent.inspect_actor.fetch(:macrostep_in_progress), 'the parent is still busy'
      [parent, child]
    end
  end
end

TinyactorsTest.scenarios 'invoked completion' do
  # Catches: a completion dropped because the parent was busy when the
  # child finished, or one delivered into a parent mid-macrostep. The child
  # keeps the completion and the delivery it could not make, and the parent
  # gets its done.invoke once it is idle again.
  scenario 'a child that finishes while its parent is busy keeps its completion' do
    parent, child = InvokedCompletion.park(self)

    assert_equal 1, child.inspect_actor.fetch(:delayed_send_count),
                 'the undelivered completion is held for a retry'
    assert_equal [], parent.data('.log').grep(/done/), 'the busy parent has not been told yet'

    run_system('let the parent finish its macrostep')
    assert_equal ['progress 21', 'done 42'], parent.data('.log')
    assert_equal [], invoked_actors, 'the child is reclaimed once its parent has been told'
  end

  # Catches: a retry that delivers the completion twice, or that fails
  # because the completion is already in flight. Publishing again while the
  # parent is still busy is the documented retry and must be harmless.
  scenario 'publishing again while the parent is busy is harmless' do
    parent, child = InvokedCompletion.park(self)

    assert_equal 'ok', publish_completion(child)
    assert_equal 'ok', publish_completion(child), 'the retry is idempotent'
    assert_equal 1, child.inspect_actor.fetch(:delayed_send_count), 'still one pending delivery'

    run_system('let the parent finish its macrostep')
    assert_equal ['progress 21', 'done 42'], parent.data('.log'),
                 'the parent was told once, however often the host retried'
  end

  # Catches: publication for an actor that is not a finished invoked child.
  scenario 'only a finished invoked child has a completion to publish' do
    parent, = InvokedCompletion.park(self)
    assert_equal 'err_invalid', publish_completion(parent), 'the parent is nobody\'s invoked child'
    run_system('settle')

    lonely = spawn :traffic_light
    assert_equal 'err_invalid', publish_completion(lonely), 'a running actor has no completion'
    assert_equal 'err_not_found', publish_completion(999_999), 'an unknown ID names no actor'
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
