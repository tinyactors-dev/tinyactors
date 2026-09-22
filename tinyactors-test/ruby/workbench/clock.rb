# frozen_string_literal: true

module Workbench
  # Advances logical time on its own, so a chart driven by delayed sends
  # plays by itself instead of waiting for `advance`.
  #
  # `rate` is logical milliseconds per wall-clock millisecond: 1.0 runs the
  # chart in real time, 10.0 runs it ten times faster. Each tick advances by
  # the wall time that actually elapsed, so a slow step falls behind rather
  # than silently compressing time.
  #
  # A thread rather than a fiber: the command loop blocks in Reline, which
  # never yields, so nothing would resume a fiber while the user is typing.
  # The block this is given is expected to take the workbench lock itself.
  class Clock
    TICK = 0.1

    attr_reader :rate

    # The block is called with a number of logical milliseconds and returns
    # false to stop the clock (a step that failed).
    def initialize(&step)
      @step = step
      @rate = 1.0
      @thread = nil
    end

    def running? = !@thread.nil?

    def describe = running? ? "clock #{format('%g', @rate)}x" : nil

    def start(rate = 1.0)
      stop
      @rate = rate.to_f
      @thread = Thread.new do
        Thread.current.name = 'workbench-clock'
        last = Process.clock_gettime(Process::CLOCK_MONOTONIC)
        loop do
          sleep(TICK)
          now = Process.clock_gettime(Process::CLOCK_MONOTONIC)
          elapsed = now - last
          last = now
          milliseconds = (elapsed * 1000.0 * @rate).round
          next if milliseconds <= 0
          break unless @step.call(milliseconds)
        end
        @thread = nil
      end
      self
    end

    def stop
      thread = @thread
      @thread = nil
      thread&.kill
      thread&.join(1)
      nil
    end
  end
end
