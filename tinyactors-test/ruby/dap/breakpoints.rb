# frozen_string_literal: true

module DAP
  # The breakpoints a client set that need no source spans: function
  # breakpoints by name, the exception filters, and data breakpoints on
  # data-model locations. Each is found in the trace records a resume
  # wrote, so a stop comes at the first unit boundary after the record
  # (the host single-steps on `action_budget: 1` while any is set).
  class Breakpoints
    EXCEPTION_FILTERS = [
      { 'filter' => 'error.execution', 'label' => 'error.execution',
        'description' => 'An action, a guard or a <data> failed, and core raised error.execution' },
      { 'filter' => 'error.communication', 'label' => 'error.communication',
        'description' => 'A <send> or an invocation could not reach its target' },
      { 'filter' => 'error.platform', 'label' => 'error.platform',
        'description' => 'The platform raised an error event' },
      { 'filter' => 'ignored', 'label' => 'Ignored events',
        'description' => 'An event was taken and enabled no transition (ADR 0072)' },
      { 'filter' => 'stopped', 'label' => 'Out of memory',
        'description' => 'A run stopped because an allocation failed (ADR 0070)' }
    ].freeze
    FILTERS = EXCEPTION_FILTERS.map { |filter| filter['filter'] }.freeze

    # A function breakpoint: `past_due` (entry), `exit:past_due`,
    # `on:payment.failed` (a macrostep takes that event), `idle → busy` (a
    # transition, named as Labels#transition names it; `->` also works).
    Function = Struct.new(:id, :name, :kind, :target, :condition, :hit_condition, :hits, keyword_init: true)
    # A data breakpoint on one actor's location: `path` as jq's path() answers it.
    Watch = Struct.new(:id, :data_id, :label, :path, :condition, :hit_condition, :hits, keyword_init: true)

    def initialize(labels, expressions)
      @labels = labels
      @expressions = expressions
      @functions = []
      @data = []
      @filters = []
      @next_id = 0
    end

    def any? = !@functions.empty? || !@data.empty? || !@filters.empty?

    def watching_data? = !@data.empty?

    # setFunctionBreakpoints: replaces them all. Answers DAP Breakpoints.
    def set_functions(specs)
      @functions = specs.map do |spec|
        name = spec.fetch('name').to_s.strip
        kind, target = parse(name)
        Function.new(id: (@next_id += 1), name: name, kind: kind, target: target, condition: spec['condition'],
                     hit_condition: spec['hitCondition'], hits: 0)
      end
      @functions.map { |breakpoint| { 'id' => breakpoint.id, 'verified' => true } }
    end

    def set_filters(filters)
      unknown = filters - FILTERS
      raise RequestError, "unknown exception filter(s): #{unknown.join(', ')}" unless unknown.empty?

      @filters = filters
    end

    # setDataBreakpoints: replaces them all. Answers DAP Breakpoints.
    def set_data(specs)
      @data = []
      specs.map do |spec|
        label, path = Breakpoints.parse_data_id(spec['dataId'])
        if label.nil?
          next { 'verified' => false, 'message' => "#{spec['dataId'].inspect} is not a data-model location" }
        end
        unless spec['accessType'].nil? || spec['accessType'] == 'write'
          next { 'verified' => false, 'message' => 'only writes can be watched: core traces assignments, not reads' }
        end

        breakpoint = Watch.new(id: (@next_id += 1), data_id: spec['dataId'], label: label, path: path,
                              condition: spec['condition'], hit_condition: spec['hitCondition'], hits: 0)
        @data << breakpoint
        { 'id' => breakpoint.id, 'verified' => true }
      end
    end

    # A dataId names an actor by its thread label and a location by its
    # path: `acct1 ["failures"]`.
    def self.data_id(label, path) = "#{label} #{JSON.generate(path)}"

    def self.parse_data_id(text)
      label, path = text.to_s.split(' ', 2)
      path = JSON.parse(path.to_s)
      path.is_a?(Array) ? [label, path] : nil
    rescue JSON::ParserError
      nil
    end

    # ------------------------------------------------------------------
    # Hits
    #
    # A hit is {kind:, session:, breakpoints:, description:, exception:}.
    # `observe` finds candidates in each record as it is read; the Control
    # confirms function and data hits with `fire?` when it decides to stop,
    # so conditions are evaluated against the data the stop shows.

    # `on:` breakpoints for an actor about to take `event`, before its
    # macrostep begins, confirmed. Answers their IDs.
    def taking(thread, event)
      return [] if event.nil?

      name = event[:name_text] || @expressions.context.event_text(event[:name])
      @functions.select do |breakpoint|
        breakpoint.kind == :on && matches_event?(breakpoint.target, name) && fire?(breakpoint, thread)
      end.map(&:id)
    end

    # Yields the hits `record` makes. `track` is its actor's Track, already
    # brought up to date with the record.
    def observe(record, track, thread, &hit)
      return unless any?

      exception(record, track, &hit)
      functions(record, track, &hit) unless @functions.empty?
      data(record, thread, &hit) unless @data.empty? || thread.nil?
    end

    # The condition holds and the hit condition is met, counting this hit.
    def fire?(breakpoint, thread)
      if breakpoint.condition && !breakpoint.condition.strip.empty? && thread
        begin
          return false unless @expressions.holds?(thread, breakpoint.condition)
        rescue RequestError
          # A condition that fails stops, so the author sees it fail.
          nil
        end
      end
      breakpoint.hits += 1
      hit_condition_met?(breakpoint.hit_condition, breakpoint.hits)
    end

    private

    def exception(record, track)
      track.fault = record[:fault] if record[:has_fault]
      filter = exception_filter(record)
      return unless filter && @filters.include?(filter)

      event = record[:event]
      fault = filter.start_with?('error.') ? track.fault : nil
      yield({ kind: :exception, session: track.session, filter: filter, event: event, fault: fault,
              description: exception_text(filter, event, fault) })
    end

    # Entry, exit and transition breakpoints are noted when the MICROSTEP
    # record names their state or transition, and hit once the actor
    # stands at it: before the state's onentry or onexit, or before the
    # transition's content. When there is no unit to stand before (a state
    # without handlers), they hit at the next place the actor stops.
    def functions(record, track, &hit)
      definition = record[:definition]
      case record[:kind]
      when 'microstep'
        overdue(track, &hit)
        @functions.each do |breakpoint|
          target = pending_target(breakpoint, record, definition)
          track.pending << { breakpoint: breakpoint, at: target[0], id: target[1] } if target
        end
      when 'paused'
        position = Position.from_record(record)
        reached, track.pending = track.pending.partition { |pending| reached?(pending, position) }
        reached.each { |pending| yield function_hit(track, pending[:breakpoint]) }
      when 'macrostep_end', 'finished', 'destroyed'
        overdue(track, &hit)
      when 'macrostep_begin'
        return if track.entry_stopped || record[:event].nil?

        @functions.each do |breakpoint|
          next unless breakpoint.kind == :on && matches_event?(breakpoint.target, record.dig(:event, :name_text))

          yield function_hit(track, breakpoint)
        end
      end
    end

    def overdue(track)
      track.pending.each { |pending| yield function_hit(track, pending[:breakpoint]) }
      track.pending = []
    end

    def function_hit(track, breakpoint)
      { kind: :function, session: track.session, breakpoints: [breakpoint], description: describe(breakpoint) }
    end

    # [phase, ID] a breakpoint waits for in this microstep, or nil.
    def pending_target(breakpoint, record, definition)
      case breakpoint.kind
      when :entry
        state = Array(record[:entered]).find { |id| @labels.state(definition, id) == breakpoint.target }
        state && ['enter', Integer(state)]
      when :exit
        state = Array(record[:exited]).find { |id| @labels.state(definition, id) == breakpoint.target }
        state && ['exit', Integer(state)]
      when :transition
        transition = Array(record[:transitions]).find do |id|
          label = normalize(@labels.transition(definition, id))
          label == breakpoint.target || label.start_with?("#{breakpoint.target} on ")
        end
        transition && ['transition', Integer(transition)]
      end
    end

    # A microstep exits states highest ID first, runs transition content,
    # then enters states lowest ID first (ADR 0061 section 2).
    PHASES = { 'exit' => 0, 'transition' => 1, 'enter' => 2 }.freeze

    def reached?(pending, position)
      rank = PHASES.fetch(position.at, 3)
      target = PHASES.fetch(pending[:at])
      return rank > target unless rank == target

      case pending[:at]
      when 'exit' then Integer(position.state) <= pending[:id]
      when 'enter' then Integer(position.state) >= pending[:id]
      else Integer(position.transition) == pending[:id]
      end
    end

    # A write to a watched location of the actor.
    def data(record, thread)
      return unless record[:kind] == 'assign'

      watching = @data.select { |breakpoint| breakpoint.label == thread.label }
      return if watching.empty?

      location = @labels.expression(record[:definition], record[:expr])
      return if location.nil?

      written = begin
        @expressions.path(thread, location)
      rescue RequestError
        return
      end
      hits = watching.select { |breakpoint| overlap?(breakpoint.path, written) }
      return if hits.empty?

      yield({ kind: :data, session: record.dig(:key, :session), breakpoints: hits,
              description: "#{thread.label}: #{location} = #{JSON.generate(record[:value])}" })
    end

    def parse(name)
      case name
      when /\Aexit:\s*(\S+)\z/ then [:exit, Regexp.last_match(1)]
      when /\Aon:\s*(\S+)\z/ then [:on, Regexp.last_match(1)]
      when /(→|->)/ then [:transition, normalize(name)]
      else [:entry, name]
      end
    end

    def normalize(text) = text.gsub('->', '→').gsub(/\s+/, ' ').strip

    def describe(breakpoint)
      case breakpoint.kind
      when :entry then "entered #{breakpoint.target}"
      when :exit then "exited #{breakpoint.target}"
      when :on then "took #{breakpoint.target}"
      else breakpoint.target
      end
    end

    # SCXML event descriptors match by prefix at dot boundaries (3.12.1).
    def matches_event?(descriptor, name)
      return false if name.nil?

      descriptor = descriptor.delete_suffix('.*').delete_suffix('.')
      descriptor == '*' || name == descriptor || name.start_with?("#{descriptor}.")
    end

    # `5` and `>= 5` stop from the fifth hit on, `== 5` only at it, `% 5`
    # at every fifth.
    def hit_condition_met?(text, hits)
      return true if text.nil? || text.strip.empty?

      match = /\A\s*(>=|>|==|<=|<|%)?\s*(\d+)\s*\z/.match(text)
      return true if match.nil?

      count = Integer(match[2])
      case match[1]
      when '>' then hits > count
      when '==' then hits == count
      when '<=' then hits <= count
      when '<' then hits < count
      when '%' then count.positive? && (hits % count).zero?
      else hits >= count
      end
    end

    def overlap?(watched, written)
      length = [watched.size, written.size].min
      watched.first(length) == written.first(length)
    end

    def exception_filter(record)
      case record[:kind]
      when 'raised'
        name = record.dig(:event, :name_text).to_s
        FILTERS.first(3).find { |filter| name == filter || name.start_with?("#{filter}.") }
      when 'ignored' then 'ignored'
      when 'stopped' then 'stopped'
      end
    end

    def exception_text(filter, event, fault)
      case filter
      when 'ignored' then "#{event&.dig(:name_text)} enabled no transition"
      when 'stopped' then 'an allocation failed; the run stopped where it stood'
      else
        message = fault && (fault[:message] || "code #{fault[:code]}")
        [event&.dig(:name_text), message].compact.join(': ')
      end
    end
  end
end
