# frozen_string_literal: true

module DAP
  # The debug console: what a client sends as `evaluate` with context
  # `repl`, and the completions it offers there.
  #
  #   send [@actor] <event> [json]           deliver to the actor, with the debugger as its origin
  #   goto [@actor] <state…> [from <state>]  an operator transition (core ADR 0082)
  #   abandon [@actor]                       abandon a macrostep that never ends (core ADR 0082)
  #   cancel [@actor] <send ID>              withdraw one pending delayed send (core ADR 0084)
  #   fork [@actor]                          copy the actor into a system of its own (core ADR 0085)
  #   clock [@actor] +<duration>             advance the logical time of the actor's system
  #   <expression>                           jq over the actor's data model
  #
  # The actor is the frame's, or the one `@actor` names by its thread label.
  #
  # The debugger is a sender an actor can answer (core ADR 0068): the
  # adapter registers an I/O processor of type `debug` in every system it
  # debugs (the workbench's and each fork's), opens one scope there, and
  # delivers each `send` with an origin in that scope, so
  # `<send targetexpr="$_event.origin" typeexpr="$_event.origintype">`
  # reaches the console. Closing the scope at the end of the session leaves
  # later replies with no origin, which fail as error.communication.
  class Console
    TYPE = 'debug'
    URI = 'debug://console'
    # The instrument that makes the origins: io.connection holds a scope per
    # URI and delivers with a SCOPED origin, and it journals replies as sends.
    # io.recording would journal replies too, but it cannot make an origin,
    # and the host does not bind ta_io_origin_make (bindings.rb).
    INSTRUMENT = 'io.connection'
    DURATION = /\A\+?(\d+(?:\.\d+)?)(ms|s|m|h|d)?\z/
    SCALE = { nil => 1, 'ms' => 1, 's' => 1_000, 'm' => 60_000, 'h' => 3_600_000, 'd' => 86_400_000 }.freeze

    COMMANDS = %w[send goto abandon cancel fork clock].freeze

    # Console commands the test host does not offer, and why.
    PENDING = {
      'replace' => 'not offered here: save the chart file, and the workbench replaces its actors in place'
    }.freeze

    def initialize(threads:, expressions:, output:)
      @threads = threads
      @expressions = expressions
      @output = output
      @variables = nil
      @operator = nil
      @forker = nil
      # context => the debug I/O processor of that system
      @ios = {}
      @queued = []
      @reply_cursors = {}
    end

    # The Variables that present values, the Operator that jumps, abandons
    # and withdraws, and what makes a fork (called with a thread, answers
    # the console's text); all are made after the console.
    attr_writer :variables, :operator, :forker

    # Registers the `debug` processor in `context`'s system and opens the
    # scope there. Tells `expressions` how to show the origin of an event
    # the console sent. Every system registers it first, so it has the same
    # registry index in each.
    def open(context)
      io = context.endpoint_io(TYPE, INSTRUMENT)
      io.control('open', processor: io.processor, uri: URI)
      @expressions.io_origins[io.processor] = { type: TYPE, uri: URI }
      @ios[context] = io
    end

    def close
      @ios.each_value do |io|
        io.control('close', uri: URI)
      rescue TinyactorsTest::RPCError
        nil
      end
    end

    # The processors $_ioprocessors lists besides SCXML's, with the location
    # io.connection renders (`%s` is the session ID).
    def self.processors = { TYPE => 'ws://here/session/%s' }

    # ------------------------------------------------------------------
    # evaluate, context repl

    # Answers an evaluate body. `thread` is the frame's thread, or nil.
    def call(line, thread, event)
      word, rest = line.strip.split(/\s+/, 2)
      case word
      when 'send' then deliver(rest.to_s, thread)
      when 'goto' then goto(*addressed(rest, thread))
      when 'abandon' then text(@operator.abandon(addressed(rest, thread).first))
      when 'cancel' then text(@operator.withdraw(*addressed(rest, thread)))
      when 'fork' then text(@forker.call(addressed(rest, thread).first))
      when 'clock' then clock(*addressed(rest, thread))
      when *PENDING.keys then raise RequestError, "#{word}: #{PENDING.fetch(word)}"
      else expression(line, thread, event)
      end
    end

    def expression(text, thread, event)
      thread ||= default_thread or raise RequestError, 'no actor to evaluate in'
      outputs = @expressions.evaluate(thread, text, event: event)
      value = outputs.size == 1 ? outputs.first : outputs
      body = @variables.present(value)
      body['result'] = outputs.empty? ? '(no value)' : body['result']
      body
    end

    # Debugger messages delivered to a busy actor wait in the instrument,
    # oldest first. Answers what they are for `thread`.
    def queued_for(thread) = @queued.select { |message| message[:handle] == thread.handle }

    def queued? = !@queued.empty?

    # Offers waiting messages again; delivered ones leave the queue, and
    # their system runs them. Answers how many were delivered.
    def flush
      return 0 if @queued.empty?

      delivered = 0
      @ios.each do |context, io|
        next unless @queued.any? { |message| message[:context].equal?(context) }

        count = Array(io.control('flush')[:results]).size
        mine = @queued.select { |message| message[:context].equal?(context) }.first(count)
        @queued -= mine
        mine.each { |message| @output.call('console', "#{message[:label]} took #{message[:event]} from the console") }
        context.scheduler.wake if count.positive?
        delivered += count
      end
      delivered
    end

    # A fork's system is gone: its waiting messages with it.
    def forget(context)
      @ios.delete(context)
      @reply_cursors.delete(context)
      @queued.reject! { |message| message[:context].equal?(context) }
    end

    # Replies the actors sent to the console since the last call.
    def replies
      @ios.each do |context, io|
        entries = io.journal(since: @reply_cursors[context]).select { |entry| entry[:op] == 'send' }
        next if entries.empty?

        @reply_cursors[context] = entries.last[:seq]
        entries.each do |entry|
          who = source_thread(context, entry[:source])&.label || 'an actor'
          data = entry[:data].nil? ? '' : " #{JSON.generate(entry[:data])}"
          refused = entry[:refused] ? " (refused: #{entry[:refused]})" : ''
          @output.call('console', "#{who} replied #{entry[:event_text]}#{data}#{refused}")
        end
      end
    end

    # ------------------------------------------------------------------
    # completions

    # Completion targets for `text` with the cursor at `column` (0-based).
    def completions(text, column, thread)
      before = text[0, column].to_s
      thread ||= default_thread
      if (match = /\Asend\s+(?:@(\S+)\s+)?(\S*)\z/.match(before))
        target = match[1] ? @threads.by_label(match[1]) : thread
        prefix = match[2]
        return events_of(target).select { |name| name.start_with?(prefix) }
                                .map { |name| target(name, column - prefix.length, prefix.length, 'event') }
      end
      if (match = /\Agoto\s+(?:@(\S+)\s+)?(?:\S+\s+)*(\S*)\z/.match(before))
        target = match[1] ? @threads.by_label(match[1]) : thread
        prefix = match[2]
        names = target ? @operator.states(target.definition).map(&:last) : []
        return names.select { |name| name.start_with?(prefix) }
                    .map { |name| target(name, column - prefix.length, prefix.length, 'value') }
      end
      if (match = /\A\s*(\w*)\z/.match(before)) && !match[1].empty?
        commands = COMMANDS.select { |word| word.start_with?(match[1]) }
        return commands.map { |word| target(word, column - match[1].length, match[1].length, 'keyword') } unless commands.empty?
      end
      locations(before, column, thread)
    end

    private

    def default_thread = @threads.to_a.first

    # [thread, the rest of the line]: `@actor` names the thread by its
    # label; otherwise it is the frame's, or the first.
    def addressed(rest, thread)
      rest = rest.to_s.strip
      if rest.start_with?('@')
        label, rest = rest.split(/\s+/, 2)
        thread = @threads.by_label(label.delete_prefix('@')) or raise RequestError, "no actor #{label}"
      end
      thread ||= default_thread or raise RequestError, 'no actor'
      [thread, rest.to_s]
    end

    def text(result) = { 'result' => result, 'variablesReference' => 0 }

    def goto(thread, rest)
      words = rest.split(/\s+/)
      from = nil
      if (at = words.index('from'))
        from = words[at + 1] or raise RequestError, 'goto <state…> from <state>'
        words = words[0...at]
      end
      text(@operator.jump(thread, words, from: from))
    end

    def deliver(rest, thread)
      thread, rest = addressed(rest, thread)
      event, payload = rest.to_s.split(/\s+/, 2)
      raise RequestError, 'send [@actor] <event> [json]' if event.nil? || event.empty?

      data = payload.nil? || payload.strip.empty? ? nil : parse_json(payload)
      context = thread.context
      io = @ios.fetch(context)
      args = { processor: io.processor, actor: thread.handle, event: event, uri: URI, hold: true }
      args[:data] = data unless data.nil?
      answer = io.control('receive', **args)
      case answer[:result]
      when 'held'
        @queued << { context: context, handle: thread.handle, label: thread.label, event: event, data: data }
        text = "queued #{event} for #{thread.label}: it is inside a macrostep"
      when 'ok'
        text = "sent #{event} to #{thread.label}"
        text += '; it runs when you continue' if context.scheduler.stopped?
        context.scheduler.wake
      else
        raise RequestError, "#{thread.label} refused #{event}: #{answer[:result]}"
      end
      { 'result' => text, 'variablesReference' => 0 }
    end

    # Logical time belongs to a system (ADR 0012): the frame's actor's, the
    # workbench's or a fork's.
    def clock(thread, rest)
      match = DURATION.match(rest.strip)
      raise RequestError, 'clock +<duration>, such as +500ms, +5s, +2m, +1h or +3d' unless match && rest.strip.start_with?('+')

      milliseconds = (Float(match[1]) * SCALE.fetch(match[2])).round
      context = thread.context
      # Time moves now; what falls due runs once the answer is out, like
      # everything the adapter runs.
      context.set_time(context.now + milliseconds)
      context.scheduler.wake
      where = thread.fork? ? "#{thread.label}: " : ''
      { 'result' => "#{where}t=#{context.now}ms", 'variablesReference' => 0 }
    end

    def parse_json(text)
      JSON.parse(text)
    rescue JSON::ParserError => error
      raise RequestError, "the event data is not JSON: #{error.message}"
    end

    # The events the thread's chart has transitions for: a descriptor may
    # name several, separated by spaces.
    def events_of(thread)
      return [] if thread.nil?

      @operator.events(thread.definition)
    end

    # `.a.b` completes the keys of `.a` that start with `b`.
    def locations(before, column, thread)
      return [] if thread.nil?

      match = /((?:\.[A-Za-z_][A-Za-z0-9_]*)*)\.([A-Za-z_][A-Za-z0-9_]*)?\z/.match(before)
      return [] if match.nil?

      parent = match[1].empty? ? '.' : match[1]
      prefix = match[2].to_s
      keys = @expressions.evaluate(thread, "#{parent} | if type == \"object\" then keys else [] end").first
      Array(keys).select { |key| key.start_with?(prefix) }
                 .map { |key| target(key, column - prefix.length, prefix.length, 'property') }
    rescue RequestError
      []
    end

    def target(label, start, length, type)
      { 'label' => label, 'text' => label, 'start' => start, 'length' => length, 'type' => type }
    end

    # The thread of a send's source in `context`'s system: a named actor
    # by its name (a fork keeps its source's), any other by its runtime ID.
    def source_thread(context, source)
      here = @threads.to_a.select { |thread| thread.context.equal?(context) }
      if source.is_a?(Hash) && source[:kind] == 'named'
        return here.find { |thread| thread.fork? || thread.actor&.address&.dig(:name) == source[:name] }
      end

      value = source.is_a?(Hash) ? (source[:id] || source[:actor] || source[:runtime]) : source
      here.find { |thread| thread.handle == Integer(value) }
    rescue ArgumentError, TypeError
      nil
    end
  end
end
