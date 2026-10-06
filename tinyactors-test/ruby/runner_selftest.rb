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
    @runs = 0
  end

  def ta = @proxy.ta

  def call(method, params = nil)
    @calls << [method, params]
    return { result: 'err_invalid', out: {} } if method == fail_method

    case method
    when 'ta.system.create' then ok(out: { '$handle': 'system:1' })
    when 'ta.system.register_invoker' then ok
    when 'ta.system.load_needed' then { result: 'err_not_found', out: {} }
    when 'ta.definition.compile_spec' then ok(out: { '$handle': 'definition:2' })
    when 'ta.system.register_definition' then ok(out: 0)
    when 'ta.actor.spawn' then ok(out: 5)
    when 'ta.run'
      # Each idle answer follows a paused macrostep and a call that stopped
      # with inputs left.
      @runs += 1
      ok(outcomes: [], outcome_count: 0, status: %w[paused more idle][(@runs - 1) % 3])
    when 'system.actors'
      # At quiescence no session is left in a macrostep.
      params[:runnable] ? [] : [5]
    when 'ta.system.set_time' then ok(dispatched: 0, more_due: false, has_wake: false)
    when 'ta.system.time' then { value: 0 }
    when 'ta.actor.deliver' then ok
    when 'ta.system.intern_atom' then ok(out: @atoms += 1)
    when 'ta.event.name_intern' then ok(out: 9)
    when 'ta.actor.inspect'
      ok(out: { active_state_count: 2, delayed_sends: [{ due: 10_000, event: 9, submitted: false }] })
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
setup = %w[ta.system.create ta.system.intern_atom
           ta.system.register_invoker ta.definition.compile_spec ta.system.register_definition]
check(methods.first(5) == setup, "setup sequence: #{methods.first(5)}")
invoker = host.calls.find { |method, _| method == 'ta.system.register_invoker' }.last
check(invoker[:type] == 1 && invoker.dig(:invoker, '$adapter') == 'invoker.scxml' &&
        invoker.dig(:invoker, 'config', 'charts').key?('traffic_light'),
      'the SCXML invoker is registered with every chart')
check(methods.include?('ta.system.load_needed'), 'the scheduler asks core for the load requests it owes')
compile = host.calls.find { |method, _| method == 'ta.definition.compile_spec' }.last
check(compile[:spec][:datamodel] == 'jq', 'the chart spec carries its SCXML datamodel')
check(compile[:datamodel] == { '$adapter' => 'datamodel.jq' }, 'the jq adapter implements it')
register = host.calls.find { |method, _| method == 'ta.system.register_definition' }.last
check(register[:definition] == { '$handle': 'definition:2' }, 'the compiled handle is registered')
segments = host.calls.select do |method, params|
  method == 'ta.system.intern_atom' && params[:text] != TinyactorsTest::Context::SCXML_INVOKER_TYPE
end
check(segments.empty?, 'a delivered event name is not interned (core ADR 0100)')
check(host.calls.none? { |method, _| method == 'ta.event.name_intern' }, 'no event name is interned')
deliver = host.calls.find { |method, _| method == 'ta.actor.deliver' }.last
check(deliver[:name] == 'fault.minor', "event names travel as text: #{deliver[:name]}")
check(deliver[:event] == { type: 'external', invoke_id: TinyactorsTest::INVOKE_ID_NONE,
                           data: { reason: 'test' } }, "deliver event: #{deliver[:event]}")
dispatches = host.calls.count { |method, params| method == 'ta.system.set_time' && params[:budget] }
check(methods.count('ta.run') >= 3 * dispatches,
      'paused macrosteps, and calls with inputs left, are run until core is idle')
check(host.calls.any? { |method, params| method == 'system.actors' && params[:runnable] },
      'at quiescence, every session left in a macrostep is cross-checked')
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

