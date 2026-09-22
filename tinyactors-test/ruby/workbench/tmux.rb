# frozen_string_literal: true

require 'open3'
require 'shellwords'

module Workbench
  # The window the workbench runs in: a REPL pane the program reads from, and
  # three display panes it writes to through their ttys.
  #
  #   +----------------------+----------------------+
  #   | actors               | data model           |
  #   +----------------------+----------------------+
  #   | logs                                        |
  #   +---------------------------------------------+
  #   | repl (the workbench itself)                 |
  #   +---------------------------------------------+
  #
  # The display panes run a command that produces no output and never exits,
  # so nothing writes to those terminals except this program.
  module Tmux
    IDLE = 'while :; do sleep 86400; done'
    SESSION = 'tinyactors-workbench'

    class Error < StandardError; end

    module_function

    def run(*args)
      out, err, status = Open3.capture3('tmux', *args)
      raise Error, "tmux #{args.join(' ')}: #{err.strip}" unless status.success?

      out.chomp
    end

    def available?
      system('tmux', '-V', out: File::NULL, err: File::NULL)
    end

    def session?(name)
      system('tmux', 'has-session', '-t', "=#{name}", out: File::NULL, err: File::NULL)
    end

    def pane_tty(target) = run('display-message', '-p', '-t', target, '#{pane_tty}')

    def pane_size(target)
      run('display-message', '-p', '-t', target, '#{pane_width} #{pane_height}').split.map { |n| Integer(n) }
    end

    # Builds the window and returns the pane IDs. The REPL pane is left
    # running the idle command: the program that belongs there needs the
    # display panes' IDs, which only exist once the window is built, so the
    # caller respawns it with `respawn` below.
    def launch(session:, dir:)
      kill(session) if session?(session)
      run('new-session', '-d', '-s', session, '-c', dir.to_s, '-x', '200', '-y', '50', IDLE)
      repl = run('list-panes', '-t', session, '-F', '#{pane_id}').lines.first.chomp

      # Split off the display area above the REPL, then divide it.
      logs = run('split-window', '-b', '-v', '-l', '70%', '-t', repl, '-c', dir.to_s, '-P', '-F', '#{pane_id}', IDLE)
      actors = run('split-window', '-b', '-v', '-l', '65%', '-t', logs, '-c', dir.to_s, '-P', '-F', '#{pane_id}', IDLE)
      data = run('split-window', '-h', '-l', '50%', '-t', actors, '-c', dir.to_s, '-P', '-F', '#{pane_id}', IDLE)

      { actors: actors, data: data, logs: logs }.each do |title, pane|
        run('select-pane', '-t', pane, '-T', title.to_s)
      end
      run('set-option', '-t', session, 'pane-border-status', 'top')
      run('set-option', '-t', session, 'pane-border-format', ' #{pane_title} ')
      run('set-option', '-t', session, 'status', 'off')
      run('select-pane', '-t', repl)
      { session: session, repl: repl, actors: actors, data: data, logs: logs }
    end

    # Replaces what a pane runs with `command`, given as an argument list.
    def respawn(pane, dir:, command:)
      run('respawn-pane', '-k', '-t', pane, '-c', dir.to_s,
          command.map { |part| Shellwords.escape(part) }.join(' '))
    end

    def kill(session)
      run('kill-session', '-t', "=#{session}")
    rescue Error
      nil
    end

    def attach(session)
      exec('tmux', 'attach-session', '-t', "=#{session}")
    end
  end
end
