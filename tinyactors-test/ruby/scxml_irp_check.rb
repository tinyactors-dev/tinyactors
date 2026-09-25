# frozen_string_literal: true

# Checks the imported W3C SCXML IRP suite without the core:
#
# 1. the generated files match what the importer produces from the vendored
#    suite (run `mise run import-scxml-irp` after changing the importer);
# 2. every generated scenario file loads;
# 3. every generated document imports in the host through the SCXML codec
#    (ta_definition_import with codec.scxml, ADR 0053): the codec reads it
#    and the core compiles it with the data model it names.

require 'pathname'
require 'tmpdir'
require_relative 'tinyactors_test'
require_relative 'scxml_irp/importer'

root = Pathname.new(__dir__).join('..').expand_path
host = root.join('build/tinyactors-test')
abort "tinyactors-test host not found at #{host}; run `mise run build`" unless host.executable?

scenarios_dir = root.join('ruby/scenarios/scxml_irp')

Dir.mktmpdir do |tmp|
  tmp = Pathname.new(tmp)
  fresh = tmp.join('scenarios')
  SCXML::IRP::Importer.new(suite: root.join('vendor/scxml-irp'), scenarios_dir: fresh, root: root).write_all
  names = (Dir.children(scenarios_dir) + Dir.children(fresh)).uniq.sort
  stale = names.reject do |name|
    scenarios_dir.join(name).file? && fresh.join(name).file? && scenarios_dir.join(name).read == fresh.join(name).read
  end.map { |name| scenarios_dir.join(name).relative_path_from(root).to_s }
  abort "stale generated IRP files; run `mise run import-scxml-irp`:\n  #{stale.join("\n  ")}" unless stale.empty?
end

Dir[scenarios_dir.join('*.rb').to_s].sort.each { |file| require file }
charts = TinyactorsTest.charts.select { |name, _| name.start_with?('irp_') }
scenarios = TinyactorsTest.groups.sum { |group| group.scenarios.size }

allocator = { '$adapter' => 'allocator.libc' }
driver = TinyactorsTest::Driver.new(host.to_s)
failures = []
begin
  system = driver.ta.system.create(config: { allocator: allocator, initial_time: 0 }).dig(:out, :out)
  charts.each do |name, chart|
    datamodel = TinyactorsTest::Context::DATAMODELS.fetch(chart.datamodel)
    response = driver.ta.definition.import(system: system, codec: { '$adapter' => 'codec.scxml' }, input: chart.source,
                                           resolver: { '$adapter' => 'resolver.map', 'config' => { 'files' => chart.files } },
                                           datamodels: [datamodel], allocator: allocator)
    next if response[:result] == 'ok'

    failures << "#{name}: #{response[:result]} " \
                "#{TinyactorsTest.describe_import_error(driver.ta, response.dig(:out, :error))}"
  rescue TinyactorsTest::RPCError => e
    failures << "#{name}: #{e.message} #{e.data.inspect}"
  end
ensure
  driver.close
end
abort "documents the host cannot import:\n  #{failures.join("\n  ")}" unless failures.empty?

puts "scxml-irp check passed: #{charts.size} documents import, #{scenarios} scenarios load"
