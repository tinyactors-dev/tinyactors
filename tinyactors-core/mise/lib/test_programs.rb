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
module TestPrograms
  DIRECTORY = Pathname.new(__dir__).join('../../tests').expand_path
  MARKER = %r{\A/\* test: (runtime|standalone)( asan)? \*/\n}

  Program = Struct.new(:name, :runtime, :asan, keyword_init: true)

  # Every test program, sorted by name. Aborts on a file without a marker,
  # so a test cannot be left out by accident.
  def self.all
    @all ||= DIRECTORY.glob('*_test.c').sort.map do |path|
      marker = File.open(path, encoding: 'UTF-8', &:gets).to_s.match(MARKER)
      abort "#{path}: the first line must be /* test: runtime|standalone [asan] */" unless marker
      Program.new(name: path.basename('.c').to_s, runtime: marker[1] == 'runtime', asan: !marker[2].nil?)
    end.freeze
  end

  def self.names = all.map(&:name)
  def self.runtime = all.select(&:runtime).map(&:name)
  def self.standalone = all.reject(&:runtime).map(&:name)
  def self.asan = all.select(&:asan)
end
