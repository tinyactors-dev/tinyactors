# frozen_string_literal: true

# Checks the imported W3C SCXML IRP suite without the core:
#
# 1. the generated files match what the importer produces from the vendored
#    suite (run `mise run import-scxml-irp` after changing the importer);
# 2. every generated chart passes the chart DSL's load-time checks;
# 3. every chart spec decodes in the host (codec.roundtrip.ta_scxml_spec), so
#    the specs are valid input for ta_definition_compile_spec.

require 'pathname'
require 'tmpdir'
require_relative 'tinyactors_test'
require_relative 'scxml_irp/importer'

root = Pathname.new(__dir__).join('..').expand_path
host = root.join('build/tinyactors-test')
abort "tinyactors-test host not found at #{host}; run `mise run build`" unless host.executable?

charts_dir = root.join('ruby/charts/scxml_irp')
scenarios_dir = root.join('ruby/scenarios/scxml_irp')

Dir.mktmpdir do |tmp|
  tmp = Pathname.new(tmp)
  ScxmlIrp::Importer.new(suite: root.join('vendor/scxml-irp'), charts_dir: tmp.join('charts'),
                         scenarios_dir: tmp.join('scenarios'), root: root).write_all
  stale = [[charts_dir, tmp.join('charts')], [scenarios_dir, tmp.join('scenarios')]].flat_map do |committed, fresh|
    names = (Dir.children(committed) + Dir.children(fresh)).uniq.sort
    names.reject do |name|
      committed.join(name).file? && fresh.join(name).file? && committed.join(name).read == fresh.join(name).read
    end.map { |name| committed.join(name).relative_path_from(root).to_s }
  end
  abort "stale generated IRP files; run `mise run import-scxml-irp`:\n  #{stale.join("\n  ")}" unless stale.empty?
end

Dir[charts_dir.join('*.rb').to_s].sort.each { |file| require file }
Dir[scenarios_dir.join('*.rb').to_s].sort.each { |file| require file }
charts = TinyactorsTest.charts.select { |name, _| name.start_with?('irp_') }
scenarios = TinyactorsTest.groups.sum { |group| group.scenarios.size }

driver = TinyactorsTest::Driver.new(host.to_s)
failures = []
begin
  charts.each do |name, chart|
    driver.call('codec.roundtrip.ta_scxml_spec', { value: chart.to_spec })
  rescue TinyactorsTest::RpcError => e
    failures << "#{name}: #{e.message} #{e.data.inspect}"
  end
ensure
  driver.close
end
abort "chart specs the host cannot decode:\n  #{failures.join("\n  ")}" unless failures.empty?

puts "scxml-irp check passed: #{charts.size} charts decode, #{scenarios} scenarios load"
