# frozen_string_literal: true

# What staticjs's tasks share: where things are, how the library is
# compiled, and its test programs with their arguments.
require 'pathname'
require 'shellwords'
require 'platform_files'
require 'toolchain'

module StaticJS
  ROOT = Pathname.new(__dir__).join('..').expand_path
  BUILD = ROOT.join('build')
  OBJECTS = BUILD.join('objects')
  WARNINGS = %w[-std=c11 -Wall -Wextra -Werror -pedantic].freeze
  INCLUDES = ["-I#{ROOT.join('include')}", "-I#{ROOT.join('src')}"].freeze
  # The library is compiled freestanding, without the host's C library
  # headers: clang's own stdint.h, stddef.h and stdbool.h remain.
  FREESTANDING = %w[-ffreestanding -nostdlibinc].freeze
  # The compiler may still emit calls to these for aggregate copies.
  MEMORY = %w[memcpy memset memmove memcmp bcmp].freeze
  # Each test program, with the arguments of the fast pass and of --full
  # (mise run test:full). The full pass also fails every allocation of every
  # spec case, and of a larger program in api_test.
  TESTS = {
    'number_test' => { fast: [], full: [] },
    'api_test' => { fast: [], full: ['--full'] },
    'spec_test' => { fast: ['tests/spec'], full: ['--full', 'tests/spec'] }
  }.freeze

  module_function

  def sources = PlatformFiles.sources(ROOT.join('src')).map { |path| Pathname.new(path) }

  def tests = TESTS.keys

  def library = BUILD.join('libstaticjs.a')

  def run(*command, chdir: ROOT)
    puts command.shelljoin
    pid = Process.spawn(*command.map(&:to_s), chdir: chdir.to_s)
    _pid, status = Process.wait2(pid)
    exit(status.exitstatus || 1) unless status.success?
  end
end
