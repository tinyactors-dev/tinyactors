# frozen_string_literal: true

module DAP
  # The debug console: what a client sends as `evaluate` with context
  # `repl`, and the completions it offers there.
  #
  #   send [@actor] <event> [json]           deliver to the actor, with the debugger as its origin
  #   goto [@actor] <state…> [from <state>]  an operator transition (core ADR 0082)
  #   abandon [@actor]                       abandon a macrostep that never ends (core ADR 0082)
  #   cancel [@actor] <send ID>              withdraw one pending delayed send (core ADR 0084)
  #   clock +<duration>                      advance the workbench's logical time
  #   <expression>                           jq over the actor's data model
  #
  # The actor is the frame's, or the one `@actor` names by its thread label.
  #
  # The debugger is a sender an actor can answer (core ADR 0068): the
  # adapter registers an I/O processor of type `debug` in the workbench's
  # system, opens one scope there, and
  # delivers each `send` with an origin in that scope, so
  # `<send targetexpr="$_event.origin" typeexpr="$_event.origintype">`
  # reaches the console. Closing the scope at the end of the session leaves
  # later replies with no origin, which fail as error.communication.
  #
  # What the console sends an actor, events and jumps alike, waits in that
  # actor's mailbox, oldest first, as a message to one of napi's actors
  # does: the Scheduler's monitor hands the next one over (`deliver_mail`)
  # when the actor rests and its system runs, so a message sent while the
  # system is stopped is taken on `continue` or a step, behind mail already
  # waiting.
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

    COMMANDS = %w[send goto abandon cancel clock].freeze

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
      # The debug I/O processor of the workbench's system.
      @io = nil
      # thread ID => [{event:, data:} or {goto: {targets:, from:}}]
      @mail = Hash.new { |hash, id| hash[id] = [] }
      @reply_cursor = nil
    end

    # The Variables that present values, the Operator that jumps, abandons
    # and withdraws, and what says whether the system is stopped; all are
    # made after the console.
    attr_writer :variables, :operator, :stopped

    # Registers the `debug` processor in `context`'s system and opens the
    # scope there. Tells `expressions` how to show the origin of an event
    # the console sent.
    def open(context)
      @context = context
      @io = context.endpoint_io(TYPE, INSTRUMENT)
      @io.control('open', processor: @io.processor, uri: URI)
      @expressions.io_origins[@io.processor] = { type: TYPE, uri: URI }
    end

    def close
      @io&.control('close', uri: URI)
    rescue TinyactorsTest::RPCError
      nil
    end

    # A <log> record as the program's output, as napi's adapter writes it:
    # `label: value`, a string value as it is and any other as JSON.
    def self.log_text(entry)
      value = entry[:value].is_a?(String) ? entry[:value] : JSON.generate(entry[:value])
      "#{entry[:label].to_s.empty? ? 'log' : entry[:label]}:#{entry[:has_value] ? " #{value}" : ''}"
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
      when 'clock' then clock(rest.to_s)
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

    # ------------------------------------------------------------------
    # Mailboxes

    # What waits in `thread`'s mailbox, oldest first.
    def mail_for(thread) = @mail.fetch(thread.id, [])

    # Puts a message in `thread`'s mailbox, and has its system run to hand
    # it over. Answers what the console adds to its answer: where the
    # message waits while the system is stopped.
    def post(thread, message)
      box = @mail[thread.id]
      box << message
      @context.scheduler.wake
      return '' unless @stopped.call

      "; it waits in the mailbox (#{box.size} message#{box.size == 1 ? '' : 's'}): continue or step to deliver it"
    end

    # Hands each actor that rests and is not held its next message: an
    # event through the `debug` processor, a jump as an operator
    # transition. Answers the handles of the actors that took one, which
    # are runnable now.
    def deliver_mail
      scheduler = @context.scheduler
      @threads.to_a.filter_map do |thread|
        box = @mail.fetch(thread.id, nil)
        next if box.nil? || box.empty? || scheduler.held?(thread.handle)

        view = thread.view
        next if view.nil? || view[:macrostep_in_progress] || view[:residency] == 'done'

        thread.handle if hand_over(thread, box)
      end
    end

    # Replies the actors sent to the console since the last call.
    def replies
      entries = @io.journal(since: @reply_cursor).select { |entry| entry[:op] == 'send' }
      return if entries.empty?

      @reply_cursor = entries.last[:seq]
      entries.each do |entry|
        who = source_thread(entry[:source])&.title || 'an actor'
        data = entry[:data].nil? ? '' : " #{JSON.generate(entry[:data])}"
        refused = entry[:refused] ? " (refused: #{entry[:refused]})" : ''
        @output.call('console', "#{who} replied #{entry[:event_text]}#{data}#{refused}")
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
      note = post(thread, { event: event, data: data })
      { 'result' => "sent #{event} to #{thread.title}#{note}", 'variablesReference' => 0 }
    end

    # Hands `thread` the oldest message of `box`. Answers whether it took
    # it; a message it refuses leaves the mailbox, and the console says why.
    def hand_over(thread, box)
      message = box.first
      result = if message[:goto]
                 @operator.deliver_jump(thread, message.dig(:goto, :targets), message.dig(:goto, :from))
               else
                 receive(thread, message)
               end
      return false if result == 'err_busy'

      box.shift
      return true if result == 'ok'

      what = message[:goto] ? "goto #{message.dig(:goto, :targets).join(' ')}" : message[:event]
      why = message[:goto] ? @operator.jump_refusal(thread, message.dig(:goto, :targets), message.dig(:goto, :from), result) : result
      @output.call('important', "#{thread.title} refused #{what}: #{why}")
      false
    end

    def receive(thread, message)
      args = { processor: @io.processor, actor: thread.handle, event: message[:event], uri: URI }
      args[:data] = message[:data] unless message[:data].nil?
      @io.control('receive', **args)[:result]
    end

    # Logical time belongs to a system (ADR 0012): the workbench's, a
    # developer's machine, where moving it for every actor is what `clock`
    # is for.
    def clock(rest)
      match = DURATION.match(rest.strip)
      raise RequestError, 'clock +<duration>, such as +500ms, +5s, +2m, +1h or +3d' unless match && rest.strip.start_with?('+')

      milliseconds = (Float(match[1]) * SCALE.fetch(match[2])).round
      # Time moves now; what falls due runs once the answer is out, like
      # everything the adapter runs.
      @context.set_time(@context.now + milliseconds)
      @context.scheduler.wake
      { 'result' => "t=#{@context.now}ms", 'variablesReference' => 0 }
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

    # The thread of a send's source: a named actor by its name, any other
    # by its runtime ID.
    def source_thread(source)
      here = @threads.to_a
      if source.is_a?(Hash) && source[:kind] == 'named'
        return here.find { |thread| thread.actor&.address&.dig(:name) == source[:name] }
      end

      value = source.is_a?(Hash) ? (source[:id] || source[:actor] || source[:runtime]) : source
      here.find { |thread| thread.handle == Integer(value) }
    rescue ArgumentError, TypeError
      nil
    end
  end
end
