# frozen_string_literal: true

module Workbench
  # Watches a directory of *.rb files and reports when any of them changes.
  #
  # Ruby ships no filesystem-event API, so this polls mtimes and sizes on a
  # background thread. For the handful of files one workbench directory holds
  # that costs a stat each, a few times a second, which is far cheaper than
  # taking on a native watcher dependency.
  #
  # A change is reported only once the directory has been quiet for
  # `settle`, so an editor that writes a file in several steps (truncate,
  # write, rename) produces one reload rather than three.
  class Watcher
    def initialize(dir, interval: 0.15, settle: 0.2)
      @dir = Pathname.new(dir).expand_path
      @interval = interval
      @settle = settle
      @thread = nil
    end

    # The mtime and size of every source file, which together detect a write
    # that leaves mtime unchanged at this clock's resolution.
    def snapshot
      Dir[@dir.join('*.rb').to_s].sort.to_h do |path|
        stat = File.stat(path)
        [path, [stat.mtime.to_f, stat.size]]
      rescue Errno::ENOENT
        [path, nil]
      end
    end

    # Calls the block whenever the directory changes and settles. The block
    # runs on the watcher thread and is expected to take the workbench lock.
    def start(&changed)
      @thread = Thread.new do
        Thread.current.name = 'workbench-watcher'
        Thread.current.abort_on_exception = false
        previous = snapshot
        loop do
          sleep(@interval)
          current = snapshot
          next if current == previous

          # Wait for the directory to stop moving before reporting.
          loop do
            sleep(@settle)
            settled = snapshot
            break if settled == current

            current = settled
          end
          previous = current
          changed.call
        end
      end
      self
    end

    def stop
      @thread&.kill
      @thread = nil
    end
  end
end
