# frozen_string_literal: true

require 'pathname'
require_relative 'driver'

root = Pathname.new(__dir__).join('..').expand_path
driver = TinyactorsTest::Driver.new(root.join('build/tinyactors-test'))

discovery = driver.call('rpc.discover')
raise 'unexpected server name' unless discovery.fetch(:name) == 'tinyactors-test'
raise 'libjq was not reported' unless discovery.fetch(:jq) == '1.8.2'
host_methods = %w[handle.fixture host.shutdown json.transform metrics.snapshot rpc.discover system.ping]
methods = discovery.fetch(:methods)
raise 'dispatch table was not reflected in discovery' unless (host_methods - methods).empty?
raise 'discovery lists methods in order' unless methods == methods.sort
raise 'generated methods are missing' unless methods.any? { |name| name.start_with?('ta.') }
ready = driver.notifications.first
raise 'ready notification was not received' unless ready&.fetch(:method) == 'host.ready'
raise 'ready notification params were lost' unless ready.dig(:params, :transport) == 'stdio'

payload = { hello: 'world', values: [1, 2, 3] }
raise 'ping did not round-trip' unless driver.call('system.ping', payload) == payload

output = driver.call('json.transform', filter: '.values | add', input: payload)
raise 'jq transform failed' unless output == [6]

begin
  driver.call('missing.method')
  raise 'missing method unexpectedly succeeded'
rescue TinyactorsTest::ProtocolError => error
  raise 'wrong missing-method error' unless error.message.include?('-32601')
end

driver.notify('system.ping', notification: true)
driver.notify('missing.notification.method')
driver.notify('json.transform', invalid: true)
metrics = driver.call('metrics.snapshot')
raise 'request count was not recorded' unless metrics.fetch(:requests) >= 8
raise 'incoming notifications were not counted' unless metrics.fetch(:notifications_received) == 3
raise 'outgoing notification was not counted' unless metrics.fetch(:notifications_sent) == 1

driver.close
puts 'tinyactors-test smoke test passed'
