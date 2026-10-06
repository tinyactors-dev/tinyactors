# frozen_string_literal: true

module TinyactorsTest
  # Runs a system's actors. Core leaves scheduling to the host (ADR 0011),
  # and in this host the Ruby driver is the host: after every step the
  # scenario runner and the workbench come here.
  #
  # Core holds every input it admitted on its run queue, invoked children's
  # included, and ta_run takes them in arrival order. The scheduler calls it
  # until it answers idle; then it dispatches due delayed sends and answers
  # the load requests core owes for mail to targets that are not resident
  # (Context#answer_loads), and it is done when nothing was dispatched,
  # nothing more is due at the current logical time, and no request was
  # owed.
  class Scheduler
    # Calls before a run is declared non-quiescent, and calls that leave one
    # macrostep paused before it is.
    MAX_ROUNDS = 10_000

    # `run` is called as run.call(budget:, action_budget:,
    # pause_at_microstep_start:) and answers the ta.run response of one
    # logical call.
    def initialize(context, run:, budget:, action_budget:, dispatch_budget:, pause_at_microstep_start: false)
      @context = context
      @run = run
      @budgets = { budget: budget, action_budget: action_budget,
                   pause_at_microstep_start: pause_at_microstep_start }.freeze
      @dispatch_budget = dispatch_budget
    end

    # Runs until nothing is left to do at the current logical time.
    def run(step)
      rounds = 0
      pauses = 0
      loop do
        status = @context.core('ta_run', @run.call(**@budgets)).fetch(:status)
        case status
        when 'paused'
          pauses += 1
          raise ScenarioError, "#{step}: a macrostep did not finish in #{MAX_ROUNDS} calls" if pauses >= MAX_ROUNDS

          next
        when 'more'
          pauses = 0
        when 'idle'
          pauses = 0
          return settled(step) if dispatch == :quiescent
        else
          raise CoreResultError.new('ta_run', "status #{status}")
        end
        rounds += 1
        raise ScenarioError, "#{step}: the system did not become quiescent in #{MAX_ROUNDS} rounds" if rounds > MAX_ROUNDS
      end
    end

    private

    # Cross-checks core's idle answer at quiescence: every session left in a
    # macrostep is one a fault stopped, which waits for its release; any
    # other has lost the input or the wake that would run it.
    def settled(step)
      runnable = @context.rpc('system.actors', system: @context.system_handle, runnable: true)
      lost = runnable - @context.stopped_sessions
      return if lost.empty?

      raise ScenarioError, "#{step}: quiescent, but #{lost.inspect} still in a macrostep nothing will run"
    end

    def dispatch
      due = @context.core('ta_system_dispatch_due',
                          @context.ta.system.dispatch_due(system: @context.system_handle, budget: @dispatch_budget))
      return :round if due.fetch(:dispatched).positive? || due.fetch(:more_due)
      return :round if @context.answer_loads.positive?

      :quiescent
    end
  end
end
