# frozen_string_literal: true

# Property check for ta_definition_export (ADR 0042): for every chart the
# Ruby suite compiles (ruby/charts and every chart a scenario file defines,
# the scxml_irp charts included):
#
#   compile_spec -> register -> export (RPC) -> compile_spec -> register
#
# and then
#
# 1. the recompiled definition has the same identity (family and revision)
#    as the original, read through ta_definition_inspect;
# 2. the export carries no explicit identity, since the chart has none;
# 3. export is a fixed point: exporting the recompiled definition gives the
#    same JSON;
# 4. the exported JSON is the chart's own spec, up to the chart DSL's
#    shorthand (empty lists, absent members, inferable union kinds).
#
# A chart given as an SCXML document (TinyactorsTest.scxml_chart) is imported
# through the SCXML codec instead of compiled from a spec, and has no spec of
# its own to compare with (4). It is also encoded back to SCXML with
# ta_definition_encode, and that document must import to the same revision:
# the codec's round trip (ADR 0053).
#
# Charts whose data model has no adapter in the host yet (ecmascript) cannot
# be compiled; they are counted as skipped while their compile fails.

require 'json'
require 'pathname'
require_relative 'tinyactors_test'

root = Pathname.new(__dir__).join('..').expand_path
host = root.join('build/tinyactors-test')
abort "tinyactors-test host not found at #{host}; run `mise run build`" unless host.executable?

Dir[root.join('ruby/charts/**/*.rb').to_s].sort.each { |file| require file }
Dir[root.join('ruby/scenarios/**/*.rb').to_s].sort.each { |file| require file }

Context = TinyactorsTest::Context
ALLOCATOR = { '$adapter' => 'allocator.libc' }.freeze

# The chart DSL's shorthand, removed from both sides before comparing:
# empty lists and nulls are absent, and `kind` is dropped where the one
# other member already names it ({"kind": "expr", "expr": "0"}).
def canonical(value)
  case value
  when Hash
    stripped = value.transform_values { |each| canonical(each) }.reject { |_, each| each.nil? || each == [] }
    others = stripped.keys - ['kind']
    stripped.delete('kind') if others.size == 1 && stripped['kind'] == others.first
    stripped
  when Array then value.map { |each| canonical(each) }
  else value
  end
end

def json(value) = JSON.parse(JSON.generate(value))