# The residency and named-actor helpers are thin wrappers over core calls.
# This fake answers them in the documented shapes, so their argument names are
# checked against the real schemas and their result handling runs even where a
# scenario would stop earlier.
class ResidencyFakeHost < FakeHost
  def initialize(discovery)
    super
    @deadlines = [{ result: 'ok', out: { out: 30_000 } }, { result: 'err_not_found', out: { out: nil } }]
  end

  def call(method, params = nil)
    case method
    when 'ta.system.set_actor_directory', 'ta.actor.unload', 'ta.actor.destroy', 'ta.actor.remove_unloaded',
         'ta.actor.publish'
      record(method, params)
      ok
    when 'ta.definition.inspect' then record(method, params) && ok(out: { identity: { family: { name: 'traffic_light' } } })
    when 'ta.actor.capture' then record(method, params) && ok(out: { '$handle': 'session_image:7' })
    when 'ta.actor.load' then record(method, params) && ok(out: 6)
    when 'ta.actor.find' then record(method, params) && ok(out: 6)
    when 'ta.actor.prepare' then record(method, params) && ok(out: 9)
    when 'ta.session.image_next_deadline' then record(method, params) && @deadlines.shift
    when 'ta.session.image_destroy' then record(method, params) && {}
    else super
    end
  end

  private

  # Checks argument names against the real schema, as the proxy does.
  def record(method, params)
    @calls << [method, params]
    path = method.split('.')
    schema = path.reduce(@proxy) { |proxy, segment| segment == path.last ? proxy : proxy.public_send(segment) }
                 .schema(path.last)
    unknown = params.keys.map(&:to_s) - schema[:params].keys.map(&:to_s)
    raise "fake host: #{method} got unknown argument(s) #{unknown}" unless unknown.empty?

    true
  end
end

residency = ResidencyFakeHost.new(discovery)
context = TinyactorsTest::Context.new(residency, group, TinyactorsTest.charts)
context.instance_exec do
  light = spawn name: 'l1'
  address = { family: { name: 'traffic_light' }, name: 'l1' }
  check(light.address == address, "a named actor knows its address: #{light.address}")
  cycle_residency(light)
  check(light.handle == 6, 'cycle_residency rebinds the actor to the loaded runtime ID')
  check(actors.include?(light), 'a loaded actor is checked by invariants again')
  check(actor_label(6) == 'traffic_light.1', 'a loaded actor keeps its trace label')
  image = capture(light)
  check(image == { '$handle': 'session_image:7' }, 'capture returns the image handle')
  check(image_deadline(image) == 30_000 && image_deadline(image).nil?, 'image_deadline reads the deadline or nil')
  check(find(address) == ['ok', 6], 'find returns the result and the runtime ID')
  check(remove_unloaded(address) == 'ok', 'remove_unloaded returns the result')
  replacement = prepare(:traffic_light, incumbent: light, binding: { cycles: 1 })
  check(replacement.handle == 9 && replacement.address == address, 'a replacement shares the incumbent address')
  check(!actors.include?(replacement), 'an unpublished replacement is not checked by invariants')
  check(publish(replacement, incumbent: light) == 'ok' && actors.include?(replacement),
        'a published replacement is checked by invariants')
end
calls = residency.calls
spawn_call = calls.find { |method, _| method == 'ta.actor.spawn' }.last
check(spawn_call[:address] == { family: { name: 'traffic_light' }, name: 'l1' }, "named spawn address: #{spawn_call}")
check(calls.any? { |method, _| method == 'ta.system.set_actor_directory' }, 'named spawn installs a directory')
load_call = calls.find { |method, _| method == 'ta.actor.load' }.last
check(load_call[:image] == { '$handle': 'session_image:7' } && load_call[:datamodel_binding] == { cycles: 1 },
      "load passes the image and the captured data root: #{load_call}")
unload_call = calls.find { |method, _| method == 'ta.actor.unload' }.last
check(unload_call[:actor] == 5, 'unload names the resident runtime ID')
check(calls.count { |method, _| method == 'ta.session.image_destroy' } == 1, 'a residency cycle destroys its image')

puts 'tinyactors-test runner self-test passed'
