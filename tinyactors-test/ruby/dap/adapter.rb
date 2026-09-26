# frozen_string_literal: true

require 'optparse'

module DAP
  # The debug adapter of the test host: DAP over stdio, driving a workbench
  # directory headless (no tmux, no REPL) through the same Workbench::Session
  # the interactive workbench uses, and the same Scheduler as every scenario.
  #
  # One thread runs everything that touches the host: requests, running the
  # system, and reloads. A reader thread turns stdin into messages and the
  # workbench watcher turns saves into reloads, and both only put work on
  # the inbox. While the system runs, the Scheduler asks between two resumes
  # whether the inbox holds anything, so a `pause` lands at once even when a
  # chart never comes to rest.
  #
  # See ruby/dap/README.md.
  class Adapter
    DEFAULT_HOST = 'build/tinyactors-test'

    CAPABILITIES = {
      'supportsConfigurationDoneRequest' => true,
      'supportsFunctionBreakpoints' => true,
      'supportsConditionalBreakpoints' => true,
      'supportsHitConditionalBreakpoints' => true,
      'supportsEvaluateForHovers' => true,
      'exceptionBreakpointFilters' => Breakpoints::EXCEPTION_FILTERS,
      'supportsSetVariable' => true,
      'supportsSetExpression' => true,
      'supportsCompletionsRequest' => true,
      'completionTriggerCharacters' => ['.', ' '],
      'supportsModulesRequest' => true,
      'supportsExceptionInfoRequest' => true,
      'supportsDelayedStackTraceLoading' => true,
      'supportsLoadedSourcesRequest' => true,
      'supportsTerminateRequest' => true,
      'supportsDataBreakpoints' => true,
      'supportsClipboardContext' => true,
      'supportsSteppingGranularity' => true,
      'supportTerminateDebuggee' => false,
      'supportSuspendDebuggee' => false
    }.freeze

    # Requests the test host answers with an error, and why. Their
    # capabilities are not declared.
    REFUSED = {
      'attach' => 'the test host launches a workbench directory; attaching to a running actor is ' \
                  'tinyactors-napi\'s (use "launch" with "workbench")',
      'restart' => 'restarting needs a fork with its own system (core ADR 0085)',
      'setBreakpoints' => 'line breakpoints need source spans (core ADR 0086); use function breakpoints',
      'breakpointLocations' => 'breakpoint locations need source spans (core ADR 0086)',
      'goto' => 'goto needs operator transitions (core ADR 0082)',
      'gotoTargets' => 'goto targets need operator transitions (core ADR 0082) and source spans (core ADR 0086)',
      'stepBack' => 'stepping back needs images that restore into a debug system (core ADR 0085)',
      'reverseContinue' => 'reverse continue needs images that restore into a debug system (core ADR 0085)',
      'restartFrame' => 'restarting a frame needs images that restore into a debug system (core ADR 0085)',
      'cancel' => 'every request answers at once, so there is nothing to cancel',
      'stepInTargets' => 'an action has one way in',
      'terminateThreads' => 'ending a thread would destroy an actor',
      'setInstructionBreakpoints' => 'a chart has no instructions',
      'locations' => 'no value in a chart names a place in a source',
      'readMemory' => 'a chart has no memory to show',
      'writeMemory' => 'a chart has no memory to show',
      'disassemble' => 'a chart has no instructions'
    }.freeze

    def self.main(argv)
      options = { host: nil, port: nil }
      OptionParser.new do |parser|
        parser.banner = 'Usage: dap [--host PATH] [--port PORT]   (DAP on stdin and stdout, or on a port)'
        parser.on('--host PATH', 'The tinyactors-test executable') { |path| options[:host] = path }
        parser.on('--port PORT', Integer, 'Serve sessions on 127.0.0.1:PORT, one at a time (0: any)') do |port|
          options[:port] = port
        end
      end.parse!(argv)
      # stdout carries DAP and nothing else: a chart file that prints, or a
      # warning, goes to stderr instead.
      output = $stdout.dup
      $stdout.reopen($stderr)
      return serve(options[:port], options[:host]) if options[:port]

      $stdin.binmode
      output.binmode
      new(input: $stdin, output: output, host: options[:host]).run
    end

    # For a client that connects to a running adapter, such as VS Code's
    # `debugServer`: each connection is one session with its own host.
    def self.serve(port, host)
      require 'socket'
      server = TCPServer.new('127.0.0.1', port)
      warn "dap: listening on 127.0.0.1:#{server.addr[1]}"
      loop do
        socket = server.accept
        socket.binmode
        begin
          new(input: socket, output: socket, host: host).run
        rescue StandardError => error
          warn "dap: session ended: #{error.class}: #{error.message}"
        ensure
          socket.close unless socket.closed?
        end
      end
    end

    def initialize(input:, output:, host: nil)
      @protocol = Protocol.new(input, output)
      @host = host
      @inbox = Thread::Queue.new
      @client = {}
      @launched = false
      @configured = false
      @started = false
      @done = false
    end

    def run
      reader = Thread.new do
        Thread.current.name = 'dap-reader'
        loop do
          message = @protocol.read
          @inbox << (message ? [:message, message] : [:eof])
          break if message.nil?
        end
      rescue StandardError => error
        diagnostic("reading: #{error.class}: #{error.message}")
        @inbox << [:eof]
      end
      until @done
        item = @inbox.pop(timeout: running? ? 0 : nil)
        handle(item) if item
        pump if !@done && running?
      end
    ensure
      reader&.kill
      shutdown
    end

    private

    # ------------------------------------------------------------------
    # The loop

    def handle(item)
      case item.first
      when :message
        dispatch(item.last)
        settle
      when :reload then reload
      when :eof then @done = true
      end
    rescue TinyactorsTest::ProtocolError, TinyactorsTest::CoreResultError, TinyactorsTest::ScenarioError => error
      output('stderr', "#{error.class.name.split('::').last}: #{error.message}")
    end

    def dispatch(message)
      return unless message['type'] == 'request'

      command = message['command']
      method = "on_#{command.gsub(/([A-Z])/) { "_#{Regexp.last_match(1).downcase}" }}"
      if REFUSED.key?(command)
        @protocol.refuse(message, REFUSED.fetch(command))
      elsif respond_to?(method, true)
        body = send(method, message['arguments'] || {}, message)
        @protocol.respond(message, body) unless body == :responded
      else
        @protocol.refuse(message, "#{command} is not supported by the tinyactors-test adapter")
      end
    rescue RequestError, TinyactorsTest::ScenarioError, ArgumentError => error
      @protocol.refuse(message, error.message)
    rescue TinyactorsTest::CoreResultError, TinyactorsTest::ProtocolError => error
      diagnostic("#{message['command']}: #{error.class}: #{error.message}")
      @protocol.refuse(message, error.message)
    end

    # True while the system has work and the debugger lets it run.
    def running?
      return false unless @started && !@control.stopped?

      @context.scheduler.pending? || (@console.queued? && !@context.scheduler.stopped?)
    end

    # Runs the system until it rests, stops, or a message arrives.
    def pump
      @context.scheduler.run('debug')
      @console.flush unless @context.scheduler.stopped?
      if !@control.stopped? && @control.pause_requested? && !@context.scheduler.pending?
        @control.halt('pause', @threads.to_a.first)
      end
      settle
    rescue TinyactorsTest::CoreResultError, TinyactorsTest::ScenarioError, TinyactorsTest::RPCError => error
      output('stderr', "the system stopped running: #{error.message}")
      @control.halt('exception', nil)
      settle
    end

    # After anything that may have changed the system: what the client
    # should hear about it.
    def settle
      return unless @started

      @control.collect
      @session.drain_chart_logs
      @console.replies
      announce_threads
      announce_catalog
      if @control.take_macrosteps_ended.positive? && @client['supportsInvalidatedEvent']
        @protocol.event('invalidated', 'areas' => %w[threads variables])
      end
      announce_entry
      announce_stop
      announce_waiting
    end

    def announce_threads
      @threads.refresh
      started, exited = @threads.take_changes
      started.each { |id| @protocol.event('thread', 'reason' => 'started', 'threadId' => id) }
      exited.each { |entry| @protocol.event('thread', 'reason' => 'exited', 'threadId' => entry.id) }
      stop = @control.stop
      return if stop.nil? || stop.thread.nil? || @threads[stop.thread]&.session == stop.session

      # The stopped actor went away, or a reload replaced it with a fresh
      # session: nothing is left to be stopped in, so everything runs again.
      @control.resume!
      reset_views
      @protocol.event('continued', 'threadId' => stop.thread, 'allThreadsContinued' => true)
    end

    def announce_catalog
      modules, sources = @catalog.refresh(@threads)
      modules.each { |reason, body| @protocol.event('module', 'reason' => reason, 'module' => body) }
      sources.each { |reason, body| @protocol.event('loadedSource', 'reason' => reason, 'source' => body) }
    end

    def announce_stop
      stop = @control.stop
      return if stop.nil? || stop.equal?(@announced)

      @announced = stop
      body = { 'reason' => stop.reason, 'allThreadsStopped' => true }
      body['threadId'] = stop.thread if stop.thread
      body['description'] = stop.description if stop.description
      body['text'] = stop.exception[:description] if stop.exception
      body['hitBreakpointIds'] = stop.breakpoints unless stop.breakpoints.to_a.empty?
      @protocol.event('stopped', body)
    end

    # A step of an idle actor waits for its next event. When nothing will
    # bring one at the current logical time, the console says what would.
    def announce_waiting
      step = @control.step
      return unless step&.waiting && !@context.scheduler.pending? && !step.equal?(@waiting_noted)

      @waiting_noted = step
      thread = @threads[step.thread]
      due = @session.next_deadline
      hint = due ? "the next delayed send is due at t=#{due}ms: `clock +#{due - @context.now}ms`" : 'nothing is due'
      output('console', "#{thread&.label} waits for its next event; #{hint}, or `send` it one")
    end

    # A saved chart replaces its actors in place, even while the client has
    # everything stopped: the actors that are stopped are held out of the
    # loop, and only the replacements run, to the end of their bootstraps.
    def reload
      return unless @started

      held = @control.stopped? ? @threads.to_a.map(&:handle) : []
      held.each { |handle| @context.scheduler.hold(handle) }
      begin
        @control.aside { @session.reload! }
      ensure
        held.each { |handle| @context.scheduler.release(handle) }
      end
      settle
    end

    # ------------------------------------------------------------------
    # Session

    def on_initialize(args, request)
      @client = args
      @columns_from = args.fetch('columnsStartAt1', true) ? 1 : 0
      @protocol.respond(request, CAPABILITIES)
      @protocol.event('initialized')
      :responded
    end

    def on_launch(args, request)
      raise RequestError, 'launch was already done' if @launched

      dir = args['workbench'] || args['program']
      raise RequestError, 'launch needs "workbench": a directory of chart files' if dir.nil? || dir.to_s.empty?

      dir = Pathname.new(dir).expand_path
      raise RequestError, "#{dir} is not a directory" unless dir.directory?

      @stop_on_entry = args['stopOnEntry'] == true
      start_workbench(dir, args['host'] || @host)
      @launched = true
      @protocol.respond(request)
      @protocol.event('process', 'name' => dir.to_s, 'systemProcessId' => @driver.pid, 'isLocalProcess' => true,
                                 'startMethod' => 'launch')
      begin_running if @configured
      :responded
    end

    def on_configuration_done(_args, request)
      @configured = true
      @protocol.respond(request)
      begin_running if @launched
      :responded
    end

    def on_disconnect(_args, request)
      shutdown
      @protocol.respond(request)
      @done = true
      :responded
    end

    # The debuggee is never terminated: the session ends, and the actors
    # stay as they are until the client disconnects.
    def on_terminate(_args, request)
      @protocol.respond(request)
      @protocol.event('terminated')
      :responded
    end

    # ------------------------------------------------------------------
    # Threads and execution

    def on_threads(_args, _request)
      return { 'threads' => [] } unless @started

      @threads.refresh
      { 'threads' => @threads.to_a.map { |thread| { 'id' => thread.id, 'name' => thread.name } } }
    end

    def on_pause(args, _request)
      require_started!
      @control.request_pause(thread_arg(args, required: false))
      {}
    end

    def on_continue(_args, _request)
      require_started!
      resume
      { 'allThreadsContinued' => true }
    end

    def on_next(args, request) = step(args, request, :over)

    def on_step_in(args, request) = step(args, request, :in)

    def on_step_out(args, request) = step(args, request, :out)

    def step(args, request, kind)
      require_started!
      thread = thread_arg(args)
      reset_views
      waiting = @control.start_step(thread, kind, args['granularity'])
      @protocol.respond(request)
      # A step of an idle actor lets everything run until that actor takes
      # its next event: the client is told the others run too.
      @protocol.event('continued', 'threadId' => thread.id, 'allThreadsContinued' => true) if waiting
      :responded
    end

    def resume
      @control.resume!
      reset_views
    end

    # ------------------------------------------------------------------
    # Stack and data

    def on_stack_trace(args, _request)
      require_started!
      frames = @frames.for(thread_arg(args))
      start = args['startFrame'].to_i
      levels = args['levels'].to_i
      shown = frames.drop(start)
      shown = shown.first(levels) if levels.positive?
      { 'stackFrames' => shown.map(&:to_dap), 'totalFrames' => frames.size }
    end

    def on_scopes(args, _request)
      { 'scopes' => @variables.scopes(frame_thread!(args['frameId'])) }
    end

    def on_variables(args, _request)
      { 'variables' => @variables.variables(args.fetch('variablesReference'), start: args['start'],
                                                                               count: args['count'],
                                                                               filter: args['filter']) }
    end

    def on_set_variable(args, _request)
      @variables.set(args.fetch('variablesReference'), args.fetch('name'), args.fetch('value').to_s)
    end

    def on_set_expression(args, _request)
      thread = args['frameId'] ? frame_thread!(args['frameId']) : stopped_thread!
      @variables.assign(thread, args.fetch('expression'), args.fetch('value').to_s)
    end

    def on_evaluate(args, _request)
      require_started!
      thread = args['frameId'] ? frame_thread!(args['frameId']) : stopped_thread
      expression = args.fetch('expression').to_s
      case args['context']
      when 'repl'
        @console.call(expression, thread, event_of(thread))
      when 'clipboard'
        thread ||= @threads.to_a.first or raise RequestError, 'no actor to evaluate in'
        outputs = @expressions.evaluate(thread, expression, event: event_of(thread))
        { 'result' => JSON.pretty_generate(outputs.size == 1 ? outputs.first : outputs), 'variablesReference' => 0 }
      else
        @console.expression(expression, thread, event_of(thread))
      end
    end

    def on_completions(args, _request)
      require_started!
      thread = args['frameId'] ? frame_thread!(args['frameId']) : stopped_thread
      column = args.fetch('column').to_i - @columns_from.to_i
      targets = @console.completions(args.fetch('text').to_s, column, thread)
      targets.each { |target| target['start'] += @columns_from.to_i }
      { 'targets' => targets }
    end

    def on_exception_info(args, _request)
      thread = thread_arg(args)
      stop = @control.stop
      unless stop&.exception && stop.thread == thread.id
        raise RequestError, "#{thread.label} did not stop for an exception"
      end

      hit = stop.exception
      details = { 'message' => hit[:description], 'typeName' => hit[:filter] }
      details['innerException'] = [{ 'message' => JSON.generate(hit[:event][:data]) }] if hit[:event]&.dig(:data)
      { 'exceptionId' => hit[:event]&.dig(:name_text) || hit[:filter], 'description' => hit[:description],
        'breakMode' => 'always', 'details' => details }
    end

    # ------------------------------------------------------------------
    # Breakpoints

    def on_set_function_breakpoints(args, _request)
      { 'breakpoints' => @breakpoints.set_functions(Array(args['breakpoints'])) }
    end

    def on_set_exception_breakpoints(args, _request)
      filters = Array(args['filters']) + Array(args['filterOptions']).map { |option| option['filterId'] }
      @breakpoints.set_filters(filters.uniq)
      { 'breakpoints' => filters.uniq.map { { 'verified' => true } } }
    end

    def on_data_breakpoint_info(args, _request)
      require_started!
      target = if args['variablesReference']
                 @variables.data_id(args['variablesReference'], args.fetch('name'))
               else
                 thread = args['frameId'] ? frame_thread!(args['frameId']) : (stopped_thread || @threads.to_a.first)
                 thread && [thread, @expressions.path(thread, args.fetch('name'))]
               end
      if target.nil?
        return { 'dataId' => nil, 'description' => "#{args['name']} is not a data-model location" }
      end

      thread, path = target
      { 'dataId' => Breakpoints.data_id(thread.label, path),
        'description' => "#{thread.label} #{Expressions.location(path)}", 'accessTypes' => ['write'],
        'canPersist' => true }
    rescue RequestError => error
      { 'dataId' => nil, 'description' => error.message }
    end

    def on_set_data_breakpoints(args, _request)
      { 'breakpoints' => @breakpoints.set_data(Array(args['breakpoints'])) }
    end

    # ------------------------------------------------------------------
    # Sources and modules

    def on_modules(args, _request)
      modules = @started ? @catalog.modules : []
      start = args['startModule'].to_i
      count = args['moduleCount'].to_i
      shown = modules.drop(start)
      shown = shown.first(count) if count.positive?
      { 'modules' => shown, 'totalModules' => modules.size }
    end

    def on_loaded_sources(_args, _request)
      { 'sources' => @started ? @catalog.sources : [] }
    end

    def on_source(args, _request)
      reference = args['sourceReference'] || args.dig('source', 'sourceReference')
      text = reference.to_i.positive? && @catalog ? @catalog.text(reference) : nil
      path = args.dig('source', 'path')
      text ||= File.read(path) if path && File.file?(path)
      raise RequestError, 'no such source' if text.nil?

      { 'content' => text, 'mimeType' => 'text/x-ruby' }
    end

    # ------------------------------------------------------------------
    # Setting up

    def start_workbench(dir, host)
      journal = Workbench::Journal.new
      journal.listener = ->(entry) { output(JOURNAL_CATEGORIES.fetch(entry.level, 'console'), entry.text) }
      sources = Workbench::Sources.new(dir)
      sources.reload!
      @driver = TinyactorsTest::Driver.new(resolve_host(host), timeout: 30)
      @session = Workbench::Session.new(driver: @driver, sources: sources, journal: journal)
      @context = @session.context
      @context.recording_trace
      @labels = Labels.new(@context)
      @threads = Threads.new(@session, @labels)
      @expressions = Expressions.new(@session, @labels, Console.processors)
      @breakpoints = Breakpoints.new(@labels, @expressions)
      @console = Console.new(session: @session, threads: @threads, expressions: @expressions,
                             output: method(:output))
      @control = Control.new(session: @session, labels: @labels, threads: @threads, expressions: @expressions,
                             breakpoints: @breakpoints, output: method(:output), interrupt: -> { !@inbox.empty? })
      @variables = Variables.new(session: @session, labels: @labels, threads: @threads, control: @control,
                                 expressions: @expressions, console: @console)
      @console.variables = @variables
      @frames = Frames.new(@session, @labels, @threads, @control)
      @catalog = Catalog.new(@session)
      @console.open
      @context.scheduler.monitor = @control
      @dir = dir
    end

    # `<log>` records are the program's output; the workbench's own notes
    # are the console's; what went wrong is important or an error.
    JOURNAL_CATEGORIES = { chart: 'stdout', info: 'console', warn: 'important', error: 'stderr' }.freeze

    # configurationDone and launch have both arrived: spawn the actors and
    # start watching the directory. `stopOnEntry` stops once every actor
    # has started, before any event reaches one, unless a breakpoint in a
    # bootstrap stopped it first.
    def begin_running
      return if @started

      @started = true
      @entry_pending = @stop_on_entry
      @session.start!
      @watcher = Workbench::Watcher.new(@dir).start { @inbox << [:reload] }
      settle
    end

    # The entry stop comes once the bootstraps have run.
    def announce_entry
      return unless @entry_pending
      return if @context.scheduler.pending? && !@control.stopped?

      @entry_pending = false
      return if @control.stopped?

      @threads.refresh
      @control.halt('entry', @threads.to_a.first)
    end

    def resolve_host(host)
      path = host ? Pathname.new(host).expand_path : Pathname.new(__dir__).join('../..', DEFAULT_HOST).expand_path
      raise RequestError, "no tinyactors-test host at #{path}; run `mise run build` in tinyactors-test" unless path.executable?

      path
    end

    def shutdown
      return if @shut

      @shut = true
      @watcher&.stop
      @console&.close
      @driver&.close
    rescue StandardError => error
      diagnostic("shutdown: #{error.message}")
      begin
        @driver&.terminate
      rescue StandardError
        nil
      end
    end

    # ------------------------------------------------------------------
    # Helpers

    def output(category, text)
      @protocol.event('output', 'category' => category, 'output' => "#{text}\n")
    end

    def diagnostic(text) = $stderr.puts("dap: #{text}")

    def require_started!
      raise RequestError, 'launch a workbench first' unless @started
    end

    def thread_arg(args, required: true)
      id = args['threadId']
      return nil if id.nil? && !required
      raise RequestError, 'threadId is required' if id.nil?

      @threads.refresh if @threads[id].nil?
      @threads[id] or raise RequestError, "no thread #{id}"
    end

    def frame_thread!(frame_id)
      frame = @frames[frame_id] or raise RequestError, "frame #{frame_id} is not valid (any more)"
      @threads[frame.thread] or raise RequestError, "the thread of frame #{frame_id} is gone"
    end

    def stopped_thread
      id = @control&.stop&.thread
      id && @threads[id]
    end

    def stopped_thread!
      stopped_thread or raise RequestError, 'no thread is stopped; name a frame'
    end

    def event_of(thread) = thread && @control.event_of(thread)

    # Frames and variable references name the system as it stood.
    def reset_views
      @frames.reset
      @variables.reset
    end
  end
end
