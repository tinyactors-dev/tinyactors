# frozen_string_literal: true

require 'optparse'

module Workbench
  # The workbench program. Run without `--panes` it builds the tmux window and
  # attaches; tmux then runs it again inside the REPL pane, with the display
  # panes' IDs, and that second run is the workbench proper.
  #
  # Three threads touch the system: the command loop, the file watcher, and
  # the clock. The driver is one JSON-RPC pipe and a request must not
  # interleave with another, so every one of them goes through `synchronize`.
  # The command loop holds the lock only while a command runs, never while it
  # waits for a line, so a reload or a tick lands promptly while you type.
  class App
    DEFAULT_HOST = 'build/tinyactors-test'

    def self.main(argv)
      new(parse(argv)).run
    end

    def self.parse(argv)
      options = { dir: Dir.pwd, host: nil, tmux: true, panes: nil, session: Tmux::SESSION }
      OptionParser.new do |parser|
        parser.banner = 'Usage: workbench [options]'
        parser.on('--dir DIR', 'Directory of chart and actor files (default: the current directory)') do |dir|
          options[:dir] = dir
        end
        parser.on('--host PATH', 'The tinyactors-test executable') { |path| options[:host] = path }
        parser.on('--session NAME', 'tmux session name') { |name| options[:session] = name }
        parser.on('--no-tmux', 'Run as a plain REPL, without the display panes') { options[:tmux] = false }
        parser.on('--panes IDS', 'Internal: the tmux pane IDs to draw in') do |ids|
          options[:panes] = ids.split(',')
        end
      end.parse!(argv)
      options
    end

    def initialize(options)
      @options = options
      @dir = Pathname.new(options[:dir]).expand_path
      @journal = Journal.new
      @lock = Mutex.new
    end

    def run
      abort "workbench: #{@dir} is not a directory" unless @dir.directory?

      return launch if @options[:tmux] && @options[:panes].nil?

      start
    end

    private

    def synchronize(&block) = @lock.synchronize(&block)

    # First run: build the window, then hand over to the copy tmux starts in
    # the REPL pane.
    def launch
      unless Tmux.available?
        abort 'workbench: tmux is not installed; run with --no-tmux for a plain REPL'
      end

      program = [RbConfig.ruby, File.expand_path('../workbench_main.rb', __dir__),
                 '--dir', @dir.to_s, '--session', @options[:session]]
      program.push('--host', @options[:host]) if @options[:host]

      panes = Tmux.launch(session: @options[:session], dir: @dir)
      Tmux.respawn(panes[:repl], dir: @dir,
                   command: program + ['--panes', [panes[:actors], panes[:data], panes[:logs]].join(',')])
      # Without a terminal to attach to (a script, a test), leave the session
      # running and say where it is.
      unless $stdout.tty?
        puts "workbench running in tmux session #{@options[:session]}"
        puts "attach with: tmux attach -t #{@options[:session]}"
        return
      end

      Tmux.attach(@options[:session])
    end

    # Second run: own the host, the system, and the loop.
    def start
      host = resolve_host
      @journal.info("host #{host}")
      @journal.info("watching #{@dir}")

      sources = Sources.new(@dir)
      sources.reload!
      driver = TinyactorsTest::Driver.new(host, timeout: 30)
      session = Session.new(driver: driver, sources: sources, journal: @journal)
      panes = open_panes(session)
      clock = Clock.new { |milliseconds| tick(session, panes, milliseconds) }
      session.clock = clock
      repl = Repl.new(session: session, journal: @journal, clock: clock, lock: @lock)
      watcher = Watcher.new(@dir).start do
        synchronize do
          guard('reload') { session.reload! }
          render(panes)
        end
      end

      synchronize do
        session.start!
        render(panes)
      end
      loop_until_done(repl, panes)
    ensure
      clock&.stop
      watcher&.stop
      panes&.each_value(&:close)
      shutdown(driver)
      Tmux.kill(@options[:session]) if @options[:panes]
    end

    def loop_until_done(repl, panes)
      loop do
        begin
          line = repl.read_line
        rescue Interrupt
          # Ctrl-C abandons the line being typed, as a shell does.
          next
        end
        break if line.nil?

        break if synchronize { repl.call(line) } == :quit

        synchronize { render(panes) }
      end
    end

    # One step of the running clock; returning false stops it.
    def tick(session, panes, milliseconds)
      synchronize do
        session.tick(milliseconds)
        render(panes)
      end
      true
    rescue StandardError => error
      @journal.exception('the clock stopped', error)
      synchronize { render(panes) }
      false
    end

    def guard(what)
      yield
    rescue StandardError => error
      @journal.exception(what, error)
    end

    def open_panes(session)
      return {} if @options[:panes].nil?

      actors, data, logs = @options[:panes]
      { actors: [Pane.new(actors), Panes::Overview.new(session)],
        data: [Pane.new(data), Panes::DataModel.new(session)],
        logs: [Pane.new(logs), Panes::Logs.new(@journal)] }
        .transform_values { |pane, renderer| PaneView.new(pane, renderer) }
    end

    def render(panes)
      panes.each_value(&:draw)
    end

    def resolve_host
      return Pathname.new(@options[:host]).expand_path if @options[:host]

      # The workbench directory is usually not the project, so the default
      # host is found relative to this file rather than the working directory.
      host = Pathname.new(__dir__).join('../..', DEFAULT_HOST).expand_path
      unless host.executable?
        abort "workbench: no host at #{host}; run `mise run build` in tinyactors-test"
      end

      host
    end

    def shutdown(driver)
      driver&.close
    rescue StandardError
      begin
        driver&.terminate
      rescue StandardError
        nil
      end
    end
  end

  # A pane and the renderer that fills it.
  class PaneView
    def initialize(pane, renderer)
      @pane = pane
      @renderer = renderer
    end

    def draw
      @pane.draw { |width, height| @renderer.lines(width, height) }
    end

    def close = @pane.close
  end
end
