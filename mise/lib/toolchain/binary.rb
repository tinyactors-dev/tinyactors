# frozen_string_literal: true

require 'pathname'

module Toolchain
  # A set of C names, sorted, with a difference that reads well in an
  # error message.
  class Symbols
    include Enumerable

    def initialize(names)
      @names = names.map(&:to_s).uniq.sort.freeze
    end

    def each(&) = @names.each(&)
    def to_a = @names
    def empty? = @names.empty?
    def size = @names.size
    def include?(name) = @names.include?(name)
    def -(other) = Symbols.new(@names - other.to_a)
    def &(other) = Symbols.new(@names & other.to_a)
    def ==(other) = other.respond_to?(:to_a) && @names == other.to_a.map(&:to_s).uniq.sort
    def to_s = @names.join(' ')

    # "unexpected: ... / missing: ..." against `expected`.
    def difference(expected)
      expected = Symbols.new(expected)
      "  unexpected: #{(self - expected).to_s}\n  missing:    #{(expected - self).to_s}"
    end
  end

  # An object file, archive, shared library or module on disk. It knows
  # its object format (from llvm-readobj), reads its own symbols through
  # llvm-nm and answers them as C names, and checks itself: each must_*!
  # raises Mismatch with what differs.
  class Binary
    Mismatch = Class.new(Error)

    attr_reader :path

    def initialize(path)
      @path = Pathname.new(path)
    end

    def to_s = @path.to_s
    def basename = @path.basename
    def to_path = @path.to_s

    def format = @format ||= ObjectFormat.of(headers)
    def architecture = headers[/^Arch: (\S+)/, 1]&.to_sym

    # The functions and data it defines and exports.
    def exports = Symbols.new(nm('-g', '--defined-only')) - format.linker_defined

    # Everything it defines, local symbols too: in a linked wasm module, the
    # functions its name section names.
    def defined = Symbols.new(nm('--defined-only')) - format.linker_defined

    # What it leaves for the linker or loader to find.
    def undefined = Symbols.new(nm('-u'))

    # Every global symbol, defined or not.
    def globals = Symbols.new(nm('-g'))

    # The shared libraries it loads, by the names it records.
    def dependencies = format.dependencies(@path)

    # Its soname (ELF) or install name (Mach-O).
    def identity = format.identity(@path)

    # The oldest OS release it runs on, where the format records one.
    def minimum_os = format.minimum_os(@path)

    def must_export_exactly!(names)
      return self if exports == names

      raise Mismatch, "#{basename} exports other functions than it should:\n#{exports.difference(names)}"
    end

    # Undefined symbols beyond `allowed` and what the platform's runtime
    # provides (ObjectFormat#runtime_undefined) are an error.
    def must_leave_undefined_only!(allowed)
      extra = undefined - allowed - format.runtime_undefined(shared: shared?)
      return self if extra.empty?

      raise Mismatch, "#{basename} leaves undefined: #{extra} (allowed: #{allowed.empty? ? 'none' : Symbols.new(allowed)})"
    end

    def must_keep_local!(names)
      global = globals & names
      return self if global.empty?

      raise Mismatch, "#{basename}: #{global} must be local"
    end

    # `libraries` are the files of the shared libraries it must load,
    # beside the platform's runtime (libSystem); each is recorded by the
    # name the format gives it (a soname, or @rpath/<file>).
    def must_load_only!(libraries)
      loaded = Symbols.new(dependencies) - format.runtime_libraries
      expected = libraries.map { |library| format.install_name(library.to_s) }
      return self if loaded == expected

      raise Mismatch, "#{basename} loads other libraries than it should:\n#{loaded.difference(expected)}"
    end

    # Its soname or install name names `file`.
    def must_be_named!(file)
      return self if identity == format.install_name(file.to_s)

      raise Mismatch, "#{basename} names itself #{identity.inspect}, not #{format.install_name(file.to_s)}"
    end

    def must_be_for!(target)
      return self if headers.scan(/^Arch: (\S+)/).flatten.uniq == [target.arch.to_s]

      raise Mismatch, "#{basename} is not #{target.arch}:\n#{headers}"
    end

    private

    def headers = @headers ||= Tool[:llvm_readobj].run('--file-headers', @path)
    def shared? = format.shared?(headers)

    # C names from `llvm-nm`: an archive's member lines are dropped.
    def nm(*flags)
      Tool[:llvm_nm].run(*format.symbol_table(shared?), *flags, '--format=just-symbols', @path)
                    .lines.map(&:strip).reject { |line| line.empty? || line.end_with?(':') }
                    .map { |symbol| format.c_name(symbol) }
    end
  end
end
