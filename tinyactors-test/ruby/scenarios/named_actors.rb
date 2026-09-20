# frozen_string_literal: true

# Named actors (ADRs 0004, 0014, 0022, 0037): a named actor claims a logical
# address (definition family, name) in the host's directory. Unloading is a
# residency change, not termination: the address stays claimed while the
# actor is unloaded, its timers survive in the image, and loading continues
# it under a new runtime ID. Directory updates are compare-and-swaps against
# the expected entry, so a failed step leaves everything as it was.
# Replacement upgrades a named actor to another revision of its family.
#
# Core claims, resolves and releases addresses, and captures and reconstructs
# sessions, so a named actor is routable, replies by address, and can be
# imaged. It does not implement the residency protocol itself yet: every
# scenario that changes residency stops at ta_actor_unload with RPC -32001,
# and replacement stops at ta_actor_prepare_replacement. The code after the
# first such call has not run against a real core; runner_selftest.rb checks
# the helpers it uses.
module NamedActorScenarios
  UNIMPLEMENTED = /: -32001 /

  # The directory operations that change entries, as
  # [op, expected kind, new kind] (resolve is a read and is left out).
  def self.updates(directory)
    directory.journal.filter_map do |entry|
      case entry[:op]
      when 'claim' then ['claim', nil, entry.dig(:entry, :kind), entry[:result]]
      when 'replace' then ['replace', entry.dig(:expected, :kind), entry.dig(:replacement, :kind), entry[:result]]
      when 'remove' then ['remove', entry.dig(:expected, :kind), nil, entry[:result]]
      end
    end
  end
end

