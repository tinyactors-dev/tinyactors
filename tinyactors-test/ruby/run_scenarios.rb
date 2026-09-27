# frozen_string_literal: true

# Usage: ruby ruby/run_scenarios.rb [-n PATTERN] [--host PATH] [--jobs N]
#                                   [--fast] [--timings N] [SCENARIO_FILE...]
#
# Loads every chart in ruby/charts, then the given scenario files (default:
# every file in ruby/scenarios), and runs each scenario against a fresh host,
# in --jobs workers (default: one per CPU). --fast leaves out the exhaustive
# groups, which `mise run test:full` runs. Exits non-zero when any scenario
# fails or passes unexpectedly.

require 'etc'
require 'optparse'
require 'pathname'
require_relative 'tinyactors_test'

root = Pathname.new(__dir__).join('..').expand_path
host = root.join('build/tinyactors-test')
filter = nil
jobs = Etc.nprocessors
exhaustive = true
timings = 0

OptionParser.new do |options|
  options.banner = 'Usage: run_scenarios.rb [options] [SCENARIO_FILE...]'
  options.on('-n', '--name PATTERN', 'Run scenarios whose "group: name" matches PATTERN') do |pattern|
    filter = Regexp.new(pattern)
  end
  options.on('--host PATH', 'Host executable (default: build/tinyactors-test)') { |path| host = Pathname.new(path) }
  options.on('-j', '--jobs N', Integer, 'Scenarios run at once (default: one per CPU)') { |count| jobs = [count, 1].max }
  options.on('--fast', 'Leave out the exhaustive groups') { exhaustive = false }
  options.on('--timings N', Integer, 'Report the N slowest scenarios') { |count| timings = count }
end.parse!

abort "tinyactors-test host not found at #{host}; run `mise run build`" unless host.executable?

Dir[root.join('ruby/charts/**/*.rb').to_s].sort.each { |file| require file }
files = ARGV.empty? ? Dir[root.join('ruby/scenarios/**/*.rb').to_s].sort : ARGV.map { |f| File.expand_path(f) }
files.each { |file| require file }

runner = TinyactorsTest::Runner.new(executable: host, charts: TinyactorsTest.charts)
results = runner.run(TinyactorsTest.groups, filter: filter, jobs: jobs, exhaustive: exhaustive, timings: timings)
exit(results.all?(&:ok?) ? 0 : 1)
