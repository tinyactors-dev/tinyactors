# frozen_string_literal: true

require 'reline'

module Workbench
  # The command pane. Steps typed here run against the same live system the
  # display panes show, so an experiment, a reload, and a running clock
  # interleave freely.
  #
  # Input goes through Reline, the line editor Ruby ships and IRB uses: line
  # editing, history, and completion over command and actor names. When stdin
  # is not a terminal the loop falls back to `gets`, so a scripted run and the
  # self-test behave the same as a person typing.
  class REPL
    DURATION = /\A(\d+(?:\.\d+)?)(ms|s|m)?\z/
    RATE = /\A(\d+(?:\.\d+)?)x?\z/

    Command = Data.define(:names, :usage, :summary, :run)

    def initialize(session:, journal:, clock:, lock: Mutex.new, out: $stdout)
      @session = session
      @journal = journal
      @clock = clock
      @lock = lock
      @out = out
      @commands = build
      @index = @commands.flat_map { |command| command.names.map { |name| [name, command] } }.to_h
      @interactive = $stdin.tty?
      install_completion if @interactive
    end

    def prompt = "\e[36mworkbench\e[0m> "

    # One line of input, or nil at end of input.
    def read_line
      return Reline.readline(prompt, true) if @interactive

      @out.print(prompt)
      @out.flush
      $stdin.gets&.chomp
    end

    # Runs one line. Returns :quit to stop the workbench.
    def call(line)
      line = line.strip
      return nil if line.empty? || line.start_with?('#')

      name, rest = line.split(/\s+/, 2)
      command = @index[name]
      unless command
        say("unknown command #{name.inspect}; try `help`")
        return nil
      end

      command.run.call(rest.to_s.strip)
    rescue ArgumentError, TinyactorsTest::ScenarioError => error
      say("\e[31m#{error.message}\e[0m")
      nil
    rescue TinyactorsTest::CoreResultError => error
      say("\e[31m#{error.function} answered #{error.result}\e[0m")
      @journal.error("#{error.function} answered #{error.result}")
      nil
    rescue TinyactorsTest::RPCError => error
      say("\e[31m#{error.message}\e[0m")
      @journal.exception('rpc', error)
      nil
    end

    def say(text) = @out.puts(text)

    private

    # Completion runs on this thread while no command holds the lock, but the
    # watcher and the clock can be rewriting the actor table, so the snapshot
    # is taken under it.
    def install_completion
      Reline.completion_append_character = ' '
      Reline.completion_proc = lambda do |word|
        line = Reline.line_buffer.to_s
        @lock.synchronize { candidates(line) }.uniq.select { |candidate| candidate.start_with?(word) }
      end
    end

    # `deliver NAME <tab>` offers the events that actor listens for; anything
    # else offers command, actor and chart names.
    def candidates(line)
      parts = line.split(/\s+/)
      # Which word is being completed: a trailing space means the next one.
      position = line.end_with?(' ') ? parts.size : parts.size - 1
      if %w[deliver d].include?(parts.first) && position == 2 && parts[1]
        return event_names(parts[1])
      end

      @index.keys + @session.actors.keys.map(&:to_s) + TinyactorsTest.charts.keys.map(&:to_s)
    end

    # A descriptor may name several events, separated by spaces, as SCXML
    # allows; each is worth offering on its own.
    def event_names(actor)
      @session.events(actor.to_sym).flat_map { |row| row[:event].split(/\s+/) }.uniq.sort
    rescue StandardError
      []
    end

    def build
      [
        Command.new(names: %w[help ?], usage: 'help', summary: 'this list', run: ->(_) { help }),
        Command.new(names: %w[actors ls], usage: 'actors', summary: 'the named actors and their configuration',
                    run: ->(_) { actors }),
        Command.new(names: %w[charts], usage: 'charts', summary: 'the charts loaded from the directory',
                    run: ->(_) { charts }),
        Command.new(names: %w[focus f], usage: 'focus NAME', summary: 'point the data-model pane at an actor',
                    run: ->(rest) { focus(rest) }),
        Command.new(names: %w[deliver d], usage: 'deliver NAME EVENT [JSON]', summary: 'deliver an external event',
                    run: ->(rest) { deliver(rest) }),
        Command.new(names: %w[advance a], usage: 'advance 5s | 500ms | 30000',
                    summary: 'advance logical time and run what falls due', run: ->(rest) { advance(rest) }),
        Command.new(names: %w[run], usage: 'run [STEPS]', summary: 'hop from one pending deadline to the next',
                    run: ->(rest) { run(rest) }),
        Command.new(names: %w[clock c], usage: 'clock [start [RATE] | stop]',
                    summary: 'let logical time run on its own (RATE like 1x, 10x)',
                    run: ->(rest) { clock(rest) }),
        Command.new(names: %w[events e], usage: 'events [NAME]',
                    summary: 'the events an actor listens for, and which apply now',
                    run: ->(rest) { events(rest) }),
        Command.new(names: %w[data], usage: 'data [NAME]', summary: 'print a data-model root here',
                    run: ->(rest) { data(rest) }),
        Command.new(names: %w[reload r], usage: 'reload', summary: 're-read the directory now',
                    run: ->(_) { @session.reload! && nil }),
        Command.new(names: %w[quit exit q], usage: 'quit', summary: 'shut the host down and leave',
                    run: ->(_) { :quit })
      ]
    end

    def help
      say('')
      @commands.each do |command|
        say(format('  %-30s %s', command.usage, command.summary))
      end
      say('')
      say("  charts and actors are read from #{@session.sources.dir}")
      say('  a change there reloads automatically; every named actor is replaced in place')
      say('  logical time only moves when you move it, or while the clock runs')
      say('')
      nil
    end

    def actors
      overview = @session.overview
      return say('  no actors declared') if overview.empty?

      overview.each do |actor|
        say(format('  %-14s %-22s %-9s %s', actor[:name], actor[:address], actor[:residency],
                   actor[:active].join(' ')))
      end
      nil
    end

    def charts
      TinyactorsTest.charts.each do |name, chart|
        family = chart.document_name || chart.name
        say(format('  %-22s family %-18s datamodel %s', name, family, chart.datamodel || 'jq'))
      end
      say("  (none; add a chart to #{@session.sources.dir})") if TinyactorsTest.charts.empty?
      nil
    end

    def focus(rest)
      raise ArgumentError, 'focus NAME' if rest.empty?

      name = rest.to_sym
      @session.fetch(name)
      @session.focus = name
      nil
    end

    def deliver(rest)
      name, event, payload = rest.split(/\s+/, 3)
      raise ArgumentError, 'deliver NAME EVENT [JSON]' if name.nil? || event.nil?

      data = payload.nil? || payload.empty? ? nil : parse_json(payload)
      @session.deliver(name.to_sym, event, data: data)
      nil
    end

    def advance(rest)
      raise ArgumentError, 'advance 5s | 500ms | 30000' if rest.empty?

      @session.advance(milliseconds(rest))
      nil
    end

    def run(rest)
      steps = rest.empty? ? 32 : Integer(rest)
      @session.run(steps: steps)
      nil
    end

    def clock(rest)
      word, rate = rest.split(/\s+/, 2)
      case word
      when nil, ''
        say(@clock.running? ? "  running at #{format('%g', @clock.rate)}x" : '  stopped')
      when 'start', 'on'
        @clock.start(parse_rate(rate))
        @journal.info("clock started at #{format('%g', @clock.rate)}x")
      when 'stop', 'off'
        @clock.stop
        @journal.info("clock stopped at t=#{@session.now}ms")
      else
        raise ArgumentError, 'clock [start [RATE] | stop]'
      end
      nil
    end

    def events(rest)
      name = rest.empty? ? @session.focus : rest.to_sym
      raise ArgumentError, 'events NAME' if name.nil?

      rows = @session.events(name)
      return say('  this chart listens for no events') if rows.empty?

      here, elsewhere = rows.partition { |row| row[:active] }
      say('')
      show_events('accepted now', here, where: false)
      show_events('elsewhere in the chart', elsewhere, where: true)
      nil
    end

    # `holds` is the guard evaluated against the data root as it stands: a
    # transition whose guard is false will not fire even though its state is
    # active, which is exactly the thing that is invisible otherwise.
    def show_events(title, rows, where:)
      return if rows.empty?

      say("  #{title}")
      rows.each do |row|
        guard = row[:condition] ? "when #{row[:condition]}" : ''
        guard += case row[:holds]
                 when true then ' [true]'
                 when false then ' [false]'
                 else ''
                 end
        target = row[:targets].empty? ? '(stays put)' : "-> #{row[:targets].join(' ')}"
        say(format('    %-10s %-38s %s%s', row[:event], guard, where ? "in #{row[:state]} " : '', target))
      end
      say('')
    end

    def data(rest)
      name = rest.empty? ? @session.focus : rest.to_sym
      raise ArgumentError, 'data NAME' if name.nil?

      actor = @session.fetch(name)
      say(JSON.pretty_generate(actor.data))
      nil
    end

    def parse_json(text)
      JSON.parse(text)
    rescue JSON::ParserError => error
      raise ArgumentError, "the event data is not JSON: #{error.message}"
    end

    # Logical milliseconds per wall-clock millisecond.
    def parse_rate(text)
      return 1.0 if text.nil? || text.strip.empty?

      match = RATE.match(text.strip)
      raise ArgumentError, "#{text.inspect} is not a rate such as 1x, 10x, or 0.5x" unless match

      rate = Float(match[1])
      raise ArgumentError, 'the rate must be greater than zero' unless rate.positive?

      rate
    end

    # SCXML-style durations, plus a bare count of milliseconds.
    def milliseconds(text)
      match = DURATION.match(text)
      raise ArgumentError, "#{text.inspect} is not a duration such as 5s, 500ms, or 30000" unless match

      scale = { nil => 1, 'ms' => 1, 's' => 1_000, 'm' => 60_000 }.fetch(match[2])
      (Float(match[1]) * scale).round
    end
  end
end
