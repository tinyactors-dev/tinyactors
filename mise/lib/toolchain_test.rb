# frozen_string_literal: true

# Checks the release toolchain (mise/lib/toolchain.rb) on every target: a
# one-function C file becomes an object, a static archive and a shared
# library (a module on wasm) for the architecture the target names; the
# archive is byte-identical when built twice; a Linux shared library needs
# no library and has its soname; a macOS one has its install name; the wasm
# module imports nothing. Run by `mise run toolchain-test`.

require 'json'
require 'open3'
require 'tmpdir'
require 'toolchain'

SOURCE = <<~C
  __attribute__((visibility("default"))) unsigned long long wide_high(unsigned long long a, unsigned long long b) {
      return (unsigned long long)(((unsigned __int128)a * b) >> 64);
  }
  static int helper(void) { return 7; }
  __attribute__((visibility("default"))) int seven(void) { return helper(); }
C

# What `file` says for each target's objects and libraries.
ARCHITECTURE = {
  'aarch64-macos' => /Mach-O 64-bit arm64/,
  'x86_64-macos' => /Mach-O 64-bit x86_64/,
  'x86_64-linux' => /ELF 64-bit LSB .*x86-64/,
  'aarch64-linux' => /ELF 64-bit LSB .*ARM aarch64/,
  'wasm32-freestanding' => /WebAssembly \(wasm\) binary/
}.freeze

$failures = []

def check(condition, message)
  $failures << message unless condition
end

def file_type(path) = Open3.capture2('file', '-b', path.to_s).first

def tool?(name) = system('which', name, out: File::NULL, err: File::NULL)

Dir.mktmpdir('toolchain-test') do |dir|
  dir = Pathname.new(dir)
  source = dir.join('seven.c')
  source.write(SOURCE)
  Toolchain::TARGETS.each_value do |target|
    out = dir.join(target.name)
    object = Toolchain.compile(target, source, out.join('seven.o'))
    check(file_type(object).match?(ARCHITECTURE.fetch(target.name)), "#{target.name}: object is #{file_type(object)}")

    if target.wasm?
      wasm = Toolchain.wasm_module(target, [object], out.join('seven.wasm'), exports: %w[seven wide_high])
      check(file_type(wasm).match?(ARCHITECTURE.fetch(target.name)), "#{target.name}: module is #{file_type(wasm)}")
      # Its imports and exports, as the WebAssembly API reads them.
      script = 'const m = new WebAssembly.Module(require("fs").readFileSync(process.argv[1]));' \
               'console.log(JSON.stringify({ imports: WebAssembly.Module.imports(m).length,' \
               ' exports: WebAssembly.Module.exports(m).map((e) => e.name).sort() }))'
      text, status = Open3.capture2('node', '-e', script, wasm.to_s)
      if status.success?
        seen = JSON.parse(text)
        check(seen['imports'].zero?, "#{target.name}: the module imports #{seen['imports']} things")
        check((%w[memory seven wide_high] - seen['exports']).empty?, "#{target.name}: exports #{seen['exports']}")
      else
        check(false, "#{target.name}: node could not read the module")
      end
      next
    end

    first = Toolchain.archive(target, [object], out.join('libseven-1.a'))
    second = Toolchain.archive(target, [object], out.join('libseven-2.a'))
    check(File.binread(first) == File.binread(second), "#{target.name}: the archive differs between two builds")

    name = target.macos? ? 'libseven.0.1.dylib' : 'libseven.so.0.1'
    library = Toolchain.shared(target, [object], out.join(name), name: name)
    check(file_type(library).match?(ARCHITECTURE.fetch(target.name)), "#{target.name}: library is #{file_type(library)}")
    if target.linux?
      dynamic = Open3.capture2('readelf', '-d', library.to_s).first
      check(!dynamic.include?('(NEEDED)'), "#{target.name}: the library needs a library:\n#{dynamic}")
      check(dynamic.include?("Library soname: [#{name}]"), "#{target.name}: no soname #{name}")
      symbols = Open3.capture2('readelf', '--dyn-syms', '-W', library.to_s).first.lines
                     .map(&:split).select { |f| f[4] == 'GLOBAL' && f[6] != 'UND' }.map { |f| f[7] }
      check(symbols.sort == %w[seven wide_high], "#{target.name}: exports #{symbols.sort}")
    elsif target.macos? && tool?('llvm-otool')
      loads = Open3.capture2('llvm-otool', '-D', library.to_s).first
      check(loads.include?("@rpath/#{name}"), "#{target.name}: install name is not @rpath/#{name}:\n#{loads}")
    end
  end
end

if $failures.empty?
  puts "toolchain-test: #{Toolchain::TARGETS.size} targets compile, archive and link as expected"
else
  $failures.each { |failure| warn "toolchain-test: #{failure}" }
  exit 1
end
