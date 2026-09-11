require 'json'
require 'open3'

failures = []
Dir['src/*.c', 'tests/*.c', 'benchmarks/*.c', 'examples/*.c'].each do |file|
  output, errors, result = Open3.capture3('clang', '-std=c99', '-Isrc', '-Itests',
    '-Xclang', '-ast-dump=json', '-fsyntax-only', file)
  abort errors unless result.success?
  pending = [JSON.parse(output)]
  until pending.empty?
    node = pending.pop
    children = node.fetch('inner', [])
    if node['kind'] == 'FunctionDecl' && children.any? { |c| c['kind'] == 'CompoundStmt' }
      count = children.count { |c| c['kind'] == 'ParmVarDecl' }
      failures << "#{file}: #{node['name']} has #{count} parameters" if count > 3
    end
    pending.concat(children)
  end
end
abort failures.join("\n") unless failures.empty?
puts 'All implemented C functions take at most three parameters.'
