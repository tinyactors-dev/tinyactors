# frozen_string_literal: true

require 'stringio'

module TinyactorsTest
  # Outcomes:
  #   :pass  - passed, no failure expected
  #   :xfail - failed exactly as its expect_failure describes
  #   :xpass - passed although a failure was expected (the suite fails)
  #   :fail  - failed for any other reason (the suite fails)
  Result = Data.define(:scenario, :outcome, :error, :shutdown_error, :diagnostics) do
    def ok?
      %i[pass xfail].include?(outcome)
    end
  end

  class Runner
    LABELS = { pass: 'PASS ', xfail: 'XFAIL', xpass: 'XPASS', fail: 'FAIL ' }.freeze

    def initialize(executable:, charts:, out: $stdout, timeout: Driver::DEFAULT_TIMEOUT)
      @executable = executable
      @charts = charts
      @out = out
      @timeout = timeout
    end

    # Runs the scenarios of `groups` and reports each, in order. `jobs` > 1
    # runs them in that many forked workers; each scenario has its own host
    # process either way. `exhaustive: false` leaves out the scenarios and
    # groups declared exhaustive: true. `timings` reports the
    # slowest scenarios after the summary.
    def run(groups, filter: nil, jobs: 1, exhaustive: true, timings: 0)
      scenarios = groups.flat_map(&:scenarios)
      scenarios = scenarios.reject(&:exhaustive) unless exhaustive
      scenarios = scenarios.select { |scenario| filter.match?(scenario.full_name) } if filter
      results = if jobs > 1 && scenarios.size > 1
                  run_forked(scenarios, [jobs, scenarios.size].min)
                else
                  scenarios.map { |scenario| timed(scenario).tap { |(result, _)| report(result) } }
                end
      summarize(results.map(&:first))
      report_timings(results, timings) if timings.positive?
      results.map(&:first)
    end

    # A result from a worker: the report it printed, as text.
    Reported = Data.define(:scenario, :outcome, :text) do
      def ok?
        %i[pass xfail].include?(outcome)
      end
    end

    def run_scenario(scenario)
      driver = Driver.new(@executable, timeout: @timeout)
      error = nil
      begin
        context = Context.new(driver, scenario.group, @charts)
        context.instance_exec(&scenario.block)
        # Destroying the system checks that it released everything it allocated.
        context.teardown!
      rescue StandardError => e
        error = e
      end

      shutdown_error = nil
      begin
        driver.close
      rescue StandardError => e
        shutdown_error = e
      end

      Result.new(scenario: scenario,
                 outcome: classify(scenario, error, shutdown_error),
                 error: error,
                 shutdown_error: shutdown_error,
                 diagnostics: driver.diagnostics)
    end

    private

    def timed(scenario)
      started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
      result = run_scenario(scenario)
      [result, Process.clock_gettime(Process::CLOCK_MONOTONIC) - started]
    end

    # Workers take scenario indexes from one pipe, as fixed-size records
    # written before any worker reads, and answer on their own pipes with
    # [index, outcome, report text, seconds]. Reports print in scenario
    # order as soon as every earlier one is in.
    def run_forked(scenarios, jobs)
      jobs_read, jobs_write = IO.pipe
      scenarios.each_index { |index| jobs_write.syswrite([index].pack('Q>')) }
      jobs_write.close
      @out.flush
      workers = Array.new(jobs) do
        answers_read, answers_write = IO.pipe
        pid = fork do
          answers_read.close
          work(scenarios, jobs_read, answers_write)
        end
        answers_write.close
        [pid, answers_read]
      end
      jobs_read.close
      collect(scenarios, workers)
    end

    def work(scenarios, jobs, answers)
      loop do
        record = begin
          jobs.sysread(8)
        rescue EOFError
          break
        end
        index = record.unpack1('Q>')
        text = StringIO.new
        out = @out
        @out = text
        begin
          result, seconds = timed(scenarios[index])
          report(result)
        ensure
          @out = out
        end
        message = Marshal.dump([index, result.outcome, text.string, seconds])
        answers.write([message.bytesize].pack('N'), message)
        answers.flush
      end
      exit!(0)
    rescue StandardError => e
      warn "scenario worker failed: #{e.class}: #{e.message}"
      exit!(1)
    end

    def collect(scenarios, workers)
      done = Array.new(scenarios.size)
      printed = 0
      open = workers.map(&:last)
      until open.empty?
        IO.select(open)[0].each do |io|
          header = io.read(4)
          if header.nil?
            open.delete(io)
            next
          end
          index, outcome, text, seconds = Marshal.load(io.read(header.unpack1('N'))) # rubocop:disable Security/MarshalLoad
          done[index] = [Reported.new(scenario: scenarios[index], outcome: outcome, text: text), seconds]
          while printed < done.size && done[printed]
            @out.write(done[printed].first.text)
            printed += 1
          end
        end
      end
      failed = workers.map(&:first).count { |pid| !Process.wait2(pid).last.success? }
      missing = done.count(&:nil?)
      raise ScenarioError, "#{failed} scenario workers failed; #{missing} scenarios did not run" if failed.positive? || missing.positive?

      done
    end

    def report_timings(results, count)
      @out.puts
      @out.puts "slowest #{count}:"
      results.sort_by { |(_, seconds)| -seconds }.first(count).each do |(result, seconds)|
        @out.puts format('  %7.2fs  %s', seconds, result.scenario.full_name)
      end
    end

    def classify(scenario, error, shutdown_error)
      expected = scenario.expected_failure
      return :fail if shutdown_error
      return(expected ? :xpass : :pass) if error.nil?
      return :fail if error.is_a?(ScenarioError) || error.is_a?(ChartError)

      expected&.matches?(error) ? :xfail : :fail
    end

    def report(result)
      scenario = result.scenario
      expected = scenario.expected_failure
      @out.puts "#{LABELS.fetch(result.outcome)} #{scenario.full_name}"
      case result.outcome
      when :xfail
        detail('expected', expected.describe)
        detail('got', result.error.message)
      when :xpass
        detail('passed', "remove expect_failure #{expected.reason.inspect} (#{scenario.location})")
      when :fail
        detail('expected', expected.describe) if expected && result.error
        describe_error('error', result.error, scenario) if result.error
        describe_error('shutdown', result.shutdown_error, scenario) if result.shutdown_error
        detail('host stderr', result.diagnostics.strip) unless result.diagnostics.strip.empty?
      end
    end

    def describe_error(label, error, scenario)
      detail(label, "#{error.class.name.split('::').last}: #{error.message}")
      file = scenario.location.split(':').first
      site = error.backtrace&.find { |line| line.start_with?(file) }
      detail('at', site ? site.sub(/:in .*/, '') : scenario.location)
    end

    def detail(label, text)
      lines = text.to_s.lines.map(&:chomp)
      @out.puts format('      %-11s %s', "#{label}:", lines.first)
      lines.drop(1).each { |line| @out.puts "#{' ' * 18}#{line}" }
    end

    def summarize(results)
      counts = results.map(&:outcome).tally
      @out.puts
      @out.puts "#{results.size} scenarios: #{counts.fetch(:pass, 0)} passed, " \
                "#{counts.fetch(:xfail, 0)} expected failures, " \
                "#{counts.fetch(:xpass, 0)} unexpected passes, #{counts.fetch(:fail, 0)} failed"
    end
  end
end
