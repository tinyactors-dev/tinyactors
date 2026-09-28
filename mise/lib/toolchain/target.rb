# frozen_string_literal: true

require 'fileutils'
require 'open3'
require 'shellwords'

module Toolchain
  # What the build compiles for. There are three kinds, and which one a
  # script asks for says what the result is for:
  #
  #   Target.release('x86_64-linux')  what is shipped: the pinned `zig cc`,
  #                                   a pinned triple, fixed flags, from any
  #                                   host (docs/adr/0001).
  #   Target.check                    what a check reads: the pinned clang
  #                                   for x86_64 Linux ELF, freestanding, so
  #                                   a check answers the same on every host.
  #   Target.host                     what runs here: $CC, for the test
  #                                   programs and the development builds.
  #
  # A script tells a target to compile, archive or link, and gets a Binary
  # back to check. It does not branch on the platform: what differs lives
  # in the target's ObjectFormat.
  class Target
    attr_reader :name, :triple, :arch, :format

    # The oldest macOS a binary runs on: 11.0, the first with Apple silicon,
    # for both architectures. zig reads it from the triple, and writes it as
    # the Mach-O LC_BUILD_VERSION's minos.
    MACOS_MIN = '11.0'

    def initialize(name:, triple:, os:, arch:, format:, compiler:)
      @name = name
      @triple = triple
      @os = os
      @arch = arch
      @format = format
      @compiler = compiler
    end

    def to_s = @name

    # A Linux triple is musl's: the libraries and the addon link no C
    # library (`-nostdlib`), so a Linux binary serves glibc and musl alike
    # (ADR decision 10), and musl's headers are the ones compiled against.
    RELEASES = [
      ['aarch64-macos', "aarch64-macos.#{MACOS_MIN}", :macos, :aarch64, ObjectFormat::MachO],
      ['x86_64-macos', "x86_64-macos.#{MACOS_MIN}", :macos, :x86_64, ObjectFormat::MachO],
      ['x86_64-linux', 'x86_64-linux-musl', :linux, :x86_64, ObjectFormat::ELF],
      ['aarch64-linux', 'aarch64-linux-musl', :linux, :aarch64, ObjectFormat::ELF],
      ['wasm32-freestanding', 'wasm32-freestanding', :wasm, :wasm32, ObjectFormat::WASM]
    ].freeze

    def self.releases
      @releases ||= RELEASES.map do |name, triple, os, arch, format|
        new(name: name, triple: triple, os: os, arch: arch, format: format, compiler: Zig.new)
      end.freeze
    end

    def self.release(name)
      releases.find { |target| target.name == name } or
        raise Error, "unknown target #{name}; known: #{releases.map(&:name).join(', ')}"
    end

    # The release targets that build a native shared library or addon.
    def self.native_releases = releases.reject(&:wasm?)

    def self.check
      @check ||= new(name: 'check', triple: 'x86_64-unknown-linux-gnu', os: :linux, arch: :x86_64,
                     format: ObjectFormat::ELF, compiler: Check.new)
    end

    # This machine, compiling with $CC (the root mise.toml sets the pinned
    # clang); Host knows its format and architecture.
    def self.host = @host ||= Host.current.development_target(Hosted.new)

    # --- compiling ------------------------------------------------------

    # The command that compiles a C file for this target, its cflags and
    # `flags` included, without the file: for Cache.compile, which adds -c,
    # -o and the dependency flags.
    def compile_command(*flags) = [*@compiler.command(self), *cflags, *flags.map(&:to_s)]

    # The flags every compile for this target gets (a release's are fixed;
    # $CFLAGS is never read). Recorded in a release's manifest.
    def cflags = @compiler.flags(self)

    # Compiles one C file into `into`.
    def compile(source, into:, includes: [], defines: [], flags: [])
      FileUtils.mkdir_p(File.dirname(into))
      @compiler.run(self, *cflags, *includes.map { |dir| "-I#{dir}" }, *defines.map { |define| "-D#{define}" },
                    *flags, '-c', source, '-o', into)
      Binary.new(into)
    end

    # --- linking --------------------------------------------------------

    # One relocatable object of `objects`; `keep` roots garbage collection
    # where the format's linker can do it (ELF).
    def combine(objects, into:, keep: nil)
      FileUtils.mkdir_p(File.dirname(into))
      @format.combine(self, objects, into, keep: keep)
      Binary.new(into)
    end

    # A static archive of `objects`, byte-identical for the same objects.
    def archive(objects, into:)
      FileUtils.mkdir_p(File.dirname(into))
      FileUtils.rm_f(into)
      @compiler.archive(self, objects, into)
      Binary.new(into)
    end

    # The versioned shared library lib<library> in `dir`, named as the
    # format names one (its soname or @rpath install name). `exports`, a
    # file of symbols, limits what it exports where the format's linker
    # takes such a list; `needs` are shared libraries it links against.
    def shared_library(objects, library:, version:, dir:, exports: nil)
      file = @format.shared_library_file(library, version)
      FileUtils.mkdir_p(dir)
      link_shared(objects, dir.join(file), file, @format.shared_flags(exports: exports))
      @format.install_links(file, library, dir)
      Binary.new(dir.join(file))
    end

    # A shared object that a host process loads, and whose undefined
    # symbols that process provides: a Node-API addon.
    def plugin(objects, into:)
      FileUtils.mkdir_p(File.dirname(into))
      link_shared(objects, into, File.basename(into), [*@format.plugin_flags, *@format.shared_flags])
      Binary.new(into)
    end

    # How $CC links a plugin for this machine (Target.host).
    def plugin_flags = @format.host_plugin_flags

    # A standalone wasm module of `objects`: no entry point, exporting
    # `exports` and its memory. zig links its compiler-rt, for the 128-bit
    # arithmetic core uses (`__multi3`).
    def wasm_module(objects, into:, exports:)
      raise Error, "#{@name} is not a wasm target" unless wasm?

      FileUtils.mkdir_p(File.dirname(into))
      @compiler.run(self, '-Wl,--no-entry', *exports.map { |symbol| "-Wl,--export=#{symbol}" }, *objects, '-o', into)
      Binary.new(into)
    end

    # A program of `inputs` (C files, objects, archives, libraries).
    def program(inputs, into:, flags: [])
      FileUtils.mkdir_p(File.dirname(into))
      @compiler.run(self, *flags, *inputs, '-o', into)
      Binary.new(into)
    end

    # --- what the target is ---------------------------------------------

    # The platform suffixes of the files it builds (mise/lib/platform_files.rb):
    # its OS, its architecture and the pair.
    def platform_suffixes = [@os.to_s, @arch.to_s, "#{@os}_#{@arch}"]

    # The docker platform that runs its programs, for a Linux target.
    def container_platform
      { aarch64: 'linux/arm64', x86_64: 'linux/amd64' }.fetch(@arch) if linux?
    end

    # The platform predicates are for mise/lib/toolchain/ alone; a build
    # script calls a method instead of branching on them
    # (mise/ast-grep/rules/platform-branch.yml).
    def wasm? = @os == :wasm
    def macos? = @os == :macos
    def linux? = @os == :linux

    def ==(other) = other.is_a?(Target) && other.name == @name
    alias eql? ==
    def hash = @name.hash

    private

    def link_shared(objects, out, file, flags)
      FileUtils.rm_f(out)
      @compiler.run(self, '-shared', *(@format.links_libc ? [] : ['-nostdlib']), *@format.identity_flags(file),
                    *flags, *objects, '-o', out)
    end

    # zig cc for a release target.
    class Zig
      def command(target) = Tool[:zig].command('cc', '-target', target.triple)

      # Paths in debug info and macros are made relative to the repository,
      # so a binary does not name the machine it was built on.
      def flags(target)
        [*%W[-O2 -fno-stack-protector -ffile-prefix-map=#{ROOT}=. -fvisibility=hidden], *target.format.position_independent]
      end

      def run(target, *arguments) = Tool[:zig].run('cc', '-target', target.triple, *arguments)

      # llvm's ar with `D` writes no timestamps, owners or modes.
      def archive(target, objects, out) = Tool[:zig].run('ar', *target.format.archive_flags, 'rcsD', out, *objects)
    end

    # The pinned clang for the check target.
    class Check
      def command(target) = Tool[:clang].command("--target=#{target.triple}")
      def flags(_target) = %w[-ffreestanding]
      def run(target, *arguments) = Tool[:clang].run("--target=#{target.triple}", *arguments)
      def archive(*) = raise(Error, 'the check target builds no archive')
    end

    # $CC for this machine; only this class reads it. Archives are
    # llvm-ar's, deterministic, in the host's format.
    class Hosted
      def command(_target) = Shellwords.split(ENV.fetch('CC', 'cc'))
      def flags(_target) = []

      def run(target, *arguments)
        out, status = Open3.capture2e(*command(target), *arguments.map(&:to_s))
        raise Error, "#{[*command(target), *arguments].join(' ')} failed:\n#{out}" unless status.success?

        out
      end

      def archive(target, objects, out) = Tool[:llvm_ar].run(*target.format.archive_flags, 'rcsD', out, *objects)
    end
  end
end
