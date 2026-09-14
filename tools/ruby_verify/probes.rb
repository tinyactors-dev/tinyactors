# Integrator-owned executable acceptance contract. Never load agent test helpers here.
require_relative 'runtime_probes'
class RubyBindingProbes
  class Failure < StandardError; end
  class ProtocolError < StandardError; end
  attr_reader :assertions
  include RubyRuntimeProbes
  def initialize(input)
    @input, @assertions = input, 0
  end
  def assert(value, message = 'Assertion failed')
    @assertions += 1
    raise Failure, message unless value
  end
  def equal(expected, actual)
    assert(expected == actual, "Expected #{expected.inspect}, got #{actual.inspect}")
  end
  def rejects(*classes)
    begin
      yield
    rescue *classes
      assert(true)
      return
    end
    assert(false, "Expected #{classes.map(&:name).join(' or ')}")
  end
  def raw; TinyActors::Raw; end
  def constant(name); raw.const_get(name, false); end
  def klass(key)
    layer, name = key.split('.')
    (layer == 'raw' ? raw : TinyActors).const_get(name, false)
  end
  def execute(check)
    case check.fetch('probe')
    when 'native_loaded'
      directory = File.realpath(File.join(@input.fetch('root'), 'bindings/ruby/lib/tinyactors'))
      paths = $LOADED_FEATURES.select { |f| f.match?(/\/tinyactors_raw\.(bundle|so|dylib)\z/) }
      equal(1, paths.length)
      assert(File.file?(paths.first), 'Loaded native feature must exist')
      equal(directory, File.dirname(File.realpath(paths.first)))
    when 'constant'
      name = check.fetch('symbol')
      raise ProtocolError, 'C oracle did not produce ' + name unless @input.fetch('expected').key?(name)
      equal(@input.fetch('expected').fetch(name), constant(name))
    when 'export', 'native_export'
      name = check.fetch('symbol')
      assert(raw.singleton_methods(false).include?(name.to_sym), "Missing Raw.#{name}")
      equal(nil, raw.method(name).source_location) if check['probe'] == 'native_export'
    when 'ledger_entry'
      ledger = JSON.parse(File.read(File.join(@input.fetch('root'), 'bindings/ruby/api-coverage.json')))
      entry = ledger.fetch('entries').find { |item| item.fetch('symbol') == check.fetch('symbol') }
      equal('implemented', entry.fetch('status'))
      %w[source test].each { |key| assert(File.file?(File.join(@input.fetch('root'), entry.fetch(key))), "Missing #{key} evidence") }
    when 'class'
      assert(klass(check.fetch('klass')).is_a?(Class))
    when 'method'
      type = klass(check.fetch('klass'))
      method = check.fetch('method').to_sym
      assert(check['side'] == 'class' ? type.respond_to?(method) : type.public_instance_methods.include?(method), "Missing #{check['klass']} #{check['side']} method #{method}")
    else
      method = 'probe_' + check.fetch('probe')
      raise ProtocolError, 'Unknown probe ' + method unless respond_to?(method, true)
      send(method, check)
    end
  end
  def probe_result(c)
    status = constant(c.fetch('status'))
    value = {diagnostic: 'kept'}
    result = raw::Result.new(status: status, value: value)
    equal(status, result.status)
    assert(result.value.equal?(value), 'Result changed its output object')
    equal(status == constant('TA_OK'), result.ok?)
    assert(result.frozen?, 'Result must be immutable')
    rejects(FrozenError) { result.instance_variable_set(:@status, 99) }
  end
  def probe_result_type(c)
    rejects(TypeError) { raw::Result.new(status: c['value']) }
  end
  def probe_native_value(c)
    equal(nil, raw::Value.instance_method(:initialize).source_location)
    assert(File.file?(File.join(@input.fetch('root'), 'bindings/ruby/ext/tinyactors_raw/raw_value.c')),
           'Raw::Value needs the native ownership implementation specified by its brief')
  end
  def probe_foundation_files(c)
    c.fetch('files').each do |path|
      assert(File.file?(File.join(@input.fetch('root'), path)), 'Missing required foundation interface: ' + path)
    end
  end
  def probe_value(c)
    value = c['value']
    wrapped = raw::Value.new(value)
    assert(wrapped.object.equal?(value), 'Raw::Value must preserve object identity')
    GC.start
    GC.compact if GC.respond_to?(:compact)
    assert(wrapped.object.equal?(value), 'Raw::Value root failed after GC/compaction')
  end
  def probe_text(c)
    equal(c['expected'], raw.ta_text_from_cstr(c['input']))
  end
  def probe_target(c)
    if c['layer'] == 'raw'
      result = raw.ta_target_parse(c['input'])
      if c['invalid']
        assert(result.status != constant('TA_OK'), 'Invalid target was accepted')
        equal(nil, result.value)
        return
      end
      equal(constant('TA_OK'), result.status)
      record = result.value
    else
      if c['invalid']
        rejects(TinyActors::Error, ArgumentError, TypeError) { TinyActors::Target.parse(c['input']) }
        return
      end
      target = if c['constructor']
        c.key?('input') ? TinyActors::Target.public_send(c['constructor'], c['input']) : TinyActors::Target.public_send(c['constructor'])
      else
        TinyActors::Target.parse(c['input'])
      end
      assert(target.frozen?)
      record = target.to_raw
    end
    equal(constant(c.fetch('expected_kind')), record.fetch(:kind))
    equal(c['text'], record[:text]) if c.key?('text')
  end
  def probe_memory(c)
    size = c.fetch('size', 16)
    memory = raw::Memory.allocate(size: size, alignment: c.fetch('alignment', 8))
    begin
      equal(size, memory.size)
      equal(false, memory.closed?)
      if c['invalid']
        rejects(ArgumentError, RangeError) { memory.read(c.fetch('offset'), c.fetch('length')) }
        rejects(ArgumentError, RangeError) { memory.write(c.fetch('offset'), 'x' * c.fetch('write_length', 1)) }
      else
        bytes = "\x00\xffabc".b
        offset = size - bytes.bytesize
        memory.write(offset, bytes)
        copy = memory.read(offset, bytes.bytesize)
        equal(bytes, copy)
        equal(Encoding::BINARY, copy.encoding)
        copy.replace('changed')
        equal(bytes, memory.read(offset, bytes.bytesize))
        equal(''.b, memory.read(size, 0))
      end
    ensure
      memory.release
    end
    equal(true, memory.closed?)
    rejects(RuntimeError) { memory.read(0, 0) }
  end
  def probe_message(c)
    payload = c.fetch('payload').to_sym
    value = c['value']
    value = value.pack('C*') if payload == :bytes
    message = TinyActors::Message.new(c.fetch('name', 'PING.λ'), data: value, payload: payload)
    equal(c.fetch('name', 'PING.λ'), message.name)
    record = message.to_raw
    equal(message.name, record.fetch(:name))
    data = record.fetch(:data)
    equal(constant('TA_PAYLOAD_' + payload.to_s.upcase), data.fetch(:kind))
    equal(value, data.fetch(:as).fetch(:native).object) if payload == :native
    equal(value, data.fetch(:as).fetch(:bytes)) if payload == :bytes
    assert(!record.key?(:origin), 'Host message must not fabricate origin')
    assert(!record.key?(:invokeid), 'Host message must not fabricate invokeid')
  end
  def probe_message_reject(c)
    options = case c.fetch('case')
    when 'origin' then {origin: '#_scxml_forged'}
    when 'invokeid' then {invokeid: 'forged'}
    when 'tag' then {payload: :unknown}
    when 'bytes' then {payload: :bytes, data: 42}
    end
    rejects(ArgumentError, TypeError) { TinyActors::Message.new('PING', **options) }
  end
  def probe_event(c)
    if c['case'] == 'absent'
      equal(nil, TinyActors::Event.from_raw(nil))
      return
    end
    payload = case c.fetch('payload', 'none')
    when 'none' then {kind: constant('TA_PAYLOAD_NONE')}
    when 'native' then {kind: constant('TA_PAYLOAD_NATIVE'), as: {native: raw::Value.new(c['value'])}}
    when 'bytes' then {kind: constant('TA_PAYLOAD_BYTES'), as: {bytes: c.fetch('value').pack('C*')}}
    end
    record = {name: +'PING.λ', type: constant('TA_EVENT_' + c.fetch('type', 'external').upcase),
              sendid: +'send-1', origin: +'#_scxml_sender.1', origintype: +constant('TA_SCXML_IOPROCESSOR'),
              invokeid: +'worker', data: payload, message_id: 2**63 + 7, trace: nil}
    %i[sendid origin origintype invokeid].each { |key| record[key] = nil } if c['case'] == 'missing_metadata'
    original = record.dup
    event = TinyActors::Event.from_raw(record)
    %i[name sendid origin origintype invokeid message_id trace].each { |key| equal(original[key], event.public_send(key)) }
    equal(c.fetch('type', 'external').to_sym, event.type)
    expected_data = c.fetch('payload', 'none') == 'bytes' ? c.fetch('value').pack('C*') : c['value']
    equal(expected_data, event.data)
    equal(record[:type], event.to_raw.fetch(:type))
    equal(payload[:kind], event.to_raw.fetch(:data).fetch(:kind))
    record[:name].replace('MUTATED')
    equal('PING.λ', event.name)
    rejects(NoMethodError, FrozenError) { event.public_send(:origin=, 'forged') }
  end
  def probe_registry(c)
    registry = TinyActors::CodeRegistry.new
    callable = -> { c.fetch('use') }
    use = c.fetch('use').to_sym
    token = registry.register(callable, use: use)
    assert(token.is_a?(Integer) && token > 0)
    GC.start
    GC.compact if GC.respond_to?(:compact)
    assert(registry.fetch(token, use: use).equal?(callable))
    rejects(ArgumentError, KeyError, RuntimeError) { registry.fetch(0, use: use) }
    rejects(ArgumentError, KeyError, RuntimeError) { registry.fetch(token + 1_000_000, use: use) }
    wrong = use == :action ? :guard : :action
    rejects(ArgumentError, KeyError, RuntimeError) { registry.fetch(token, use: wrong) }
    assert(registry.code_owner.is_a?(raw::Value))
    registry.seal
    rejects(ArgumentError, FrozenError, RuntimeError) { registry.register(callable, use: use) }
  end
  def probe_error(c)
    status = constant(c.fetch('status'))
    if c['allowed']
      value = raw::Result.new(status: status, value: nil)
      assert(TinyActors::Error.check(value, operation: 'test', allowed: [status]).equal?(value))
    elsif status == constant('TA_OK')
      value = Object.new
      assert(TinyActors::Error.check(raw::Result.new(status: status, value: value), operation: 'test').equal?(value))
    else
      begin
        TinyActors::Error.check(status, operation: 'test')
      rescue TinyActors::Error => error
        equal(status, error.status)
        equal('test', error.operation)
        return
      end
      assert(false, 'Error.check accepted an unallowed failure status')
    end
  end
end
