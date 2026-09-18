# frozen_string_literal: true

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

    def run(groups, filter: nil)
      scenarios = groups.flat_map(&:scenarios)
      scenarios = scenarios.select { |scenario| filter.match?(scenario.full_name) } if filter
      results = scenarios.map { |scenario| run_scenario(scenario).tap { |result| report(result) } }
      summarize(results)
      results
    end

    def run_scenario(scenario)
      driver = Driver.new(@executable, timeout: @timeout)
      error = nil
      begin
        Context.new(driver, scenario.group, @charts).instance_exec(&scenario.block)
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
