# frozen_string_literal: true

# A finished actor reports the final state it reached (ADR 0056).
#
# When a session reaches a top-level <final>, core exits every state, so the
# active configuration is empty and In() is false for everything. What stays
# is ta_actor_inspection.final_state: the top-level <final> whose entry ended
# the session. It is TA_STATE_NONE while the actor runs and while it is
# paused inside a microstep. An invoked child that is cancelled never
# finishes: it is exited and destroyed, and reports no final state before.

# Three possible outcomes, and a nested <final> that is not one of them: it
# ends the compound state `review`, whose done.state event leads on.
TinyactorsTest.chart :outcomes do
  data log: { expr: '[]' }

  state :idle do
    transition on: 'work', target: :idle do
      foreach '[1, 2, 3]', item: 'i' do
        assign '.log', '.log + ["work \(.i)"]'
      end
    end
    transition on: 'review', target: :review
    transition on: 'accept', target: :accepted
    transition on: 'reject', target: :rejected
  end
  state :review, initial: :checking do
    state :checking do
      transition on: 'checked', target: :checked
    end
    final :checked
    transition on: 'done.state.review', target: :timed_out
  end
  final :accepted
  final :rejected
  final :timed_out
end

# A parent whose invoked child is cancelled before it finishes.
TinyactorsTest.chart :outcome_parent do
  state :running do
    invoke chart: :outcomes, id: :worker
    transition on: 'stop', target: :stopped
  end
  state :stopped
end

module FinalState
  NONE = 0xFFFF_FFFF

  # The final state an actor reports, as a symbol, or nil for TA_STATE_NONE.
  def self.of(context, actor)
    state = actor.inspect_actor.fetch(:final_state)
    state == NONE ? nil : context.state_name(actor.definition, state)
  end
end

TinyactorsTest.scenarios 'final state', chart: :outcomes do
  # Catches: a finished actor that reports no final state, the first
  # top-level final instead of the one reached, or the nested final that
  # ended a compound state instead of the one that ended the session.
  scenario 'a finished actor reports which top-level final it reached' do
    accepted = spawn
    accepted.deliver 'accept'
    rejected = spawn
    rejected.deliver 'reject'
    reviewed = spawn
    reviewed.deliver 'review'
    reviewed.deliver 'checked'
    [[accepted, :accepted], [rejected, :rejected], [reviewed, :timed_out]].each do |actor, expected|
      assert actor.done?, "#{actor} finished"
      assert_equal expected, FinalState.of(self, actor)
    end
  end

  # Catches: core keeping the final state active after exitInterpreter, so
  # the configuration, active_state_count or In() would say it is active.
  scenario 'the active configuration is empty after finishing' do
    actor = spawn
    actor.deliver 'reject'
    assert_equal 0, actor.inspect_actor.fetch(:active_state_count)
    assert_equal [], actor.active
    assert_equal :rejected, FinalState.of(self, actor)
  end

  # Catches: a final state reported before the session ends: while running,
  # or while paused inside a microstep (ADR 0061).
  scenario 'there is no final state while the actor runs or is paused' do
    actor = spawn
    assert FinalState.of(self, actor).nil?, 'a running actor has no final state'
    assert_equal 'ok', actor.post('work')
    paused = 0
    while actor.resume(budget: 1000, action_budget: 1) == 'paused'
      paused += 1
      assert FinalState.of(self, actor).nil?, 'a paused actor has not finished'
    end
    assert paused.positive?, 'the action budget paused the actor'
    assert_equal ['work 1', 'work 2', 'work 3'], actor.data('.log')
    assert FinalState.of(self, actor).nil?, 'no final state before the accept event'
    actor.deliver 'accept'
    assert_equal :accepted, FinalState.of(self, actor)
  end

  # Catches: an invoked child that reports a final state it never entered.
  # A cancelled child is exited and destroyed within the parent's microstep,
  # so no inspection sees it afterwards; before that it has no final state,
  # and it never answers done.
  scenario 'a cancelled invoked child never reports a final state' do
    parent = spawn :outcome_parent
    child = invoked_actors.fetch(0)
    assert FinalState.of(self, child).nil?, 'a running child has no final state'
    assert_equal 'ok', parent.post('stop')
    assert_equal 'stable', parent.resume(budget: 1000)
    assert_equal %i[stopped], parent.active
    assert !child.resident?, 'the cancelled child is gone, not finished'
    run_system('after cancelling the child')
    assert_equal [], invoked_actors
  end

  # Catches: a session image that drops the final state, so a finished actor
  # restored by load or reconstruct reports none, or reports another one.
  scenario 'the final state survives capture and restore' do
    actor = spawn name: 'outcome'
    actor.deliver 'reject'
    image = capture(actor)
    binding = actor.data

    reconstructed = core('ta_actor_reconstruct',
                         ta.actor.reconstruct(system: system_handle, image: image,
                                              datamodel_binding: binding)).fetch(:out)
    copy = TinyactorsTest::Actor.new(self, reconstructed, :outcomes, actor.definition)
    assert_equal :rejected, FinalState.of(self, copy), 'reconstruct keeps the final state'
    assert copy.done?, 'the reconstructed actor is finished'
    assert_equal [], copy.active
    core('ta_actor_destroy', ta.actor.destroy(system: system_handle, actor: reconstructed))

    assert_equal 'ok', unload(actor, image)
    assert_equal 'ok', load(actor, image, binding: binding)
    assert_equal :rejected, FinalState.of(self, actor), 'unload and load keep the final state'
    assert actor.done?
    assert_equal 'err_done', actor.post('accept'), 'the loaded actor is still finished'
    destroy_image(image)
  end
end
