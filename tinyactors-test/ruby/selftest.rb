# frozen_string_literal: true

# Verifies the scenario DSL itself: outcome classification of expected
# failures, and the load-time checks that keep chart and DSL mistakes from
# hiding behind an expected failure. Uses only methods the host has today.

require 'pathname'
require 'stringio'
require_relative 'tinyactors_test'

root = Pathname.new(__dir__).join('..').expand_path
host = root.join('build/tinyactors-test')

def check(condition, message)
  raise "scenario DSL self-test failed: #{message}" unless condition
end

def raises?(error_class, pattern)
  yield
  false
rescue error_class => e
  pattern.match?(e.message) || raise("wrong #{error_class} message: #{e.message}")
end

group = TinyactorsTest::Group.new('selftest', chart: nil)
group.scenario('passes') { assert_equal({ a: 1 }, rpc('system.ping', a: 1)) }

group.expect_failure 'matching code', rpc_error: -32601
group.scenario('xfail on matching code') { rpc('missing.method') }

group.expect_failure 'matching code and method', rpc_error: -32601, rpc_method: 'missing.method'
group.scenario('xfail on matching method') { rpc('missing.method') }

group.expect_failure 'matching assertion message', message: /expected 1, got 2/
group.scenario('xfail on assertion message') { assert_equal 1, 2 }

group.expect_failure 'fails, but passes', rpc_error: -32601
group.scenario('xpass') { rpc('system.ping') }

group.expect_failure 'wrong code', rpc_error: -32601
group.scenario('fail on other code') { rpc('json.transform', filter: 1) }

group.expect_failure 'wrong method', rpc_error: -32601, rpc_method: 'definition.compile'
group.scenario('fail on other method') { rpc('missing.method') }

group.expect_failure 'assertion instead of RPC error', rpc_error: -32601
group.scenario('fail on assertion') { assert false, 'boom' }

group.expect_failure 'DSL misuse never matches', message: /./
group.scenario('fail on DSL misuse') { advance_time(-1) }

group.scenario('fail without expectation') { assert_equal 1, 2 }
group.finish!

out = StringIO.new
runner = TinyactorsTest::Runner.new(executable: host, charts: {}, out: out)
outcomes = runner.run([group]).to_h { |result| [result.scenario.name, result.outcome] }
expected = {
  'passes' => :pass,
  'xfail on matching code' => :xfail,
  'xfail on matching method' => :xfail,
  'xfail on assertion message' => :xfail,
  'xpass' => :xpass,
  'fail on other code' => :fail,
  'fail on other method' => :fail,
  'fail on assertion' => :fail,
  'fail on DSL misuse' => :fail,
  'fail without expectation' => :fail
}
expected.each do |name, outcome|
  check(outcomes[name] == outcome, "#{name}: expected #{outcome}, got #{outcomes[name]}\n#{out.string}")
end
check(out.string.include?('remove expect_failure "fails, but passes"'), "XPASS report\n#{out.string}")
check(out.string.include?('selftest.rb:'), "failure location\n#{out.string}")

dangling = TinyactorsTest::Group.new('dangling', chart: nil)
dangling.expect_failure 'nothing follows', rpc_error: -32601
check(raises?(TinyactorsTest::ScenarioError, /has no scenario/) { dangling.finish! }, 'dangling expect_failure')
check(raises?(TinyactorsTest::ScenarioError, /give rpc_error: or message:/) do
  TinyactorsTest::ExpectedFailure.new('vague')
end, 'expectation without a matcher')

chart = ->(&block) { TinyactorsTest::Chart.define(:broken, &block) }
check(raises?(TinyactorsTest::ChartError, /unknown state "nowhere"/) do
  chart.call { state(:a) { transition on: 'go', target: :nowhere } }
end, 'unknown transition target')
check(raises?(TinyactorsTest::ChartError, /duplicate state IDs a/) do
  chart.call { state(:a) { state :a } }
end, 'duplicate state ID')
check(raises?(TinyactorsTest::ChartError, /initial b is not a descendant/) do
  chart.call { state(:a, initial: :b) { state :c }; state :b }
end, 'initial outside its state')
check(raises?(TinyactorsTest::ChartError, /default b is outside a/) do
  chart.call { state(:a) { state :c; history :h, default: :b }; state :b }
end, 'history default outside its parent')
check(raises?(TinyactorsTest::ChartError, /cancels send ID "t", which no <send> declares/) do
  chart.call { state(:a) { on_exit { cancel :t } } }
end, 'cancel without send')
check(raises?(TinyactorsTest::ChartError, /delay "5 seconds" is not an SCXML duration/) do
  chart.call { state(:a) { on_entry { send_event 'e', delay: '5 seconds' } } }
end, 'malformed duration')
check(raises?(TinyactorsTest::ChartError, /id and its expression form are exclusive/) do
  chart.call { state(:a) { on_entry { send_event 'e', id: :x, id_location: '.x' } } }
end, 'id and idlocation together')
check(raises?(TinyactorsTest::ChartError, /cancel needs an ID or expr:/) do
  chart.call { state(:a) { on_exit { cancel } } }
end, 'cancel without an ID')

