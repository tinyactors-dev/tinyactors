# frozen_string_literal: true

require "json"
require "open3"
require "optparse"
require "rbconfig"

root = File.expand_path("..", __dir__)
options = { manifest: File.join(root, "bindings/ruby/api-coverage.json"), complete: false }
OptionParser.new do |parser|
  parser.banner = "Usage: ruby tools/check_ruby_interface.rb [--require-complete]"
  parser.on("--manifest PATH", "Check an alternate ledger, for audit tests") { |path| options[:manifest] = path }
  parser.on("--require-complete", "Reject every planned procedure") { options[:complete] = true }
end.parse!
abort "Unexpected arguments: #{ARGV.join(' ')}" unless ARGV.empty?

ledger = JSON.parse(File.read(options[:manifest]))
abort "Unsupported Ruby coverage schema" unless ledger["schema_version"] == 1
abort "Coverage must reference src/tinyactors.h" unless ledger["header"] == "src/tinyactors.h"
entries = ledger["entries"]
abort "Coverage entries must be an Array of records" unless entries.is_a?(Array) && entries.all? { |entry| entry.is_a?(Hash) }

output, errors, status = Open3.capture3(
  "clang", "-std=c99", "-Isrc", "-Xclang", "-ast-dump=json",
  "-fsyntax-only", ledger["header"], chdir: root
)
abort errors unless status.success?

declared = {}
pending = [JSON.parse(output)]
until pending.empty?
  node = pending.pop
  if node["kind"] == "FunctionDecl" && node["storageClass"] == "extern" &&
      node["name"]&.start_with?("ta_")
    declared[node["name"]] = node.fetch("type").fetch("qualType")
  end
  pending.concat(node.fetch("inner", []))
end

problems = []
symbols = entries.map { |entry| entry["symbol"] }
duplicates = symbols.group_by { |symbol| symbol }.select { |_, group| group.size > 1 }.keys
problems << "Duplicate assignments: #{duplicates.inspect}" unless duplicates.empty?
missing = declared.keys - symbols
extra = symbols - declared.keys
problems << "Missing C procedures: #{missing.join(', ')}" unless missing.empty?
problems << "Unknown C procedures: #{extra.inspect}" unless extra.empty?

entries.each do |entry|
  symbol = entry["symbol"]
  if declared.key?(symbol) && declared[symbol] != entry["c_signature"]
    problems << "C signature changed: #{symbol}"
  end
  unless %w[planned implemented].include?(entry["status"])
    problems << "Invalid implementation status: #{symbol}"
  end
  %w[brief source test].each do |field|
    path = entry[field]
    if !path.is_a?(String) || !path.start_with?("bindings/ruby/") || path.split("/").include?("..")
      problems << "Invalid #{field} path: #{symbol}"
      next
    end
    if (field == "brief" || entry["status"] == "implemented") && !File.file?(File.join(root, path))
      problems << "Missing #{field} file for #{symbol}: #{path}"
    end
  end
end
abort problems.join("\n") unless problems.empty?

# Query the actual namespace. A ledger entry alone never proves a wrapper exists.
query = 'require "json"; require "tinyactors/raw"; puts JSON.generate(TinyActors::Raw.singleton_methods(false).grep(/\Ata_/).map(&:to_s).sort)'
output, errors, status = Open3.capture3(
  RbConfig.ruby, "-I", File.join(root, "bindings/ruby/lib"), "-e", query, chdir: root
)
abort "Cannot inspect Raw exports:\n#{errors}" unless status.success?
exported = JSON.parse(output)
implemented = entries.select { |entry| entry["status"] == "implemented" }.map { |entry| entry["symbol"] }
planned = symbols - implemented
inline_helpers = ["ta_text_from_cstr"]

absent_exports = implemented - exported
unrecorded_exports = (exported - inline_helpers) - implemented
problems << "Implemented entries without callable exports: #{absent_exports.join(', ')}" unless absent_exports.empty?
problems << "Exports without implemented evidence: #{unrecorded_exports.join(', ')}" unless unrecorded_exports.empty?
problems << "Incomplete Ruby binding: #{planned.size} procedures remain planned" if options[:complete] && !planned.empty?
abort problems.join("\n") unless problems.empty?

puts "Ruby C API ledger: #{declared.size} procedures assigned; #{implemented.size} implemented; #{planned.size} planned."
puts "This check validates coverage evidence, not runtime behavior."
