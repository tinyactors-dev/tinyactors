# frozen_string_literal: true

require 'fileutils'
require 'open3'
require 'pathname'

# The release toolchain (docs/adr/0001-packaging-and-releases.md): every
# release binary is built with `zig cc`, pinned in the root mise.toml, from
# any host, for every target. This module is the one place that knows the
# targets and their flags; `$CFLAGS` is never read.
#
# A target's Linux triple is musl's: the libraries and the addon link no C
# library (`-nostdlib`), so a Linux binary serves glibc and musl alike
# (ADR decision 10), and musl's headers are the ones compiled against.
module Toolchain
  ROOT = Pathname.new(__dir__).join('../..').expand_path

  Target = Data.define(:name, :triple, :os, :arch) do
    def wasm? = os == :wasm
    def macos? = os == :macos
    def linux? = os == :linux
  end

  # The oldest macOS a binary runs on: 11.0, the first with Apple silicon,
  # for both architectures. zig reads it from the triple, and writes it as
  # the Mach-O LC_BUILD_VERSION's minos.
  MACOS_MIN = '11.0'

  TARGETS = [
    Target.new(name: 'aarch64-macos', triple: "aarch64-macos.#{MACOS_MIN}", os: :macos, arch: :aarch64),
    Target.new(name: 'x86_64-macos', triple: "x86_64-macos.#{MACOS_MIN}", os: :macos, arch: :x86_64),
    Target.new(name: 'x86_64-linux', triple: 'x86_64-linux-musl', os: :linux, arch: :x86_64),
    Target.new(name: 'aarch64-linux', triple: 'aarch64-linux-musl', os: :linux, arch: :aarch64),
    Target.new(name: 'wasm32-freestanding', triple: 'wasm32-freestanding', os: :wasm, arch: :wasm32)
  ].to_h { |target| [target.name, target] }.freeze

  class Error < StandardError; end

  module_function

  def target(name)
    TARGETS.fetch(name) { raise Error, "unknown target #{name}; known: #{TARGETS.keys.join(', ')}" }
  end

  # The pinned zig, resolved through mise from the repository, so it works
  # from any directory a task runs in.
  def zig
    @zig ||= begin
      out, status = Open3.capture2('mise', 'which', 'zig', chdir: ROOT.to_s)
      raise Error, 'zig is not installed: run `mise install` at the repository root' unless status.success?

      out.strip
    end
  end

  # The flags every compile for `target` uses. Paths in debug info and
  # macros are made relative to the repository, so a binary does not name
  # the machine it was built on.
  def cflags(target)
    flags = %W[-O2 -fno-stack-protector -ffile-prefix-map=#{ROOT}=. -fvisibility=hidden]
    flags << '-fPIC' unless target.wasm?
    flags
  end

  # Compiles one C file for `target` into `out`.
  def compile(target, source, out, includes: [], defines: [], flags: [])
    FileUtils.mkdir_p(File.dirname(out))
    run(zig, 'cc', '-target', target.triple, *cflags(target),
        *includes.map { |dir| "-I#{dir}" }, *defines.map { |name| "-D#{name}" }, *flags,
        '-c', source.to_s, '-o', out.to_s)
    out
  end

  # A static archive of `objects`, byte-identical for the same objects:
  # llvm's `ar` with `D` writes no timestamps, owners or modes.
  def archive(target, objects, out)
    FileUtils.rm_f(out)
    format = target.macos? ? ['--format=darwin'] : []
    run(zig, 'ar', *format, 'rcsD', out.to_s, *objects.map(&:to_s))
    out
  end

  # A shared library of `objects`: `name` is its soname on Linux and its
  # install name (`@rpath/<name>`) on macOS. `libc: false` links no C
  # library (`-nostdlib`), leaving what it calls to be bound at load time.
  # `flags` are extra linker arguments, such as macOS's
  # `-undefined dynamic_lookup` for a Node-API addon.
  def shared(target, objects, out, name:, libc: false, flags: [])
    raise Error, 'a wasm target has no shared library; use wasm_module' if target.wasm?

    identity = target.macos? ? "-Wl,-install_name,@rpath/#{name}" : "-Wl,-soname,#{name}"
    run(zig, 'cc', '-target', target.triple, '-shared', *(libc ? [] : ['-nostdlib']), identity, *flags,
        *objects.map(&:to_s), '-o', out.to_s)
    out
  end

  # A standalone wasm module of `objects`: no entry point, exporting
  # `exports` and its memory. zig links its compiler-rt, for the 128-bit
  # arithmetic core uses (`__multi3`).
  def wasm_module(target, objects, out, exports:)
    raise Error, "#{target.name} is not a wasm target" unless target.wasm?

    run(zig, 'cc', '-target', target.triple, '-Wl,--no-entry', *exports.map { |symbol| "-Wl,--export=#{symbol}" },
        *objects.map(&:to_s), '-o', out.to_s)
    out
  end

  def run(*command)
    out, status = Open3.capture2e(*command)
    raise Error, "#{command.join(' ')}\n#{out}" unless status.success?

    out
  end
end
