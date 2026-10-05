# frozen_string_literal: true

# Actor and definition lifecycle: destroy is terminal and cancels what the
# actor owns, and only that (invariants 10 and 12, ADR 0014, ADR 0016);
# delayed-send arguments are frozen when the send runs (invariant 9); a
# retired definition keeps its resident actors running and is freed with
# the last one (ADR 0043); a finished actor keeps its completion until
# destroyed. The teardown leak check applies to every scenario here.
TinyactorsTest.scenarios 'lifecycle', chart: :traffic_light do
  # Catches: destroy leaving the actor's timer armed (its `timer` would be
  # submitted to a dead ID when it falls due); destroy cancelling by send ID
  # instead of by owner (both lights use the ID `light`, so the survivor's
  # timer would vanish); a destroyed actor's ID answering again.
  scenario 'destroy cancels the actor\'s own delayed sends and no one else\'s' do
    router = recording_router
    doomed = spawn
    advance_time 10_000
    survivor = spawn
    assert_equal 30_000, next_deadline
    doomed.destroy

    assert !doomed.resident?, 'a destroyed actor is gone'
    assert_equal 'err_not_found', doomed.post('fault')
    assert_equal [{ due: 40_000, event: 'timer' }], survivor.sends.map { |send| send.slice(:due, :event) }
    assert_equal 40_000, next_deadline, 'only the survivor\'s timer is pending'

    advance_time 30_000
    assert_active survivor, :green
    destinations = router.entries('submit').map { |entry| entry.dig(:destination, :runtime) }
    assert_equal [survivor.handle], destinations.uniq, 'nothing was routed to the destroyed actor'
  end

  # Catches: runtime IDs reused after destroy (ADR 0028): an event meant for
  # the old actor would reach the new one.
  scenario 'a destroyed actor\'s ID never names another actor' do
    first = spawn
    first.destroy
    second = spawn
    assert first.handle != second.handle, 'the new actor has a new ID'
    assert_equal 'err_not_found', first.post('fault')
    assert_configuration second, :operating, :red
  end

  # Catches: <cancel> matching send IDs across actors (invariant 10).
  scenario 'an actor cannot cancel another actor\'s delayed send' do
    light = spawn
    canceller = spawn :canceller
    canceller.deliver 'cancel_it'
    assert_equal [{ due: 30_000, event: 'timer' }], light.sends.map { |send| send.slice(:due, :event) }
    advance_time 30_000
    assert_active light, :green
  end

  # Catches: a delayed send that stores expressions and evaluates them when
  # it falls due (invariant 9): it would carry 2 or 12, not 1.
  scenario 'delayed-send data is fixed when the send runs' do
    freezer = spawn :freezer
    freezer.deliver 'arm'
    assert_equal [{ due: 10_000, event: 'fire', data: { v: 1 } }],
                 freezer.sends.map { |send| send.slice(:due, :event, :data) }
    freezer.deliver 'bump'
    advance_time 10_000
    assert_equal({ v: 12, got: 1 }, freezer.data)
    assert_equal [], freezer.sends
  end

  # Catches: destroy that frees the actor but leaves its services running
  # (ADR 0016): the scripted service would see no cancel, and the SCXML
  # child would stay resident.
  scenario 'destroying a parent cancels its invocations' do
    service = scripted_invoker('urn:tinyactors:test:service')
    client = spawn :service_client
    caller = spawn :caller
    started = service.started
    assert_equal 1, started.size
    assert_equal 3, rpc('system.actors', system: system_handle).size, 'the SCXML child is resident'

    client.destroy
    assert_equal [[started[0][:service], true]],
                 service.entries('cancel').map { |entry| entry.values_at(:service, :known) }
    caller.destroy
    assert_equal [], rpc('system.actors', system: system_handle), 'the SCXML child went with its parent'
  end

  # Catches: destroy of a busy actor that leaks its paused macrostep (the
  # current event, the internal queue) or the input its self-send queued
  # behind it, or crashes when that input is taken for the dead ID. Core
  # admitted the send while the actor was paused (ADR 0099) and discards it
  # with a REFUSED notice when the actor is destroyed.
  scenario 'destroying a paused actor releases it and the input queued behind it' do
    router = recording_router
    relay = spawn :relay
    bystander = spawn :relay
    relay.post('go')
    3.times { assert_equal 'paused', relay.resume(budget: 1) }
    relay.destroy

    submits = router.entries('submit').map { |entry| entry.values_at(:status, :result) }
    assert_equal [%w[accepted ok]], submits
    run_system('discard the queued input')
    assert_equal [bystander.handle], rpc('system.actors', system: system_handle)
    bystander.deliver 'go'
    assert_equal ['go', 'n=5', 'counted', 'ping'], bystander.data('.log')
  end

  # Catches: retire that stops or frees a definition resident actors still
  # use; retire that does not stop new spawns; a definition never freed
  # after its last actor is gone (the leak check would report it).
  scenario 'a retired definition keeps its actors running and is freed with the last one' do
    light = spawn
    id = definition
    assert_equal 'ok', ta.system.retire_definition(system: system_handle, definition: id)[:result]
    assert_equal 'err_not_found', ta.actor.spawn(system: system_handle, definition: id)[:result]
    facts = core('ta_definition_inspect', ta.definition.inspect(system: system_handle, definition: id)).fetch(:out)
    assert_equal [true, 1], facts.values_at(:retired, :actor_count)

    advance_time 30_000
    assert_active light, :green
    light.destroy
    assert_equal 'err_not_found', ta.definition.inspect(system: system_handle, definition: id)[:result]
    assert_equal 'ok', ta.system.retire_definition(system: system_handle, definition: id)[:result],
                 'retiring a freed definition is still idempotent'
  end

  # Catches: a finished actor that accepts events, or loses its done data
  # before the host destroys it.
  scenario 'a finished actor refuses events and keeps its completion until destroyed' do
    caller = spawn :caller
    caller.deliver 'finish'
    assert caller.done?, 'the caller reached its final state'
    assert_equal 'err_done', caller.post('finish'), 'a finished actor answers done, not invalid (ADR 0055)'
    assert_equal ['progress 21', 'done 42'], caller.data('.log')
    caller.destroy
    assert_equal 'err_not_found', ta.actor.get_completion(system: system_handle, actor: caller.handle)[:result]
  end

  # Catches: history recorded at the wrong time (on entry, or not replaced
  # on a later exit) or read from the wrong region; ta_actor_history
  # disagreeing with the state history actually restores.
  scenario 'history records the last active child on each exit and restores it' do
    light = spawn
    assert_equal [], light.history(:resume), 'nothing is recorded before the first exit'
    advance_time 30_000
    light.deliver 'fault'
    assert_equal [:green], light.history(:resume)
    light.deliver 'repair'
    assert_active light, :green
    advance_time 25_000
    light.deliver 'fault'
    assert_equal [:yellow], light.history(:resume)
    assert_equal({ value: now }, ta.system.time(system: system_handle))
  end
end
