# One independently supervised process per contract check.
require 'json'
require_relative 'probes'

input = JSON.parse(File.read(ARGV.fetch(0)))
check = input.fetch('check')
probe = RubyBindingProbes.new(input)
result = {check_id: check.fetch('id'), nonce: input.fetch('nonce'), assertions: 0, skips: 0}
begin
  $LOAD_PATH.unshift(File.join(input.fetch('root'), 'bindings/ruby/lib'))
  require 'tinyactors'
  probe.execute(check)
  raise RubyBindingProbes::Failure, 'No assertions executed' if probe.assertions.zero?
  result.merge!(status: 'pass', message: check.fetch('title'))
rescue Exception => error # Record LoadError/SystemExit too; native signals are supervised by Python.
  result.merge!(status: error.is_a?(RubyBindingProbes::ProtocolError) ? 'error' : 'fail',
                message: "#{error.class}: #{error.message}", backtrace: error.backtrace&.first(20))
ensure
  result[:assertions] = probe.assertions
  File.write(input.fetch('result_path'), JSON.generate(result))
end
exit(result[:status] == 'pass' ? 0 : 1)
