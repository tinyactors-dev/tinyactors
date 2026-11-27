# frozen_string_literal: true

require 'json'
require 'open3'

# The MCP suite (docs/mcp.md): one MCP server implementation, started as a
# command per case, given a fixture, then driven through MCP's own messages
# (newline-delimited JSON-RPC 2.0 on its standard input and output, MCP's
# stdio transport) and the fixture's methods (fixture/*). Each tool result
# is compared with what the case expects; the runner interprets nothing
# else.
module MCP
  SUITE = File.expand_path('../suites/mcp', __dir__)
  CATALOG = File.expand_path('../../tinyactors-core/mcp/tools.json', __dir__)
  # The protocol revision the runner asks for: one every SDK the bindings pin answers.
  PROTOCOL = '2025-06-18'

  # The command ended, hung, or wrote what is not a message.
  class Crashed < StandardError; end

  # A result that is not what the case expects.
  class Mismatch < StandardError; end

  # One running server: a message out, the reply to it back.
  class Server
    def initialize(command, timeout:)
      @timeout = timeout
      @stdin, @stdout, @stderr, @thread = Open3.popen3(*command)
      @stdin.sync = true
      @errors = +''
      @drain = Thread.new { @stderr.each_line { |line| @errors << line } rescue nil } # rubocop:disable Style/RescueModifier
      @next_id = 0
    end

    attr_reader :errors

    # The result of `method`; an error reply raises Crashed with it.
    def call(method, params = nil)
      id = (@next_id += 1)
      message = { 'jsonrpc' => '2.0', 'id' => id, 'method' => method }
      message['params'] = params if params
      write(message)
      loop do
        reply = read
        next unless reply.key?('id') && reply['id'] == id # a notification, or a request of the server's

        raise Crashed, "#{method}: #{reply['error'].to_json}" if reply.key?('error')

        return reply['result']
      end
    end

    def notify(method, params = nil)
      message = { 'jsonrpc' => '2.0', 'method' => method }
      message['params'] = params if params
      write(message)
    end

    def close
      @stdin.close unless @stdin.closed?
      Process.kill('TERM', @thread.pid) if @thread.alive? && !@thread.join(5)
      @thread.join
      @drain.join(1)
    rescue Errno::ESRCH, IOError
      nil
    end

    private

    def write(message)
      @stdin.write("#{JSON.generate(message)}\n")
    rescue Errno::EPIPE, IOError
      raise Crashed, 'the command ended'
    end

    def read
      ready = IO.select([@stdout], nil, nil, @timeout)
      raise Crashed, "no reply in #{@timeout} s" unless ready

      line = @stdout.gets
      raise Crashed, 'the command ended' unless line

      JSON.parse(line)
    rescue JSON::ParserError => e
      raise Crashed, "not a JSON-RPC message: #{e.message}"
    end
  end

  # ---- matching ------------------------------------------------------------------

  # Whether `actual` is what `expected` says (docs/mcp.md, "Expectations"):
  # a map's keys a subset, a list element by element and of the same
  # length, a scalar equal, and the matchers {"$gt": n}, {"$ge": n},
  # {"$length": n}, {"$type": name}, {"$match": regex}, {"$any": true}.
  # Raises Mismatch naming the path that differs.
  def self.match(expected, actual, path = '')
    if expected.is_a?(Hash) && expected.keys.any? { |key| key.start_with?('$') }
      matcher(expected, actual, path)
    elsif expected.is_a?(Hash)
      raise Mismatch, "#{path}: #{actual.to_json} is not a map" unless actual.is_a?(Hash)

      expected.each do |key, value|
        raise Mismatch, "#{path}.#{key}: missing from #{actual.keys.to_json}" unless actual.key?(key)

        match(value, actual[key], "#{path}.#{key}")
      end
    elsif expected.is_a?(Array)
      raise Mismatch, "#{path}: #{actual.to_json} is not a list" unless actual.is_a?(Array)
      raise Mismatch, "#{path}: #{actual.length} items, not #{expected.length}: #{actual.to_json}" unless actual.length == expected.length

      expected.each_with_index { |value, i| match(value, actual[i], "#{path}[#{i}]") }
    elsif expected != actual
      raise Mismatch, "#{path}: #{actual.to_json}, not #{expected.to_json}"
    end
  end

  TYPES = { 'string' => String, 'number' => Numeric, 'integer' => Integer, 'list' => Array, 'map' => Hash }.freeze

  def self.matcher(expected, actual, path)
    expected.each do |name, value|
      ok = case name
           when '$any' then true
           when '$gt' then actual.is_a?(Numeric) && actual > value
           when '$ge' then actual.is_a?(Numeric) && actual >= value
           when '$length' then actual.respond_to?(:length) && actual.length == value
           when '$type' then actual.is_a?(TYPES.fetch(value))
           when '$match' then actual.is_a?(String) && actual.match?(Regexp.new(value))
           else raise Mismatch, "#{path}: no matcher #{name}"
           end
      raise Mismatch, "#{path}: #{actual.to_json} is not #{name} #{value.to_json}" unless ok
    end
  end

  # The value at `path` ("a.b[0].c") of `value`.
  def self.dig(value, path)
    path.scan(/[^.\[\]]+/).reduce(value) do |at, key|
      at.is_a?(Array) ? at.fetch(Integer(key)) : at.fetch(key)
    end
  rescue KeyError, IndexError, TypeError, ArgumentError
    raise Mismatch, "nothing at #{path} to save"
  end

  # `value` with each string "$name" replaced by the saved value, and each
  # "${name}" inside a string by its text.
  def self.substitute(value, saved)
    case value
    when Hash then value.transform_values { |item| substitute(item, saved) }
    when Array then value.map { |item| substitute(item, saved) }
    when /\A\$(\w+)\z/ then saved.fetch(Regexp.last_match(1)) { raise Mismatch, "nothing saved as #{value}" }
    when String then value.gsub(/\$\{(\w+)\}/) { saved.fetch(Regexp.last_match(1)).to_s }
    else value
    end
  end

  # ---- a case ---------------------------------------------------------------------

  # One case of the suite: its fixture opened on a new server, MCP's
  # handshake, then each step. Raises Mismatch or Crashed.
  def self.run_case(command, kase, timeout:, suite: SUITE)
    fixture = JSON.parse(File.read(File.join(suite, 'fixtures', "#{kase.fetch('fixture')}.json")))
    fixture['options'] = fixture.fetch('options', {}).merge(kase.fetch('options', {}))
    server = Server.new(command, timeout: timeout)
    begin
      server.call('fixture/open', fixture)
      server.call('initialize', { 'protocolVersion' => PROTOCOL, 'capabilities' => {},
                                  'clientInfo' => { 'name' => 'tinyactors-test', 'version' => '0' } })
      server.notify('notifications/initialized')
      saved = {}
      kase.fetch('steps').each_with_index { |step, i| run_step(server, step, saved, "step #{i + 1}") }
    rescue Mismatch, Crashed => e
      raise e.class, "#{e.message}#{server.errors.empty? ? '' : "\n  its standard error:\n#{server.errors.gsub(/^/, '    ')}"}"
    ensure
      server.close
    end
  end

  def self.run_step(server, step, saved, where)
    if step.key?('fixture')
      result = server.call("fixture/#{step['fixture']}", substitute(step.fetch('params', {}), saved))
      step.fetch('save', {}).each { |name, path| saved[name] = dig(result, path) }
    elsif step.key?('tools')
      tools_step(server.call('tools/list')['tools'], step['tools'], where)
    else
      call_step(server, step, saved, where)
    end
  end

  # tools/list: the catalog's tools, each as the catalog writes it.
  def self.tools_step(listed, expected, where)
    raise Mismatch, "#{where}: tools: no expectation but 'catalog'" unless expected == 'catalog'

    catalog = JSON.parse(File.read(CATALOG))['tools']
    by_name = listed.to_h { |tool| [tool['name'], tool] }
    names = catalog.map { |tool| tool['name'] }
    raise Mismatch, "#{where}: tools #{by_name.keys.sort.to_json}, not #{names.sort.to_json}" unless by_name.keys.sort == names.sort

    catalog.each do |tool|
      %w[description inputSchema].each do |key|
        match(tool[key], by_name[tool['name']][key], "#{where}: #{tool['name']}.#{key}")
      end
      match(tool['annotations'], by_name[tool['name']].fetch('annotations', {}), "#{where}: #{tool['name']}.annotations")
    end
  end

  def self.call_step(server, step, saved, where)
    name = step.fetch('call')
    result = server.call('tools/call', { 'name' => name, 'arguments' => substitute(step.fetch('arguments', {}), saved) })
    label = "#{where} (#{name})"
    error = result['isError'] == true
    text = (result['content'] || []).map { |block| block['text'] }.join("\n")
    if step['isError']
      raise Mismatch, "#{label}: no error: #{text}" unless error
      raise Mismatch, "#{label}: the error #{text.inspect} does not match /#{step['error']}/" if step['error'] && !text.match?(Regexp.new(step['error']))
    elsif error
      raise Mismatch, "#{label}: an error: #{text}"
    end
    match(step['expect'], result['structuredContent'], label) if step.key?('expect')
    match(step['text'], text, "#{label} text") if step.key?('text')
    step.fetch('save', {}).each { |key, path| saved[key] = dig(result['structuredContent'], path) }
  rescue Mismatch => e
    raise Mismatch, e.message.start_with?(where) ? e.message : "#{label}#{e.message}"
  end

  # Every case of the suite (`only`: the named ones), each printed with
  # pass or fail; true when every case passed.
  def self.run(command, out: $stdout, only: [], timeout: 60, suite: SUITE)
    cases = Dir[File.join(suite, 'cases', '*.json')].sort.map { |path| JSON.parse(File.read(path)) }
    cases.select! { |kase| only.include?(kase['name']) } unless only.empty?
    failed = 0
    cases.each do |kase|
      run_case(command, kase, timeout: timeout, suite: suite)
      out.puts "pass mcp #{kase['name']}"
    rescue Mismatch, Crashed => e
      failed += 1
      out.puts "fail mcp #{kase['name']}: #{e.message}"
    end
    out.puts "mcp: #{cases.length - failed} of #{cases.length} cases passed"
    !cases.empty? && failed.zero?
  end
end
