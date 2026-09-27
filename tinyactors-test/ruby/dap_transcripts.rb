# frozen_string_literal: true

# Plays the shared DAP transcripts (dap/README.md) against the test host's
# adapter: for each transcript, a workbench directory with one actor of its
# chart, launched with stopOnEntry, then its steps. napi's
# test/dap-transcripts.js plays the same transcripts against its adapter.
#
#   ruby ruby/dap_transcripts.rb                # every transcript
#   ruby ruby/dap_transcripts.rb stepping goto  # these, by file name
#   DAP_TRACE=1 ruby ruby/dap_transcripts.rb    # and print every message

require 'json'
require 'pathname'
require 'tmpdir'

require_relative 'dap_client'

ROOT = Pathname.new(__dir__).join('..').expand_path
DIRECTORY = ROOT.join('dap')
HOST = ROOT.join('build/tinyactors-test')
abort "tinyactors-test host not found at #{HOST}; run `mise run build`" unless HOST.executable?

# The name of this host in a transcript's `only`, and of its form of an
# expression in `$expr`.
HOST_NAME = 'test'
EXPRESSION_FORM = 'jq'
# The name of the one actor of each transcript's workbench.
ACTOR = 'a1'

# A step whose messages did not match.
class Mismatch < StandardError; end

# Structural matching of what a transcript expects against what the adapter
# sent (dap/README.md, "Matching"). Each call answers the bindings with what
# the match captured, or raises Mismatch.
module Matcher
  module_function

  def match(expected, actual, bindings, path = '')
    case expected
    when Hash
      return special(expected, actual, bindings, path) if expected.size == 1 && expected.keys.first.start_with?('$')

      fail_at(path, 'an object', actual) unless actual.is_a?(Hash)
      expected.reduce(bindings) do |bound, (key, value)|
        if value == { '$absent' => true }
          fail_at("#{path}.#{key}", 'nothing', actual[key]) if actual.key?(key)
          bound
        else
          fail_at("#{path}.#{key}", JSON.generate(value), nil) unless actual.key?(key)
          match(value, actual[key], bound, "#{path}.#{key}")
        end
      end
    when Array
      fail_at(path, "#{expected.size} elements", actual) unless actual.is_a?(Array) && actual.size == expected.size
      elements(expected, actual, bindings, path)
    else
      fail_at(path, JSON.generate(expected), actual) unless expected == actual
      bindings
    end
  end

  def special(expected, actual, bindings, path)
    key, argument = expected.first
    case key
    when '$any' then bindings
    when '$capture' then bindings.merge(argument => actual)
    when '$var'
      fail_at(path, "no variable #{argument}", actual) unless bindings.key?(argument)
      match(bindings[argument], actual, bindings, path)
    when '$expr' then match(argument.fetch(EXPRESSION_FORM), actual, bindings, path)
    when '$text'
      wanted = argument.gsub(/\{(\w+)\}/) do
        name = Regexp.last_match(1)
        bindings.fetch(name) { fail_at(path, "no variable #{name}", actual) }.to_s
      end
      fail_at(path, JSON.generate(wanted), actual) unless actual == wanted
      bindings
    when '$contains'
      fail_at(path, "a string containing #{argument.inspect}", actual) unless actual.is_a?(String) && actual.include?(argument)
      bindings
    when '$prefix'
      if argument.is_a?(String)
        fail_at(path, "a string starting with #{argument.inspect}", actual) unless actual.is_a?(String) && actual.start_with?(argument)
        return bindings
      end
      fail_at(path, "at least #{argument.size} elements", actual) unless actual.is_a?(Array) && actual.size >= argument.size
      elements(argument, actual.first(argument.size), bindings, path)
    when '$some'
      fail_at(path, 'an array', actual) unless actual.is_a?(Array)
      actual.each do |element|
        return match(argument, element, bindings, path)
      rescue Mismatch
        next
      end
      fail_at(path, "an element matching #{JSON.generate(argument)}", actual)
    else raise ArgumentError, "unknown matcher #{key} at #{path}"
    end
  end

  def elements(expected, actual, bindings, path)
    expected.each_with_index.reduce(bindings) do |bound, (value, index)|
      match(value, actual[index], bound, "#{path}[#{index}]")
    end
  end

  # A request's arguments with `$var` and `$expr` replaced.
  def substitute(value, bindings)
    case value
    when Hash
      if value.size == 1 && value.key?('$var')
        bindings.fetch(value['$var']) { raise Mismatch, "no variable #{value['$var']}" }
      elsif value.size == 1 && value.key?('$expr')
        value['$expr'].fetch(EXPRESSION_FORM)
      else
        value.transform_values { |inner| substitute(inner, bindings) }
      end
    when Array then value.map { |inner| substitute(inner, bindings) }
    else value
    end
  end

  def fail_at(path, wanted, actual)
    raise Mismatch, "at #{path.empty? ? 'the top' : path}: expected #{wanted}, got #{JSON.generate(actual)}"
  end
end

