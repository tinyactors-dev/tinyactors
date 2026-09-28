# frozen_string_literal: true

# Checks the toolchain (mise/lib/toolchain/) on every target, from
# whatever host runs it: zig cross-compiles, so a Linux machine reads
# Mach-O objects here and a Mac reads ELF ones, and each host exercises
# every format's code, not only its own. Run by `mise run toolchain-test`.
#
# - Every Tool is pinned in the root mise.toml and runs.
# - For each release target, a one-function C file becomes an object for
#   the target's architecture whose exports read as C names; a static
#   archive, byte-identical when built twice; and a shared library that
#   exports the same names, loads nothing beyond the platform's runtime and
#   names itself (a wasm module that imports nothing, on wasm).
# - clang and LLVM's tools are one LLVM release.
# - The check target and this host's target read their objects as C names,
#   and a program against this machine's C library links and runs.
# - Platform files are chosen by name (docs/adr/0001, decision 9).

require 'fileutils'
require 'json'
require 'open3'
require 'tmpdir'
require 'toolchain'
require 'platform_files'

module Toolchain
  module Selftest
    SOURCE = <<~C
      __attribute__((visibility("default"))) unsigned long long wide_high(unsigned long long a, unsigned long long b) {
          return (unsigned long long)(((unsigned __int128)a * b) >> 64);
      }
      static int helper(void) { return 7; }
      __attribute__((visibility("default"))) int seven(void) { return helper(); }
    C
    EXPORTS = %w[seven wide_high].freeze

    module_function

    def run
      failures = []
      check = lambda do |what, &block|
        block.call
      rescue Error => e
        failures << "#{what}: #{e.message}"
      end

      Tool.each do |tool|
        check.call("tool #{tool}") do
          raise Error, "is pinned in #{tool.pinned_in.inspect}, not the root mise.toml" unless tool.pinned_in&.then { |file| File.expand_path(file) == ROOT.join('mise.toml').to_s }

          tool.identity
        end
      end

      # clang and LLVM's tools are one toolchain: one LLVM release.
      check.call('LLVM') do
        llvm = Tool.each.select { |tool| tool.package == Tool[:clang].package || tool.program.start_with?('llvm-') }
        majors = llvm.to_h { |tool| [tool.program, tool.major_version] }
        raise Error, "clang and LLVM's tools are different releases: #{majors}" unless majors.values.uniq.size == 1
      end

      Dir.mktmpdir('toolchain-test') do |dir|
        dir = Pathname.new(dir)
        source = dir.join('seven.c')
        source.write(SOURCE)
        Target.releases.each { |target| check.call(target.name) { release(target, source, dir.join(target.name)) } }
        [Target.check, Target.host].each do |target|
          check.call("#{target.name} target") do
            target.compile(source, into: dir.join(target.name, 'seven.o')).must_be_for!(target).must_export_exactly!(EXPORTS)
          end
        end
        check.call('host program') { host_program(dir.join('hello')) }
      end

      check.call('host') do
        raise Error, "#{Host.current.target} is not a native release target" unless Target.native_releases.include?(Host.current.target)
      end
      platform_files(failures)

      if failures.empty?
        puts "toolchain-test: #{Tool::PINNED.size} tools pinned; #{Target.releases.size} release targets, the check " \
             "target and this #{Host.current} host compile, archive, link and read as expected"
      else
        failures.each { |failure| warn "toolchain-test: #{failure}" }
        exit 1
      end
    end

    def release(target, source, out)
      object = target.compile(source, into: out.join('seven.o')).must_be_for!(target).must_export_exactly!(EXPORTS)
      return wasm(target, object, out) if target.wasm?

      first = target.archive([object], into: out.join('one/libseven.a'))
      second = target.archive([object], into: out.join('two/libseven.a'))
      raise Error, 'the archive differs between two builds' unless File.binread(first) == File.binread(second)

      first.must_export_exactly!(EXPORTS)
      library = target.shared_library([object], library: 'seven', version: '0.1', dir: out.join('lib'))
      library.must_be_for!(target).must_export_exactly!(EXPORTS).must_load_only!([])
             .must_leave_undefined_only!([]).must_be_named!(library.basename)
      return unless target.macos?

      minos = library.minimum_os
      raise Error, "minos is #{minos.inspect}, not #{Target::MACOS_MIN}" unless minos == Target::MACOS_MIN
    end

    # A program against this machine's C library links and runs.
    def host_program(dir)
      source = dir.join('hello.c')
      FileUtils.mkdir_p(dir)
      source.write(%(#include <stdio.h>\nint main(void) { puts("hello"); return 0; }\n))
      program = Target.host.program([source], into: dir.join('hello')).must_be_for!(Target.host)
      out, status = Open3.capture2e(program.to_s)
      raise Error, "the host program printed #{out.inspect}" unless status.success? && out == "hello\n"
    end

    # Its imports and exports, as the WebAssembly API reads them.
    def wasm(target, object, out)
      wasm = target.wasm_module([object], into: out.join('seven.wasm'), exports: EXPORTS).must_be_for!(target)
      script = 'const m = new WebAssembly.Module(require("fs").readFileSync(process.argv[1]));' \
               'console.log(JSON.stringify({ imports: WebAssembly.Module.imports(m).length,' \
               ' exports: WebAssembly.Module.exports(m).map((e) => e.name).sort() }))'
      seen = JSON.parse(Tool[:node].run('-e', script, wasm))
      raise Error, "the module imports #{seen['imports']} things" unless seen['imports'].zero?
      raise Error, "the module exports #{seen['exports']}" unless (%w[memory] + EXPORTS - seen['exports']).empty?
    end

    def platform_files(failures)
      files = %w[src/arena.c src/txml_allocator_wasm.c src/raw_wasmer.c src/io_wasm.h src/io.h]
      portable = %w[src/arena.c src/raw_wasmer.c src/io.h]
      {
        nil => portable,
        Target.release('wasm32-freestanding') => files,
        Target.release('x86_64-linux') => portable,
        Target.host => portable
      }.each do |target, expected|
        chosen = PlatformFiles.select(files, target: target)
        failures << "platform files: #{target || 'the portable build'} takes #{chosen}" unless chosen == expected
      end
    end
  end
end
