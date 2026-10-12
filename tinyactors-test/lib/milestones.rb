# frozen_string_literal: true

require 'pathname'

# The cases a milestone of the bindings' plan holds a binding to
# (suites/milestones.txt): the run task's --milestone. A milestone's set is
# its own lines and those of every milestone above it in the file, so a
# binding's test task names one milestone and moves it on when the
# milestone is done.
module Milestones
  FILE = Pathname.new(__dir__).join('../suites/milestones.txt').expand_path
  SUITES = %w[protocol scenarios w3c].freeze

  # A line of milestones.txt that cannot be read, or a milestone not in it.
  class Error < StandardError; end

  # What a run under one milestone runs: the suites, and of the protocol
  # suite the vector files (by name, without .txt) and of the scenarios
  # their names. The W3C suite has no subset: it runs whole, but the skip
  # lists, once its milestone is reached.
  Selection = Data.define(:milestone, :suites, :protocol, :scenarios) do
    def to_s
      parts = suites.map do |suite|
        names = { 'protocol' => protocol, 'scenarios' => scenarios }.fetch(suite, [])
        names.empty? ? suite : "#{suite} (#{names.join(', ')})"
      end
      "#{milestone}: #{parts.join(', ')}"
    end
  end

  module_function

  # { milestone => [[suite, names], ...] }, in the file's order.
  def read(path = FILE)
    lines = {}
    File.readlines(path, chomp: true).each_with_index do |line, index|
      next if line.strip.empty? || line.lstrip.start_with?('#')

      milestone, suite, *names = line.split
      where = "#{path}:#{index + 1}"
      raise Error, "#{where}: no suite #{suite} (#{SUITES.join(', ')})" if suite && !SUITES.include?(suite)
      raise Error, "#{where}: protocol and scenarios need names" if %w[protocol scenarios].include?(suite) && names.empty?
      raise Error, "#{where}: w3c runs whole, with no names" if suite == 'w3c' && !names.empty?

      (lines[milestone] ||= []) << [suite, names] if suite
      lines[milestone] ||= []
    end
    lines
  end

  # The milestones in order.
  def names(path = FILE) = read(path).keys

  # The cumulative set of `milestone`.
  def select(milestone, path = FILE)
    all = read(path)
    unless all.key?(milestone)
      raise Error, "no milestone #{milestone.inspect} in #{path} (#{all.keys.join(', ')})"
    end

    lines = all.take_while { |name, _| name != milestone }.flat_map(&:last) + all.fetch(milestone)
    names = ->(suite) { lines.select { |each, _| each == suite }.flat_map(&:last).uniq }
    Selection.new(milestone: milestone, suites: SUITES.select { |suite| lines.any? { |each, _| each == suite } },
                  protocol: names.call('protocol'), scenarios: names.call('scenarios'))
  end

  # What is wrong with the file against the suites: a name that is not
  # there, and a vector file or scenario that is in no milestone.
  def problems(path, protocol:, scenarios:)
    files = Dir[Pathname.new(protocol).join('*.txt').to_s].map { |each| File.basename(each, '.txt') }
                                                           .reject { |each| each == 'README' }
    present = { 'protocol' => files, 'scenarios' => Dir[Pathname.new(scenarios).join('*/steps.txt').to_s]
                                                    .map { |each| File.basename(File.dirname(each)) } }
    all = read(path)
    last = all.keys.last or return ["#{path} names no milestone"]

    every = select(last, path)
    named = { 'protocol' => every.protocol, 'scenarios' => every.scenarios }
    present.flat_map do |suite, there|
      (named[suite] - there).map { |name| "#{suite} #{name}: not in suites/#{suite}" } +
        (there - named[suite]).map { |name| "#{suite} #{name}: in no milestone (#{path})" }
    end
  end
end
