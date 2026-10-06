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
end

# A child that, on `finish`, sends its parent more events than the pool
# has envelopes, and ends with done data. Its mail waits on its macrostep's
# outgoing list until the macrostep ends (core ADR 0099), so the pool is
# full when its completion is published.
TinyactorsTest.chart :crowding_callee do
  state :working do
    transition on: 'finish', target: :finished do
      (1..12).each { send_event 'noise', to: '#_parent' }
    end
  end

  final :finished do
    done_data params: { result: '42' }
  end
end

# A parent that forwards `finish` to its child and logs its child's done.
TinyactorsTest.chart :crowding_caller do
  data log: { expr: '[]' }

  state :calling do
    invoke chart: :crowding_callee, id: :worker, autoforward: true
    transition on: 'done.invoke.worker', target: :done do
      assign '.log', '.log + ["done \($_event.data.result)"]'
    end
  end

  final :done
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

  # Catches: a completion lost, or told twice, when the parent's pool is
  # full as the child finishes. An invoked child holds its invocation token,
  # one envelope, from its spawn (core ADR 0101): on `finish` the child
  # sends its parent more mail than the pool holds (the send that finds no
  # envelope fails as error.execution.capacity.pool), and its completion
  # still reaches the parent, once, through the token.
  scenario 'a completion reaches its parent through the child\'s token when the pool is full' do
    system_params(pool_envelopes: 8)
    recording_trace
    parent = spawn :crowding_caller
    child = invoked_actors.fetch(0)
    assert_equal 'ok', parent.post('finish')
    run_system('the child fills the pool and finishes')
    raised = trace_records.select { |record| record[:kind] == 'raised' }
                          .map { |record| record.dig(:event, :name_text) }
    assert raised.include?('error.execution.capacity.pool'), "the child filled the pool: #{raised}"
    assert_equal ['done 42'], parent.data('.log'), 'the parent was told, through the child\'s token'
    assert_equal 'err_not_found', publish_completion(child), 'told once, the child has nothing left to publish'
    run_system('nothing more to tell')
    assert_equal ['done 42'], parent.data('.log'), 'the parent was told exactly once'
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
