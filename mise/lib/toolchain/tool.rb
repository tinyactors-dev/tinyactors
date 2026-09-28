# frozen_string_literal: true

require 'open3'

module Toolchain
  # A program the build runs whose behaviour differs between hosts when it
  # is not pinned: a compiler, a linker, LLVM's binutils, tar. Each one is
  # pinned in the root mise.toml and found through `mise which`, so every
  # host runs the same version, and a missing one fails with the fix
  # instead of falling back to whatever is on PATH.
  #
  #   Toolchain::Tool[:llvm_nm].run('-g', object)   # => stdout, or raises Failed
  #
  # A program that behaves the same everywhere (git, mise, gh, docker, sh)
  # is run directly.
  class Tool
    Missing = Class.new(Error)
    Failed = Class.new(Error)

    # name => [program, the mise.toml entry that provides it]
    PINNED = {
      zig: %w[zig zig],
      clang: %w[clang clang],
      llvm_ar: %w[llvm-ar conda:llvm-tools],
      llvm_nm: %w[llvm-nm conda:llvm-tools],
      llvm_objcopy: %w[llvm-objcopy conda:llvm-tools],
      llvm_objdump: %w[llvm-objdump conda:llvm-tools],
      llvm_otool: %w[llvm-otool conda:llvm-tools],
      llvm_readobj: %w[llvm-readobj conda:llvm-tools],
      tar: %w[tar conda:tar],
      gzip: %w[gzip conda:gzip],
      node: %w[node node]
    }.freeze

    # The argument that prints a tool's version; `--version` unless listed.
    VERSION_ARGUMENT = { zig: 'version' }.freeze

    def self.[](name)
      @tools ||= {}
      @tools[name] ||= begin
        program, package = PINNED.fetch(name) do
          raise Error, "#{name} is not a pinned tool: add it to Toolchain::Tool::PINNED and to the root mise.toml"
        end
        new(name, program, package)
      end
    end

    def self.each(&) = PINNED.each_key.map { |name| self[name] }.each(&)

    attr_reader :program, :package

    def initialize(name, program, package)
      @name = name
      @program = program
      @package = package
    end

    # The absolute path of the pinned program.
    def path
      @path ||= begin
        out, status = Open3.capture2e('mise', 'which', @program, chdir: ROOT.to_s)
        raise Missing, "#{@program} is not installed: pin #{@package} in the root mise.toml and run `mise install`" unless status.success?

        out.strip
      end
    end

    # The command line that runs the tool with `arguments`, for a caller
    # that hands a command to something else (Cache.compile).
    def command(*arguments) = [path, *arguments.map(&:to_s)]

    # Runs the tool; answers its stdout, and raises Failed, with the
    # command and its output, when it fails.
    def run(*arguments, chdir: nil, stdin: nil, env: {}, binary: false)
      out, err, status = capture(*arguments, chdir: chdir, stdin: stdin, env: env, binary: binary)
      raise Failed, "#{command(*arguments).join(' ')} failed:\n#{out}#{err}" unless status.success?

      out
    end

    # Runs the tool; answers [stdout, stderr, status] whatever happens.
    def capture(*arguments, chdir: nil, stdin: nil, env: {}, binary: false)
      options = { stdin_data: stdin, binmode: binary }
      options[:chdir] = chdir.to_s if chdir
      Open3.capture3(env, *command(*arguments), **options)
    end

    # The first line of the tool's version, for cache keys and manifests.
    def identity
      @identity ||= run(VERSION_ARGUMENT.fetch(@name, '--version')).lines.first.to_s.strip
    end

    # The mise.toml that pins the tool, as mise reports it.
    def pinned_in
      out, status = Open3.capture2e('mise', 'tool', '--config-source', @package, chdir: ROOT.to_s)
      status.success? ? out.strip : nil
    end

    def to_s = @program
  end
end
