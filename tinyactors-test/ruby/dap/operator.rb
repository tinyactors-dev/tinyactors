# frozen_string_literal: true

module DAP
  # What an operator does to an actor that its chart did not prepare for:
  # jump it to states (core ADR 0082, an operator transition), abandon a
  # macrostep that never ends (ADR 0082), and withdraw one pending delayed
  # send (ADR 0084). The console's `goto`, `abandon` and `cancel` and the
  # DAP `goto` request come here.
  class Operator
    # The event an operator transition is delivered with: `_event` in the
    # handlers it runs, so an <onentry> can tell a jump from a chart's
    # transition.
    EVENT = 'operator.goto'

    def initialize(session:, labels:, control:)
      @session = session
      @labels = labels
      @control = control
    end

    def context = @session.context

    # Delivers a jump of `thread` to the states named `targets`, from the
    # active state `from` or the root, and runs its macrostep: the system
    # stops with reason `goto` when that macrostep ends, as after a step.
    # Answers the console's text.
    def jump(thread, targets, from: nil)
      raise RequestError, 'goto <state…> [from <state>]' if targets.empty?

      definition = thread.definition
      ids = targets.map { |name| state!(definition, name) }
      source = from ? state!(definition, from) : TinyactorsTest::STATE_NONE
      data = { 'targets' => targets.map(&:to_s) }
      data['from'] = from.to_s if from
      event = { name: context.event_name(EVENT), type: 'external', invoke_id: TinyactorsTest::INVOKE_ID_NONE,
                data: data }
      result = context.ta.actor.deliver(system: context.system_handle, actor: thread.handle, event: event,
                                        transition: { source: source, targets: ids })[:result]
      refuse_jump(thread, targets, from, result) unless result == 'ok'

      @control.start_goto(thread)
      "#{thread.label}: goto #{targets.join(' ')}#{from ? " from #{from}" : ''}"
    end

    # Abandons `thread`'s macrostep. One stopped inside a microstep first
    # finishes that microstep (a resume with a microstep budget of 1 and no
    # action budget), since core abandons only between microsteps.
    def abandon(thread)
      view = @session.inspect_actor(thread.handle) or raise RequestError, "#{thread.label} is not resident"
      raise RequestError, "#{thread.label} has no macrostep to abandon" unless view[:macrostep_in_progress]

      finished = view[:microstep_in_progress] && finish_microstep(thread)
      return "#{thread.label} finished its macrostep while finishing its microstep" if finished

      result = context.ta.actor.abandon(system: context.system_handle, actor: thread.handle)[:result]
      case result
      when 'ok'
        @control.collect
        "#{thread.label}: abandoned its macrostep; it is idle in its last configuration"
      when 'err_busy' then raise RequestError, "#{thread.label} has not finished its bootstrap; continue it first"
      when 'err_done' then raise RequestError, "#{thread.label} is done: its session ended"
      else raise RequestError, "ta_actor_abandon answered #{result}"
      end
    end

    # Withdraws `thread`'s pending delayed send `send`, the ID the Timers
    # scope shows.
    def withdraw(thread, text)
      send = Integer(text, exception: false)
      raise RequestError, 'cancel <send ID>: the number the Timers scope shows' if send.nil?

      result = context.ta.actor.withdraw_send(system: context.system_handle, actor: thread.handle, send: send)[:result]
      case result
      when 'ok' then "#{thread.label}: withdrew send #{send}"
      when 'err_not_found' then raise RequestError, "#{thread.label} has no pending send #{send}"
      when 'err_done'
        raise RequestError, "send #{send} was offered to its target once and waits to be offered again; " \
                            'it belongs to its receiver now'
      when 'err_invalid' then raise RequestError, "#{send} names no send"
      else raise RequestError, "ta_actor_withdraw_send answered #{result}"
      end
    end

    # Every state of a definition, as [ID, name], for goto targets and
    # completions.
    def states(definition) = @labels.states(definition).map { |id, info| [id, info[:id]] }

    private

    def state!(definition, name)
      found = states(definition).find { |_, state| state == name.to_s }
      raise RequestError, "#{name} is not a state of this chart" if found.nil?

      found.first
    end

    def refuse_jump(thread, targets, from, result)
      case result
      when 'err_busy'
        raise RequestError, "#{thread.label} is inside a macrostep, or holds an event it has not begun: " \
                            'continue it, or `abandon` the macrostep first'
      when 'err_invalid'
        raise RequestError, "#{targets.join(' ')} cannot be entered together" \
                            "#{from ? ", or #{from} is not active" : ''}"
      when 'err_done' then raise RequestError, "#{thread.label} is done: its session ended"
      else raise RequestError, "ta_actor_deliver answered #{result}"
      end
    end

    def finish_microstep(thread)
      response = context.ta.actor.resume(system: context.system_handle, actor: thread.handle, budget: 1,
                                         action_budget: 0)
      status = context.core('ta_actor_resume', response).fetch(:status)
      @control.collect
      status != 'paused'
    end
  end
end
