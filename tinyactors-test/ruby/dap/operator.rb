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

    def initialize(labels:, control:, mailbox:)
      @labels = labels
      @control = control
      @mailbox = mailbox
    end

    # A jump of `thread` to the states named `targets`, from the active
    # state `from` or the root. With the system stopped and the actor at
    # rest with nothing waiting for it, it is delivered now and its
    # macrostep runs: the system stops with reason `goto` when that
    # macrostep ends, as after a step. Otherwise it waits in the actor's
    # mailbox behind what is there, as napi's adapter queues it, and the
    # actor takes it when it next rests with the system running. Answers
    # the console's text.
    def jump(thread, targets, from: nil)
      raise RequestError, 'goto <state…> [from <state>]' if targets.empty?

      definition = thread.definition
      [*targets, *from].each { |name| state!(definition, name) }
      text = "#{thread.title}: goto #{targets.join(' ')}#{from ? " from #{from}" : ''}"
      view = thread.view
      if @control.stopped? && view && !view[:macrostep_in_progress] && @mailbox.mail_for(thread).empty?
        result = deliver_jump(thread, targets, from)
        raise RequestError, jump_refusal(thread, targets, from, result) unless result == 'ok'

        @control.start_goto(thread)
        return text
      end
      "#{text} queued#{@mailbox.post(thread, goto: { targets: targets.map(&:to_s), from: from&.to_s })}"
    end

    # Delivers a jump to `thread` now (core ADR 0082). Answers core's
    # result: `ok`, or why it was refused (jump_refusal says it in words).
    def deliver_jump(thread, targets, from)
      context = thread.context
      definition = thread.definition
      ids = targets.map { |name| state!(definition, name) }
      source = from ? state!(definition, from) : TinyactorsTest::STATE_NONE
      data = { 'targets' => targets.map(&:to_s) }
      data['from'] = from.to_s if from
      event = { name: context.event_name(EVENT), type: 'external', invoke_id: TinyactorsTest::INVOKE_ID_NONE,
                data: data }
      context.ta.actor.deliver(system: context.system_handle, actor: thread.handle, event: event,
                               transition: { source: source, targets: ids })[:result]
    end

    def jump_refusal(thread, targets, from, result)
      case result
      when 'err_busy'
        "#{thread.title} is inside a macrostep, or holds an event it has not begun: continue it, or `abandon` " \
          'the macrostep first'
      when 'err_invalid'
        "#{targets.join(' ')} cannot be entered together#{from ? ", or #{from} is not active" : ''}"
      when 'err_done' then "#{thread.title} is done: its session ended"
      else "ta_actor_deliver answered #{result}"
      end
    end

    # Abandons `thread`'s macrostep. One stopped inside a microstep first
    # finishes that microstep (a resume with a microstep budget of 1 and no
    # action budget), since core abandons only between microsteps.
    def abandon(thread)
      view = thread.view or raise RequestError, "#{thread.title} is not resident"
      raise RequestError, "#{thread.title} has no macrostep to abandon" unless view[:macrostep_in_progress]

      macrostep = view.dig(:step, :macrostep)
      finished = view[:microstep_in_progress] && finish_microstep(thread)
      return "#{thread.title} finished its macrostep while finishing its microstep" if finished

      context = thread.context
      result = context.ta.actor.abandon(system: context.system_handle, actor: thread.handle)[:result]
      case result
      when 'ok'
        @control.collect
        # A system stopped in that macrostep stops again where the actor
        # now rests.
        @control.restopped(thread, 'pause', 'its macrostep was abandoned') if @control.stopped?
        "#{thread.title}: abandoned macrostep #{macrostep}; it keeps the configuration after its last completed " \
          'microstep'
      when 'err_busy' then raise RequestError, "#{thread.title} has not finished its bootstrap; continue it first"
      when 'err_done' then raise RequestError, "#{thread.title} is done: its session ended"
      else raise RequestError, "ta_actor_abandon answered #{result}"
      end
    end

    # Withdraws `thread`'s pending delayed send whose ID is `text`, as the
    # Timers scope shows it and a chart holds it (ADR 0048): the author's
    # ID, or `#_send.<n>`.
    def withdraw(thread, text)
      raise RequestError, 'cancel <send ID>, as the Timers scope shows it' if text.to_s.empty?

      context = thread.context
      send = context.send_id(text)
      raise RequestError, "#{thread.title} has no pending send #{text}" if send.nil?

      result = context.ta.actor.withdraw_send(system: context.system_handle, actor: thread.handle, send: send)[:result]
      case result
      when 'ok' then "#{thread.title}: withdrew send #{text}"
      when 'err_not_found' then raise RequestError, "#{thread.title} has no pending send #{text}"
      when 'err_done'
        raise RequestError, "send #{text} was offered to its target once and waits to be offered again; " \
                            'it belongs to its receiver now'
      when 'err_invalid' then raise RequestError, "#{text} names no send"
      else raise RequestError, "ta_actor_withdraw_send answered #{result}"
      end
    end

    # Every state of a definition, as [ID, name], for goto targets and
    # completions.
    def states(definition) = @labels.states(definition).map { |id, info| [id, info[:id]] }

    # The events a definition's transitions name, for completions.
    def events(definition)
      (0...@labels.count(definition, :transition)).flat_map do |id|
        @labels.transition_info(definition, id)&.dig(:event).to_s.split(/\s+/)
      end.uniq.sort
    end

    private

    def state!(definition, name)
      found = states(definition).find { |_, state| state == name.to_s }
      raise RequestError, "#{name} is not a state of the chart" if found.nil?

      found.first
    end

    def finish_microstep(thread)
      context = thread.context
      response = context.ta.actor.resume(system: context.system_handle, actor: thread.handle, budget: 1,
                                         action_budget: 0)
      status = context.core('ta_actor_resume', response).fetch(:status)
      @control.collect
      status != 'paused'
    end
  end
end
