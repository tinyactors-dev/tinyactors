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
# and replacement stops at ta_actor_prepare. The code after the
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
  # address, asks the host's directory which session holds it (the host owns
  # location and residency), and queues the mail for that session (core ADR
  # 0099 section 4); here the directory is the memory_directory instrument.
  #
  # Catches: a named target that is dropped, delivered to the wrong actor,
  # or resolved by core instead of by the directory.
  scenario 'a send to a logical address is routed through the host directory' do
    directory = memory_directory
    sender = spawn :messenger, binding: { peer: '#_actor_messenger/m2' }
    receiver = spawn :messenger
    directory.put(family: 'messenger', name: 'm1', kind: :resident, actor: sender.handle)
    directory.put(family: 'messenger', name: 'm2', kind: :resident, actor: receiver.handle)

    sender.deliver 'greet'
    # The sender is anonymous here, so its origin is its session URI; the
    # receiver replies to that.
    assert_equal [{ event: 'hello', from: sender.session_uri }], receiver.data('.inbox'),
                 'the named target is resolved and delivered'
    assert_equal [{ event: 'ack', from: receiver.session_uri }], sender.data('.inbox')
    # The hello goes to the address, which the directory resolves; the reply
    # goes to the sender's session URI, which needs no resolution.
    assert_equal [['m2', 'resident', receiver.handle]],
                 directory.entries('resolve').map { |entry| [entry[:name], entry.dig(:out, :kind), entry.dig(:out, :actor)] }
  end

  # The same routing with no host code. Core places mail for a resident
  # named address itself, so a named spawn is enough for one actor to address
  # another; a host only answers LOAD_NEEDED when the address is not resident.
  #
  # Catches: a named destination refused, resolved to the wrong actor, or a
  # resident address reported as missing.
  scenario 'core resolves a logical address to the resident actor' do
    sender = spawn :messenger, name: 'm1', binding: { peer: '#_actor_messenger/m2' }
    receiver = spawn :messenger, name: 'm2'

    sender.deliver 'greet'
    assert_equal [{ event: 'hello', from: '#_actor_messenger/m1' }], receiver.data('.inbox'),
                 'core resolved the named target'
    assert_equal [{ event: 'ack', from: '#_actor_messenger/m2' }], sender.data('.inbox'),
                 'the reply went back to the named origin'
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
  scenario 'an unloaded actor keeps its address and its timer, and continues after load' do
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
    assert_equal [], load_notices, 'nothing fires while the actor is unloaded'

    assert_equal 'ok', load(light, image, binding: binding)
    destroy_image(image)
    run_system('load l1')
    assert light.handle != old, 'load gives the actor a new runtime ID'
    assert_equal ['ok', light.handle], find(light.address)
    assert_equal %i[operating green], light.active, 'the timer due while unloaded fires once, after load'
    assert_equal [['claim', nil, 'resident', 'ok'], ['replace', 'resident', 'unloaded', 'ok'],
                  ['replace', 'unloaded', 'resident', 'ok']],
                 NamedActorScenarios.updates(instruments.fetch('directory'))
  end

  # Catches: unload that accepts an image captured before the actor changed
  # (the host would checkpoint an old session), or that half-applies.
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
    directory = memory_directory
    sender = spawn :messenger, name: 'm1', binding: { peer: '#_actor_messenger/m2' }
    receiver = spawn :messenger, name: 'm2'
    sender.deliver 'greet'

    assert_equal [{ event: 'hello', from: '#_actor_messenger/m1' }], receiver.data('.inbox')
    assert_equal [{ event: 'ack', from: '#_actor_messenger/m2' }], sender.data('.inbox'),
                 'the reply went to the address the hello came from'
    assert_equal [['m2', receiver.handle], ['m1', sender.handle]],
                 directory.entries('resolve').map { |entry| [entry[:name], entry.dig(:out, :actor)] }
  end

  # Catches: a send to an unloaded actor that is silently dropped or
  # delivered to a stale runtime ID instead of reaching the sender as a
  # communication error it can act on.
  scenario 'a send to an unloaded actor reaches the sender as a communication error' do
    sender = spawn :messenger, name: 'm1', binding: { peer: '#_actor_messenger/m2' }
    receiver = spawn :messenger, name: 'm2'
    image = capture(receiver)
    binding = receiver.data
    assert_equal 'ok', unload(receiver, image)

    sender.deliver 'greet'
    assert_equal ['messenger/m2'], loaded_addresses,
                 'core asked the host to load the address; the host cannot'
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
  scenario 'unloading and loading at any step is invisible' do
    cycle = ->(context) { context.actors.each { |actor| context.cycle_residency(actor) } }
    # A residency change *is* two directory swaps, so comparing them would
    # only restate what the interruption did. Everything else — the actors,
    # their timers, and every other instrument — still has to match. Core
    # writes to the directory only from spawn, unload, load and destroy, so
    # nothing a chart does can hide here.
    ignore_swaps = lambda do |state|
      journals = state[:journals].to_h do |name, entries|
        [name, name == 'directory' ? entries.reject { |entry| entry[:op] == 'replace' } : entries]
      end
      state.merge(journals: journals)
    end
    assert_equivalent_runs(interruptions: { 'unload and load' => cycle }, normalize: ignore_swaps) do
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
  scenario 'a replacement takes over the address only when activated' do
    incumbent = spawn name: 'c1'
    incumbent.deliver 'tick'
    incumbent.deliver 'tick'
    assert_equal 2, incumbent.data('.count')

    replacement = prepare(:counter_v2, incumbent: incumbent, binding: incumbent.data)
    run_system('prepare the replacement')
    assert_equal false, replacement.inspect_actor[:published]
    assert_equal 'err_busy', ta.actor.capture(system: system_handle, actor: replacement.handle)[:result]
    assert_equal ['ok', incumbent.handle], find(incumbent.address), 'the incumbent keeps the route'

    assert_equal 'ok', publish(replacement, incumbent: incumbent)
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

  scenario 'a refused activation leaves the incumbent routed and is retryable' do
    incumbent = spawn name: 'c1'
    replacement = prepare(:counter_v2, incumbent: incumbent, binding: incumbent.data)
    directory = instruments.fetch('directory')

    directory.arm_fault(:replace, result: 'err_stale')
    assert_equal 'err_stale', publish(replacement, incumbent: incumbent)
    assert_equal ['ok', incumbent.handle], find(incumbent.address)
    assert_equal false, replacement.inspect_actor[:published]

    assert_equal 'ok', publish(replacement, incumbent: incumbent)
    run_system('retry replacement activation')
    assert_equal ['ok', replacement.handle], find(incumbent.address)
    assert_equal({ count: 0, greeted: true }, replacement.data)
  end

  scenario 'bootstrap logs, timers, and invocations wait for activation' do
    log_sink = recording_log
    service = scripted_invoker('urn:tinyactors:test:replacement')
    incumbent = spawn name: 'c1'
    replacement = prepare(:counter_effects_v2, incumbent: incumbent)

    assert_equal [], log_sink.logged
    assert_equal [], service.started
    assert_equal 'err_not_found', ta.system.next_deadline(system: system_handle)[:result]

    assert_equal 'ok', publish(replacement, incumbent: incumbent)
    assert_equal [['revision', 'v2']], log_sink.logged
    assert_equal 1000, next_deadline
    assert_equal [], service.started, 'the invocation remains queued until the activated actor runs'

    run_system('release the replacement invocation')
    assert_equal 1, service.started.size
    advance_time 1000
    assert_equal true, replacement.data('.ready')
  end
end
