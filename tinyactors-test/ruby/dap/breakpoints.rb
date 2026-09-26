# frozen_string_literal: true

module DAP
  # The breakpoints a client set: line breakpoints (and log points) on the
  # chart's Ruby files, mapped to compiled IDs through source spans (core
  # ADR 0086, SourceMap); function breakpoints by name; the exception
  # filters; and data breakpoints on data-model locations. Each is found in
  # the trace records a resume wrote, or in where the actor stands after it
  # (core ADR 0083), so a stop comes at the first unit boundary after the
  # record (the host single-steps on `action_budget: 1` while any is set).
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
    Watch = Struct.new(:id, :data_id, :title, :path, :condition, :hit_condition, :hits, keyword_init: true)
    # A line breakpoint, or a log point when it has a `log_message`. `line`
    # is where the client put it; `targets` maps each Definition to the
    # element the line snapped to (SourceMap::Element), and `actual` is the
    # line of that element, which the client shows.
    Line = Struct.new(:id, :path, :line, :condition, :hit_condition, :log_message, :hits, :targets, :actual,
                      keyword_init: true) do
      def verified? = !targets.empty?

      def to_dap(message = nil)
        body = { 'id' => id, 'verified' => verified?, 'line' => actual || line }
        body['source'] = { 'name' => File.basename(path), 'path' => path }
        body['message'] = message || 'no state, transition or action of a loaded chart is on or near this line' unless verified?
        body
      end
    end

    def initialize(labels, expressions, source_map)
      @labels = labels
      @expressions = expressions
      @map = source_map
      @functions = []
      @data = []
      @filters = []
      @lines = {}
      @next_id = 0
    end

    def any? = !@functions.empty? || !@data.empty? || !@filters.empty? || @lines.values.any? { |set| !set.empty? }

    # Whether `observe` needs the thread of a record: data breakpoints
    # evaluate the location an assignment wrote in its actor.
    def needs_thread? = !@data.empty?

    # setBreakpoints: replaces the line breakpoints of one file, resolved
    # against `definitions`. Answers DAP Breakpoints.
    def set_lines(path, specs, definitions)
      @lines[path] = specs.map do |spec|
        breakpoint = Line.new(id: (@next_id += 1), path: path, line: Integer(spec.fetch('line')),
                              condition: spec['condition'], hit_condition: spec['hitCondition'],
                              log_message: spec['logMessage'], hits: 0, targets: {})
        resolve(breakpoint, definitions)
        breakpoint
      end
      @lines[path].map(&:to_dap)
    end

    # Resolves every line breakpoint again, after definitions came or went
    # (a hot reload, a fork). Answers the ones whose verification or line
    # changed, for `breakpoint` events.
    def refresh_lines(definitions)
      @lines.values.flatten.select do |breakpoint|
        before = [breakpoint.verified?, breakpoint.actual]
        resolve(breakpoint, definitions)
        before != [breakpoint.verified?, breakpoint.actual]
      end
    end

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
        title, path = Breakpoints.parse_data_id(spec['dataId'])
        if title.nil?
          next { 'verified' => false, 'message' => "#{spec['dataId'].inspect} is not a data-model location" }
        end
        unless spec['accessType'].nil? || spec['accessType'] == 'write'
          next { 'verified' => false, 'message' => 'only writes can be watched: core traces assignments, not reads' }
        end

        breakpoint = Watch.new(id: (@next_id += 1), data_id: spec['dataId'], title: title, path: path,
                              condition: spec['condition'], hit_condition: spec['hitCondition'], hits: 0)
        @data << breakpoint
        { 'id' => breakpoint.id, 'verified' => true }
      end
    end

    def log_point?(breakpoint) = breakpoint.is_a?(Line) && !breakpoint.log_message.to_s.empty?

    # A log point's message, with each `{expression}` replaced by its value
    # in the actor's data model.
    def log_message(breakpoint, thread)
      breakpoint.log_message.gsub(/\{([^{}]+)\}/) do
        expression = Regexp.last_match(1)
        begin
          value = @expressions.value(thread, expression, event: nil)
          value.is_a?(String) ? value : JSON.generate(value)
        rescue RequestError => error
          "{#{expression}: #{error.message}}"
        end
      end
    end

    # A dataId names an actor by its thread's title and a location by its
    # path: `subscription/acct1 ["failures"]`, as napi's adapter does.
    def self.data_id(title, path) = "#{title} #{JSON.generate(path)}"

    # A title can have spaces (`counter/acct1 (fork 1)`), so the path is
    # what follows the first ` [` that starts a JSON array.
    def self.parse_data_id(text)
      text = text.to_s
      at = text.index(' [')
      while at&.positive?
        begin
          path = JSON.parse(text[(at + 1)..])
          return path.is_a?(Array) && !path.empty? ? [text[0...at], path] : nil
        rescue JSON::ParserError
          at = text.index(' [', at + 1)
        end
      end
      nil
    end

    # ------------------------------------------------------------------
    # Hits
    #
    # A hit is {kind:, key:, breakpoints:, description:, exception:}, `key`
    # the track key of the actor it is in. `observe` finds candidates in
    # each record as it is read, and `reach` in where the actor stands after
    # a resume; the Control confirms line, function and data hits with
    # `fire?` when it decides to stop, so conditions are evaluated against
    # the data the stop shows.

    # `on:` breakpoints for an actor about to take `event`, before its
    # macrostep begins, confirmed. Answers their IDs.
    def taking(thread, event)
      return [] if event.nil?

      name = event[:name_text] || thread.context.event_text(event[:name])
      @functions.select do |breakpoint|
        breakpoint.kind == :on && matches_event?(breakpoint.target, name) && fire?(breakpoint, thread)
      end.map(&:id)
    end

    # Yields the hits `record` makes. `track` is its actor's Track, already
    # brought up to date with the record.
    def observe(record, track, thread, &hit)
      return unless any?

      exception(record, track, &hit)
      blocks(record, track, &hit)
      data(record, track, thread, &hit) unless @data.empty? || thread.nil?
    end

    # Yields what an actor that stands at `position` (core ADR 0083: the
    # unit it runs next) has reached: a line breakpoint on that action, and
    # the entry, exit and transition breakpoints whose block it is about to
    # run, or passed.
    def reach(track, position)
      reached, track.pending = track.pending.partition { |pending| reached?(pending, position) }
      reached.each { |pending| yield block_hit(track, pending[:breakpoint]) }
      return if position.action.nil?
      # A <foreach> that has not run and the same loop before its first
      # iteration stand at one position (ADR 0083, open point 1): one hit.
      return if position == track.reached

      track.reached = position
      lines = line_breakpoints(track.definition, :action, position.action)
      yield line_hit(track, lines, 'before') unless lines.empty?
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

    def resolve(breakpoint, definitions)
      breakpoint.targets = definitions.filter_map do |definition|
        element = @map.resolve(definition, breakpoint.path, breakpoint.line)
        [definition, element] if element
      end.to_h
      # The workbench's definitions come first: the file on disk is theirs.
      breakpoint.actual = breakpoint.targets.values.first&.line
    end

    # The line breakpoints on the element `kind` `id` of `definition`.
    def line_breakpoints(definition, kind, id)
      @lines.values.flatten.select do |breakpoint|
        element = breakpoint.targets[definition]
        element && element.kind == kind && element.id == Integer(id)
      end
    end

    def line_hit(track, breakpoints, where)
      element = breakpoints.first.targets[track.definition]
      { kind: :line, key: track.key, breakpoints: breakpoints,
        description: "#{where} line #{element.line} of #{File.basename(breakpoints.first.path)}" }
    end

    def exception(record, track)
      track.fault = record[:fault].merge(where: fault_where(record)) if record[:has_fault]
      filter = exception_filter(record)
      return unless filter && @filters.include?(filter)

      event = record[:event]
      fault = filter.start_with?('error.') ? track.fault : nil
      yield({ kind: :exception, key: track.key, filter: filter, event: event, fault: fault,
              description: exception_text(filter, event, fault) })
    end

    # Entry, exit and transition breakpoints, and line breakpoints on a
    # state (its entry) or a transition (its content), are noted when the
    # MICROSTEP record names their state or transition, and hit (`reach`)
    # once the actor stands before the block they name: the state's onentry
    # or onexit, or the transition's content. When there is no unit to
    # stand before (a state without handlers), they hit at the next place
    # the actor stops. A microstep's first block is reached at its start,
    # where the actor stands before its first unit (ADR 0087).
    def blocks(record, track, &hit)
      definition = record[:definition]
      case record[:kind]
      when 'microstep'
        overdue(track, &hit)
        pending_blocks(record, definition).each { |pending| track.pending << pending }
      when 'macrostep_end', 'finished', 'destroyed'
        overdue(track, &hit)
      when 'macrostep_begin'
        return if track.entry_stopped || record[:event].nil?

        @functions.each do |breakpoint|
          next unless breakpoint.kind == :on && matches_event?(breakpoint.target, record.dig(:event, :name_text))

          yield block_hit(track, breakpoint)
        end
      end
    end

    def overdue(track)
      track.pending.each { |pending| yield block_hit(track, pending[:breakpoint]) }
      track.pending = []
    end

    def block_hit(track, breakpoint)
      return line_hit(track, [breakpoint], 'at') if breakpoint.is_a?(Line)

      { kind: :function, key: track.key, breakpoints: [breakpoint], description: describe(breakpoint) }
    end

    # {breakpoint:, at:, id:} of what this microstep will reach: [phase, ID].
    def pending_blocks(record, definition)
      pending = @functions.filter_map do |breakpoint|
        target = function_target(breakpoint, record, definition)
        target && { breakpoint: breakpoint, at: target[0], id: target[1] }
      end
      Array(record[:entered]).each do |state|
        line_breakpoints(definition, :state, state).each do |breakpoint|
          pending << { breakpoint: breakpoint, at: 'enter', id: Integer(state) }
        end
      end
      Array(record[:transitions]).each do |transition|
        line_breakpoints(definition, :transition, transition).each do |breakpoint|
          pending << { breakpoint: breakpoint, at: 'transition', id: Integer(transition) }
        end
      end
      pending
    end

    # [phase, ID] a function breakpoint waits for in this microstep, or nil.
    def function_target(breakpoint, record, definition)
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
    def data(record, track, thread)
      return unless record[:kind] == 'assign'

      watching = @data.select { |breakpoint| breakpoint.title == thread.title }
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

      yield({ kind: :data, key: track.key, breakpoints: hits,
              description: "#{written.join('.')} = #{JSON.generate(record[:value])}" })
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
        message = fault && (fault[:message].to_s.empty? ? nil : fault[:message])
        [event&.dig(:name_text), fault&.dig(:where), message].compact.join(': ')
      end
    end

    # What faulted, as napi's adapter names it: the action, or the
    # transition whose guard failed.
    def fault_where(record)
      definition = record[:definition]
      action = record[:action]
      action = record.dig(:position, :action) if Labels.none?(action)
      return @labels.action_name(definition, action) unless Labels.none?(action)

      transition = record.dig(:position, :transition)
      Labels.none?(transition) ? nil : @labels.transition_name(definition, transition)
    end
  end
end
