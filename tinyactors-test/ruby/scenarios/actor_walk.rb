# frozen_string_literal: true

# ta_system_visit_actors: the enumeration an admin endpoint, debugger, or
# metrics collector walks the resident actors with (docs/manual/inspection.md).
# Its contract is in include/tinyactors.h: every resident actor once,
# invoked children included, with the same view ta_actor_inspect() gives;
# the walk stops at the first visit that does not answer TA_OK and returns
# that result; the order is not part of the contract.
#
# The scenarios use an inspector.recording instrument, so what core handed
# the callback is readable afterwards.
TinyactorsTest.scenarios 'actor walk', chart: :caller do
  # Catches: a walk that skips invoked children (the population an admin
  # endpoint would miss), visits an actor twice, or hands the callback a
  # view that differs from the one ta_actor_inspect answers with.
  scenario 'the walk visits every resident actor once, children included' do
    inspector = recording_inspector
    parent = spawn
    bystander = spawn :relay

    # The population the walk must produce, taken from somewhere else: the
    # two actors the scenario spawned, plus the child the parent's own
    # inspection reports as the service of its live invocation.
    invokes = parent.inspect_actor.fetch(:invokes)
    assert_equal 1, invokes.size, 'the parent has one live invocation'
    child = invokes.first.fetch(:service_handle)
    expected = [parent.handle, bystander.handle, child].sort

    assert_equal 'ok', visit_actors
    visited = inspector.visits.map { |entry| entry[:actor] }
    assert_equal expected, visited.sort, 'every resident actor, the invoked child included'
    assert_equal visited.uniq, visited, 'no actor is visited twice'

    inspector.visits.each do |entry|
      view = core('ta_actor_inspect', ta.actor.inspect(system: system_handle, actor: entry[:actor])).fetch(:out)
      assert_equal view, entry[:view], "the view of actor #{entry[:actor]}"
    end
  end

  # Catches: a walk that keeps visiting after a failing visit, that swallows
  # the inspector's result, or that leaves the system unwalkable afterwards.
  scenario 'a visitor that fails stops the walk and answers its result' do
    inspector = recording_inspector
    spawn :relay
    spawn :relay
    spawn :relay

    inspector.arm_fault(after: 1, result: 'err_busy')
    assert_equal 'err_busy', visit_actors
    assert_equal 2, inspector.visits.size, 'one visit answered ok, the failing one stopped the walk'
    assert_equal %w[ok err_busy], inspector.visits.map { |entry| entry[:result] }

    # The refused walk changed nothing: the next one sees every actor.
    assert_equal 'ok', visit_actors
    assert_equal 3, inspector.visits.size - 2
  end

  # Catches: a walk over stale actor slots — a destroyed actor's slot is
  # reused, and visiting it would show a freed or foreign interpreter
  # (ADR 0028: a destroyed actor's ID never names another actor).
  scenario 'a destroyed actor is not visited' do
    inspector = recording_inspector
    keeper = spawn :relay
    doomed = spawn :relay

    assert_equal 'ok', visit_actors
    assert_equal [doomed.handle, keeper.handle].sort, inspector.visits.map { |entry| entry[:actor] }.sort

    doomed.destroy
    assert_equal 'ok', visit_actors
    assert_equal [keeper.handle], inspector.visits.drop(2).map { |entry| entry[:actor] }
  end

  # An inspection view reports an actor's invocations as `ta_invoke_id`s, and
  # ta_system_invoke_id_text says which invocation each one is (ADR 0049) -
  # the author's `<invoke id>`, the same text `done.invoke.<id>` carries and
  # `#_<invokeid>` spells. Without it a host can list invocations but cannot
  # name them.
  #
  # Catches: an ID rendered off by the offset it carries, or from the wrong
  # table, which would give a debugger the name of an unrelated atom - and a
  # view that reports the invocations of a state in the wrong order.
  scenario 'an inspected invocation says which one it is' do
    scripted_invoker('urn:tinyactors:test:service')
    caller = spawn :service_pair

    invokes = caller.inspect_actor.fetch(:invokes)
    assert_equal 2, invokes.size, 'both invocations of the state are live'
    names = invokes.map do |invoke|
      core('ta_system_invoke_id_text',
           ta.system.invoke_id_text(system: system_handle, invoke: invoke.fetch(:id))).fetch(:out)
    end
    assert_equal %w[first second], names, 'the author IDs, in document order'

    # An ID this system never issued is a question, not a failure.
    assert_equal 'err_not_found',
                 ta.system.invoke_id_text(system: system_handle, invoke: 987_654)[:result]
  end
end