class Check
  def initialize(driver)
    @driver = driver
    @ta = driver.ta
    @system = ok('ta_system_create', @ta.system.create(config: { allocator: ALLOCATOR, initial_time: 0 }))
  end

  def ok(function, response)
    raise "#{function}: #{response[:result]}" unless response[:result] == 'ok'

    response.fetch(:out).fetch(:out)
  end

  # A host without the chart's data-model adapter rejects the params.
  def compile(spec, datamodel)
    response = begin
      @ta.definition.compile_spec(system: @system, spec: spec, datamodel: datamodel, allocator: ALLOCATOR)
    rescue TinyactorsTest::RpcError => e
      return [nil, e.data]
    end
    return [nil, response[:result]] unless response[:result] == 'ok'

    definition = ok('ta_system_register_definition',
                    @ta.system.register_definition(system: @system, definition: response.dig(:out, :out)))
    [definition, 'ok']
  end

  def export(definition)
    ok('ta_definition_export', @ta.definition.export(system: @system, definition: definition))
  end

  # The definition's exact identity.
  def identity(definition)
    ok('ta_definition_inspect', @ta.definition.inspect(system: @system, definition: definition)).fetch(:identity)
  end

  CODEC = { '$adapter' => 'codec.scxml' }.freeze

  def import(source, datamodel, files)
    response = @ta.definition.import(system: @system, codec: CODEC, input: source,
                                     resolver: { '$adapter' => 'resolver.map', 'config' => { 'files' => files } },
                                     datamodel: datamodel, allocator: ALLOCATOR)
    unless response[:result] == 'ok'
      return [nil, "#{response[:result]} #{TinyactorsTest.describe_import_error(@ta, response.dig(:out, :error))}"]
    end

    [response.dig(:out, :out), 'ok']
  end

  def encode(definition)
    response = @ta.definition.encode(system: @system, definition: definition, codec: CODEC)
    raise "ta_definition_encode: #{response[:result]} #{response.dig(:out, :error).inspect}" unless response[:result] == 'ok'

    response.dig(:out, :out)
  end

  # Returns :skipped, or a list of failures.
  def run(chart)
    datamodel = Context::DATAMODELS.fetch(chart.datamodel || Context::DEFAULT_DATAMODEL)
    return run_scxml(chart, datamodel) if chart.is_a?(TinyactorsTest::ScxmlChart)

    spec = json(chart.to_spec)
    original, result = compile(spec, datamodel)
    return :skipped if original.nil? && chart.datamodel == 'ecmascript'
    return ["compile_spec: #{result}"] if original.nil?

    exported = json(export(original))
    failures = []
    failures << "the export names an identity: #{exported['identity'].inspect}" if exported.key?('identity')
    again, result = compile(exported, datamodel)
    return failures + ["compile_spec of the export: #{result}"] if again.nil?

    before = identity(original)
    after = identity(again)
    failures << "identity #{before} became #{after}" unless before == after
    failures << 'exporting the recompiled definition differs' unless json(export(again)) == exported
    failures << "the export differs from the chart spec:\n#{diff(canonical(spec), canonical(exported))}" \
      unless canonical(spec) == canonical(exported)
    failures
  end

  # An SCXML chart: import, export and recompile (1-3), then encode and
  # import the encoding.
  def run_scxml(chart, datamodel)
    original, result = import(chart.source, datamodel, chart.files)
    return ["ta_definition_import: #{result}"] if original.nil?

    exported = json(export(original))
    failures = []
    failures << "the export names an identity: #{exported['identity'].inspect}" if exported.key?('identity')
    again, result = compile(exported, datamodel)
    return failures + ["compile_spec of the export: #{result}"] if again.nil?

    before = identity(original)
    failures << "identity #{before} became #{identity(again)} through export" unless before == identity(again)
    failures << 'exporting the recompiled definition differs' unless json(export(again)) == exported

    encoded = encode(original)
    reimported, result = import(encoded, datamodel, chart.files)
    return failures + ["import of the encoding: #{result}\n#{encoded}"] if reimported.nil?

    failures << "identity #{before} became #{identity(reimported)} through encode" unless before == identity(reimported)
    failures << 'encoding the reimported definition differs' unless encode(reimported) == encoded
    failures
  end

  private

  def diff(expected, actual, path = '$')
    return "#{path}: #{expected.inspect} != #{actual.inspect}" unless expected.instance_of?(actual.class)

    case expected
    when Hash
      (expected.keys | actual.keys).each do |key|
        next if expected[key] == actual[key]

        return diff(expected[key], actual[key], "#{path}.#{key}")
      end
    when Array
      return "#{path}: #{expected.size} != #{actual.size} elements" unless expected.size == actual.size

      expected.each_index do |index|
        next if expected[index] == actual[index]

        return diff(expected[index], actual[index], "#{path}[#{index}]")
      end
    end
    "#{path}: #{expected.inspect} != #{actual.inspect}"
  end
end

charts = TinyactorsTest.charts
driver = TinyactorsTest::Driver.new(host.to_s)
failures = {}
skipped = []
begin
  check = Check.new(driver)
  charts.each do |name, chart|
    outcome = check.run(chart)
    if outcome == :skipped
      skipped << name
    elsif !outcome.empty?
      failures[name] = outcome
    end
  rescue TinyactorsTest::RpcError, RuntimeError => e
    failures[name] = ["#{e.class.name.split('::').last}: #{e.message} #{e.respond_to?(:data) ? e.data.inspect : ''}"]
  end
ensure
  driver.close
end

unless failures.empty?
  failures.each { |name, list| warn "#{name}:\n  #{list.join("\n  ")}" }
  abort "export round-trip check failed for #{failures.size} of #{charts.size} charts"
end

irp = charts.keys.count { |name| name.start_with?('irp_') }
checked = charts.size - skipped.size
puts "export round-trip check passed: #{checked} charts (#{irp - skipped.count { |name| name.start_with?('irp_') }} scxml_irp) " \
     "keep their revision; #{skipped.size} ecmascript charts skipped (no adapter)"
