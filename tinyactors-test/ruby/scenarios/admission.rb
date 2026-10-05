# frozen_string_literal: true

# ta_actor_deliver is the admission test (ADR 0055, ADR 0099). A host decides
# what to do with a queued message from deliver's result alone, without
# inspecting the actor first:
#
#   ok           core admitted the event; it runs when the system runs, after
#                the macrostep in progress, if there is one
#   err_full     core's pool is at its admission watermark; keep the message
#                and offer it again once the system has run
#   err_done     the actor reached a top-level final state; drop the message
#   err_not_found no resident actor has this ID; drop or load
#   err_invalid  a caller error only: a malformed event
#
# A finished actor answers done, never invalid, and a malformed event is
# invalid whatever state the actor is in.

# An actor that works through a <foreach> on `work`, so an action budget can
# pause it inside a microstep (ADR 0061), and finishes on `finish`.
TinyactorsTest.chart :admission do
  data log: { expr: '[]' }

  state :idle do
    transition on: 'work', target: :working do
      foreach '[1, 2, 3]', item: 'i' do
        assign '.log', '.log + ["work \(.i)"]'
      end
    end
  end
  state :working do
    transition on: 'finish', target: :end
  end
  final :end
end

# Offers an event by its raw name ID, so a scenario can offer a malformed
# one: the root name, or a name this system never issued.
module Admission
  def self.offer_raw(context, actor, name)
    context.ta.actor.deliver(system: context.system_handle, actor: actor.handle,
                             event: { name: name, type: 'external',
                                      invoke_id: TinyactorsTest::INVOKE_ID_NONE })[:result]
  end
end

TinyactorsTest.scenarios 'admission', chart: :admission do
  # Catches: a finished actor answering invalid (a host could not tell it
  # from its own bug) or busy (a host would keep the message forever), or a
  # refused event that still changes the actor.
  scenario 'a finished actor answers done, and the event changes nothing' do
    actor = spawn
    actor.deliver 'work'
    actor.deliver 'finish'
    assert actor.done?, 'the actor reached its final state'
    before = actor.observe
    assert_equal 'err_done', actor.post('work')
    assert_equal 'err_done', actor.post('finish'), 'done again, for any event'
    assert_equal before, actor.observe, 'a refused event leaves the finished actor unchanged'
    assert_equal ['work 1', 'work 2', 'work 3'], actor.data('.log')
    actor.destroy
    assert_equal 'err_not_found', Admission.offer_raw(self, actor, event_name('work')),
                 'a destroyed actor is not found, not done'
  end

  # Catches: a paused actor whose macrostep takes a second event, or that
  # refuses it, or answers done or invalid while it is still working.
  scenario 'an actor paused inside a microstep admits an event and holds it until the macrostep ends' do
    actor = spawn
    assert_equal 'ok', actor.post('work')
    paused = 0
    while actor.resume(budget: 1000, action_budget: 1) == 'paused'
      paused += 1
      assert_equal 'ok', actor.post('finish') if paused == 1
      assert !actor.done?, 'the held event is not taken inside the macrostep'
    end
    assert paused.positive?, 'the action budget paused the actor at least once'
    assert_equal [:working], actor.active, 'the macrostep ended without the held event'
    assert_equal ['work 1', 'work 2', 'work 3'], actor.data('.log')
    assert_equal 'done', actor.resume(budget: 1000), 'the held event is taken once the macrostep is done'
    assert actor.done?
  end

  # Catches: a malformed event reported as done or busy because of the
  # actor's state; TA_ERR_INVALID from deliver means a caller error only.
  scenario 'a malformed event is invalid, whatever state the actor is in' do
    root = 0
    unknown = 987_654
    running = spawn
    assert_equal 'err_invalid', Admission.offer_raw(self, running, root), 'the root name'
    assert_equal 'err_invalid', Admission.offer_raw(self, running, unknown), 'a name this system never issued'

    busy = spawn
    assert_equal 'ok', busy.post('work')
    assert_equal 'err_invalid', Admission.offer_raw(self, busy, unknown), 'invalid for an actor with an input waiting'

    finished = spawn
    finished.deliver 'work'
    finished.deliver 'finish'
    assert_equal 'err_invalid', Admission.offer_raw(self, finished, root), 'invalid before done'
    assert_equal 'err_done', finished.post('work')
  end
end
