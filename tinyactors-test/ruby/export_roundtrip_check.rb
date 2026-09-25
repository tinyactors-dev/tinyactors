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
#    shorthand (empty lists, absent members, inferable union kinds);
# 5. the lookups by ID (ADR 0071) agree with the export: every transition's
#    source, event, targets, guard and content, every initial transition,
#    and where every action sits, named by its path in the exported tree.
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

# Check 5: what ta_definition_transition and ta_definition_action report
# (ADR 0071) matches the exported spec. Both sides are reduced to the same
# plain facts: transitions as tuples, and every action as the path of its
# place in the exported tree, such as
# "state:a/onentry[0]/1/then/0=raise".
class LookupAgreement
  def initialize(check, definition)
    @check = check
    @definition = definition
  end

  def failures(exported)
    view = @check.lookup(:inspect, definition: @definition)
    @states = (0...view[:state_count]).map { |id| @check.lookup(:state, definition: @definition, state: id) }
    @transitions = (0...view[:transition_count]).map do |id|
      @check.lookup(:transition, definition: @definition, transition: id)
    end
    @actions = (0...view[:action_count]).map { |id| @check.lookup(:action, definition: @definition, action: id) }
    expected = FromExport.new(exported)
    failures = []
    compare(failures, 'transitions', expected.transitions, authored_transitions)
    compare(failures, 'initial transitions', expected.initials, initial_transitions)
    compare(failures, 'actions', expected.actions, action_paths)
    compound = @states.count { |state| state[:kind] == 'compound' }
    initials = @transitions.count { |transition| transition[:origin] == 'initial' }
    failures << "#{initials} initial transitions for #{compound} compound states" unless initials == compound
    bootstraps = @transitions.count { |transition| transition[:origin] == 'bootstrap' }
    failures << "#{bootstraps} bootstrap transitions" unless bootstraps == 1
    failures
  end

  private

  # Order does not matter, and duplicates do: compare as multisets.
  def compare(failures, what, expected, actual)
    return if expected.tally == actual.tally

    failures << "the lookups disagree with the export on #{what}:\n" \
                "    only in the export: #{(expected - actual).inspect}\n" \
                "    only in the lookups: #{(actual - expected).inspect}"
  end

  def name(state) = @states.fetch(state)[:id]

  def targets(transition) = (transition[:targets] || []).map { |state| name(state) }

  def authored_transitions
    @transitions.select { |transition| transition[:origin] == 'authored' }.map do |transition|
      [name(transition[:source]), transition[:event], targets(transition),
       transition[:condition] != 0xFFFF_FFFF, transition[:type], transition[:action_count]]
    end
  end

  # Initial transitions that the export writes out: authored targets or
  # content. A default one with no content exports as nothing.
  def initial_transitions
    @transitions.reject { |transition| transition[:origin] == 'authored' }
                .reject { |transition| transition[:default_targets] && transition[:action_count].zero? }
                .map do |transition|
      owner = transition[:origin] == 'bootstrap' ? 'document' : "state:#{name(transition[:source])}"
      [owner, transition[:default_targets] ? nil : targets(transition), transition[:action_count]]
    end
  end

  def action_paths
    @actions.each_index.map { |id| "#{action_path(id)}=#{@actions[id][:kind]}" }
  end

  # The path of an action, walking up through the <if>s and <foreach>es
  # that hold it.
  def action_path(id)
    suffix = []
    loop do
      action = @actions.fetch(id)
      suffix.unshift(action[:index].to_s)
      case action[:block]
      when 'then', 'else', 'foreach'
        suffix.unshift(action[:block] == 'foreach' ? 'body' : action[:block])
        id = action[:owner]
      else
        return [block_path(action), *suffix].join('/')
      end
    end
  end

  def block_path(action)
    case action[:block]
    when 'onentry', 'onexit' then "state:#{name(action[:owner])}/#{action[:block]}[#{action[:block_index]}]"
    when 'finalize' then "state:#{name(action[:owner])}/invoke[#{action[:block_index]}]/finalize"
    when 'script' then 'script'
    when 'transition' then transition_path(action[:owner])
    else raise "an action in an unknown block: #{action.inspect}"
    end
  end

  def transition_path(id)
    transition = @transitions.fetch(id)
    case transition[:origin]
    when 'bootstrap' then 'document/initial'
    when 'initial' then "state:#{name(transition[:source])}/initial"
    else
      # Its place among its source's <transition>s: IDs follow document order.
      nth = (0...id).count { |earlier| @transitions[earlier][:origin] == 'authored' &&
                                       @transitions[earlier][:source] == transition[:source] }
      "state:#{name(transition[:source])}/transition[#{nth}]"
    end
  end

  # The same facts, read from an exported spec.
  class FromExport
    attr_reader :transitions, :initials, :actions

    def initialize(exported)
      @transitions = []
      @initials = []
      @actions = []
      walk_actions([exported['script']].compact.map { |_| { 'kind' => 'script' } }, 'script')
      initial(exported['initial'], 'document')
      pending = (exported['states'] || []).reverse
      until pending.empty?
        state = pending.pop
        visit(state)
        pending.concat((state['states'] || []).reverse)
      end
    end

    private

    def visit(state)
      prefix = "state:#{state['id']}"
      initial(state['initial'], prefix)
      (state['onentry'] || []).each_with_index { |handler, nth| walk_actions(handler['actions'], "#{prefix}/onentry[#{nth}]") }
      (state['onexit'] || []).each_with_index { |handler, nth| walk_actions(handler['actions'], "#{prefix}/onexit[#{nth}]") }
      (state['transitions'] || []).each_with_index do |transition, nth|
        @transitions << [state['id'], transition['event'] || '', transition['targets'] || [],
                         !transition['condition'].nil?, transition['kind'] || 'external',
                         (transition['actions'] || []).size]
        walk_actions(transition['actions'], "#{prefix}/transition[#{nth}]")
      end
      (state['invokes'] || []).each_with_index do |invoke, nth|
        walk_actions(invoke['finalize'], "#{prefix}/invoke[#{nth}]/finalize")
      end
    end

    def initial(spec, owner)
      return if spec.nil?

      @initials << [owner, spec['targets'], (spec['actions'] || []).size]
      walk_actions(spec['actions'], "#{owner}/initial")
    end

    def walk_actions(actions, prefix)
      pending = (actions || []).each_with_index.map { |action, index| [action, "#{prefix}/#{index}"] }.reverse
      until pending.empty?
        action, path = pending.pop
        @actions << "#{path}=#{action['kind']}"
        nested = []
        if action['kind'] == 'if'
          nested << ['then', action['then_actions']] << ['else', action['else_actions']]
        elsif action['kind'] == 'foreach'
          nested << ['body', action['body']]
        end
        nested.reverse_each do |block, list|
          (list || []).each_with_index.reverse_each { |child, index| pending << [child, "#{path}/#{block}/#{index}"] }
        end
      end
    end
  end
