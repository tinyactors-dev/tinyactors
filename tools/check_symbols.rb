require 'open3'

output, status = Open3.capture2e('nm', '-g', ARGV.fetch(0))
abort output unless status.success?
defined = {}
undefined = {}
output.each_line do |line|
  fields = line.split
  next if fields.size < 2
  kind, name = fields[-2..-1]
  if kind == 'U'
    undefined[name] = true
  elsif kind.match?(/\A[A-Za-z]\z/)
    defined[name] = true
  end
end
external = undefined.keys.reject { |name| defined[name] }.sort
abort "Unresolved external symbols:\n#{external.join("\n")}" unless external.empty?
puts 'All library references resolve inside the archive (no host libc dependency).'
header = File.read('src/tinyactors.h').gsub(%r{/\*.*?\*/}m, '')
public_names = header.scan(/TA_API\s+[^;{}]*?\b(ta_\w+)\s*\(/m).flatten.uniq
missing = public_names.reject { |name| defined[name] || defined["_#{name}"] }
abort "Missing public functions: #{missing.join(', ')}" unless missing.empty?
puts "All #{public_names.size} declared public functions are implemented."
