# The harness reporter, not an agent's process exit code, establishes evidence.
require 'json'
require 'minitest'
input = JSON.parse(File.read(ARGV.fetch(0)))
result = {check_id: input.fetch('check').fetch('id'), nonce: input.fetch('nonce'), assertions: 0, skips: 0}
class EvidenceReporter < Minitest::StatisticsReporter
  attr_reader :observations
  def initialize
    super($stdout, {})
    @observations = []
  end
  def record(result)
    super
    @observations << {name: result.name, assertions: result.assertions,
                      failures: result.failures.map { |failure| "#{failure.class}: #{failure.message}" }}
  end
end
begin
  $LOAD_PATH.unshift(File.join(input.fetch('root'), 'bindings/ruby/lib'))
  $LOAD_PATH.unshift(File.join(input.fetch('root'), 'bindings/ruby/test'))
  require 'tinyactors'
  # Suppress a second autorun at_exit run after our explicit run.
  Minitest.class_variable_set(:@@installed_at_exit, true)
  input.fetch('files').each { |file| require file }
  reporter = EvidenceReporter.new
  Minitest.extensions.replace(['ruby_verify'])
  Minitest.define_singleton_method(:plugin_ruby_verify_init) { |_options| self.reporter << reporter }
  Minitest.run(['--no-plugins', '--seed', '1'])
  result.merge!(assertions: reporter.assertions, skips: reporter.skips, tests: reporter.count,
                observations: reporter.observations)
  valid = reporter.count > 0 && reporter.assertions > 0 && reporter.failures.zero? && reporter.errors.zero? && reporter.skips.zero?
  result.merge!(status: valid ? 'pass' : 'fail',
                message: "#{reporter.count} tests, #{reporter.assertions} assertions, #{reporter.failures} failures, #{reporter.errors} errors, #{reporter.skips} skips")
rescue Exception => error
  result.merge!(status: 'fail', message: "#{error.class}: #{error.message}", backtrace: error.backtrace&.first(20))
ensure
  File.write(input.fetch('result_path'), JSON.generate(result))
end
exit(result[:status] == 'pass' ? 0 : 1)