# One transcript played against a fresh adapter.
class Transcript
  attr_reader :name

  def initialize(path)
    @path = path
    @name = path.basename('.json').to_s
    @spec = JSON.parse(path.read)
  end

  def skip_reason
    only = @spec['only']
    "only for #{only}: #{@spec.fetch('why')}" if only && only != HOST_NAME
  end

  def play
    chart = @spec.fetch('chart')
    Dir.mktmpdir("dap-#{@name}") do |tmp|
      @workbench = Pathname.new(tmp)
      @workbench.join("#{chart}.rb").write(DIRECTORY.join('charts', "#{chart}.rb").read)
      @workbench.join('actors.rb').write("Workbench.actor :#{ACTOR}, chart: :#{chart}\n")
      @client = DAPClient.new([RbConfig.ruby, ROOT.join('bin/dap').to_s])
      @bindings = {}
      @initialized = false
      begin
        @spec.fetch('steps').each_with_index do |step, index|
          run_step(step)
        rescue Mismatch => error
          raise Mismatch, "step #{index + 1} (#{describe(step)}): #{error.message}"
        end
      ensure
        @client.request('disconnect') rescue nil
        @client.close
      end
    end
  end

  private

  def describe(step)
    return 'start' if step.key?('start')
    return step['request'] if step.key?('request')

    (step['events'] || [step]).map { |event| event['event'] }.join(', ')
  end

  def run_step(step)
    if step.key?('start') then start
    elsif step.key?('request') then request(step)
    elsif step.key?('events') then expect_events(step['events'])
    elsif step.key?('event') then expect_events([step])
    else raise ArgumentError, "#{@name}: a step is start, request, event or events: #{JSON.generate(step)}"
    end
  end

  def initialize_session
    @client.request('initialize', 'adapterID' => 'tinyactors', 'linesStartAt1' => true, 'columnsStartAt1' => true,
                                  'supportsInvalidatedEvent' => true)
    @initialized = true
  end

  # The session start of this host: launch the workbench, stopped once
  # its actor has started. `thread` is that actor's thread.
  def start
    initialize_session unless @initialized
    launched = @client.request('launch', 'workbench' => @workbench.to_s, 'stopOnEntry' => true)
    raise Mismatch, "launch failed: #{launched['message']}" unless launched['success']

    @client.request('configurationDone')
    @client.event('stopped') { |body| body['reason'] == 'entry' }
    threads = @client.request('threads')['body']['threads']
    subject = threads.find { |thread| thread['name'].start_with?("#{@spec['chart']}/#{ACTOR} ") }
    raise Mismatch, "no thread for #{ACTOR} among #{threads.map { |thread| thread['name'] }}" if subject.nil?

    @bindings['thread'] = subject['id']
    @bindings['title'] = subject['name'].split(' · ').first
  end

  def request(step)
    command = step['request']
    arguments = Matcher.substitute(step['arguments'] || {}, @bindings)
    @initialized = true if command == 'initialize'
    response = @client.request(command, arguments)
    expected = { 'success' => true }.merge(step['response'] || {})
    @bindings = Matcher.match(expected, response, @bindings, 'response')
  end

  # Waits until each expected event has a distinct pending event of its
  # name that matches, then takes those and every earlier pending event of
  # the same names.
  def expect_events(expected)
    found = nil
    begin
      @client.read_until { (found = assign(expected)) }
    rescue RuntimeError => error
      raise Mismatch, "#{missing(expected)}\n  (#{error.message.lines.first.strip})"
    end
    events, @bindings = found
    pending = @client.pending
    last = events.group_by { |event| event['event'] }
                 .transform_values { |list| list.map { |event| pending.index { |other| other.equal?(event) } }.max }
    kept = pending.each_with_index.reject { |event, index| last.key?(event['event']) && index <= last[event['event']] }
    pending.replace(kept.map(&:first))
  end

  # The first pending event for each expectation, in order, or nil.
  def assign(expected)
    taken = []
    bindings = @bindings
    expected.each do |expectation|
      name = expectation.fetch('event')
      found = @client.pending.find do |event|
        next false if event['event'] != name || taken.any? { |other| other.equal?(event) }

        bindings = Matcher.match(expectation['body'] || {}, event['body'] || {}, bindings, "#{name} event")
        true
      rescue Mismatch
        false
      end
      return nil if found.nil?

      taken << found
    end
    [taken, bindings]
  end

  def missing(expected)
    lines = expected.map do |expectation|
      name = expectation['event']
      bodies = @client.pending.select { |event| event['event'] == name }.map { |event| JSON.generate(event['body']) }
      "  expected #{name} #{JSON.generate(expectation['body'] || {})}; pending #{name} events: #{bodies.empty? ? 'none' : bodies.join(', ')}"
    end
    "timed out waiting for events\n#{lines.join("\n")}"
  end
end

wanted = ARGV
paths = DIRECTORY.join('transcripts').glob('*.json').sort
paths = paths.select { |path| wanted.include?(path.basename('.json').to_s) } unless wanted.empty?
abort 'no transcripts' if paths.empty?

failures = []
paths.each do |path|
  transcript = Transcript.new(path)
  if (reason = transcript.skip_reason)
    puts "  skip #{transcript.name}: #{reason}"
    next
  end
  transcript.play
  puts "  ok   #{transcript.name}"
rescue Mismatch => error
  failures << transcript.name
  puts "  FAIL #{transcript.name}: #{error.message}"
rescue StandardError => error
  failures << transcript.name
  puts "  FAIL #{transcript.name}: #{error.class}: #{error.message}"
  puts error.backtrace.first(6).map { |line| "         #{line}" }
end

puts
if failures.empty?
  puts "dap transcripts passed (#{paths.size})"
else
  puts "dap transcripts failed: #{failures.join(', ')}"
  exit 1
end
