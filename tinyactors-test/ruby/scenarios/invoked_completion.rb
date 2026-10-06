# frozen_string_literal: true

# An invoked child that reaches its final state has to tell its parent
# (`done.invoke.<id>`), and the parent may be busy with its own macrostep
# when that happens. `docs/internal/actor-lifecycle.md` describes the
# protocol: the child retains a pending completion until its parent's pool
# admits it; a parent inside a macrostep admits it and takes it after that
# macrostep; "ta_actor_publish_completion() retries publication, and
# destruction is busy while the parent invocation remains active."
#
# A scenario reaches that window by driving the macrosteps itself: the
# parent accepts `finish` and runs one microstep, which autoforwards it to
# the child, and pauses. Mail a macrostep sends waits until that macrostep
# ends (core ADR 0099), so the host posts `finish` to the child itself, and
# the child runs to its end while the parent is still inside its own
# macrostep. The forwarded `finish` reaches the child once the parent's
# macrostep ends, finished by then, and is refused.
module InvokedCompletion
  # [parent, child] with the child finished and its completion pending.
  def self.park(context)
    context.instance_exec do
      parent = spawn :caller
      child = invoked_actors.fetch(0)
      assert_equal 'ok', parent.post('finish')
      assert_equal 'paused', parent.resume(budget: 1)
      assert_equal 'ok', child.post('finish')
      child.resume(budget: 64)
      assert_equal 'done', child.inspect_actor.fetch(:residency)
      assert child.inspect_actor.fetch(:has_completion), 'the child holds its done data'
      assert parent.inspect_actor.fetch(:macrostep_in_progress), 'the parent is still busy'
      [parent, child]
    end
  end

  # Eight sends to itself, due at 1s: falling due, they fill a pool of
  # eight envelopes until it takes them.
  def self.filler
    TinyactorsTest::Chart.define(:filler) do
      state :filling do
        on_entry do
          (1..8).each { |index| send_event 'fill', delay: '1s', id: "f#{index}" }
        end
      end
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

    assert_equal [], Array(child.inspect_actor[:delayed_sends]),
                 'the completion was admitted for the parent: nothing waits for a retry'
    assert_equal [], parent.data('.log').grep(/done/), 'the busy parent has not taken it yet'

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
    assert_equal [], Array(child.inspect_actor[:delayed_sends]), 'nothing waits for a retry'
    assert_equal [], parent.data('.log').grep(/done/), 'the busy parent has not taken it yet'

    run_system('let the parent finish its macrostep')
    assert_equal ['progress 21', 'done 42'], parent.data('.log'),
                 'the parent was told once, however often the host retried'
  end

  # Catches: a completion lost when the parent's pool refused it, or one
  # told twice when the host publishes it again. In a pool of eight, the
  # parent's paused input, its forwarded `finish`, the child's `finish` and
  # five of the filler's sends, fallen due, leave no envelope free: the
  # child's completion finds no room (TA_ERR_FULL), and the child keeps it
  # as one submitted delayed send; publishing it while the pool is still
  # full (another of the filler's sends took the envelope the child's
  # input gave back) keeps it so, and publishing after a run that made room
  # admits it, once.
  scenario 'a completion the parent\'s pool refused is published again after a run' do
    system_params(pool_envelopes: 8)
    spawn InvokedCompletion.filler
    parent = spawn :caller
    child = invoked_actors.fetch(0)
    assert_equal 'ok', parent.post('finish')
    assert_equal 'paused', parent.resume(budget: 1)
    assert_equal 'ok', child.post('finish')
    set_time(1000)
    due = core('ta_system_dispatch_due', ta.system.dispatch_due(system: system_handle, budget: 100))
    assert_equal [5, true], due.values_at(:dispatched, :more_due), 'five of the filler\'s sends fill the pool'
    child.resume(budget: 64)
    assert_equal 'done', child.inspect_actor.fetch(:residency)
    assert parent.inspect_actor.fetch(:macrostep_in_progress), 'the parent is still busy'

    held = Array(child.inspect_actor[:delayed_sends])
    assert_equal 1, held.size, 'the refused completion is held for a retry'
    assert_equal true, held.first[:submitted], 'the held completion waits to be offered again'
    # The child's input, ended, freed its envelope: the filler's sixth send takes it.
    due = core('ta_system_dispatch_due', ta.system.dispatch_due(system: system_handle, budget: 1))
    assert_equal [1, true], due.values_at(:dispatched, :more_due), 'the pool is full again'
    assert_equal 'err_full', parent.post('noise'), 'admission is closed: the pool is past its watermark'
    assert_equal 'ok', publish_completion(child), 'publishing into the full pool keeps it'
    assert_equal 1, Array(child.inspect_actor[:delayed_sends]).size, 'still one pending delivery'
    assert_equal [], parent.data('.log').grep(/done/), 'the busy parent has not been told'

    assert_equal 'stable', parent.resume(budget: 64), 'the parent ends its macrostep and gives its input back'
    assert_equal 'ok', publish_completion(child)
    assert_equal [], Array(child.inspect_actor[:delayed_sends]), 'admitted: nothing waits for a retry'
    run_system('let the parent take it')
    assert_equal ['progress 21', 'done 42'], parent.data('.log'), 'the parent was told exactly once'
    assert_equal [], invoked_actors, 'the child is reclaimed once its parent has been told'
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
