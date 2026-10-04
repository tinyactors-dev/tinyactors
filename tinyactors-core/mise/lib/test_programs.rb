# frozen_string_literal: true

require 'pathname'

# Core's test programs, read from the files themselves.
#
# Every tests/*_test.c says on its first line how it is built and run:
#
#   /* test: runtime asan */
#
# `runtime` means the program links the amalgamation object; `standalone`
# means it includes the amalgamation itself, or needs none. `asan` means
# `mise run asan` also runs it under AddressSanitizer and UBSan. A new test
# is one new file: no task keeps a list of names, so two changes that add
# tests never touch the same line.
#
# A program that is exhaustive (AGENTS.md, "Where a new test goes": a sweep,
# a long generated workload, or more than about five seconds on its own)
# says so with a last word:
#
#   /* test: standalone asan exhaustive */
#
# Every program runs in the fast pass; an exhaustive one scales itself. The
# environment variable TA_TEST_FULL=1 asks for the large sizes: the core test
# task sets it for `mise run test --full` (and so for `mise run test:full`),
# for the sanitizer run and for every program. Unset, or "0", a program runs
# its small version of the same property, which still exercises every
# behaviour. The word is checked against the source: a program marked
# exhaustive must read TA_TEST_FULL, and a program that reads it must be
# marked, so the marker and the code cannot drift apart.
module TestPrograms
  DIRECTORY = Pathname.new(__dir__).join('../../tests').expand_path
  MARKER = %r{\A/\* test: (runtime|standalone)( asan)?( exhaustive)? \*/\n}

  Program = Struct.new(:name, :runtime, :asan, :exhaustive, keyword_init: true)

  # Every test program, sorted by name. Aborts on a file without a marker,
  # so a test cannot be left out by accident.
  def self.all
    @all ||= DIRECTORY.glob('*_test.c').sort.map do |path|
      marker = File.open(path, encoding: 'UTF-8', &:gets).to_s.match(MARKER)
      abort "#{path}: the first line must be /* test: runtime|standalone [asan] [exhaustive] */" unless marker
      exhaustive = !marker[3].nil?
      reads_flag = path.read(encoding: 'UTF-8').include?('TA_TEST_FULL')
      abort "#{path}: marked exhaustive, but it never reads TA_TEST_FULL" if exhaustive && !reads_flag
      abort "#{path}: reads TA_TEST_FULL, so its marker must end in exhaustive" if reads_flag && !exhaustive
      Program.new(name: path.basename('.c').to_s, runtime: marker[1] == 'runtime', asan: !marker[2].nil?,
                  exhaustive: exhaustive)
    end.freeze
  end

  def self.names = all.map(&:name)
  def self.runtime = all.select(&:runtime).map(&:name)
  def self.standalone = all.reject(&:runtime).map(&:name)
  def self.asan = all.select(&:asan)
  def self.exhaustive = all.select(&:exhaustive).map(&:name)
end
