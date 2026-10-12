# frozen_string_literal: true

require 'pathname'
require_relative 'cbor'

# The scenarios (suites/scenarios/README.md): a directory of documents in the
# W3C IRP notation and a steps.txt, read into the params of the driver's
# scenario.run (docs/driver.md, "Scenario steps").
module Scenarios
  ROOT = Pathname.new(__dir__).join('../suites/scenarios').expand_path

  # A line of steps.txt that is not a step.
  class Error < StandardError; end

  Scenario = Struct.new(:name, :directory) do
    # The documents' names, as steps.txt and <invoke src> use them.
    def documents = Dir[directory.join('*.txml').to_s].sort.map { |path| File.basename(path, '.txml') }

    def steps = Scenarios.steps(directory.join('steps.txt'))
  end

  module_function

  # Every scenario under `root`: each directory with a steps.txt.
  def all(root = ROOT)
    Dir[Pathname.new(root).join('*/steps.txt').to_s].sort.map do |path|
      directory = Pathname.new(path).dirname
      Scenario.new(directory.basename.to_s, directory)
    end
  end

  # The steps of a steps.txt, as maps for scenario.run.
  def steps(path)
    steps = []
    File.readlines(path, chomp: true).each_with_index do |line, index|
      next if line.strip.empty? || line.lstrip.start_with?('#')

      where = "#{path}:#{index + 1}"
      if line.start_with?(' ', "\t")
        raise Error, "#{where}: an indented line belongs to send-many" unless steps.last&.key?('sendMany')

        to, event, data = words(line.strip, 2)
        raise Error, "#{where}: needs <alias> <event>" unless event

        steps.last['sendMany'] << message({ 'to' => to, 'event' => event }, data, where)
        next
      end
      steps << step(line, where)
    end
    steps
  end

  def step(line, where)
    command, rest = line.split(' ', 2)
    case command
    when 'image'
      names = rest.to_s.split
      raise Error, "#{where}: image needs documents" if names.empty?

      { 'image' => names.map { |name| "#{name}.scxml" } }
    when 'spawn'
      name, chart, params = words(rest, 2)
      raise Error, "#{where}: needs spawn <alias> <chart>" unless chart

      step = { 'spawn' => name, 'chart' => chart }
      step['params'] = value(params, where) if params
      step
    when 'send'
      name, event, data = words(rest, 2)
      raise Error, "#{where}: needs send <alias> <event>" unless event

      message({ 'send' => name, 'event' => event }, data, where)
    when 'send-many' then { 'sendMany' => [] }
    when 'advance' then { 'advance' => Integer(rest.to_s.strip, exception: false) || raise(Error, "#{where}: advance <ms>") }
    when 'hibernate', 'snapshot', 'restore'
      raise Error, "#{where}: #{command} needs a name" unless rest&.match?(/\A\S+\z/)

      { command => rest }
    when 'migrate' then { 'migrate' => migration(rest.to_s.split, where) }
    else raise Error, "#{where}: unknown step #{command.inspect}"
    end
  end

  # The first `count` words, then the rest of the line (a value) or nil.
  def words(text, count)
    parts = text.to_s.strip.split(' ', count + 1)
    parts.fill(nil, parts.size...count + 1)
  end

  def message(step, data, where)
    step['data'] = value(data, where) if data
    step
  end

  def value(text, where)
    CBOR::Parser.new(text).parse
  rescue CBOR::Error => e
    raise Error, "#{where}: #{e.message}"
  end

  def migration(words, where)
    options = {}
    until words.empty?
      case words.shift
      when 'wake' then options['wake'] = true
      when 'timeout' then options['timeout'] = Integer(words.shift.to_s, exception: false) || raise(Error, "#{where}: timeout <ms>")
      else raise Error, "#{where}: migrate [wake] [timeout <ms>]"
      end
    end
    options
  end
end