authored = TinyactorsTest::Chart.define(:authored) do
  state :a do
    on_entry do
      send_event 'e', delay: 1_500, id: :t, to: '#_parent', type: 'scxml'
      send_event 'f', delay_expr: '.timeout', id_location: '.sid', to_expr: '.reply_to'
      log 'entered', expr: '.count'
    end
    on_exit do
      cancel :t
      cancel expr: '.sid'
    end
  end
end.to_spec[:states].first
check(authored[:onentry] == [
  { kind: 'send', event: { name: 'e' }, id: { literal: 't' }, type: { literal: 'scxml' },
    target: { literal: '#_parent' }, delay: { literal: '1500ms' } },
  { kind: 'send', event: { name: 'f' }, id: { location: '.sid' }, target: { expr: '.reply_to' },
    delay: { expr: '.timeout' } },
  { kind: 'log', label: 'entered', expr: '.count' }
], "send and log specs\n#{authored[:onentry]}")
check(authored[:onexit] == [
  { kind: 'cancel', sendid: { literal: 't' } },
  { kind: 'cancel', sendid: { expr: '.sid' } }
], "cancel specs\n#{authored[:onexit]}")

check(raises?(TinyactorsTest::ChartError, /declares done_data but is not a final state/) do
  chart.call { state(:a) { done_data expr: '1' } }
end, 'done_data outside a final state')
check(raises?(TinyactorsTest::ChartError, /invoke names chart missing_child, which is not defined yet/) do
  chart.call { state(:a) { invoke type: 'scxml', chart: :missing_child } }
end, 'invoke of an undefined chart')
check(raises?(TinyactorsTest::ChartError, /cancels send ID "t", which no <send> declares/) do
  chart.call { state(:a) { on_entry { conditional { branch('.x') { cancel :t } } } } }
end, 'cancel inside a conditional')

structured = TinyactorsTest::Chart.define(:structured) do
  state :a do
    data :pending, count: { expr: '1' }
    initial(:b) { log 'initial' }
    invoke type: 'scxml', src: 'child', id: 'kid', params: { x: '.x', y: { location: '.y' } }, autoforward: true do
      log 'finalize'
    end
    state :b do
      on_entry do
        conditional do
          branch('.x == 1') { raise_event 'one' }
          branch('.x == 2') { raise_event 'two' }
          otherwise { cancel 'elsewhere', unmatched: true }
        end
        foreach '.items', item: 'item', index: 'i' do
          send_event event_expr: '.name', content: '[1,2]'
        end
      end
    end
    final :c do
      done_data params: { result: '.x' }
    end
  end
end.to_spec[:states].first
check(structured[:data] == [{ name: 'pending' }, { name: 'count', initializer: { expr: '1' } }],
      "state data specs\n#{structured[:data]}")
check(structured[:initial] == { targets: ['b'], actions: [{ kind: 'log', label: 'initial' }] },
      "initial with actions\n#{structured[:initial]}")
check(structured[:invokes] == [
  { kind: 'invoke', id: { literal: 'kid' }, type: { literal: 'scxml' }, src: { literal: 'child' },
    autoforward: true,
    params: [{ name: 'x', value: { expr: '.x' } }, { name: 'y', value: { location: '.y' } }],
    finalize: [{ kind: 'log', label: 'finalize' }] }
], "invoke specs\n#{structured[:invokes]}")
check(structured[:states][0][:onentry] == [
  { kind: 'if', condition: '.x == 1', then_actions: [{ kind: 'raise', event: 'one' }],
    else_actions: [{ kind: 'if', condition: '.x == 2', then_actions: [{ kind: 'raise', event: 'two' }],
                     else_actions: [{ kind: 'cancel', sendid: { literal: 'elsewhere' } }] }] },
  { kind: 'foreach', array: '.items', item: 'item', index: 'i',
    body: [{ kind: 'send', event: { expr: '.name' }, data: { content: '[1,2]' } }] }
], "conditional and foreach specs\n#{structured[:states][0][:onentry]}")
check(structured[:states][1][:done_data] == { params: [{ name: 'result', value: { expr: '.x' } }] },
      "done_data specs\n#{structured[:states][1][:done_data]}")

puts 'tinyactors-test scenario DSL self-test passed'
