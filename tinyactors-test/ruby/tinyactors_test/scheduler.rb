# frozen_string_literal: true

module TinyactorsTest
  # Runs a system's inputs. Core queues what it is given and runs it in the
  # order it was admitted (core ADR 0099), and in this host the Ruby driver
  # is the host: after every step the scenario runner and the workbench come
  # here.
  #
  # One round is one ta_run call, with the budgets the context chose. Core
  # takes the next admitted input (a bootstrap, a delivered event, mail a
  # macrostep sent) and runs it until its macrostep ends or a budget stops
  # it; a stopped input keeps the one running slot, so the next call
  # continues it. When ta_run answers "idle" nothing is runnable: the driver
  # dispatches due delayed sends, and it is done when nothing was dispatched
  # and nothing more is due at the current logical time.
  class Scheduler
    # Rounds before a run is declared non-quiescent, and calls that end in a
    # budget stop before one input is.
    MAX_ROUNDS = 10_000

    # `run` is called as run.call(budget:, action_budget:,
    # pause_at_microstep_start:) and answers the ta.run response.
    def initialize(context, run:, budget:, action_budget:, dispatch_budget:, pause_at_microstep_start: false)
      @context = context
      @run = run
      @budgets = { budget: budget, action_budget: action_budget,
                   pause_at_microstep_start: pause_at_microstep_start }.freeze
      @dispatch_budget = dispatch_budget
      @stops = 0
    end

    # Runs until nothing is left to do at the current logical time.
    def run(step)
      rounds = 0
      loop do
        case advance
        when :round
          rounds += 1
          raise ScenarioError, "#{step}: the system did not become quiescent in #{MAX_ROUNDS} rounds" if rounds > MAX_ROUNDS
        when :quiescent
          return
        end
      end
    end

    private

    # One piece of work: one ta_run call, or one dispatch.
    def advance
      status = @context.core('ta_run', @run.call(**@budgets)).fetch(:status)
      case status
      when 'idle'
        @stops = 0
        dispatch
      when 'paused'
        @stops += 1
        raise ScenarioError, "an input did not finish in #{MAX_ROUNDS} calls" if @stops >= MAX_ROUNDS

        :round
      when 'more', 'notices_full'
        @stops = 0
        :round
      else
        raise CoreResultError.new('ta_run', "status #{status}")
      end
    end

    def dispatch
      due = @context.core('ta_system_dispatch_due',
                          @context.ta.system.dispatch_due(system: @context.system_handle, budget: @dispatch_budget))
      return :round if due.fetch(:dispatched).positive? || due.fetch(:more_due)

      :quiescent
    end
  end
end
