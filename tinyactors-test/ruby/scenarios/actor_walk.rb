# frozen_string_literal: true

# ta_system_visit_actors (ADR 0062): the enumeration an admin endpoint,
# debugger, or metrics collector walks the resident actors with
# (docs/manual/inspection.md). Its contract is in include/tinyactors.h:
# every resident actor once, invoked children included, with the same view
# ta_actor_inspect() gives, in increasing position order; the walk stops at
# the first visit that does not answer TA_OK and returns that result.
#
# One call walks everything from position 0, or one page: it starts at a
# position, visits at most `limit` actors, and reports where the next page
# starts. A position is the actor's slot, the low 32 bits of its ID;
# actor_visit_end means nothing is left. Unlike a callback, a position can
# cross RPC, so the paging scenarios walk page by page from the test driver.
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

    assert_equal 'ok', visit_actors.first
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
    assert_equal 'err_busy', visit_actors.first
    assert_equal 2, inspector.visits.size, 'one visit answered ok, the failing one stopped the walk'
    assert_equal %w[ok err_busy], inspector.visits.map { |entry| entry[:result] }

    # The refused walk changed nothing: the next one sees every actor.
    assert_equal 'ok', visit_actors.first
    assert_equal 3, inspector.visits.size - 2
  end

  # Catches: a walk over stale actor slots — a destroyed actor's slot is
  # reused, and visiting it would show a freed or foreign interpreter
  # (ADR 0028: a destroyed actor's ID never names another actor).
  scenario 'a destroyed actor is not visited' do
    inspector = recording_inspector
    keeper = spawn :relay
    doomed = spawn :relay

    assert_equal 'ok', visit_actors.first
    assert_equal [doomed.handle, keeper.handle].sort, inspector.visits.map { |entry| entry[:actor] }.sort

    doomed.destroy
    assert_equal 'ok', visit_actors.first
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
  # Catches: pages that skip or repeat actors at page boundaries, a limit
  # that is off by one, or a walk out of position order.
  scenario 'pages of any size give the full walk, in position order' do
    12.times { spawn :relay }
    2.times { spawn } # callers, each with an invoked child

    assert_equal 'ok', visit_actors.first
    full = recording_inspector.visits.map { |entry| entry[:actor] }
    assert_equal 16, full.size, 'fourteen spawned actors and two invoked children'
    ordered = full.sort_by { |id| actor_position(id) }
    assert_equal ordered, full, 'a walk of everything is in position order'

    [1, 7, full.size, full.size + 5].each do |limit|
      visited, cursors = walk_in_pages(limit)
      assert_equal full, visited, "pages of #{limit} visit the same actors in the same order"
      pages = cursors.size
      assert pages >= (full.size + limit - 1) / limit, "#{pages} pages of #{limit}"
      assert_equal actor_visit_end, cursors.last
      cursors[0...-1].each_with_index do |cursor, index|
        last_of_page = visited[[(index + 1) * limit, visited.size].min - 1]
        assert_equal actor_position(last_of_page) + 1, cursor,
                     "page #{index} ends right after the last actor it visited"
      end
    end
  end

  # Catches: a cursor that visits an actor twice after slots are reused, a
  # walk that visits an actor destroyed before it got there, or one that
  # misses an actor spawned ahead of it.
  scenario 'changes between pages follow the position' do
    actors = Array.new(10) { spawn :relay }
    inspector = recording_inspector

    result, cursor = visit_actors(inspector, from: 0, limit: 4)
    assert_equal 'ok', result
    first_page = inspector.visits.map { |entry| entry[:actor] }
    assert_equal 4, first_page.size

    # Behind the cursor: destroy a visited actor and spawn into its slot.
    behind = actors.find { |actor| actor.handle == first_page.first }
    behind.destroy
    reborn_behind = spawn :relay
    assert_equal actor_position(behind.handle), actor_position(reborn_behind.handle),
                 'spawn reuses the freed slot'
    assert actor_position(reborn_behind.handle) < cursor

    # Ahead of the cursor: destroy an actor not yet visited, and spawn into
    # its slot again.
    ahead = actors.reverse.find { |actor| actor_position(actor.handle) >= cursor }
    ahead_position = actor_position(ahead.handle)
    ahead.destroy
    reborn_ahead = spawn :relay
    assert_equal ahead_position, actor_position(reborn_ahead.handle)

    # A new slot at the end of the table.
    fresh = spawn :relay

    rest_start = inspector.visits.size
    until cursor == actor_visit_end
      result, cursor = visit_actors(inspector, from: cursor, limit: 3)
      assert_equal 'ok', result
    end
    rest = inspector.visits.drop(rest_start).map { |entry| entry[:actor] }

    assert !rest.include?(reborn_behind.handle), 'spawned behind the cursor: not visited'
    assert !rest.include?(ahead.handle), 'destroyed before the walk reached it: not visited'
    assert rest.include?(reborn_ahead.handle), 'the actor resident when the walk reached the slot'
    assert rest.include?(fresh.handle), 'spawned ahead of the cursor: visited'
    visited = first_page + rest
    assert_equal visited.map { |id| actor_position(id) }.uniq, visited.map { |id| actor_position(id) },
                 'no position is visited twice'
    untouched = actors.map(&:handle) - [behind.handle, ahead.handle]
    untouched.each { |id| assert_equal 1, visited.count(id), "actor #{id} resident throughout: visited once" }
  end

  # Catches: a failed visit whose actor the next page skips, or a cursor that
  # does not point at it.
  scenario 'a visit that fails ends the page at its actor' do
    5.times { spawn :relay }
    inspector = recording_inspector
    inspector.arm_fault(after: 2, result: 'err_busy')

    result, cursor = visit_actors(inspector, from: 0, limit: 10)
    assert_equal 'err_busy', result
    visits = inspector.visits
    assert_equal %w[ok ok err_busy], visits.map { |entry| entry[:result] }
    refused = visits.last[:actor]
    assert_equal actor_position(refused), cursor, 'next is the refused actor'

    result, = visit_actors(inspector, from: cursor, limit: 1)
    assert_equal 'ok', result
    assert_equal refused, inspector.visits.last[:actor], 'the next page offers it again'
  end

  # Catches: children grouped under their parent, or missing from the walk.
  # Spawn reuses freed slots last-freed first, so a parent can take a later
  # slot than its child.
  scenario 'invoked children are visited at their own positions' do
    low = spawn :relay
    high = spawn :relay
    spawn :relay # keeps the table from shrinking to the freed slots
    low_position = actor_position(low.handle)
    high_position = actor_position(high.handle)
    low.destroy
    high.destroy

    parent = spawn # takes the last freed slot, `high`'s
    child = parent.inspect_actor.fetch(:invokes).first.fetch(:service_handle)
    assert_equal high_position, actor_position(parent.handle)
    assert_equal low_position, actor_position(child), 'the child took the slot freed first'

    visited, = walk_in_pages(2)
    assert visited.include?(child), "visited"
    assert visited.index(child) < visited.index(parent.handle), 'the child comes before its parent'
  end

  # Catches: a walk of an empty system that loops or fails, a start past the
  # end that is not the end, and bad arguments that leave `next` unset.
  scenario 'an empty system, a start past the end, and bad arguments' do
    inspector = recording_inspector
    assert_equal ['ok', actor_visit_end], visit_actors(inspector, from: 0, limit: 5)
    assert_equal ['ok', actor_visit_end], visit_actors(inspector, from: actor_visit_end, limit: 5)
    spawn :relay
    assert_equal ['ok', actor_visit_end], visit_actors(inspector, from: 1_000_000, limit: 0)
    assert_equal 0, inspector.visits.size, 'nothing visited'

    response = ta.system.visit_actors(system: system_handle, from: 3, limit: 1)
    assert_equal 'err_invalid', response[:result], 'no inspector'
    assert_equal 3, response.dig(:out, :next), 'next is left at from'
  end
end
