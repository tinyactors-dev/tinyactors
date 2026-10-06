# frozen_string_literal: true

module TinyactorsTest
  class AssertionFailed < StandardError; end
  class ScenarioError < StandardError; end

  # Describes why a scenario is expected to fail. A scenario that fails for any
  # other reason is an ordinary failure, and one that passes is an unexpected
  # pass, so the marker cannot hide new bugs.
  class ExpectedFailure
    attr_reader :reason, :rpc_error, :rpc_method, :message

    def initialize(reason, rpc_error: nil, rpc_method: nil, message: nil)
      if rpc_error.nil? && message.nil?
        raise ScenarioError, "expect_failure #{reason.inspect}: give rpc_error: or message:"
      end
      if rpc_method && rpc_error.nil?
        raise ScenarioError, "expect_failure #{reason.inspect}: rpc_method: needs rpc_error:"
      end

      @reason = reason
      @rpc_error = rpc_error
      @rpc_method = rpc_method
      @message = message
    end

    def matches?(error)
      if rpc_error
        return false unless error.is_a?(RPCError) && error.code == rpc_error
        return false if rpc_method && error.rpc_method != rpc_method
      end
      return false if message && !message.match?(error.message)

      true
    end

    def describe
      parts = []
      parts << "RPC error #{rpc_error}" if rpc_error
      parts << "from #{rpc_method}" if rpc_method
      parts << "message #{message.inspect}" if message
      "#{reason} (#{parts.join(', ')})"
    end
  end

  Scenario = Data.define(:group, :name, :block, :expected_failure, :location, :exhaustive) do
    def full_name
      "#{group.name}: #{name}"
    end
  end

  Invariant = Data.define(:name, :block)

  # A named set of scenarios that share a default chart and invariants.
  class Group
    attr_reader :name, :chart, :scenarios, :invariants, :exhaustive

    # `exhaustive`: the group repeats a run for every allocation it can fail,
    # or the like, so it runs in `mise run test:full` only.
    def initialize(name, chart:, exhaustive: false)
      @name = name.to_s
      @chart = chart&.to_sym
      @exhaustive = exhaustive
      @scenarios = []
      @invariants = []
      @pending_failure = nil
    end

    # Marks the next scenario as an expected failure.
    def expect_failure(reason, **matcher)
      raise ScenarioError, "#{name}: expect_failure given twice before one scenario" if @pending_failure

      @pending_failure = ExpectedFailure.new(reason, **matcher)
    end

    # Checked against every actor after spawn, advance_time, and deliver.
    def invariant(name, &block)
      @invariants << Invariant.new(name: name, block: block)
    end

    # `exhaustive`: this scenario sweeps every allocation, or runs a long
    # random workload, so it runs in `mise run test:full` only.
    def scenario(name, exhaustive: false, &block)
      raise ScenarioError, "#{self.name}: scenario #{name.inspect} needs a block" unless block

      @scenarios << Scenario.new(group: self, name: name, block: block,
                                 expected_failure: @pending_failure,
                                 location: block.source_location.join(':'),
                                 exhaustive: @exhaustive || exhaustive)
      @pending_failure = nil
    end

    def finish!
      return unless @pending_failure

      raise ScenarioError, "#{name}: expect_failure #{@pending_failure.reason.inspect} has no scenario"
    end
  end

  # A core function answered with a result other than TA_OK.
  class CoreResultError < StandardError
    attr_reader :function, :result

    def initialize(function, result, detail = nil)
      @function = function
      @result = result
      super(detail.nil? ? "#{function} returned #{result}" : "#{function} returned #{result}: #{detail}")
    end
  end

  # A spawned actor, seen from a scenario. All observations go through the
  # public inspection API: ta_actor_inspect, ta_actor_active_states,
  # ta_actor_datamodel_root, and ta_definition_state.
  class Actor
    attr_reader :chart
    # The runtime ID, and the registered definition it runs under; both change
    # when the actor is reloaded (Context#rebind).
    attr_accessor :handle, :definition
    # The logical address {family: {name:}, name:} of a named actor, or nil.
    attr_accessor :address

    def initialize(context, handle, chart, definition, address: nil)
      @context = context
      @handle = handle
      @chart = chart
      @definition = definition
      @address = address
    end

    # Its session ID's text, and the session URI an unnamed actor's origin
    # and location are (core ADR 0088).
    def session_text = @context.session_text(inspect_actor)
    def session_uri = "#_scxml_#{session_text}"

    # The active configuration as state ID symbols, ancestors included.
    def active
      count = inspect_actor.fetch(:active_state_count)
      states = @context.core('ta_actor_active_states',
                             @context.ta.actor.active_states(system: @context.system_handle,
                                                             actor: handle, capacity: count))
      Array(states[:states]).map { |state| @context.state_name(definition, state) }
    end

    # The data-model root, or the single value a jq filter produces from it.
    def data(filter = '.')
      response = @context.ta.actor.datamodel_root(system: @context.system_handle, actor: handle)
      unless response[:result] == 'ok'
        raise ScenarioError, "#{self}: the data model reported no inspection root (#{response[:result]})"
      end

      root = response.dig(:out, :out)
      return root if filter == '.'

      outputs = @context.rpc('json.transform', filter: filter, input: root)
      unless outputs.size == 1
        raise AssertionFailed, "data filter #{filter.inspect} produced #{outputs.size} values, expected 1"
      end

      outputs.first
    end

    # How many delayed sends are pending; `sends` lists them.
    def delayed_sends
      Array(inspect_actor[:delayed_sends]).size
    end

    # True once the actor reached a top-level final state.
    def done?
      inspect_actor.fetch(:residency) == 'done'
    end

    # The done data of a finished actor (its top-level final state's
    # <donedata>), or nil when it has none.
    def completion
      out = @context.core('ta_actor_get_completion',
                          @context.ta.actor.get_completion(system: @context.system_handle, actor: handle)).fetch(:out)
      out[:has_data] ? out[:data] : nil
    end

    # Delivers an external event and runs the system until it is quiescent.
    # `trace:` is the trace parent its macrostep runs under (ADR 0067), as
    # TinyactorsTest.trace_parent builds it. `time:` is the input's logical
    # time (core ADR 0094): the system's by default, and for a replaying
    # actor the time the logged input was taken at.
    def deliver(event, data: nil, trace: nil, time: nil)
      @context.core('ta_actor_deliver', offer(event, data: data, trace: trace, time: time))
      @context.run_system("deliver #{event}")
    end

    # Offers an external event without running the system, and returns the
    # ta_actor_deliver result: "ok", "err_full", "err_done", ... Core admits
    # it onto its run queue; it runs when the system next runs, after the
    # inputs admitted before it. `correlation:` is the host's name for the
    # input, which #withdraw_input takes.
    def post(event, data: nil, trace: nil, time: nil, correlation: nil)
      offer(event, data: data, trace: trace, time: time, correlation: correlation)[:result]
    end

    # Delivers `event` with an operator transition (ADR 0082) to the states
    # `to`, from the active state `from` or the root, and runs the system.
    # The transition takes the event instead of the chart's transitions.
    # States are names; an Integer is passed as a raw state ID.
    def jump(*to, from: nil, event: 'operator.goto', data: nil, trace: nil)
      @context.core('ta_actor_deliver',
                    offer(event, data: data, trace: trace, transition: operator_transition(to, from)))
      @context.run_system("jump to #{to.join(' ')}")
    end

    # Offers a jump without running the system, and returns the
    # ta_actor_deliver result, as #post does.
    def post_jump(*to, from: nil, event: 'operator.goto', data: nil, correlation: nil)
      offer(event, data: data, transition: operator_transition(to, from), correlation: correlation)[:result]
    end

    # Abandons the actor's macrostep (ADR 0082) without running the system,
    # and returns the ta_actor_abandon result: "ok", "err_busy",
    # "err_invalid", "err_done", ...
    def abandon
      @context.ta.actor.abandon(system: @context.system_handle, actor: handle)[:result]
    end

    # One ta_run call for this actor's session alone (the debugging
    # exception), with `budget` microsteps and, when not zero, `action_budget`
    # units of executable content (ADR 0061); with `pause_at_microstep_start`
    # it also pauses at the start of each microstep it begins (ADR 0086). It
    # continues the session's paused macrostep, or takes its first admitted
    # input, and ends one input at most. A session a fault stopped is
    # released first, which also lifts the microstep limit it ran into (ADR
    # 0075). Returns the status: "paused" when the macrostep is still in
    # progress, "done" when it finished the actor, "stable" otherwise. A
    # fault fails the step; #run_once answers it instead.
    def resume(budget:, action_budget: 0, pause_at_microstep_start: false)
      run = run_once(budget: budget, action_budget: action_budget, pause_at_microstep_start: pause_at_microstep_start)
      raise CoreResultError.new('ta_run', run[:result], "actor #{handle}") unless run[:result] == 'ok'

      run[:status]
    end

    # #resume, answering what became of the call instead of failing:
    # { result:, status:, outcome: }, where `result` is ta_run's result, or
    # the code of the fault that stopped the input, `status` is #resume's,
    # and `outcome` the kind of the input's outcome ("completed", "ignored",
    # "finished", "faulted"), or nil when no input ended. With `release:
    # false` a stopped session stays stopped, and the call runs nothing of it.
    def run_once(budget:, action_budget: 0, pause_at_microstep_start: false, release: true)
      @context.ta.actor.release(system: @context.system_handle, actor: handle) if release
      response = @context.ta.run(system: @context.system_handle, session: handle, budget: budget,
                                 action_budget: action_budget, pause_at_microstep_start: pause_at_microstep_start,
                                 outcome_capacity: 1)
      @context.note_outcomes(response)
      return { result: response[:result], status: nil, outcome: nil } unless response[:result] == 'ok'

      outcome = Array(response.dig(:out, :outcomes)).first
      return { result: outcome[:code], status: nil, outcome: 'faulted' } if outcome && outcome[:kind] == 'faulted'

      status = if response.dig(:out, :status) == 'paused' then 'paused'
               elsif outcome && outcome[:kind] == 'finished' then 'done'
               else 'stable'
               end
      { result: 'ok', status: status, outcome: outcome && outcome[:kind] }
    end

    # Destroys the actor and runs the system. The actor is no longer checked
    # by invariants.
    def destroy
      @context.core('ta_actor_destroy', @context.ta.actor.destroy(system: @context.system_handle, actor: handle))
      @context.forget(self)
      @context.run_system("destroy #{self}")
    end

    # False once the runtime ID names no resident actor.
    def resident?
      @context.ta.actor.inspect(system: @context.system_handle, actor: handle)[:result] != 'err_not_found'
    end

    # The states the history pseudo-state `id` recorded, as symbols.
    def history(id)
      history = @context.state_id(definition, id)
      # A size query: capacity 0 answers err_nomem with the count unless it is 0.
      query = @context.ta.actor.history(system: @context.system_handle, actor: handle, history: history, capacity: 0)
      raise CoreResultError.new('ta_actor_history', query[:result]) unless %w[ok err_nomem].include?(query[:result])

      count = query.dig(:out, :count)
      return [] if count.zero?

      states = @context.core('ta_actor_history',
                             @context.ta.actor.history(system: @context.system_handle, actor: handle,
                                                       history: history, capacity: count))
      Array(states[:states]).map { |state| @context.state_name(definition, state) }
    end

    # Pending delayed sends: [{due:, event:, send_id:, target:, data:}], by due time.
    def sends
      observe.fetch(:delayed_sends)
    end

    # Everything a Trace records about this actor, now.
    def observe
      @context.observe(self)
    end

    def inspect_actor
      @context.core('ta_actor_inspect',
                    @context.ta.actor.inspect(system: @context.system_handle, actor: handle)).fetch(:out)
    end

    def to_s
      "#{chart}(#{handle})"
    end

    private

    def offer(event, data:, trace: nil, transition: nil, time: nil, correlation: nil)
      params = { name: @context.event_name(event), type: 'external', invoke_id: INVOKE_ID_NONE }
      params[:data] = data unless data.nil?
      delivery = { system: @context.system_handle, actor: handle, event: params, time: time || @context.now }
      delivery[:correlation] = correlation unless correlation.nil?
      delivery[:trace] = trace unless trace.nil?
      delivery[:transition] = transition unless transition.nil?
      @context.ta.actor.deliver(**delivery)
    end

    # A ta_operator_transition: state names become IDs, and no source is
    # the root.
    def operator_transition(targets, source)
      { source: source.nil? ? STATE_NONE : state_ref(source), targets: targets.map { |target| state_ref(target) } }
    end

    def state_ref(state) = state.is_a?(Integer) ? state : @context.state_id(definition, state)
  end

  # TA_INVOKE_ID_NONE; 64-bit values above 2^53-1 travel as decimal strings.
  INVOKE_ID_NONE = '18446744073709551615'

  # TA_STATE_NONE: the root, as an operator transition's source.
  STATE_NONE = 4_294_967_295

  # A trace parent (ADR 0067) as ta_actor_deliver and ta_actor_spawn take
  # it: the W3C trace-id `trace_id` (32 hex digits), its trace-flags, and
  # the host's parent span ID, which is the cause of a host delivery.
  def self.trace_parent(trace_id:, sampled: true, parent_span: 0)
    high = Integer(trace_id[0, 16], 16)
    low = Integer(trace_id[16, 16], 16)
    { context: { trace_id_high: u64(high), trace_id_low: u64(low), flags: sampled ? 1 : 0 },
      cause: { session: 0, macrostep: u64(parent_span), microstep: 0 } }
  end

  # A 64-bit value as the protocol carries it: a number up to 2^53-1, else
  # decimal text.
  def self.u64(value) = value <= 9_007_199_254_740_991 ? value : value.to_s

  # The trace-id of a record's context, as 32 hex digits.
  def self.trace_id(context)
    format('%016x%016x', Integer(context[:trace_id_high]), Integer(context[:trace_id_low]))
  end

  # The receiver of a scenario block: the steps and assertions of the DSL.
  # Steps drive the generated ta.* bindings of the public C API directly.
  class Context
    # The data-model adapter for each chart datamodel. A chart without a
    # datamodel (`datamodel: nil`) states no requirement and runs with jq.
    DATAMODELS = {
      'jq' => { '$adapter' => 'datamodel.jq' }.freeze,
      'ecmascript' => { '$adapter' => 'datamodel.ecmascript' }.freeze,
      'null' => { '$adapter' => 'datamodel.null' }.freeze
    }.freeze
    DEFAULT_DATAMODEL = 'jq'
    # Microsteps per ta_run call, and delayed sends per
    # ta_system_dispatch_due call.
    RESUME_BUDGET = 1024
    DISPATCH_BUDGET = 64

    attr_reader :now, :instruments, :trace, :run
    # With count_allocations: how many allocations each logical ta_run
    # made, in order.
    attr_reader :resume_costs
    # What each stop an armed allocation fault caused looked like:
    # {resume:, microstep:, data:}, the data-model root of the stopped actor.
    attr_reader :stops
    # The logical calls an armed allocation fault made ta_run itself answer
    # err_nomem: it could not take its next input, which it kept.
    attr_reader :refusals
    # How many stopped actors `on_stop: :cycle` unloaded and loaded again,
    # and how many it released in place, because core held admitted mail
    # for them.
    attr_reader :cycles, :skipped

    # `run` names this run; instrument names are prefixed with it, so
    # several runs can share one host process. `trace: true` records a Trace
    # after every step. `resume_budget` and `dispatch_budget` are passed to
    # ta_run and ta_system_dispatch_due; `resume_action_budget` is
    # ta_run's action budget (ADR 0061), zero for none, and
    # `resume_pause_at_microstep_start` its pause at each microstep's start
    # (ADR 0086). `after_step` is called as
    # after_step.call(context, step_index) once a step is quiescent, before
    # it is traced and its invariants are checked; the system is run to
    # quiescence again after it, without further hooks or trace steps.
    # `count_allocations: true` records what every logical ta_run the run
    # makes costs (#resume_costs); `allocation_fault:` {resume:, after:,
    # count:, on_stop:} fails allocations inside one of them (see
    # #assert_allocation_failures_invisible).
    def initialize(driver, group, charts, run: 'main', trace: false, resume_budget: RESUME_BUDGET,
                   dispatch_budget: DISPATCH_BUDGET, resume_action_budget: 0,
                   resume_pause_at_microstep_start: false, after_step: nil, count_allocations: false,
                   allocation_fault: nil)
      @driver = driver
      @group = group
      @charts = charts
      @run = run
      @scheduler = Scheduler.new(self, run: method(:run_logical), budget: resume_budget,
                                       action_budget: resume_action_budget, dispatch_budget: dispatch_budget,
                                       pause_at_microstep_start: resume_pause_at_microstep_start)
      @after_step = after_step
      @definitions = {}
      @event_names = {}
      @event_texts = {}
      @state_names = {}
      @system = nil
      @actors = []
      @actor_labels = {}
      @instruments = {}
      @step_index = 0
      @in_hook = false
      @now = 0
      @trace = trace ? Trace.new(self) : nil
      @count_allocations = count_allocations
      @allocation_fault = allocation_fault
      @resume_index = 0
      @resume_costs = []
      @stops = []
      @refusals = []
      @cycles = 0
      @skipped = 0
      @stopped_sessions = []
    end

    # The sessions a fault stopped (a FAULTED outcome) whose input has not
    # ended since: each holds its macrostep until it is released, abandoned
    # or destroyed.
    attr_reader :stopped_sessions

    # Notes what a ta.run response says about stopped sessions: a FAULTED
    # outcome stops its session, and the end of its input (COMPLETED,
    # IGNORED, FINISHED) lets it go. With #watch_mail, it also keeps the
    # mail and due sends sessions took, from their TAKEN outcomes.
    def note_outcomes(response)
      Array(response.dig(:out, :outcomes)).each do |outcome|
        case outcome[:kind]
        when 'faulted' then @stopped_sessions |= [outcome[:actor]]
        when 'completed', 'ignored', 'finished' then @stopped_sessions -= [outcome[:actor]]
        when 'taken'
          @mail_taken << { actor: outcome[:actor], event: outcome[:event] } if @mail_taken && outcome[:has_event]
        end
      end
    end

    # From now on, the runs report each input as it is taken, and keep what
    # mail and due sends sessions took: core queues them itself, and no host
    # call admits them (core ADR 0099).
    def watch_mail
      @mail_taken ||= []
    end

    # The mail and due sends sessions took since the last call, oldest
    # first: { actor:, event: } each, the event as the ta_event codec
    # encodes it. Empties the list.
    def taken_mail
      raise ScenarioError, 'taken_mail: call `watch_mail` before the steps that send' unless @mail_taken

      taken = @mail_taken
      @mail_taken = []
      taken
    end

    # Spawns an actor of a chart, named by its registry name or given as a
    # Chart built by the scenario itself (TinyactorsTest::Chart.define).
    # With `name:` the actor is named: it claims the address (the chart
    # definition's family, name) in the actor directory, which is a
    # memory_directory instrument unless the scenario installed one.
    # `binding:` is the host data-model binding the actor starts from.
    def spawn(chart = @group.chart, name: nil, binding: nil, trace: nil)
      raise ScenarioError, "#{@group.name}: spawn needs a chart" if chart.nil?

      chart_name = chart.is_a?(Chart) || chart.is_a?(SCXMLChart) ? chart.name.to_sym : chart.to_sym
      definition = definition_id(chart)
      params = { system: system_handle, definition: definition }
      params[:datamodel_binding] = binding unless binding.nil?
      params[:trace] = trace unless trace.nil?
      if name
        memory_directory
        params[:address] = { family: { name: definition_family(definition) }, name: name.to_s }
      end
      id = core('ta_actor_spawn', ta.actor.spawn(**params)).fetch(:out)
      actor = Actor.new(self, id, chart_name, definition, address: params[:address])
      @actors << actor
      spawned = @actors.count { |other| other.chart == chart_name }
      @actor_labels[id] ||= "#{chart_name}.#{spawned}"
      run_system("spawn #{chart_name}")
      actor
    end

    # Advances logical time by `milliseconds` and runs due work.
    def advance_time(milliseconds)
      unless milliseconds.is_a?(Integer) && milliseconds >= 0
        raise ScenarioError, "advance_time needs a non-negative Integer of milliseconds"
      end

      @now += milliseconds
      core('ta_system_set_time', ta.system.set_time(system: system_handle, now: @now))
      run_system("advance_time #{milliseconds} (t=#{@now}ms)")
    end

    # Sets logical time to `now` without running the system, so a scenario
    # can dispatch due sends itself (ta_system_dispatch_due) and look at
    # what is left in between. Time never goes back.
    def set_time(now)
      unless now.is_a?(Integer) && now >= @now
        raise ScenarioError, "set_time needs an Integer of milliseconds at or after t=#{@now}ms"
      end

      @now = now
      core('ta_system_set_time', ta.system.set_time(system: system_handle, now: @now))
    end

    # Advances logical time from one delayed-send deadline to the next until
    # `actor` is done. Fails when no delayed send is pending while the actor
    # still runs, or when the next deadline lies beyond `limit` milliseconds
    # of logical time.
    def run_to_completion(actor, limit: 600_000)
      until actor.done?
        response = ta.system.next_deadline(system: system_handle)
        if response[:result] == 'err_not_found'
          raise AssertionFailed, "#{actor} at t=#{@now}ms: not done and no delayed send is pending; " \
                                 "active: #{actor.active.join(', ')}"
        end

        deadline = Integer(core('ta_system_next_deadline', response).fetch(:out))
        if deadline > limit
          raise AssertionFailed, "#{actor} at t=#{@now}ms: not done before the #{limit}ms limit; " \
                                 "next deadline #{deadline}ms"
        end
        advance_time([deadline - @now, 0].max)
      end
    end

    # Escape hatch for methods the DSL does not wrap yet.
    def rpc(method, params = nil)
      @driver.call(method, params)
    end

    # Hosted mode (kernel ADR 0001 section 5): a system whose actors the host
    # kernel queues, schedules and routes. Turns run when the scenario asks.
    # Answers { hosted:, core: }: the kernel's system, and its core system for
    # the generated core methods (inspecting actors). Its memory comes from
    # the scenario's counting `allocator`, so a budget or an armed fault
    # reaches the kernel and its core, and teardown's leak check covers it;
    # `allocator: nil` gives it libc's. Answers the ta_result name when the
    # kernel could not create the system (a budget too small).
    # `max_queued_messages` is the kernel's limit on queued messages (napi
    # ADR 0035); nil leaves it at its default, none.
    def hosted_system(incarnation: 0, allocator: self.allocator, max_queued_messages: nil)
      params = { incarnation: incarnation }
      params[:allocator] = allocator.adapter if allocator
      params[:max_queued_messages] = max_queued_messages if max_queued_messages
      rpc('hosted.create', params)
    end

    # Closes a hosted system; answers the bytes it leaked (always 0).
    def close_hosted(hosted)
      rpc('hosted.close', system: hosted[:hosted])[:leaked]
    end

    # Compiles and registers a chart in a hosted system's core; answers its ID.
    def hosted_define(hosted, chart_name)
      chart = TinyactorsTest.charts.fetch(chart_name)
      compiled = core('ta_definition_compile_spec',
                      ta.definition.compile_spec(system: hosted[:core], spec: chart.to_spec,
                                                 datamodel: datamodel_adapter(chart), allocator: allocator.adapter))
      core('ta_system_register_definition',
           ta.system.register_definition(system: hosted[:core], definition: compiled.fetch(:out))).fetch(:out)
    end

    def hosted_spawn(hosted, definition, family: nil, name: nil)
      params = { system: hosted[:hosted], definition: definition }
      params.merge!(family: family, name: name) if name
      rpc('hosted.spawn', params)
    end

    def hosted_post(hosted, actor, event)
      rpc('hosted.post', system: hosted[:hosted], actor: actor, event: event.to_s)
    end

    # Posts to `family/name`, as an application does: to the resident actor,
    # or into a load when there is none and the family has a loader.
    def hosted_post_to(hosted, family, name, event, data: nil)
      params = { system: hosted[:hosted], family: family, name: name, event: event.to_s }
      params[:data] = data unless data.nil?
      rpc('hosted.post', params)
    end

    # Gives `family` a loader: mail for one of its names with no actor starts
    # a load, which hosted_run hands back in `loads`.
    def hosted_set_loader(hosted, family, max_waiting: 16)
      rpc('hosted.set_loader', system: hosted[:hosted], family: family, max_waiting: max_waiting)
    end

    # Answers a load request from hosted_run's `loads` as a loader would:
    # `outcome: 'loaded'` replays `journal` (the address's recorded events,
    # its life begun at `at`; a fresh actor of `definition` when it is
    # empty), 'spawned' spawns one, and 'not-found', 'rejected', 'failed' or
    # 'timed-out' turn the waiting mail into dead letters.
    # Answers { result:, outcome:, actor: }.
    def hosted_load(hosted, request, definition: nil, journal: nil, at: 0, outcome: 'loaded')
      params = { system: hosted[:hosted], index: request[:index], generation: request[:generation], outcome: outcome }
      params[:definition] = definition if definition
      params.merge!(journal: journal, at: at) if journal
      rpc('hosted.load', params)
    end

    # Runs turns and answers every load they start from `store` (as
    # hosted_store fills it), with the definition `definitions` names for the
    # family, until the system is quiet. Answers the answers to the loads.
    def hosted_run_loading(hosted, store, definitions, limit: 100)
      answers = []
      loop do
        run = hosted_run(hosted, limit: limit)
        run[:loads].each do |request|
          answers << hosted_load(hosted, request, definition: definitions.fetch(request[:family]),
                                                  journal: store.journals["#{request[:family]}/#{request[:name]}"])
        end
        return answers if run[:loads].empty? && run[:quiet]
      end
    end

    # Runs turns until the system is quiet, at most `limit`. Answers
    # { turns:, quiet:, dead_letters:, loads: }: the dead letters those turns
    # reported, { actor:, family:, name:, event:, reason:, data: } each,
    # `family` and `name` for a name with no actor, `data` when it had any;
    # and the loads started, { index:, generation:, family:, name:, why: }
    # each, which the scenario answers with hosted_load.
    def hosted_run(hosted, limit: 100)
      rpc('hosted.turn', system: hosted[:hosted], limit: limit)
    end

    # The kernel's mailboxes of `actors` and its counters: { mailboxes:
    # [{ actor:, state:, depth:, events: }], queued:, overflow:, delivered:,
    # dead_letters: { total:, by_reason: { 'done' => n, ... } } }. `queued`
    # includes the `overflow`, mail between actors accepted past the
    # limits (kernel ADR 0003).
    def hosted_mailboxes(hosted, *actors)
      rpc('hosted.mailboxes', system: hosted[:hosted], actors: actors)
    end

    # The events waiting in `actor`'s mailbox, oldest first.
    def hosted_mailbox(hosted, actor)
      hosted_mailboxes(hosted, actor)[:mailboxes].first[:events]
    end

    # Records the events of the named actors of `families` (kernel ADR 0006).
    def hosted_record(hosted, *families)
      rpc('hosted.record', system: hosted[:hosted], families: families.map(&:to_s))
    end

    # What an application journals: each address's events in order,
    # "family/name" => [{ event:, at: }], and its latest wake, "family/name"
    # => time or nil.
    HostedStore = Struct.new(:journals, :wakes) do
      def self.empty = new(Hash.new { |hash, key| hash[key] = [] }, {})
    end

    def empty_hosted_store = HostedStore.empty

    # The records and wakes since the last call, added to `store`.
    def hosted_store(hosted, store = HostedStore.empty)
      batch = rpc('hosted.records', system: hosted[:hosted])
      batch[:records].each do |record|
        store.journals["#{record[:family]}/#{record[:name]}"] << { event: record[:event], at: record[:at] }
      end
      batch[:wakes].each { |address, at| store.wakes[address.to_s] = at }
      store
    end

    def hosted_post_data(hosted, actor, event, data)
      rpc('hosted.post', system: hosted[:hosted], actor: actor, event: event.to_s, data: data)
    end

    # A jq filter over a hosted actor's data model root.
    def hosted_data(hosted, actor, filter = '.')
      response = ta.actor.datamodel_root(system: hosted[:core], actor: actor)
      raise ScenarioError, "hosted actor #{actor}: no data root (#{response[:result]})" unless response[:result] == 'ok'

      root = response.dig(:out, :out)
      return root if filter == '.'

      rpc('json.transform', filter: filter, input: root).first
    end

    # The kernel's memory counters: { memory: { core:, total: } }, each
    # { bytes_in_use:, peak_bytes:, largest_block: }.
    def hosted_stats(hosted)
      rpc('hosted.stats', system: hosted[:hosted])
    end

    def hosted_evict(hosted, actor)
      rpc('hosted.evict', system: hosted[:hosted], actor: actor)
    end

    # Rebuilds `family/name` from its journal (kernel ADR 0006 section 3): an
    # actor prepared at `at`, when its life began, takes each event at its
    # time and is published at the last one's. Answers the actor, or the
    # ta_result name ('err_done' for a journal that ends in a final state).
    def hosted_restore(hosted, definition, family, name, journal, at: 0)
      rpc('hosted.restore', system: hosted[:hosted], definition: definition, family: family, name: name,
                            journal: journal, at: at)
    end

    def hosted_advance(hosted, time)
      rpc('hosted.advance', system: hosted[:hosted], time: time)
    end

    # The time of the hosted core's next delayed send, or nil when none is pending.
    def hosted_next_deadline(hosted)
      response = ta.system.next_deadline(system: hosted[:core])
      return nil if response[:result] == 'err_not_found'

      Integer(core('ta_system_next_deadline', response).fetch(:out))
    end

    # The names of an actor's active atomic states, read from core.
    def hosted_states(hosted, actor)
      view = core('ta_actor_inspect', ta.actor.inspect(system: hosted[:core], actor: actor)).fetch(:out)
      states = core('ta_actor_active_states',
                    ta.actor.active_states(system: hosted[:core], actor: actor, capacity: view.fetch(:active_state_count)))
      Array(states[:states]).map do |state|
        core('ta_definition_state', ta.definition.state(system: hosted[:core], definition: view.fetch(:definition_id),
                                                        state: state)).fetch(:out).fetch(:id).to_sym
      end
    end

    # The generated bindings, e.g. `ta.actor.spawn(system:, definition:)`.
    def ta
      @driver.ta
    end

    # Unwraps a generated ta_result response, failing the step unless it is "ok".
    def core(function, response)
      raise CoreResultError.new(function, response[:result]) unless response[:result] == 'ok'

      response.fetch(:out, {})
    end

    # Runs every actor to a stable state and dispatches due delayed sends
    # until nothing is left to do at the current logical time (Scheduler).
    # Then runs the after_step hook, records the trace step, and checks
    # invariants.
    def run_system(step)
      @scheduler.run(step)
      return if @in_hook

      index = @step_index
      @step_index += 1
      if @after_step
        begin
          @in_hook = true
          @after_step.call(self, index)
          @scheduler.run("#{step}, after its hook")
        ensure
          @in_hook = false
        end
      end
      @trace&.record(step)
      check_invariants(step)
    end

    # Runs `body` as several separate runs of this scenario, each against its
    # own system in this host, and fails unless every run's Trace equals the
    # baseline run's trace step by step.
    #
    # variants:      {name => Context options}, e.g. {'paused' => {resume_budget: 1}}
    # interruptions: {name => ->(context) { ... }}; each runs once per step
    #                of the baseline, after that step only, so a baseline
    #                of N steps adds N runs per interruption
    # normalize:     ->(state) { ... } applied to every step's state of
    #                every run, to remove differences the claim allows
    #
    # Returns the baseline trace.
    def assert_equivalent_runs(variants: {}, interruptions: {}, normalize: nil, &body)
      raise ScenarioError, 'assert_equivalent_runs needs a block' unless body
      raise ScenarioError, 'assert_equivalent_runs needs variants: or interruptions:' if variants.empty? && interruptions.empty?

      baseline = traced_run('baseline', normalize, &body)
      runs = variants.map { |name, options| [name, options] }
      interruptions.each do |name, hook|
        baseline.steps.size.times do |at|
          step_hook = ->(context, index) { hook.call(context) if index == at }
          runs << ["#{name} after #{baseline.steps[at].describe}", { after_step: step_hook }]
        end
      end
      runs.each do |name, options|
        trace = traced_run(name, normalize, **options, &body)
        difference = Trace.difference(baseline, trace, actual_name: name)
        raise AssertionFailed, difference if difference
      end
      baseline
    end

    # Fails one allocation at a time in every logical ta_run that `body`
    # makes (ADR 0047, ADR 0070). A baseline run counts the allocations of
    # each; then, for each of those allocations and each entry of
    # `counts`, a fresh run fails that allocation and the count - 1 after
    # it. The ta_run must stop the actor with err_nomem (#stopped checks
    # it), or answer err_nomem itself when it could not take its next input,
    # and running again until it runs must end the run with the baseline's
    # trace, step by step. `on_stop: :cycle` also moves
    # every stopped named actor out of residency and back before it is
    # resumed.
    # `chart_errors: true` says the chart's own content raises error events,
    # so a stopped actor may have some queued; any error event the failure
    # added still shows in the trace.
    #
    # A ta_run refills core's pool before it takes an input, a safe point
    # where a failure leaves what is free and is never reported; so every
    # logical ta_run, in the baseline and in the faulted runs alike, is
    # preceded by one that refills and takes nothing (#refill_outside_faults)
    # before the fault is armed, and every allocation the sweep fails is one
    # the logical ta_run makes for what it runs.
    #
    # Returns {runs:, unreported:, stops:, cycles:, skipped:}: how many runs
    # failed an allocation; the names of those whose ta_run never answered
    # err_nomem, because the allocation that failed was a host operation's
    # (a router delivering to another actor), which handled it itself; every
    # stop (#stops) of every run; how many stops `on_stop: :cycle` moved out
    # of residency and back, and how many it released in place instead,
    # because core held admitted mail for the actor. Other options
    # (`resume_action_budget:`, ...) are the runs' Context options.
    def assert_allocation_failures_invisible(counts: [1], on_stop: nil, chart_errors: false, normalize: nil,
                                             **options, &body)
      raise ScenarioError, 'assert_allocation_failures_invisible needs a block' unless body

      reference = fresh_run('baseline', normalize, count_allocations: true, **options, &body)
      points = reference.resume_costs.each_with_index.flat_map do |allocations, resume|
        Array.new(allocations) do |after|
          counts.map do |count|
            { resume: resume, after: after, count: count, on_stop: on_stop, chart_errors: chart_errors }
          end
        end.flatten
      end
      unreported = []
      stops = []
      cycles = 0
      skipped = 0
      points.each do |fault|
        name = "allocation #{fault[:after] + 1} of resume #{fault[:resume]} failing #{fault[:count]}x"
        run = fresh_run(name, normalize, allocation_fault: fault, **options, &body)
        difference = Trace.difference(reference.trace, run.trace, actual_name: name)
        raise AssertionFailed, difference if difference

        reported = run.stops.any? { |stop| stop[:resume] == fault[:resume] } || run.refusals.include?(fault[:resume])
        unreported << name unless reported
        stops.concat(run.stops)
        cycles += run.cycles
        skipped += run.skipped
      end
      { runs: points.size, unreported: unreported, stops: stops, cycles: cycles, skipped: skipped }
    end

    # Runs one interruption after baseline step `at` and passes only when its
    # trace differs. An optional message matcher checks the reported difference.
    def assert_runs_diverge(interruptions:, at:, message: nil, normalize: nil, &body)
      raise ScenarioError, 'assert_runs_diverge needs a block' unless body
      raise ScenarioError, 'assert_runs_diverge needs exactly one interruption' unless interruptions.size == 1

      baseline = traced_run('baseline', normalize, &body)
      unless at.is_a?(Integer) && at >= 0 && at < baseline.steps.size
        raise ScenarioError, "assert_runs_diverge step #{at.inspect} is outside a #{baseline.steps.size}-step trace"
      end

      name, hook = interruptions.first
      step = baseline.steps[at]
      run_name = "#{name} after #{step.describe}"
      step_hook = ->(context, index) { hook.call(context) if index == at }
      trace = traced_run(run_name, normalize, after_step: step_hook, &body)
      difference = Trace.difference(baseline, trace, actual_name: run_name)
      raise AssertionFailed, "#{run_name} did not diverge from baseline" unless difference
      if message && !(message === difference)
        raise AssertionFailed, "trace difference did not match #{message.inspect}:\n#{difference}"
      end

      difference
    end

    def assert(condition, message = 'assertion failed')
      raise AssertionFailed, message unless condition
    end

    def assert_equal(expected, actual, message = nil)
      return if expected == actual

      raise AssertionFailed, [message, "expected #{expected.inspect}, got #{actual.inspect}"].compact.join(': ')
    end

    # Passes when every named state is in the active configuration.
    def assert_active(actor, *states)
      active = actor.active
      missing = states.map(&:to_sym) - active
      return if missing.empty?

      raise AssertionFailed, "#{actor} at t=#{@now}ms: #{missing.join(', ')} not active; active: #{active.join(', ')}"
    end

    def refute_active(actor, *states)
      active = actor.active
      present = states.map(&:to_sym) & active
      return if present.empty?

      raise AssertionFailed, "#{actor} at t=#{@now}ms: #{present.join(', ')} unexpectedly active"
    end

    # Passes when the active configuration is exactly the named states.
    def assert_configuration(actor, *states)
      active = actor.active
      return if active.sort == states.map(&:to_sym).sort

      raise AssertionFailed,
            "#{actor} at t=#{@now}ms: expected configuration #{states.join(', ')}, got #{active.join(', ')}"
    end

    # ----------------------------------------------------------------------
    # Log records (ADR 0045)

    # What the charts logged, oldest first: [{label:, value:, actor:}].
    # `value` is nil when the <log> had no expression, and `actor` is the
    # Actor that logged when the scenario spawned it, else its runtime ID.
    #
    # The sink only records from the moment it is installed, so a scenario
    # calls `recording_log` before the steps it wants to read; reading logs
    # without a sink is an error rather than an empty list.
    def logs
      sink = @instruments['log']
      unless sink
        raise ScenarioError, 'logs: install the sink with `recording_log` before the steps that log'
      end

      sink.entries('log').map do |entry|
        { label: entry[:label], value: entry[:has_value] ? entry[:value] : nil,
          actor: actor_of_ref(entry[:source]) }
      end
    end

    # Passes when the sink recorded exactly these records, in order:
    # `assert_logs(['phase', 'trial'], ['done', nil])`.
    def assert_logs(*expected)
      actual = logs.map { |record| [record[:label], record[:value]] }
      assert_equal expected.map { |label, value| [label.to_s, value] }, actual, 'the log sink recorded'
    end

    # Passes when `label` was logged with exactly `value` `times` times, by
    # `actor` when one is given.
    def assert_logged(label, value, actor: nil, times: 1)
      matching = logs.select do |record|
        record[:label] == label.to_s && record[:value] == value && (actor.nil? || record[:actor] == actor)
      end
      return if matching.size == times

      raise AssertionFailed,
            "expected #{times} log record(s) #{[label.to_s, value].inspect}#{" from #{actor}" if actor}, " \
            "got #{matching.size} of #{logs.inspect}"
    end

    # The Actor a log record's source names, or its runtime ID when the
    # scenario did not spawn it.
    def actor_of_ref(ref)
      return nil unless ref.is_a?(Hash) && ref[:kind] == 'runtime'

      id = Integer(ref[:runtime])
      @actors.find { |actor| actor.handle == id } || id
    end

    # Creates the system on first use. Mail between sessions stays in core
    # (core ADR 0099); mail for a target that is not resident waits there
    # until this host answers core's load request (#answer_loads).
    #
    # It also registers the invoker.scxml adapter for SCXML <invoke>. The
    # adapter runs an invoked machine as a child actor: the chart spec inline
    # in the invoke's <content>, or for src="file:NAME.scxml" the chart
    # irp_NAME, so it receives every defined chart's spec. Each spec carries
    # its chart's `datamodel` ("jq", "ecmascript", or none), and the invoker
    # is expected to compile the child with the adapter that datamodel names,
    # as datamodel_adapter does here.
    # The bytes of the tel ring of a scenario's system (core ADR 0098).
    TEL_BYTES = 1 << 20

    def system_handle
      @system ||= begin
        @creating_system = true
        # A tel ring large enough that one call's trace records never fill
        # it: a scenario that reads them compares every one (core ADR 0098).
        params = [{ param: 'tel_bytes', value: TEL_BYTES }]
        params += (@system_params || {}).map { |param, value| { param: param.to_s, value: value } }
        system = core('ta_system_create',
                      ta.system.create(config: { allocator: allocator.adapter, initial_time: 0,
                                                 params: params })).fetch(:out)
        @creating_system = false
        register_scxml_invoker(system)
        system
      end
    end

    # Withdraws the admitted input the host named `correlation` before a
    # macrostep takes it, and returns the ta_system_withdraw_input result:
    # "ok", or "err_not_found" once it was taken.
    def withdraw_input(correlation)
      ta.system.withdraw_input(system: system_handle, correlation: correlation)[:result]
    end

    # Sets ta_param values of the scenario's system, such as
    # `pool_envelopes: 8`, before its first line creates it.
    def system_params(**values)
      raise ScenarioError, 'system_params must come before the system is created' if @system

      @system_params = values
    end

    # The counting allocator behind this run's system and definitions. The
    # host only knows it once the system it backs has been created, so
    # asking for it creates the system first: `allocator.stats` works from
    # a scenario's first line.
    def allocator
      counting = instrument('allocator', 'allocator.counting')
      system_handle unless @creating_system
      counting
    end

    # Mail a chart sends to a target that is not resident waits in core,
    # which asks the host for its load once per destination (core ADR 0099).
    # The scheduler reads each request once the run queue is empty
    # (#answer_loads), and this host answers it: by default the target
    # cannot be loaded, so each waiting message becomes error.communication
    # for its sender, as for a target no loader brings back. `on_load_needed`
    # replaces that answer: its block gets the request (`{request:,
    # destination:, hint:, waiting:, trace:}`, as ta_load_request encodes)
    # and returns `[:settled, actor]`, `[:failed, code]` (a result name such
    # as "err_not_found"), or `:hold`, to answer later with #load_settled or
    # #load_failed.
    def on_load_needed(&block)
      @load_answer = block
    end

    # Every load request the scheduler read, oldest first.
    def load_requests
      @load_requests ||= []
    end

    # The load requests #on_load_needed held and nothing answered yet, oldest
    # first.
    def held_loads
      @held_loads ||= []
    end

    # Reads the load requests core owes and answers each (#on_load_needed).
    # Returns how many it read.
    def answer_loads
      count = 0
      loop do
        response = ta.system.load_needed(system: system_handle)
        break if response[:result] == 'err_not_found'

        request = core('ta_system_load_needed', response).fetch(:out)
        load_requests << request
        count += 1
        answer = @load_answer ? @load_answer.call(request) : [:failed, request[:hint]]
        if answer == :hold
          held_loads << request
          next
        end

        kind, value = answer
        kind == :settled ? load_settled(request, value) : load_failed(request, value)
      end
      count
    end

    # Answers `request`: `actor` holds its destination now, and the mail that
    # waited goes to it.
    def load_settled(request, actor)
      handle = actor.is_a?(Actor) ? actor.handle : Integer(actor)
      held_loads.delete_if { |held| held[:request] == request[:request] }
      core('ta_system_load_settled',
           ta.system.load_settled(system: system_handle, request: request[:request], actor: handle))
    end

    # The fault codes a failed load gives each waiting message's
    # error.communication.
    FAULT_CODES = { 'err_not_found' => -3, 'err_unloaded' => -9, 'err_done' => -11 }.freeze

    # Answers `request`: its destination cannot be loaded, and each message
    # that waited becomes error.communication for its sender.
    def load_failed(request, code = 'err_not_found', message: 'the target cannot be loaded')
      held_loads.delete_if { |held| held[:request] == request[:request] }
      core('ta_system_load_failed',
           ta.system.load_failed(system: system_handle, request: request[:request],
                                 fault: { code: FAULT_CODES.fetch(code.to_s), message: message }))
    end

    # Registers an invoker.scripted instrument for invoke `type`.
    def scripted_invoker(type)
      @instruments["invoker #{type}"] || begin
        invoker = instrument("invoker #{type}", 'invoker.scripted')
        core('ta_system_register_invoker',
             ta.system.register_invoker(system: system_handle, type: atom(type),
                                        invoker: invoker.adapter(system: system_handle)))
        invoker
      end
    end

    # Registers an io.recording instrument as the I/O processor for `type`.
    def recording_io(type)
      @instruments["io #{type}"] || begin
        io = instrument("io #{type}", 'io.recording')
        core('ta_system_register_io',
             ta.system.register_io(system: system_handle, type: atom(type), io: io.adapter(system: system_handle)))
        io
      end
    end

    # Registers an io.peer, io.connection or io.request instrument as the
    # I/O processor for `type` (ADR 0068); its `processor` is the registry
    # index core gave it.
    def endpoint_io(type, kind)
      @instruments["io #{type}"] || begin
        io = instrument("io #{type}", kind)
        out = core('ta_system_register_io',
                   ta.system.register_io(system: system_handle, type: atom(type), io: io.adapter(system: system_handle)))
        io.processor = out.fetch(:index)
        io
      end
    end

    # One event from outside through an endpoint instrument, with the
    # origin of that instrument's class, then runs the system unless `run:
    # false`. Returns the instrument's answer ({result, held}).
    def receive(io, actor, event, run: true, **args)
      answer = io.control('receive', processor: io.processor, actor: actor.handle, event: event, **args)
      run_system("receive #{event}") if run
      answer
    end

    # Installs a log.recording instrument as the system's <log> sink, so a
    # scenario can read what its charts log (ADR 0045).
    def recording_log
      @instruments['log'] || begin
        sink = instrument('log', 'log.recording')
        core('ta_system_set_log_sink',
             ta.system.set_log_sink(system: system_handle, sink: sink.adapter(system: system_handle)))
        sink
      end
    end

    # Installs a definition.recording instrument as the system's definition
    # sink (core ADR 0079). Its journal lists every registration, retirement
    # and free, on the seq counter every instrument shares.
    def recording_definitions
      @instruments['definitions'] || begin
        sink = instrument('definitions', 'definition.recording')
        core('ta_system_set_definition_sink',
             ta.system.set_definition_sink(system: system_handle, sink: sink.adapter(system: system_handle)))
        sink
      end
    end

    # The definition events the sink was told, oldest first, as
    # [kind, family, definition ID]. `since:` skips events up to that seq.
    def definition_events(since: nil)
      sink = @instruments['definitions']
      raise ScenarioError, 'definition_events: install the sink with `recording_definitions` first' unless sink

      sink.journal(since: since).select { |entry| entry[:op] == 'definition' }
          .map { |entry| [entry[:kind], entry[:family], Integer(entry[:definition])] }
    end

    # ----------------------------------------------------------------------
    # Trace records (ADR 0067)

    # Installs a trace.recording instrument, which reads the system's tel
    # ring (core ADR 0098) into its journal, and sets the system's masks.
    # `kinds:` lists the ta_trace_kind symbols it wants (every kind by
    # default), and `unsampled: true` also traces unsampled macrosteps. A
    # Trace of the run then compares its records, without PAUSED, RESUMED
    # and STOPPED (Trace#journals).
    def recording_trace(kinds: nil, unsampled: false)
      @instruments['trace'] || begin
        sink = instrument('trace', 'trace.recording')
        args = { system: system_handle }
        args[:kinds] = kinds.map(&:to_s) if kinds
        args[:unsampled] = true if unsampled
        sink.control('attach', **args)
        sink
      end
    end

    # The records core wrote to tel, oldest first, as the codec encodes
    # ta_trace_record. `since:` skips records up to that journal seq.
    def trace_records(since: nil)
      sink = @instruments['trace']
      raise ScenarioError, 'trace_records: install the sink with `recording_trace` first' unless sink

      sink.journal(since: since).select { |entry| entry[:op] == 'trace' }
    end

    # Records as one line each, for exact comparisons: the kind, the step
    # key (macrostep.microstep), where it was written, and what the kind
    # carries, with states and events by name. `actor:` keeps one actor's.
    def trace_lines(records = trace_records, actor: nil)
      records = records.select { |record| Integer(record[:actor]) == actor.handle } if actor
      records.map { |record| TraceLine.new(self, record).to_s }
    end

    # Installs a directory.memory instrument as the actor directory.
    def memory_directory
      @instruments['directory'] || begin
        directory = instrument('directory', 'directory.memory')
        core('ta_system_set_actor_directory',
             ta.system.set_actor_directory(system: system_handle, directory: directory.adapter))
        directory
      end
    end

    # Reports the outcome of a scripted invocation, given its "start"
    # journal entry, and runs the system. kind :event takes `event` (a
    # dotted name) and `data`; :done takes `data` as done data; :error takes
    # `reason` (the last token of error.communication.invoke.<id>.<reason>),
    # `data`, and `ends` (whether the invocation is retired, core ADR 0081).
    def report_invoke(start, kind, event: nil, data: nil, reason: nil, ends: false)
      params = { system: system_handle, actor: start.fetch(:parent), invoke_id: start.fetch(:invoke_id),
                 kind: kind.to_s, time: @now }
      case kind.to_sym
      when :event
        raise ScenarioError, 'report_invoke :event needs event:' unless event

        params[:event] = { name: event_name(event), type: 'external', invoke_id: start.fetch(:invoke_id) }
        params[:event][:data] = data unless data.nil?
      when :done
        params[:done_data] = data unless data.nil?
      when :error
        raise ScenarioError, 'report_invoke :error needs reason:' unless reason

        params[:error] = { reason: reason, ends: ends }
        params[:error][:data] = data unless data.nil?
      else
        raise ScenarioError, "report_invoke: unknown kind #{kind.inspect}"
      end
      core('ta_actor_report_invoke', ta.actor.report_invoke(**params))
      run_system("report_invoke #{kind}#{" #{event || reason}" if event || reason}")
    end

    # Destroys this run's system and fails when the counting allocator still
    # holds allocations, or saw a free it did not hand out, or when a trace
    # record names an ID its definition cannot answer for. Runs once.
    def teardown!
      return if @system.nil? || @torn_down

      @torn_down = true
      unresolved = unresolved_trace_ids
      ta.system.destroy(system: @system)
      unless unresolved.empty?
        raise AssertionFailed, "#{@run}: trace records name IDs their definitions cannot answer for " \
                               "(ADR 0071): #{unresolved.first(5).join('; ')}"
      end
      stats = allocator.stats
      anomalies = allocator.journal.select { |entry| entry[:op].start_with?('free_') }
      unless anomalies.empty?
        raise AssertionFailed, "#{@run}: the allocator saw #{anomalies.size} bad free(s): #{anomalies.first(3).inspect}"
      end
      return if stats[:live_allocations].zero?

      raise AssertionFailed, "#{@run}: ta_system_destroy left #{stats[:live_allocations]} allocation(s) " \
                             "(#{stats[:live_bytes]} bytes) live"
    end

    ID_NONE = 4_294_967_295

    # Every state, transition, action and expression ID in every trace
    # record, looked up in the record's definition (ADR 0071). Transitions
    # in records are always <transition> elements. A definition that is no
    # longer registered (retired and freed) is skipped: its IDs are stale.
    # Returns the IDs that did not resolve, described.
    def unresolved_trace_ids
      sink = @instruments['trace']
      return [] unless sink

      answers = {}
      sink.journal.select { |entry| entry[:op] == 'trace' }.flat_map do |record|
        definition = record[:definition]
        next [] unless definition_registered?(definition, answers)

        trace_ids(record).filter_map do |lookup, id|
          answer = answers[[definition, lookup, id]] ||= trace_id_answer(definition, lookup, id)
          "#{record[:kind]} names #{lookup} #{id}: #{answer}" unless answer == :ok
        end
      end
    end

    # [lookup, id] for every ID a record names.
    def trace_ids(record)
      position = record[:position] || {}
      ids = [[:state, position[:state]], [:transition, position[:transition]], [:action, position[:action]],
             [:state, record[:state]], [:expression, record[:expr]], [:action, record[:action]]]
      ids.concat(Array(record[:transitions]).map { |id| [:transition, id] })
      %i[exited entered history states].each { |list| ids.concat(Array(record[list]).map { |id| [:state, id] }) }
      ids.reject { |_, id| id.nil? || Integer(id) == ID_NONE }
    end

    def definition_registered?(definition, answers)
      answers.fetch([definition, :registered]) do
        answers[[definition, :registered]] =
          ta.definition.inspect(system: system_handle, definition: definition)[:result] == 'ok'
      end
    end

    # :ok, or why the lookup failed.
    def trace_id_answer(definition, lookup, id)
      response = ta.definition.public_send(lookup, system: system_handle, definition: definition, lookup => id)
      return response[:result] unless response[:result] == 'ok'
      return :ok unless lookup == :transition

      origin = response.dig(:out, :out, :origin)
      origin == 'authored' ? :ok : "a record names the #{origin} transition"
    end

    # Everything a Trace records about one actor.
    def observe(actor)
      Trace.new(self).observe(actor.handle)
    end

    # The earliest pending delayed-send deadline, or nil when none is pending.
    def next_deadline
      response = ta.system.next_deadline(system: system_handle)
      return nil if response[:result] == 'err_not_found'

      Integer(core('ta_system_next_deadline', response).fetch(:out))
    end

    # The current logical time core reports (ta_system_time), which the
    # scenario's own `now` mirrors.
    def system_time
      Integer(ta.system.time(system: system_handle).fetch(:value))
    end

    # ta_definition_inspect facts: identity, counts, and measurements.
    def definition_facts(chart = @group.chart)
      id = chart.is_a?(Integer) ? chart : definition(chart)
      core('ta_definition_inspect', ta.definition.inspect(system: system_handle, definition: id)).fetch(:out)
    end

    # Where element `id` of `kind` ('state', 'transition', 'action' or
    # 'expression') was written (ADR 0085): its span and its source's name,
    # or nil when the definition keeps none for it.
    def source_span(kind, id, chart = @group.chart)
      definition = chart.is_a?(Integer) ? chart : definition(chart)
      response = ta.definition.span(system: system_handle, definition: definition, kind: kind.to_s, id: id)
      return nil if response[:result] == 'err_not_found'

      core('ta_definition_span', response).fetch(:out)
    end

    # The author-level spec ta_definition_export rebuilds from a definition.
    def export(chart = @group.chart)
      id = chart.is_a?(Integer) ? chart : definition(chart)
      core('ta_definition_export', ta.definition.export(system: system_handle, definition: id)).fetch(:out)
    end

    # ta_event_name_matches: does `event` match the descriptor `prefix`?
    # Both are dotted names, interned through the public atom API.
    def event_matches?(event, prefix)
      ta.event.name_matches(system: system_handle, event: event_name(event),
                            prefix: event_name(prefix)).fetch(:value)
    end

    # The system-local atom of the built-in SCXML Event I/O Processor.
    def scxml_io_type
      ta.system.scxml_io_type(system: system_handle).fetch(:value)
    end

    # Retries an invoked child's completion; returns the result.
    def publish_completion(actor)
      id = actor.is_a?(Actor) ? actor.handle : actor
      ta.actor.publish_completion(system: system_handle, actor: id)[:result]
    end

    # Resident actors the scenario did not spawn itself: the children an
    # invoker started. They are Actors, so they can be inspected and resumed.
    def invoked_actors
      known = @actors.map(&:handle)
      (rpc('system.actors', system: system_handle) - known).map do |id|
        definition = core('ta_actor_inspect', ta.actor.inspect(system: system_handle, actor: id))
                     .fetch(:out).fetch(:definition_id)
        Actor.new(self, id, :invoked, definition)
      end
    end

    # ----------------------------------------------------------------------
    # Definitions the host still owns (ADR 0043)

    # ta_definition_compile_spec alone: the compiled definition is the
    # host's until it registers it. Returns the ta_definition handle.
    def compile_only(chart = @group.chart)
      chart = @charts.fetch(chart.to_sym) { raise ScenarioError, "unknown chart #{chart}" } unless chart.is_a?(Chart)
      core('ta_definition_compile_spec',
           ta.definition.compile_spec(system: system_handle, spec: chart.to_spec,
                                      datamodel: datamodel_adapter(chart),
                                      allocator: allocator.adapter)).fetch(:out)
    end

    # ta_definition_destroy: releases a compiled definition the host decided
    # not to register.
    def destroy_definition(definition)
      ta.definition.destroy(definition: definition)
      nil
    end

    # ta_system_register_definition: hands a compiled definition to the
    # system, which owns it from then on. Returns its definition ID.
    def register_definition(definition)
      core('ta_system_register_definition',
           ta.system.register_definition(system: system_handle, definition: definition)).fetch(:out)
    end

    # ta_system_retire_definition: ends a definition's availability. Takes a
    # definition ID or a chart name.
    def retire_definition(chart)
      id = chart.is_a?(Integer) ? chart : definition(chart)
      ta.system.retire_definition(system: system_handle, definition: id)[:result]
    end

    # ----------------------------------------------------------------------
    # Walking the resident actors (ta_system_visit_actors)

    # The inspector.recording instrument core walks actors with. Its
    # `entries('visit')` are the views it was handed, in visit order.
    def recording_inspector
      @instruments['inspector'] ||= instrument('inspector', 'inspector.recording')
    end

    # ta_system_visit_actors with that inspector (ADR 0062): [result, next].
    # The defaults walk everything; `from` and `limit` walk one page, and
    # `next` says where the next page starts, ACTOR_VISIT_END when nothing is
    # left.
    ACTOR_VISIT_END = 0xFFFF_FFFF

    def visit_actors(inspector = recording_inspector, from: 0, limit: 0)
      response = ta.system.visit_actors(system: system_handle, inspector: inspector.adapter,
                                        from: from, limit: limit)
      [response[:result], response.dig(:out, :next)]
    end

    # Walks the whole population in pages of `limit`: [the actors visited in
    # order, the `next` of every page].
    def walk_in_pages(limit, inspector: recording_inspector)
      # An empty page hands core the instrument, so its journal can be read.
      visit_actors(inspector, from: ACTOR_VISIT_END, limit: 1)
      seen = inspector.visits.size
      cursor = 0
      cursors = []
      until cursor == ACTOR_VISIT_END
        result, cursor = visit_actors(inspector, from: cursor, limit: limit)
        assert_equal 'ok', result
        cursors << cursor
        assert cursors.size <= 1000, 'the walk ends'
      end
      [inspector.visits.drop(seen).map { |entry| entry[:actor] }, cursors]
    end

    # ACTOR_VISIT_END, for scenario blocks, which do not see this module's constants.
    def actor_visit_end = ACTOR_VISIT_END

    # An actor's position in the walk: its slot, the low 32 bits of its ID.
    def actor_position(id) = id & 0xFFFF_FFFF

    # ----------------------------------------------------------------------
    # The SCXML target URI grammar (ADR 0044)

    # ta_scxml_target_parse: [result, the structured target or nil].
    def parse_target(text)
      response = ta.scxml.target_parse(system: system_handle, text: text)
      [response[:result], response[:result] == 'ok' ? response.dig(:out, :out) : nil]
    end

    # ta_scxml_target_format: [result, the URI or nil, the needed capacity].
    # A `capacity` smaller than needed answers err_nomem, so 0 is a size query.
    def format_target(target, capacity: 256)
      response = ta.scxml.target_format(system: system_handle, target: target, capacity: capacity)
      out = response.fetch(:out, {})
      [response[:result], out[:buffer], out[:needed]]
    end

    # ----------------------------------------------------------------------
    # Session images, residency, and named actors (ADRs 0014, 0037, 0038)

    # The actors a scenario spawned that are still resident.
    def actors = @actors.dup

    # The logical address of the named actor `name` of a chart's family.
    def address(chart_name, name)
      { family: { name: family(chart_name) }, name: name.to_s }
    end

    # The definition family of a chart (its <scxml name>, by default the chart name).
    def family(chart_name)
      definition_family(definition(chart_name))
    end

    # The family of a registered definition, for a chart the registry does not know.
    def definition_family(definition)
      facts = core('ta_definition_inspect', ta.definition.inspect(system: system_handle, definition: definition))
      facts.fetch(:out).dig(:identity, :family, :name)
    end

    # Captures a session image of the actor; returns the image handle.
    def capture(actor)
      core('ta_actor_capture', ta.actor.capture(system: system_handle, actor: actor.handle)).fetch(:out)
    end

    # The earliest delayed-send deadline an image holds, or nil.
    def image_deadline(image)
      response = ta.session.image_next_deadline(image: image)
      return nil if response[:result] == 'err_not_found'

      Integer(core('ta_session_image_next_deadline', response).fetch(:out))
    end

    def destroy_image(image)
      ta.session.image_destroy(image: image)
    end

    # ta_actor_unload with a captured image; returns the result. On "ok" the
    # actor is no longer resident: invariants skip it until it is loaded.
    def unload(actor, image)
      result = ta.actor.unload(system: system_handle, actor: actor.handle, image: image)[:result]
      forget(actor) if result == 'ok'
      result
    end

    # ta_actor_load of an unloaded named actor; returns the result. On "ok"
    # the Actor points at its new runtime ID and keeps its trace label.
    def load(actor, image, binding: nil)
      params = { system: system_handle, image: image }
      params[:datamodel_binding] = binding unless binding.nil?
      response = ta.actor.load(**params)
      if response[:result] == 'ok'
        rebind(actor, response.fetch(:out).fetch(:out))
        @actors << actor unless @actors.include?(actor)
      end
      response[:result]
    end

    # ta_actor_find: [result, runtime ID or nil].
    def find(address)
      response = ta.actor.find(system: system_handle, address: address)
      [response[:result], response[:result] == 'ok' ? response.dig(:out, :out) : nil]
    end

    def remove_unloaded(address)
      ta.actor.remove_unloaded(system: system_handle, address: address)[:result]
    end

    # Prepares an unpublished actor under a chart's definition (core ADR
    # 0089): to replace a named `incumbent`, or to take `address`. With `at`
    # it is a replaying actor on its own clock. Returns it as an Actor at its
    # address, bootstrapped.
    def prepare(chart_name, incumbent: nil, address: nil, binding: nil, at: nil)
      definition = definition_id(chart_name.to_sym)
      params = { system: system_handle, definition: definition }
      params[:incumbent] = incumbent.handle unless incumbent.nil?
      params[:address] = address unless address.nil?
      params[:datamodel_binding] = binding unless binding.nil?
      params[:at] = at unless at.nil?
      id = core('ta_actor_prepare', ta.actor.prepare(**params)).fetch(:out)
      prepared = Actor.new(self, id, chart_name.to_sym, definition, address: incumbent&.address || address)
      @actor_labels[id] ||= "#{chart_name}.#{at.nil? ? 'replacement' : 'replay'}"
      run_system("prepare #{chart_name}")
      prepared
    end

    # Publishes a prepared actor; returns the result. On "ok" it is checked
    # by invariants from now on.
    # `at:` is a replaying actor's last clock move before it is published
    # (core ADR 0094): the moment the run being replayed stopped.
    def publish(prepared, incumbent: nil, at: nil)
      params = { system: system_handle, actor: prepared.handle }
      params[:incumbent] = incumbent.handle unless incumbent.nil?
      params[:at] = at unless at.nil?
      result = ta.actor.publish(**params)[:result]
      @actors << prepared if result == 'ok' && !@actors.include?(prepared)
      result
    end

    # Capture, unload, and load a named actor with its current data root as
    # the binding: the residency change ADR 0014 says is invisible.
    def cycle_residency(actor)
      image = capture(actor)
      binding = actor.data
      core('ta_actor_unload', { result: unload(actor, image) })
      core('ta_actor_load', { result: load(actor, image, binding: binding) })
      destroy_image(image)
    end

    # Stops checking invariants against a destroyed actor.
    def forget(actor)
      @actors.delete(actor)
    end

    # The registered definition ID of a chart, compiling it on first use.
    def definition(chart = @group.chart)
      definition_id(chart)
    end

    # The state ID of the author's state `name` in a definition.
    def state_id(definition, name)
      (0..).each do |state|
        response = ta.definition.state(system: system_handle, definition: definition, state: state)
        raise ScenarioError, "definition #{definition} has no state #{name}" if response[:result] == 'err_not_found'
        return state if core('ta_definition_state', response).fetch(:out).fetch(:id) == name.to_s
      end
    end

    # The label a Trace shows for a runtime actor ID.
    def actor_label(id)
      @actor_labels[id] ||= "actor.#{@actor_labels.size + 1}"
    end

    # Replaces runtime actor IDs in a journal entry by their labels, and the
    # session number inside a session ID's text.
    def label_actors(value, key = nil)
      case value
      when Hash then value.to_h { |name, inner| [name, label_actors(inner, name)] }
      when Array then value.map { |inner| label_actors(inner, key) }
      when String
        return label_session_text(value) if SESSION_TEXT_KEYS.include?(key)

        label_actor_id(value, key)
      when Integer then label_actor_id(value, key)
      else value
      end
    end

    # A session ID is the runtime ID its session started with (ADR 0066).
    ACTOR_ID_KEYS = %i[runtime parent actor session].freeze
    # A trace record's sessions as text (core ADR 0098):
    # <incarnation>.<session>.<family>, or the session's number alone.
    SESSION_TEXT_KEYS = %i[session_id cause_session_id].freeze

    def label_actor_id(value, key)
      ACTOR_ID_KEYS.include?(key) && value.to_s.match?(/\A[1-9]\d*\z/) ? actor_label(Integer(value)) : value
    end

    # The session number in a session ID's text, as its label.
    def label_session_text(text)
      return actor_label(Integer(text)) if text.match?(/\A[1-9]\d*\z/)

      match = text.match(/\A(\d+)\.([1-9]\d*)\.(.*)\z/m)
      match ? "#{match[1]}.#{actor_label(Integer(match[2]))}.#{match[3]}" : text
    end

    # Points `actor` at a new runtime ID (after a reload); its trace label stays.
    # Points an Actor at the session it has become. Load resolves the image's
    # family and revision against whatever definition is registered for them
    # now, which need not be the ID the actor was spawned under, so the
    # definition comes back from core rather than being remembered.
    def rebind(actor, id)
      @actor_labels[id] = actor_label(actor.handle)
      actor.handle = id
      facts = ta.actor.inspect(system: system_handle, actor: id)
      actor.definition = facts.dig(:out, :out, :definition_id) if facts[:result] == 'ok'
    end

    # The dotted text of an event-name ID.
    def event_text(id)
      @event_texts[id] ||= core('ta_event_name_format',
                                ta.event.name_format(system: system_handle, name: id, capacity: 1024)).fetch(:buffer)
    end

    # The text of a send ID, as a chart sees it (ADR 0048): the author's ID,
    # or `#_send.<n>`; nil for a send without one.
    def send_id_text(id)
      response = ta.send.id_format(system: system_handle, send_id: id, capacity: 1024)
      return nil if response[:result] == 'err_not_found'

      core('ta_send_id_format', response).fetch(:buffer)
    end

    # The send ID the text a chart holds names, or nil when no send of this
    # system can have it.
    def send_id(text)
      response = ta.send.id_parse(system: system_handle, text: text)
      return nil if %w[err_not_found err_invalid].include?(response[:result])

      core('ta_send_id_parse', response).fetch(:out)
    end

    # A session ID's text (core ADR 0088), <incarnation>.<session>.<family>,
    # from an inspection view, by ta_session_id_format; the test host's
    # systems have incarnation 0.
    def session_text(view)
      family = view.dig(:definition, :family, :name).to_s
      response = ta.session.id_format(system: system_handle, session: view.fetch(:session_id), family: family,
                                      capacity: 256)
      core('ta_session_id_format', response).fetch(:buffer).to_s
    end

    # The text of an invocation's ID (ADR 0049): the author's `<invoke id>`,
    # or the one core generated; the number when core no longer has it.
    def invoke_id_text(id)
      response = ta.system.invoke_id_text(system: system_handle, invoke: id)
      response[:result] == 'ok' ? response.dig(:out, :out).to_s : id.to_s
    end

    # A pending delayed send's target, from its ta_delayed_send_info: a literal
    # target is text in the owner's definition, named by content ID.
    def send_target(definition, send)
      return send[:target] unless send[:has_target_text]

      core('ta_definition_content',
           ta.definition.content(system: system_handle, definition: definition, content: send[:target_text]))
        .fetch(:out).fetch(:bytes)
    end

    # Interns one atom of the system.
    def atom(text)
      core('ta_system_intern_atom', ta.system.intern_atom(system: system_handle, text: text.to_s)).fetch(:out)
    end

    SCXML_INVOKER_TYPE = 'http://www.w3.org/TR/scxml/'

    def register_scxml_invoker(system)
      type = core('ta_system_intern_atom', ta.system.intern_atom(system: system, text: SCXML_INVOKER_TYPE)).fetch(:out)
      charts = @charts.to_h { |name, chart| [name.to_s, chart.is_a?(SCXMLChart) ? chart.invoker_entry : chart.to_spec] }
      files = @charts.values.grep(SCXMLChart).map(&:files).reduce({}, :merge)
      invoker = { '$adapter' => 'invoker.scxml', 'config' => { 'system' => system, 'charts' => charts, 'files' => files } }
      core('ta_system_register_invoker', ta.system.register_invoker(system: system, type: type, invoker: invoker))
    end

    # Interns a dotted event name through the public atom and event-name API.
    def event_name(event)
      @event_names[event.to_s] ||= begin
        atoms = TinyactorsTest.event_atoms(event).map do |segment|
          core('ta_system_intern_atom', ta.system.intern_atom(system: system_handle, text: segment)).fetch(:out)
        end
        core('ta_event_name_intern', ta.event.name_intern(system: system_handle, segments: { atoms: atoms })).fetch(:out)
      end
    end

    # The author's ID of a state, as a symbol.
    def state_name(definition, state)
      @state_names[[definition, state]] ||= begin
        info = core('ta_definition_state',
                    ta.definition.state(system: system_handle, definition: definition, state: state))
        info.fetch(:out).fetch(:id).to_sym
      end
    end

    private

    def instrument(label, kind)
      @instruments[label] ||= Instrument.new(@driver, name: "#{@run}.#{label}", kind: kind, label: label)
    end

    # One run of an assert_equivalent_runs body in a fresh context with a
    # trace; returns the trace, normalized.
    def traced_run(name, normalize, **options, &body)
      fresh_run(name, normalize, **options, &body).trace
    end

    # Runs `body` in a fresh context with a trace, tears it down, and
    # returns the context, its trace normalized.
    def fresh_run(name, normalize, **options, &body)
      context = Context.new(@driver, @group, @charts, run: "#{@run}/#{name}", trace: true, **options)
      context.instance_exec(&body)
      context.teardown!
      context.trace.steps.map! { |step| step.with(state: normalize.call(step.state)) } if normalize
      context
    end

    # Inputs one ta_run call may end when no allocation is counted or failed.
    RUN_OUTCOMES = 64

    # One logical ta_run with the budgets the Scheduler chose. A run counting
    # allocations, or failing one, ends one input per call, so that each call
    # is one macrostep, and records what it costs. A run whose allocation
    # fault is armed for it fails the fault's allocations inside it, checks
    # each actor a failure stops (#stopped), releases it, and runs again until
    # nothing fails: those retries are part of the same logical call. Any
    # other fault fails the step. Returns the last ta.run response.
    def run_logical(budget:, action_budget: 0, pause_at_microstep_start: false)
      capacity = @allocation_fault || @count_allocations ? 1 : RUN_OUTCOMES
      run_once = lambda do
        params = { system: system_handle, budget: budget, action_budget: action_budget,
                   pause_at_microstep_start: pause_at_microstep_start, outcome_capacity: capacity }
        params[:report_taken] = true if @mail_taken
        ta.run(**params)
      end
      index = @resume_index
      @resume_index += 1
      unless @allocation_fault || @count_allocations
        response = run_once.call
        note_outcomes(response)
        check_outcomes(response)
        return response
      end

      fault = @allocation_fault if @allocation_fault && @allocation_fault[:resume] == index
      probe_event
      refill_outside_faults
      before = allocator.stats[:allocations]
      allocator.arm_fault(after: fault[:after], count: fault[:count]) if fault
      response = run_once.call
      note_outcomes(response)
      retries = 0
      while fault && (stops = nomem_stops(response))
        @refusals << index if stops.empty?
        retries += 1
        raise ScenarioError, "run #{index}: still out of memory after #{retries} retries" if retries > fault[:count] + 1

        stops.each { |actor| stopped(actor, index, fault) }
        response = run_once.call
        note_outcomes(response)
      end
      allocator.clear_faults if fault
      check_outcomes(response)
      @resume_costs << allocator.stats[:allocations] - before if @count_allocations
      response
    end

    # The actors a ta.run response says ran out of memory, or [] when the
    # call itself did (it could not take its next input, which waits at the
    # head); nil when nothing did.
    def nomem_stops(response)
      return [] if response[:result] == 'err_nomem'

      faults = Array(response.dig(:out, :outcomes)).select do |outcome|
        outcome[:kind] == 'faulted' && outcome[:code] == 'err_nomem'
      end
      faults.empty? ? nil : faults.map { |outcome| outcome[:actor] }
    end

    # A session ID no actor has: generation 0 is never issued (ADR 0059).
    NO_SESSION = 0xFFFF_FFFF

    # Refills core's pool and name area outside the fault window: a ta_run
    # filtered to a session with nothing queued refills, a safe point, and
    # takes nothing, not even a macrostep paused in the slot.
    def refill_outside_faults
      response = ta.run(system: system_handle, session: NO_SESSION, budget: 1, outcome_capacity: 1)
      return if response[:result] == 'ok' && Array(response.dig(:out, :outcomes)).empty?

      raise ScenarioError, "the refill before a run answered #{response.inspect}"
    end

    # Fails the step on an input that faulted; ta_run's own result is checked
    # by the Scheduler.
    def check_outcomes(response)
      Array(response.dig(:out, :outcomes)).each do |outcome|
        next unless outcome[:kind] == 'faulted'

        raise CoreResultError.new('ta_run', outcome[:code], "actor #{outcome[:actor]} faulted")
      end
    end

    # An event name to offer a stopped actor. Every sweep run interns it
    # before its first resume, so that offering it allocates nothing and
    # every run numbers its atoms alike.
    def probe_event
      @probe_event ||= event_name('probe')
    end

    # Checks an actor that a failed allocation stopped (ADR 0047): it is in
    # its macrostep, holds a new event back until it ends, and no error event
    # was queued for the failure; then releases it. With `on_stop: :cycle` a
    # named actor is captured, unloaded and loaded again (ADR 0038) instead,
    # and the retry runs on the loaded actor; one that core holds admitted
    # mail for does not unload (its mail names the runtime ID), and is
    # released where it is.
    def stopped(id, index, fault)
      where = "run #{index} stopped by allocation #{fault[:after] + 1}"
      inspection = core('ta_actor_inspect', ta.actor.inspect(system: system_handle, actor: id)).fetch(:out)
      root = ta.actor.datamodel_root(system: system_handle, actor: id)
      @stops << { resume: index, microstep: inspection[:microstep_in_progress], data: root.dig(:out, :out) }
      raise AssertionFailed, "#{where}: the actor is not in a macrostep" unless inspection[:macrostep_in_progress]

      unless fault[:chart_errors]
        errors = Array(inspection[:internal_events]).map { |queued| event_text(queued.dig(:event, :name)) }
                                                      .grep(/\Aerror\./)
        raise AssertionFailed, "#{where}: running out of memory queued #{errors.inspect}" unless errors.empty?
      end

      offer_probe(id, where, inspection[:has_completion])
      actor = @actors.find { |candidate| candidate.handle == id }
      if fault[:on_stop] == :cycle && actor && actor.address
        allocator.clear_faults
        if cycle_unless_mail(actor)
          @cycles += 1
          return
        end
        @skipped += 1
      end

      core('ta_actor_release', ta.actor.release(system: system_handle, actor: id))
    end

    # #cycle_residency, unless core holds admitted mail for the actor, which
    # makes the unload answer err_busy; returns whether it cycled.
    def cycle_unless_mail(actor)
      image = capture(actor)
      binding = actor.data
      result = unload(actor, image)
      if result == 'err_busy'
        destroy_image(image)
        return false
      end
      core('ta_actor_unload', { result: result })
      core('ta_actor_load', { result: load(actor, image, binding: binding) })
      destroy_image(image)
      true
    end

    # The correlation of the probe event #offer_probe delivers.
    PROBE_CORRELATION = 9_007_199_254_740_991

    # Offers a stopped actor an event: a finished one refuses it with
    # err_done; any other admits it and holds it back, so that the host can
    # still withdraw it. Its name is interned before the run, and its
    # time is the system's (core ADR 0094 section 1), so the answer is the
    # admission test's, not a time error.
    def offer_probe(id, where, finished)
      event = { name: probe_event, type: 'external', invoke_id: INVOKE_ID_NONE }
      offered = ta.actor.deliver(system: system_handle, actor: id, event: event, time: now,
                                 correlation: PROBE_CORRELATION)[:result]
      expected = finished ? 'err_done' : 'ok'
      raise AssertionFailed, "#{where}: an event offered to it was answered #{offered}" unless offered == expected
      return if finished

      withdrawn = ta.system.withdraw_input(system: system_handle, correlation: PROBE_CORRELATION)[:result]
      raise AssertionFailed, "#{where}: the stopped actor took the event offered to it" unless withdrawn == 'ok'
    end

    # Compiles and registers a chart once per scenario; the system then owns
    # it. Takes a registry name or a Chart the scenario built itself.
    def definition_id(chart)
      unless chart.is_a?(Chart) || chart.is_a?(SCXMLChart)
        name = chart.to_sym
        chart = @charts.fetch(name) { raise ScenarioError, "unknown chart #{name}" }
      end
      @definitions[chart] ||= if chart.is_a?(SCXMLChart)
                                import_scxml(chart)
                              else
                                compile_and_register(chart)
                              end
    end

    # ta_definition_import through the SCXML codec: decode, compile, register.
    # A refusal says where in the document and why (ADR 0058).
    def import_scxml(chart)
      response = ta.definition.import(system: system_handle, codec: { '$adapter' => 'codec.scxml' },
                                      input: chart.source,
                                      resolver: { '$adapter' => 'resolver.map', 'config' => { 'files' => chart.files } },
                                      datamodels: [datamodel_adapter(chart)], allocator: allocator.adapter)
      unless response[:result] == 'ok'
        detail = "#{chart.name}: #{TinyactorsTest.describe_import_error(ta, response.dig(:out, :error))}"
        raise CoreResultError.new('ta_definition_import', response[:result], detail)
      end

      response.fetch(:out, {}).fetch(:out)
    end

    def compile_and_register(chart)
      compiled = core('ta_definition_compile_spec',
                      ta.definition.compile_spec(system: system_handle, spec: chart.to_spec,
                                                 datamodel: datamodel_adapter(chart), allocator: allocator.adapter))
      core('ta_system_register_definition',
           ta.system.register_definition(system: system_handle, definition: compiled.fetch(:out)))
        .fetch(:out)
    end

    # The data-model adapter config the chart's datamodel names.
    def datamodel_adapter(chart)
      datamodel = chart.datamodel || DEFAULT_DATAMODEL
      DATAMODELS.fetch(datamodel) do
        raise ScenarioError, "chart #{chart.name}: no data-model adapter for datamodel #{datamodel.inspect}"
      end
    end

    def check_invariants(step)
      @group.invariants.each do |invariant|
        @actors.each do |actor|
          next if invariant.block.call(actor)

          raise AssertionFailed,
                "invariant #{invariant.name.inspect} violated by #{actor} after #{step}; active: #{actor.active.join(', ')}"
        end
      end
    end
  end

  def self.event_atoms(event)
    atoms = event.to_s.split('.', -1)
    raise ScenarioError, "invalid event name #{event.inspect}" if atoms.empty? || atoms.any?(&:empty?)

    atoms
  end
end
