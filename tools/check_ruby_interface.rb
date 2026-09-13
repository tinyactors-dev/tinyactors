require 'json'
require 'open3'

header = 'src/tinyactors.h'
manifest = 'bindings/ruby/interface_symbols.h'

output, errors, status = Open3.capture3(
  'clang', '-std=c99', '-Isrc', '-Xclang', '-ast-dump=json',
  '-fsyntax-only', header
)
abort errors unless status.success?

declared = []
pending = [JSON.parse(output)]
until pending.empty?
  node = pending.pop
  if node['kind'] == 'FunctionDecl' && node['storageClass'] == 'extern' &&
      node['name']&.start_with?('ta_')
    declared << node['name']
  end
  pending.concat(node.fetch('inner', []))
end
declared = declared.uniq.sort

represented = File.read(manifest)
  .scan(/\bX\((ta_[a-zA-Z0-9_]+)\)/).flatten.uniq.sort

missing = declared - represented
extra = represented - declared
unless missing.empty? && extra.empty?
  message = []
  message << "Ruby binding is missing C symbols:\n  #{missing.join("\n  ")}" unless missing.empty?
  message << "Ruby binding manifest has unknown C symbols:\n  #{extra.join("\n  ")}" unless extra.empty?
  abort message.join("\n")
end

puts "Ruby binding represents all #{declared.size} public C symbols."