TinyactorsTest.scenarios 'named actors', chart: :traffic_light do
  # A send to a logical address needs no named sender. Core parses the
  # address and hands the destination to the external-event router, because
  # the host owns location and residency; here that is the router instrument
  # resolving it through the directory.
  #
  # Catches: a named target that is dropped, delivered to the wrong actor,
  # or resolved by core instead of handed to the router.
  scenario 'a send to a logical address is routed through the host directory' do
    directory = memory_directory
    router = recording_router(directory: true)
    sender = spawn :messenger, binding: { peer: '#_actor_messenger/m2' }
    receiver = spawn :messenger
    directory.put(family: 'messenger', name: 'm1', kind: :resident, actor: sender.handle)
    directory.put(family: 'messenger', name: 'm2', kind: :resident, actor: receiver.handle)

    sender.deliver 'greet'
    # The sender is anonymous here, so its origin is its session URI; the
    # receiver replies to that.
    assert_equal [{ event: 'hello', from: "#_scxml_#{sender.handle}" }], receiver.data('.inbox'),
                 'the named target is resolved and delivered'
    assert_equal [{ event: 'ack', from: "#_scxml_#{receiver.handle}" }], sender.data('.inbox')
    # The hello goes to the address, which the router resolves; the reply
    # goes to the sender's session URI, which needs no resolution.
    assert_equal [[{ kind: 'named', family: { name: 'messenger' }, name: 'm2' },
                   { kind: 'resident', actor: receiver.handle }, 'accepted'],
                  [{ kind: 'runtime', runtime: sender.handle }, nil, 'accepted']],
                 router.entries('submit').map { |entry| entry.values_at(:destination, :resolved, :status) }
  end

  # Catches: a named spawn that does not claim its address, a second claim
  # of the same address that succeeds or disturbs the first actor, or a
  # self reference that is not the logical address.
  scenario 'a named actor claims its address, and the address resolves to it' do
    light = spawn name: 'l1'
    directory = instruments.fetch('directory')
    assert_equal [['claim', nil, 'resident', 'ok']], NamedActorScenarios.updates(directory)
    assert_equal ['ok', light.handle], find(light.address)
    assert_equal({ kind: 'named' }.merge(light.address), light.inspect_actor[:self])

    # The directory refuses the claim of a taken address with err_stale (a
    # compare-and-swap against "no entry"), which the spawn reports.
    second = ta.actor.spawn(system: system_handle, definition: definition, address: light.address)
    assert_equal 'err_stale', second[:result]
    assert_equal [{ family: 'traffic_light', name: 'l1', kind: 'resident', actor: light.handle }],
                 directory.directory_entries
    assert_equal ['ok', light.handle], find(light.address)
  end

  # Catches: unload that terminates the actor or releases its name; timers
  # that fire while the actor is unloaded, are lost, or fire twice; load
  # that reuses the old runtime ID or restores a different configuration.
  expect_failure 'ta_actor_unload is unimplemented', message: NamedActorScenarios::UNIMPLEMENTED
  scenario 'an unloaded actor keeps its address and its timer, and continues after load' do
    router = recording_router
    light = spawn name: 'l1'
    old = light.handle
    advance_time 10_000
    image = capture(light)
    binding = light.data
    assert_equal 'ok', unload(light, image)

    assert_equal 'err_not_found', ta.actor.inspect(system: system_handle, actor: old)[:result]
    assert_equal 'err_unloaded', find(light.address).first
    assert_equal nil, next_deadline, 'an unloaded actor has no timer in the resident heap'
    assert_equal 30_000, image_deadline(image), 'its timer is in the image'
    advance_time 30_000
    assert_equal [], router.entries('submit'), 'nothing fires while the actor is unloaded'

    assert_equal 'ok', load(light, image, binding: binding)
    destroy_image(image)
    run_system('load l1')
    assert light.handle != old, 'load gives the actor a new runtime ID'
    assert_equal ['ok', light.handle], find(light.address)
    assert_equal %i[operating green], light.active, 'the timer due while unloaded fires once, after load'
    assert_equal [['timer', light.handle]],
                 router.entries('submit').map { |entry| [entry.dig(:event, :name_text), entry.dig(:destination, :runtime)] }
    assert_equal [['claim', nil, 'resident', 'ok'], ['replace', 'resident', 'unloaded', 'ok'],
                  ['replace', 'unloaded', 'resident', 'ok']],
                 NamedActorScenarios.updates(instruments.fetch('directory'))
  end

  # Catches: unload that accepts an image captured before the actor changed
  # (the host would checkpoint an old session), or that half-applies.
  expect_failure 'ta_actor_unload is unimplemented', message: NamedActorScenarios::UNIMPLEMENTED
  scenario 'an image captured before a change cannot unload the actor' do
    light = spawn name: 'l1'
    stale = capture(light)
    light.deliver 'fault'
    before = light.observe
    assert_equal 'err_stale', unload(light, stale)
    assert_equal before, light.observe, 'the actor is unchanged and resident'
    assert_equal ['ok', light.handle], find(light.address)
    destroy_image(stale)

    fresh = capture(light)
    assert_equal 'ok', unload(light, fresh)
    destroy_image(fresh)
  end

  # Catches: residency transitions that are not compare-and-swaps (a failed
  # directory update that still unloads or loads the actor), or a failed
  # load that leaves a half-built resident session behind.
  expect_failure 'ta_actor_unload is unimplemented', message: NamedActorScenarios::UNIMPLEMENTED
  scenario 'a failed directory update leaves unload and load retryable' do
    light = spawn name: 'l1'
    directory = instruments.fetch('directory')
    image = capture(light)
    binding = light.data

    directory.arm_fault(:replace, result: 'err_stale')
    assert_equal 'err_stale', unload(light, image), 'unload reports the failed directory swap'
    assert light.resident?, 'the actor stays resident'
    assert_equal ['ok', light.handle], find(light.address)
    assert_equal 'ok', unload(light, image), 'the same image unloads it on retry'

    directory.arm_fault(:replace, result: 'err_stale')
    assert_equal 'err_stale', load(light, image, binding: binding), 'load reports the failed directory swap'
    assert_equal 'err_unloaded', find(light.address).first
    assert_equal [], rpc('system.actors', system: system_handle), 'no half-loaded session stays resident'
    assert_equal 'ok', load(light, image, binding: binding), 'load succeeds on retry'
    destroy_image(image)
    assert_equal %i[operating red], light.active
  end

  # Catches: names released on unload, or never released: an unloaded
  # actor's address must stay reserved until remove_unloaded, then be free.
  expect_failure 'ta_actor_unload is unimplemented', message: NamedActorScenarios::UNIMPLEMENTED
  scenario 'an unloaded actor reserves its name until it is removed' do
    light = spawn name: 'l1'
    image = capture(light)
    assert_equal 'ok', unload(light, image)
    destroy_image(image)
    claim = ta.actor.spawn(system: system_handle, definition: definition, address: light.address)
    assert_equal 'err_stale', claim[:result], 'the address of an unloaded actor is still claimed'

    assert_equal 'ok', remove_unloaded(light.address)
    assert_equal 'err_not_found', find(light.address).first
    successor = spawn name: 'l1'
    assert_equal ['ok', successor.handle], find(light.address)
    # Every directory update, the refused claim of the reserved name included.
    assert_equal [['claim', nil, 'resident', 'ok'], ['replace', 'resident', 'unloaded', 'ok'],
                  ['claim', nil, 'resident', 'err_stale'], ['remove', 'unloaded', nil, 'ok'],
                  ['claim', nil, 'resident', 'ok']],
                 NamedActorScenarios.updates(instruments.fetch('directory'))
  end

  # Catches: destroy that leaves the directory entry behind (the name would
  # stay taken by a dead actor) or removes it without comparing.
  scenario 'destroying a named actor releases its address' do
    light = spawn name: 'l1'
    light.destroy
    assert_equal 'err_not_found', find(light.address).first
    assert_equal [], instruments.fetch('directory').directory_entries
    assert_equal [['claim', nil, 'resident', 'ok'], ['remove', 'resident', nil, 'ok']],
                 NamedActorScenarios.updates(instruments.fetch('directory'))
    spawn name: 'l1'
  end

  # Catches: a named sender whose events carry its runtime ID as origin, so
  # a reply would go to a session that is gone after the next unload; a
  # named target core cannot resolve; an origin a chart cannot reply to.
  #
  # The address URI is the one the host's data models parse,
  # #_actor_<family>/<name>, so `to: $_event.origin` is a reply.
  scenario 'named actors exchange events by address, and reply to the origin' do
    router = recording_router(directory: true)
    sender = spawn :messenger, name: 'm1', binding: { peer: '#_actor_messenger/m2' }
    receiver = spawn :messenger, name: 'm2'
    sender.deliver 'greet'

    assert_equal [{ event: 'hello', from: '#_actor_messenger/m1' }], receiver.data('.inbox')
    assert_equal [{ event: 'ack', from: '#_actor_messenger/m2' }], sender.data('.inbox'),
                 'the reply went to the address the hello came from'
    assert_equal [[{ kind: 'named', family: { name: 'messenger' }, name: 'm2' },
                   { kind: 'resident', actor: receiver.handle }, 'accepted'],
                  [{ kind: 'named', family: { name: 'messenger' }, name: 'm1' },
                   { kind: 'resident', actor: sender.handle }, 'accepted']],
                 router.entries('submit').map { |entry| entry.values_at(:destination, :resolved, :status) }
  end

  # Catches: a send to an unloaded actor that is silently dropped or
  # delivered to a stale runtime ID instead of reaching the sender as a
  # communication error it can act on.
  expect_failure 'ta_actor_unload is unimplemented', message: NamedActorScenarios::UNIMPLEMENTED
  scenario 'a send to an unloaded actor reaches the sender as a communication error' do
    router = recording_router(directory: true)
    sender = spawn :messenger, name: 'm1', binding: { peer: '#_actor_messenger/m2' }
    receiver = spawn :messenger, name: 'm2'
    image = capture(receiver)
    binding = receiver.data
    assert_equal 'ok', unload(receiver, image)

    sender.deliver 'greet'
    assert_equal [['unloaded', 'error']],
                 router.entries('submit').map { |entry| [entry.dig(:resolved, :kind), entry[:status]] }
    reported = sender.data('.inbox')
    assert_equal 1, reported.size, "the sender was told once, got #{reported}"
    # ADR 0027: the name may carry a detail suffix under this prefix.
    assert reported[0][:event].start_with?('error.communication'),
           "the send failure is a communication error, got #{reported[0][:event]}"

    assert_equal 'ok', load(receiver, image, binding: binding)
    destroy_image(image)
    sender.deliver 'greet'
    assert_equal [{ event: 'hello', from: '#_actor_messenger/m1' }], receiver.data('.inbox'),
                 'the reloaded actor receives at the same address'
    assert_equal({ event: 'ack', from: '#_actor_messenger/m2' }, sender.data('.inbox').last)
  end

  # Catches: unload/load that is not invisible at some point of a run: a
  # lost or duplicated timer, history, or internal event (ADR 0014).
  expect_failure 'ta_actor_unload is unimplemented', message: NamedActorScenarios::UNIMPLEMENTED
  scenario 'unloading and loading at any step is invisible' do
    cycle = ->(context) { context.actors.each { |actor| context.cycle_residency(actor) } }
    assert_equivalent_runs(interruptions: { 'unload and load' => cycle }) do
      light = spawn name: 'l1'
      relay = spawn :relay, name: 'r1'
      advance_time 30_000
      light.deliver 'fault'
      relay.deliver 'go'
      light.deliver 'repair'
      advance_time 25_000
    end
  end
