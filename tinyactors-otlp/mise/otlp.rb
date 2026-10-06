# frozen_string_literal: true

# What tinyactors-otlp's tasks share: where things are, how the library is
# compiled, and which files are its sources and tests.
require 'pathname'
require 'shellwords'
require 'platform_files'
require 'toolchain'

module OTLPLibrary
  ROOT = Pathname.new(__dir__).join('..').expand_path
  BUILD = ROOT.join('build')
  OBJECTS = BUILD.join('objects')
  CORE_INCLUDE = ROOT.join('../tinyactors-core/include').expand_path
  # Core's amalgamation: the tests run a real core system and read its tel
  # ring, so every test program links it (compiled with the tests' command,
  # not freestanding; core's own build checks core).
  CORE_SOURCE = CORE_INCLUDE.join('tinyactors.c')
  WARNINGS = %w[-std=c11 -Wall -Wextra -Werror -pedantic].freeze
  INCLUDES = ["-I#{ROOT.join('include')}", "-I#{CORE_INCLUDE}", "-I#{ROOT.join('src')}"].freeze
  # The library is compiled freestanding, without the host's C library
  # headers: clang's own stdint.h, stddef.h and stdbool.h remain, and
  # anything else fails to compile, on every host, not only in the wasm
  # check. Every binding links it, a browser's too.
  FREESTANDING = %w[-ffreestanding -nostdlibinc].freeze
  # The compiler may still emit calls to these for aggregate copies; a
  # release resolves them with zig's compiler-rt, and a test program with
  # its C library.
  MEMORY = %w[memcpy memset memmove memcmp bcmp].freeze

  module_function

  def sources = PlatformFiles.sources(ROOT.join('src')).map { |path| Pathname.new(path) }

  def tests = Dir[ROOT.join('tests/*_test.c').to_s].sort.map { |path| Pathname.new(path).basename('.c').to_s }

  def library = BUILD.join('libtinyactors_otlp.a')

  def run(*command, chdir: ROOT)
    puts command.shelljoin
    pid = Process.spawn(*command.map(&:to_s), chdir: chdir.to_s)
    _pid, status = Process.wait2(pid)
    exit(status.exitstatus || 1) unless status.success?
  end
end
