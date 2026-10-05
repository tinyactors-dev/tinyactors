# frozen_string_literal: true

# Session images (ADR 0038, ADR 0039, ADR 0026, ADR 0052): capture records
# the whole interpreter session without changing it; restoring it against the
# exact definition revision and the host's data-model binding continues it as
# if nothing happened, except that invocations are lost and reported as
# error.communication.invoke.<id>.unavailable.
#
# ta_actor_reconstruct() is route-neutral and ta_actor_load() wraps it in the
# named residency protocol. Most continuation scenarios use cycle_residency
# (capture, unload, load); reconstruction is exercised separately where its
# independence from named routing matters.
module ResidencyScenarios
end

TinyactorsTest.scenarios 'residency', chart: :traffic_light do
  # Catches: reconstruction that republishes the captured named actor and
  # steals its address back from a replacement. A later send to the logical
  # address must still resolve to the replacement.
  scenario 'reconstructing a captured incumbent leaves its replacement routed' do
    directory = memory_directory
    incumbent = spawn :freezer, name: 'x'
    incumbent.deliver 'arm'
    image = capture(incumbent)
    binding = incumbent.data

    old_spec = TinyactorsTest.charts[:freezer].to_spec
    new_spec = old_spec.merge(data: old_spec[:data] + [{ name: 'revision', initializer: { expr: '2' } }])
    compiled = core('ta_definition_compile_spec',
                    ta.definition.compile_spec(system: system_handle, spec: new_spec,
                                               datamodel: { '$adapter' => 'datamodel.jq' }, allocator: allocator.adapter))
    new_definition = core('ta_system_register_definition',
                          ta.system.register_definition(system: system_handle,
                                                        definition: compiled.fetch(:out))).fetch(:out)
    address = incumbent.address
    core('ta_actor_unload', { result: unload(incumbent, image) })
    core('ta_actor_remove_unloaded', { result: remove_unloaded(address) })
    replacement_id = core('ta_actor_spawn',
                          ta.actor.spawn(system: system_handle, definition: new_definition,
                                         address: address)).fetch(:out)
    assert_equal ['ok', replacement_id], find(incumbent.address)

    reconstructed = core('ta_actor_reconstruct',
                         ta.actor.reconstruct(system: system_handle, image: image,
                                              datamodel_binding: binding)).fetch(:out)
    assert_equal ['ok', replacement_id], find(incumbent.address),
                 'route-neutral reconstruction does not replace the current actor'

    sender = spawn :messenger, binding: { peer: '#_actor_freezer/x' }
    resolves_before = directory.entries('resolve').length
    sender.deliver 'greet'
    resolves = directory.entries('resolve').drop(resolves_before)
    assert_equal replacement_id, resolves.last.dig(:out, :actor),
                 'a send to the logical address follows the replacement route'

    core('ta_actor_destroy', ta.actor.destroy(system: system_handle, actor: reconstructed))
    destroy_image(image)
  end

  # Catches: capture that mutates the actor (moves its timers out of the
  # heap, clears its queue, advances its epoch observably).
  scenario 'capturing an actor at any step changes nothing' do
    capture_only = ->(context) { context.actors.each { |actor| context.destroy_image(context.capture(actor)) } }
    assert_equivalent_runs(interruptions: { 'capture' => capture_only }) do
      light = spawn
      relay = spawn :relay
      advance_time 30_000
      light.deliver 'fault'
      relay.deliver 'go'
      light.deliver 'repair'
      advance_time 25_000
    end
  end

  # Catches: an image of a paused actor that loses the current external
  # event or the internal queue, so the macrostep restarts, repeats a
  # microstep, or skips `counted`.
  #
  # The relay sends `ping` to itself while it is paused. That mail is
  # waiting for its macrostep to end (core ADR 0099 section 4), and the unload
  # makes it visible, as the session leaves: it names the session, which keeps
  # its ID across the unload (ADR 0066), so once the session is loaded again
  # the mail finds it and runs after the finished macrostep. A *delayed*
  # self-send survives too, because the image carries it and load rebinds it;
  # `named actors: an unloaded actor keeps its address and its timer` covers
  # that.
  scenario 'a macrostep captured while paused continues where it paused' do
    relay = spawn :relay, name: 'r1'
    relay.post('go')
    3.times { assert_equal 'paused', relay.resume(budget: 1) }
    before = relay.observe
    assert_equal 2, before.dig(:data, :n)

    cycle_residency(relay)
    assert_equal before, relay.observe, 'the paused session is restored exactly'
    assert_equal 'go', event_text(relay.inspect_actor.dig(:current_event, :name))
    assert_equal 'paused', relay.resume(budget: 1)
    assert_equal 3, relay.data('.n'), 'the next resume runs the next microstep'
    run_system('finish the macrostep')
    assert_equal ['go', 'n=5', 'counted', 'ping'], relay.data('.log'),
                 'the macrostep ended, and then the mail it had sent itself arrived'
  end

  # Catches: restoration that restarts invocations (a duplicate external
  # effect), rebinds them silently, reports the loss in the wrong order or
  # with the wrong envelope, or still accepts events from a lost service.
  scenario 'restoration reports each lost invocation once, in invocation order' do
    service = scripted_invoker('urn:tinyactors:test:service')
    pair = spawn :service_pair, name: 'p1'
    starts = service.started
    assert_equal 2, starts.size

    cycle_residency(pair)
    run_system('restored')
    assert_equal 2, service.started.size, 'restoration starts no service'
    assert_equal [{ name: 'error.communication.invoke.first.unavailable', type: 'platform', invokeid: nil },
                  { name: 'error.communication.invoke.second.unavailable', type: 'platform', invokeid: nil }],
                 pair.data('.errors')
    assert_equal [], pair.observe[:invokes], 'no invocation is active after restoration'

    # The invocation is retired, so reporting for it names no invocation.
    lost = starts[0]
    report = ta.actor.report_invoke(system: system_handle, actor: pair.handle, invoke_id: lost[:invoke_id],
                                    kind: 'event', event: { name: event_name('reply'), type: 'external',
                                                            invoke_id: lost[:invoke_id] })
    assert_equal 'err_not_found', report[:result]
    run_system('late reply from the lost service')
    assert_equal [], pair.data('.replies'), 'events from a lost invocation are ignored'
  end

  # Catches: restoration against any definition of the same family (a silent
  # reinterpretation of state IDs), or an image that keeps its retired
  # definition alive.
  scenario 'restoration needs the exact definition revision' do
    light = spawn name: 'l1'
    advance_time 10_000
    image = capture(light)
    binding = light.data
    original = definition
    assert_equal 'ok', unload(light, image)
    ta.system.retire_definition(system: system_handle, definition: original)
    assert_equal 'err_not_found', ta.definition.inspect(system: system_handle, definition: original)[:result],
                 'an image does not keep its definition alive'

    spec = TinyactorsTest.charts[:traffic_light].to_spec
    changed = spec.merge(data: spec[:data] + [{ name: 'revision', initializer: { expr: '2' } }])
    register = lambda do |chart_spec|
      compiled = core('ta_definition_compile_spec',
                      ta.definition.compile_spec(system: system_handle, spec: chart_spec,
                                                 datamodel: { '$adapter' => 'datamodel.jq' }, allocator: allocator.adapter))
      core('ta_system_register_definition',
           ta.system.register_definition(system: system_handle, definition: compiled.fetch(:out))).fetch(:out)
    end
    register.call(changed)
    assert_equal 'err_definition_mismatch', ta.actor.load(system: system_handle, image: image,
                                                          datamodel_binding: binding)[:result]

    register.call(spec)
    assert_equal 'ok', load(light, image, binding: binding)
    destroy_image(image)
    assert_equal [{ due: 30_000, event: 'timer' }], light.sends.map { |send| send.slice(:due, :event) }
    advance_time 20_000
    assert_equal %i[operating green], light.active, 'the restored timer fires at its original due time'
    assert_equal({ cycles: 0 }, light.data)
  end

  # Catches: an image that borrows the host values its captured sends carry
  # instead of retaining them (ADR 0050). The session that held them is gone
  # between capture and restoration, so anything the image only borrowed is
  # freed under it: the frozen `{v: 1}` is the one host value in play, and it
  # has to survive its session.
  scenario 'an image retains the host values its captured sends carry' do
    freezer = spawn :freezer, name: 'f1'
    freezer.deliver 'arm'
    assert_equal 2, freezer.data('.v'), 'the send froze v=1 before the assignment'

    cycle_residency(freezer)

    advance_time 10_000
    assert_equal 1, freezer.data('.got'), 'the restored send carried the value captured with it'
  end

  # Catches: an image deadline that is relative, stale, or missing, which a
  # host needs to wake an unloaded actor on time.
  scenario 'an image reports its earliest delayed-send deadline' do
    light = spawn
    relay = spawn :relay
    advance_time 10_000
    light_image = capture(light)
    relay_image = capture(relay)
    assert_equal 30_000, image_deadline(light_image)
    assert_equal nil, image_deadline(relay_image), 'an actor without delayed sends has no deadline'
    destroy_image(light_image)
    destroy_image(relay_image)

    # With several sends it is the earliest, not whichever the image happens
    # to hold first: the sends are scheduled longest-delay first on purpose.
    chart = TinyactorsTest::Chart.define(:three_deadlines) do
      state :ready do
        on_entry do
          send_event 'third', delay: '30s', id: :c
          send_event 'second', delay: '20s', id: :b
          send_event 'first', delay: '15s', id: :a
        end
      end
    end
    many = spawn chart
    image = capture(many)
    assert_equal 3, many.sends.size
    assert_equal 25_000, image_deadline(image), 'the earliest of several deadlines'
    destroy_image(image)
  end
end
