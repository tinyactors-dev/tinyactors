# frozen_string_literal: true

module TinyactorsTest
  # Runs a system's actors. Core leaves scheduling to the host (ADR 0011),
  # and in this host the Ruby driver is the host: after every step the
  # scenario runner, the workbench and the debug adapter all come here.
  #
  # One round lists every resident actor with a macrostep in progress,
  # invoked children included, and resumes each until its macrostep ends;
  # settling one can make others runnable, so it lists again. When nothing
  # is runnable it dispatches due delayed sends, and it is done when nothing
  # was dispatched and nothing more is due at the current logical time.
  #
  # Without a monitor that is all it does, which is what the scenario runner
  # and the workbench REPL rely on. A debugger needs finer control, and the
  # Scheduler gives it without changing the order in which work is done:
  #
  # - `hold(id)` leaves an actor out of the loop: it is not resumed, and
  #   whatever it holds waits until `release(id)`;
  # - `stop!` stops the loop between two resumes until `go!`; work that is
  #   left stays pending (`pending?`), and the next `run` continues it;
  # - a monitor chooses budgets per resume, sees every resume and dispatch,
  #   and can stop the loop or interrupt it. See `monitor=`.
  class Scheduler
    # Rounds before a run without a monitor is declared non-quiescent, and
    # resumes before one actor's macrostep is.
    MAX_ROUNDS = 10_000

    # The monitor, or nil. It may answer any of:
    #
    #   budgets(id)          -> {budget:, action_budget:} for this resume, or nil for the defaults
    #   admit(ids)           -> the runnable actors to resume this round (after holds)
    #   resumed(id, status)  -> :stop to stop the loop after this resume
    #   dispatched(due)      -> :stop to stop the loop after this dispatch
    #   interrupt?           -> true to return from `run` now, leaving work pending
    #
    # A run with a monitor has no round limit: a macrostep that never ends
    # runs until the monitor stops or interrupts it, which is what pausing a
    # runaway chart needs.
    attr_accessor :monitor

    # `resume` is called as resume.call(id, budget:, action_budget:) and
    # answers [response, id]: the ta.actor.resume response, and the actor's
    # ID afterwards, which a residency cycle may change.
    def initialize(context, resume:, budget:, action_budget:, dispatch_budget:)
      @context = context
      @resume = resume
      @defaults = { budget: budget, action_budget: action_budget }.freeze
      @dispatch_budget = dispatch_budget
      @held = []
      @queue = []
      @resumes = 0
      @stopped = false
      @pending = false
      @monitor = nil
    end

    # The actors left out of the loop, by runtime ID.
    def held = @held.dup

    def held?(id) = @held.include?(id)

    def hold(id)
      @held << id unless @held.include?(id)
      @queue.delete(id)
    end

    def release(id) = @held.delete(id)

    def stop! = @stopped = true

    def go! = @stopped = false

    def stopped? = @stopped

    # True while a run was stopped or interrupted before the system came to rest.
    def pending? = @pending

    # The actor being settled, when a run stopped or was interrupted between
    # two resumes of its macrostep.
    def current = @queue.first

    # Runs until nothing is left to do at the current logical time. Answers
    # :quiescent, or :stopped or :interrupted with work left pending.
    def run(step)
      @pending = true
      rounds = 0
      loop do
        return :stopped if @stopped
        return :interrupted if @monitor&.interrupt?

        case advance
        when :round
          rounds += 1
          if @monitor.nil? && rounds > MAX_ROUNDS
            raise ScenarioError, "#{step}: the system did not become quiescent in #{MAX_ROUNDS} rounds"
          end
        when :quiescent
          @pending = false
          return :quiescent
        end
      end
    end

    private

    # One piece of work: one resume, one listing, or one dispatch.
    def advance
      return resume_current unless @queue.empty?

      runnable = @context.rpc('system.actors', system: @context.system_handle, runnable: true)
      admitted = runnable - @held
      admitted = @monitor.admit(admitted) if @monitor.respond_to?(:admit)
      return :stopped if @stopped

      unless admitted.empty?
        @queue = admitted
        return :round
      end
      dispatch(blocked: admitted.size < runnable.size)
    end

    # Resumes the actor at the head of the queue once; it leaves the queue
    # when its macrostep ends or it no longer exists. An invoked child may be
    # destroyed while others run, when its parent cancels the invocation.
    def resume_current
      id = @queue.first
      response, id = @resume.call(id, **budgets(id))
      @queue[0] = id
      if response[:result] == 'err_not_found'
        settled
        return :resumed
      end

      status = @context.core('ta_actor_resume', response).fetch(:status)
      raise CoreResultError.new('ta_actor_resume', "status #{status}") unless %w[paused stable done].include?(status)

      if status == 'paused'
        @resumes += 1
        if @monitor.nil? && @resumes >= MAX_ROUNDS
          raise ScenarioError, "actor #{id}: macrostep did not finish in #{MAX_ROUNDS} resumes"
        end
      else
        settled
      end
      @stopped = true if @monitor.respond_to?(:resumed) && @monitor.resumed(id, status) == :stop
      :resumed
    end

    def settled
      @queue.shift
      @resumes = 0
    end

    def budgets(id)
      chosen = @monitor.budgets(id) if @monitor.respond_to?(:budgets)
      chosen || @defaults
    end

    # A due send to an actor that is held, or that the monitor did not
    # admit, stays due: the router answers retry because the actor is busy.
    # Nothing else can move it, so the system is as quiet as it can be.
    def dispatch(blocked:)
      due = @context.core('ta_system_dispatch_due',
                          @context.ta.system.dispatch_due(system: @context.system_handle, budget: @dispatch_budget))
      @stopped = true if @monitor.respond_to?(:dispatched) && @monitor.dispatched(due) == :stop
      return :round if due.fetch(:dispatched).positive?
      return :round if due.fetch(:more_due) && !blocked

      :quiescent
    end
  end
end
