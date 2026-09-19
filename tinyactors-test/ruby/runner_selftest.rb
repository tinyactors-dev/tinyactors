# frozen_string_literal: true

# Drives the scenario runner against a scripted fake host. The fake answers
# the generated ta.* methods with canned results, while argument names are
# still checked against the real host's rpc.discover schemas. This covers the
# runner's use of the bindings before core implements the functions.

require 'pathname'
require_relative 'tinyactors_test'

root = Pathname.new(__dir__).join('..').expand_path
load root.join('ruby/charts/traffic_light.rb').to_s

def check(condition, message)
  raise "runner self-test failed: #{message}" unless condition
end

real = TinyactorsTest::Driver.new(root.join('build/tinyactors-test'))
discovery = begin
  real.call('rpc.discover')
ensure
  real.close
end

# Answers like a host whose core implements the public API.
class FakeHost
  STATE_IDS = %w[operating red].freeze

  attr_reader :calls
  attr_accessor :fail_method

  def initialize(discovery)
    @proxy = TinyactorsTest::Proxy.new(self, discovery)
    @calls = []
    @atoms = 0
    @resumes = 0
  end

  def ta = @proxy.ta

  def call(method, params = nil)
    @calls << [method, params]
    return { result: 'err_invalid', out: {} } if method == fail_method

    @runnable = true if %w[ta.actor.spawn ta.actor.deliver ta.system.set_time].include?(method)
    case method
    when 'ta.system.create' then ok(out: { '$handle': 'system:1' })
    when 'ta.system.set_external_event_router', 'ta.system.register_invoker' then ok
    when 'system.actors'
      # Actor 5 has work after each step until it has been listed once.
      runnable = @runnable ? [5] : []
      @runnable = false
      runnable
    when 'ta.definition.compile_spec' then ok(out: { '$handle': 'definition:2' })
    when 'ta.system.register_definition' then ok(out: 0)
    when 'ta.actor.spawn' then ok(out: 5)
    when 'ta.actor.resume'
      # Every other resume leaves the macrostep paused once.
      @resumes += 1
      ok(status: @resumes.odd? ? 'paused' : 'stable')
    when 'ta.system.dispatch_due' then ok(dispatched: 0, more_due: false)
    when 'ta.system.set_time', 'ta.actor.deliver' then ok
    when 'ta.system.intern_atom' then ok(out: @atoms += 1)
    when 'ta.event.name_intern' then ok(out: 9)
    when 'ta.actor.inspect'
      ok(out: { active_state_count: 2, delayed_send_count: 1 })
    when 'ta.actor.datamodel_root' then ok(out: { cycles: 1 })
    when 'ta.actor.active_states'
      params[:capacity] == 2 ? ok(states: [0, 1], count: 2) : { result: 'err_nomem', out: { count: 2 } }
    when 'ta.definition.state' then ok(out: { id: STATE_IDS.fetch(params[:state]), parent: 0, kind: 'atomic' })
    when 'json.transform' then [params[:input].fetch(:cycles)]
    else raise "fake host: unexpected #{method}"
    end
  end

  private

  def ok(**out) = { result: 'ok', out: out }
end

group = TinyactorsTest::Group.new('runner', chart: :traffic_light)
host = FakeHost.new(discovery)
context = TinyactorsTest::Context.new(host, group, TinyactorsTest.charts)
context.instance_exec do
  light = spawn
  assert_configuration light, :operating, :red
  advance_time 10
  light.deliver 'fault.minor', data: { reason: 'test' }
  assert_equal 1, light.data('.cycles')
  assert_equal 1, light.delayed_sends
end

methods = host.calls.map(&:first)
setup = %w[ta.system.create ta.system.set_external_event_router ta.system.intern_atom
           ta.system.register_invoker ta.definition.compile_spec ta.system.register_definition]
check(methods.first(6) == setup, "setup sequence: #{methods.first(6)}")
invoker = host.calls.find { |method, _| method == 'ta.system.register_invoker' }.last
check(invoker[:type] == 1 && invoker.dig(:invoker, '$adapter') == 'invoker.scxml' &&
        invoker.dig(:invoker, 'config', 'charts').key?('traffic_light'),
      'the SCXML invoker is registered with every chart')
check(methods.include?('system.actors'), 'every resident actor, including invoked children, is settled')
router = host.calls.find { |method, _| method == 'ta.system.set_external_event_router' }.last
check(router == { system: { '$handle': 'system:1' },
                  router: { '$adapter' => 'router.direct', 'config' => { 'system' => { '$handle': 'system:1' } } } },
      "the direct router is installed on the scenario system: #{router}")
compile = host.calls.find { |method, _| method == 'ta.definition.compile_spec' }.last
check(compile[:spec][:datamodel] == 'jq', 'the chart spec carries its SCXML datamodel')
check(compile[:datamodel] == { '$adapter' => 'datamodel.jq' }, 'the jq adapter implements it')
register = host.calls.find { |method, _| method == 'ta.system.register_definition' }.last
check(register[:definition] == { '$handle': 'definition:2' }, 'the compiled handle is registered')
segments = host.calls.select do |method, params|
  method == 'ta.system.intern_atom' && params[:text] != TinyactorsTest::Context::SCXML_INVOKER_TYPE
end
check(segments.size == 2, 'each event segment is interned once')
name = host.calls.find { |method, _| method == 'ta.event.name_intern' }.last
check(name[:segments] == { atoms: [2, 3] }, "event names travel as a ta_atom_list: #{name[:segments]}")
deliver = host.calls.find { |method, _| method == 'ta.actor.deliver' }.last
check(deliver[:event] == { name: 9, type: 'external', invoke_id: TinyactorsTest::INVOKE_ID_NONE,
                           data: { reason: 'test' } }, "deliver event: #{deliver[:event]}")
check(methods.count('ta.actor.resume') >= 2 * methods.count('ta.system.dispatch_due'),
      'paused macrosteps are resumed until stable')
check(methods.count('ta.definition.state') == 2, 'state names are resolved once per state')

# A non-OK core result fails the step with the function and result.
failing = FakeHost.new(discovery)
failing.fail_method = 'ta.actor.spawn'
begin
  TinyactorsTest::Context.new(failing, group, TinyactorsTest.charts).instance_exec { spawn }
  raise 'runner self-test failed: non-OK result was ignored'
rescue TinyactorsTest::CoreResultError => e
  check(e.message == 'ta_actor_spawn returned err_invalid', e.message)
end

puts 'tinyactors-test runner self-test passed'
