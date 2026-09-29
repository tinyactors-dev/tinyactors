# frozen_string_literal: true

# Upgrading the chart of a long-lived named actor, as a host that reacts to
# a payment provider's subscriptions does: the subscription itself lives
# outside the actor system, and only the logic that reacts to its events
# changes (docs/manual/persistence.md, "Definition upgrades").
#
# A compiled definition belongs to the host until it registers it (ADR 0043,
# include/tinyactors.h "Registration takes ownership of definition only when
# it returns TA_OK"), and a registered one is the system's to free.
module DefinitionUpgrade
  # Compiles and destroys `chart` once, so the atoms and event names it
  # interns in the system are already there and a later cycle allocates only
  # what the definition itself costs.
  def self.warm(context, chart)
    context.destroy_definition(context.compile_only(chart))
  end

  # The same for a chart the scenario registers: registering also grows the
  # system's definition table, whose slot is reused by the next
  # registration. Warming both leaves the definition itself as the only
  # thing a later cycle allocates.
  def self.warm_registered(context, chart)
    warm(context, chart)
    context.retire_definition(context.register_definition(context.compile_only(chart)))
  end
end

TinyactorsTest.scenarios 'definition ownership', chart: :subscription_v1 do
  # Catches: a ta_definition_destroy that frees nothing or only part of the
  # definition (the byte count stays up, and teardown's leak check fires);
  # a compile that hands the definition to the system before registration
  # (destroying it would then be a double free the allocator reports).
  scenario 'a definition the host never registers is the host\'s to release' do
    DefinitionUpgrade.warm(self, :subscription_v2)
    before = allocator.stats

    definition = compile_only(:subscription_v2)
    compiled = allocator.stats
    assert compiled[:live_bytes] > before[:live_bytes],
           "compiling allocates; live bytes stayed at #{compiled[:live_bytes]}"

    destroy_definition(definition)
    after = allocator.stats
    assert_equal before[:live_bytes], after[:live_bytes], 'destroying the definition returns its bytes'
    assert_equal before[:live_allocations], after[:live_allocations], 'and every allocation it made'
    assert_equal 0, after[:anomalies], 'the allocator saw no free it did not hand out'
  end

  # Catches: a registration that copies the definition instead of taking it
  # (the bytes of the original would stay live); a retire that leaves an
  # unused definition allocated, or that leaves its ID usable (ADR 0043).
  scenario 'registering a definition hands it to the system' do
    DefinitionUpgrade.warm_registered(self, :subscription_v2)
    before = allocator.stats

    definition = register_definition(compile_only(:subscription_v2))
    facts = definition_facts(definition)
    assert_equal 'subscription', facts.dig(:identity, :family, :name)
    assert_equal 0, facts[:actor_count]

    # No actor uses it, so retiring frees it at once — the host destroys
    # nothing, because registration took ownership.
    assert_equal 'ok', retire_definition(definition)
    assert_equal 'err_not_found', ta.definition.inspect(system: system_handle, definition: definition)[:result],
                 'the definition ID is stale once core freed it'
    after = allocator.stats
    assert_equal before[:live_bytes], after[:live_bytes], 'the system freed what registration took'
    assert_equal before[:live_allocations], after[:live_allocations]
  end

  # Catches: a host-side upgrade path that leaks the revision it decided
  # against — the realistic case, where compiling the candidate and
  # checking it is separate from adopting it.
  scenario 'a candidate revision the host rejects costs nothing' do
    DefinitionUpgrade.warm(self, :subscription_v2)
    subscription = spawn
    subscription.deliver 'payment.failed'
    before = allocator.stats

    3.times { destroy_definition(compile_only(:subscription_v2)) }

    assert_equal before[:live_bytes], allocator.stats[:live_bytes], 'three rejected candidates cost nothing'
    assert_active subscription, :past_due
    subscription.deliver 'payment.succeeded'
    assert_active subscription, :active
  end
end

TinyactorsTest.scenarios 'subscription upgrade', chart: :subscription_v1 do
  # Catches: an upgrade that loses the data the new revision is bound to;
  # a retired incumbent revision that is freed while its actor still runs,
  # or that is not freed once that actor is gone (ADR 0043); a replacement
  # that does not take over the logical address.
  scenario 'the chart of a live subscription is upgraded under its address' do
    v1 = definition(:subscription_v1)
    subscription = spawn name: 'sub_1', binding: { account: 'acct_42', failures: 0 }
    subscription.deliver 'payment.failed'
    assert_active subscription, :past_due
    assert_equal 1, subscription.data('.failures')

    # The host compiles and registers the new revision, then swaps the
    # interpreter under the same address, bound to the same data.
    v2 = definition(:subscription_v2)
    replacement = prepare(:subscription_v2, incumbent: subscription, binding: subscription.data)
    assert_equal 'ok', publish(replacement, incumbent: subscription)
    assert_equal ['ok', replacement.handle], find(subscription.address), 'the address names the new interpreter'
    assert_equal 1, replacement.data('.failures'), 'the upgrade keeps what the old revision collected'

    # No new actor may start on v1, while the incumbent keeps running it.
    assert_equal 'ok', retire_definition(v1)
    assert_equal true, definition_facts(v1)[:retired]
    assert_equal 1, definition_facts(v1)[:actor_count]

    subscription.destroy
    assert_equal 'err_not_found', ta.definition.inspect(system: system_handle, definition: v1)[:result],
                 'v1 is freed with its last actor'

    # The new revision reacts as v2: a failed payment now grants a grace
    # period instead of suspending at once.
    replacement.deliver 'payment.failed'
    assert_active replacement, :dunning
    assert_equal v2, replacement.inspect_actor[:definition_id]
    advance_time 3_000
    assert_active replacement, :past_due
  end
end
