# frozen_string_literal: true

module TinyactorsTest
  # Runs a system's actors. Core leaves scheduling to the host (ADR 0011),
  # and in this host the Ruby driver is the host: after every step the
  # scenario runner and the workbench come here.
  #
  # One round lists every resident actor with a macrostep in progress,
  # invoked children included, and resumes each until its macrostep ends;
  # settling one can make others runnable, so it lists again. When nothing
  # is runnable it dispatches due delayed sends, and it is done when nothing
  # was dispatched and nothing more is due at the current logical time.
  class Scheduler
    # Rounds before a run is declared non-quiescent, and resumes before one
    # actor's macrostep is.
    MAX_ROUNDS = 10_000

    # `resume` is called as resume.call(id, budget:, action_budget:,
    # pause_at_microstep_start:) and answers [response, id]: the
    # ta.actor.resume response, and the actor's ID afterwards, which a
    # residency cycle may change.
    def initialize(context, resume:, budget:, action_budget:, dispatch_budget:, pause_at_microstep_start: false)
      @context = context
      @resume = resume
      @budgets = { budget: budget, action_budget: action_budget,
                   pause_at_microstep_start: pause_at_microstep_start }.freeze
      @dispatch_budget = dispatch_budget
      @queue = []
      @resumes = 0
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

    # One piece of work: one resume, one listing, or one dispatch.
    def advance
      return resume_current unless @queue.empty?

      runnable = @context.rpc('system.actors', system: @context.system_handle, runnable: true)
      unless runnable.empty?
        @queue = runnable
        return :round
      end
      dispatch
    end

    # Resumes the actor at the head of the queue once; it leaves the queue
    # when its macrostep ends or it no longer exists. An invoked child may be
    # destroyed while others run, when its parent cancels the invocation.
    def resume_current
      id = @queue.first
      response, id = @resume.call(id, **@budgets)
      @queue[0] = id
      if response[:result] == 'err_not_found'
        settled
        return :resumed
      end

      status = @context.core('ta_actor_resume', response).fetch(:status)
      raise CoreResultError.new('ta_actor_resume', "status #{status}") unless %w[paused stable done].include?(status)

      if status == 'paused'
        @resumes += 1
        raise ScenarioError, "actor #{id}: macrostep did not finish in #{MAX_ROUNDS} resumes" if @resumes >= MAX_ROUNDS
      else
        settled
      end
      :resumed
    end

    def settled
      @queue.shift
      @resumes = 0
    end

    def dispatch
      due = @context.core('ta_system_dispatch_due',
                          @context.ta.system.dispatch_due(system: @context.system_handle, budget: @dispatch_budget))
      return :round if due.fetch(:dispatched).positive? || due.fetch(:more_due)

      :quiescent
    end
  end
end