end

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
    rescue TinyactorsTest::RPCError => e
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
                                     datamodels: [datamodel], allocator: ALLOCATOR)
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
    return run_scxml(chart, datamodel) if chart.is_a?(TinyactorsTest::SCXMLChart)

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
    @last_identity = before
    after = identity(again)
    failures << "identity #{before} became #{after}" unless before == after
    failures << 'exporting the recompiled definition differs' unless json(export(again)) == exported
    failures << "the export differs from the chart spec:\n#{diff(canonical(spec), canonical(exported))}" \
      unless canonical(spec) == canonical(exported)
    failures + LookupAgreement.new(self, original).failures(exported)
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
    @last_identity = before
    failures << "identity #{before} became #{identity(again)} through export" unless before == identity(again)
    failures << 'exporting the recompiled definition differs' unless json(export(again)) == exported
    failures.concat(LookupAgreement.new(self, original).failures(exported))

    encoded = encode(original)
    reimported, result = import(encoded, datamodel, chart.files)
    return failures + ["import of the encoding: #{result}\n#{encoded}"] if reimported.nil?

    failures << "identity #{before} became #{identity(reimported)} through encode" unless before == identity(reimported)
    failures << 'encoding the reimported definition differs' unless encode(reimported) == encoded
    failures
  end

  # The identity of the chart run last, for TA_REVISION_DUMP.
  attr_reader :last_identity

  # Calls a ta_definition_* lookup on this check's system (ADR 0071).
  def lookup(function, **params)
    ok("ta_definition_#{function}", @ta.definition.public_send(function, system: @system, **params))
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
revisions = {}
begin
  check = Check.new(driver)
  charts.each do |name, chart|
    outcome = check.run(chart)
    revisions[name] = check.last_identity unless outcome == :skipped
    if outcome == :skipped
      skipped << name
    elsif !outcome.empty?
      failures[name] = outcome
    end
  rescue TinyactorsTest::RPCError, RuntimeError => e
    failures[name] = ["#{e.class.name.split('::').last}: #{e.message} #{e.respond_to?(:data) ? e.data.inspect : ''}"]
  end
ensure
  driver.close
end

# TA_REVISION_DUMP=<file> writes every chart's identity, one per line, so a
# change that must not move revisions (ADR 0060 section 6) can be compared
# byte for byte against a dump taken before it.
if (dump = ENV['TA_REVISION_DUMP'])
  File.write(dump, revisions.sort.map { |name, identity| "#{name} #{JSON.generate(identity)}\n" }.join)
end

unless failures.empty?
  failures.each { |name, list| warn "#{name}:\n  #{list.join("\n  ")}" }
  abort "export round-trip check failed for #{failures.size} of #{charts.size} charts"
end

irp = charts.keys.count { |name| name.start_with?('irp_') }
checked = charts.size - skipped.size
puts "export round-trip check passed: #{checked} charts (#{irp - skipped.count { |name| name.start_with?('irp_') }} scxml_irp) " \
     "keep their revision; #{skipped.size} ecmascript charts skipped (no adapter)"