end

TinyactorsTest.scenarios 'replacement', chart: :counter_v1 do
  # Catches: a replacement that is routable, or whose bootstrap effects
  # escape, before activation; activation that is not a compare-and-swap
  # of the incumbent's entry; an incumbent destroyed later that removes the
  # replacement's route; a replacement that does not keep the bound data.
  expect_failure 'ta_actor_unload is unimplemented', message: NamedActorScenarios::UNIMPLEMENTED
  scenario 'a replacement takes over the address only when activated' do
    router = recording_router
    incumbent = spawn name: 'c1'
    incumbent.deliver 'tick'
    incumbent.deliver 'tick'
    assert_equal 2, incumbent.data('.count')

    replacement = prepare_replacement(incumbent, :counter_v2, binding: incumbent.data)
    run_system('prepare the replacement')
    assert_equal false, replacement.inspect_actor[:published]
    assert_equal 'err_busy', ta.actor.capture(system: system_handle, actor: replacement.handle)[:result]
    assert_equal ['ok', incumbent.handle], find(incumbent.address), 'the incumbent keeps the route'
    assert_equal [], router.entries('submit'), 'the bootstrap `hello` is held until activation'

    assert_equal 'ok', activate_replacement(incumbent, replacement)
    run_system('activate the replacement')
    assert_equal ['ok', replacement.handle], find(incumbent.address)
    assert_equal({ count: 2, greeted: true }, replacement.data)
    assert incumbent.resident?, 'the incumbent stays until it is destroyed'

    incumbent.destroy
    assert_equal ['ok', replacement.handle], find(incumbent.address), 'destroying the incumbent keeps the new route'
    replacement.deliver 'tick'
    assert_equal 12, replacement.data('.count')
    # Preparing a replacement touches no entry; activation swaps the
    # incumbent's, and destroying the incumbent afterwards changes nothing.
    assert_equal [['claim', nil, 'resident', 'ok'], ['replace', 'resident', 'resident', 'ok']],
                 NamedActorScenarios.updates(instruments.fetch('directory'))
  end
end
