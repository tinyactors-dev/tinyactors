# frozen_string_literal: true

# The conformance suite's adapter for `tinyactors run`: it speaks the driver
# protocol (../../../tinyactors-test/docs/driver.md) to the suite's runner
# on its standard input and output, and the run protocol (docs/cli.md,
# "Run") to a `tinyactors run` server it starts for each host. It is a
# client of the server's JSON-RPC, as an agent or a CI job is: every driver
# method is `run.*` calls and the handling of `effect.*` notifications;
# nothing reaches core but through the server.
#
#   ruby tests/conform/run_adapter.rb <tinyactors>
#
# Run by `mise run run:conform` with `--kind binding`: it offers every
# driver method but the C-level `value.encode`. The frames of the driver
# protocol are CBOR, read and written with the suite's codec
# (tinyactors-test/lib/cbor.rb); the server's lines are JSON (the standard
# library's). A value crosses to the server as CBOR (`cbor`, `paramsCBOR`,
# `run.post`'s `message`), so the bytes the runner compares are the ones
# core takes and gives; a value coming back is the `cbor` twin when the
# server writes one, else its JSON as CBOR again (5.1 of the proposal: JSON
# keeps integers, floats, text, arrays and maps as they were). Standard
# output holds frames only; the servers' standard error is passed on.

require 'fileutils'
require 'json'
require 'open3'
require 'tmpdir'
require_relative '../../../tinyactors-test/lib/cbor'

# The driver's statuses (tinyactors/types.h) by the names the server's
# errors carry in `data.status`.
STATUS = {
  'TA_OK' => 0, 'TA_ERROR_EXECUTION' => -1, 'TA_ERROR_COMMUNICATION' => -2, 'TA_ERROR_INVALID' => -3,
  'TA_ERROR_NO_MEMORY' => -4, 'TA_ERROR_STALE' => -5, 'TA_ERROR_STATE' => -6, 'TA_ERROR_UNSUPPORTED' => -7,
  'TA_ERROR_FATAL' => -8, 'TA_ERROR_ABORTED' => -9, 'TA_ERROR_LIMIT' => -10
}.freeze
# The server's error codes (docs/cli.md, "Errors") as statuses, for an
# error without `data.status`: data the datamodel cannot read (-32006) and
# a param the server refuses (-32602) are what core calls TA_ERROR_INVALID.
CODES = { -32_001 => -5, -32_002 => -6, -32_003 => -10, -32_004 => -4, -32_005 => -3, -32_006 => -3,
          -32_007 => -7, -32_602 => -3 }.freeze
# run.turn's statuses as ta_run's (tinyactors/system.h).
RUNS = { 'idle' => 0, 'yielded' => 1, 'exhausted' => 3, 'fault' => 4, 'held' => 5 }.freeze

BASIC_HTTP = 'http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor'
LOCATION = 'http://localhost:8080/scxml'

# A driver error (docs/driver.md, "Frames and messages").
class DriverError < StandardError
  attr_reader :code, :reason, :path

  def initialize(code, reason, path, message)
    super(message)
    @code = code
    @reason = reason
    @path = path
  end
end

# An error a server answered.
class ServerError < StandardError
  attr_reader :code, :data

  def initialize(error)
    @code = error['code']
    @data = error['data'].is_a?(Hash) ? error['data'] : {}
    super("#{@code} #{@data['reason']} #{@data['path']}: #{error['message']}")
  end

  # The driver's status for it: the C call's, or the code's.
  def status = STATUS.fetch(@data['status'].to_s) { CODES.fetch(@code, STATUS['TA_ERROR_INVALID']) }
end

# The server ended: its exit status and the lines it wrote on standard error.
class ServerEnded < StandardError; end

def base64url(bytes) = [bytes.b].pack('m0').tr('+/', '-_').delete('=')

def unbase64url(text)
  text = text.tr('-_', '+/')
  (text + ('=' * ((4 - (text.length % 4)) % 4))).unpack1('m0')
end

# A JSON value read from the server as a CBOR value (CBOR.encode's): 6.2's
# reading kept integers integers and floats floats.
def from_json(value)
  case value
  when Hash then value.to_h { |key, each| [key, from_json(each)] }
  when Array then value.map { |each| from_json(each) }
  else value
  end
end

# The CBOR of a value field and its twin: the twin when the server wrote one.
def value_bytes(holder, field = 'data')
  return unbase64url(holder['cbor']) if holder.key?('cbor')
  return nil unless holder.key?(field)

  CBOR.encode(from_json(holder[field]))
end

# A CBOR value (CBOR.value's) as JSON, for the config: what JSON cannot hold
# is refused.
def to_json_value(value, path)
  case value
  when Hash
    value.to_h do |key, each|
      raise DriverError.new(-32_602, 'type', "params.config#{path}", 'a key that is not text') unless key.is_a?(String)

      [key, to_json_value(each, "#{path}.#{key}")]
    end
  when Array then value.each_with_index.map { |each, index| to_json_value(each, "#{path}[#{index}]") }
  when Integer, String, true, false, nil then value
  when Float
    return value if value.finite? && value != value.round

    raise DriverError.new(-32_602, 'type', "params.config#{path}", 'a float JSON reads as an integer, or no number')
  else raise DriverError.new(-32_602, 'type', "params.config#{path}", "#{value.class}: JSON has no such value")
  end
end

# One `tinyactors run` process: requests written as lines, each answered
# after the notifications it caused, which are kept in `notifications` in
# the order they came.
class Server
  def initialize(command, chdir:)
    @stdin, @stdout, stderr, @wait = Open3.popen3(*command, chdir: chdir)
    @stderr = []
    @stderr_thread = Thread.new do
      stderr.each_line do |line|
        @stderr << line
        $stderr.write("tinyactors run: #{line}")
      end
    end
    @next_id = 0
    @notifications = []
  end

  # The result of a request; ServerError for its error, ServerEnded when the
  # server is gone.
  def call(method, params = nil)
    request = { 'jsonrpc' => '2.0', 'id' => (@next_id += 1), 'method' => method }
    request['params'] = params if params
    begin
      @stdin.write("#{JSON.generate(request)}\n")
      @stdin.flush
    rescue Errno::EPIPE, IOError
      raise ServerEnded, ended
    end
    loop do
      line = @stdout.gets or raise ServerEnded, ended
      message = JSON.parse(line)
      next @notifications << message if message.key?('method')
      raise ServerEnded, "a response to #{message['id'].inspect}, not #{@next_id}" unless message['id'] == @next_id
      raise ServerError, message['error'] if message.key?('error')

      return message['result']
    end
  end

  # The notifications so far, handed out once.
  def take = @notifications.slice!(0..)

  def close
    @stdin.close unless @stdin.closed?
    @stdout.each_line { |line| @notifications << JSON.parse(line) } unless @stdout.closed?
    @stdout.close unless @stdout.closed?
    @wait.join(10) or Process.kill('KILL', @wait.pid)
    @stderr_thread.join(1)
  rescue Errno::ESRCH, IOError, JSON::ParserError
    nil
  end

  # What the server said when it ended at startup: its exit status and
  # standard error.
  def ended
    @wait.join(5)
    @stderr_thread.join(1)
    status = @wait.alive? ? nil : @wait.value
    "the server ended#{status ? " (exit #{status.exitstatus})" : ''}"
  end

  def stderr_lines
    @wait.join(5)
    @stderr_thread.join(1)
    @stderr
  end

end

# What every host shares: the server command, a directory of its own for
# its files, and starting it.
class Base
  def initialize(command, config, options)
    @directory = Dir.mktmpdir('run-conform')
    File.write(File.join(@directory, 'config.json'), JSON.generate(config))
    @server = Server.new([*command, 'run', '--config', 'config.json', *options], chdir: @directory)
  end

  def close
    @server&.close
    FileUtils.rm_rf(@directory)
  end

  # A server that is up answers a lookup; one that refused its config at
  # startup has ended.
  def started? = !@server.call('run.actorByName', 'name' => 'run-conform').nil?
end

# The driver's datamodel and realm as the server's options: quickjs only.
def engine(params)
  datamodel = params.fetch('datamodel', 'quickjs')
  raise DriverError.new(-32_602, 'range', 'params.datamodel', "no datamodel #{datamodel}: the server runs quickjs") unless datamodel == 'quickjs'

  params['realm'] ? ['--realm', params['realm']] : []
end

def required(params, name)
  params.fetch(name) { raise DriverError.new(-32_602, 'missing', "params.#{name}", "params.#{name}: missing") }
end

# ---- the protocol vectors' host ------------------------------------------------------------

# host.create to host.free: one server, its config the driver's (its
# ioProcessors a map), with no I/O processor served but by failing its sends.
class VectorHost < Base
  def initialize(command, config, options)
    super(command, VectorHost.config_of(config), options)
    @sends = []
  end

  # The config as the protocol takes it: core's, but ioProcessors a map of
  # each type to its location. A config whose shape a map cannot hold (not
  # a map, an entry that is not one with a type) is a reference-only case.
  def self.config_of(config)
    raise DriverError.new(-32_602, 'type', 'params.config', 'a config that is not a map (a reference-only case)') unless config.is_a?(Hash)

    json = to_json_value(config, '')
    entries = json['ioProcessors']
    return json if entries.nil?

    cannot = DriverError.new(-32_602, 'type', 'params.config.ioProcessors', 'I/O processors a map cannot hold (a reference-only case)')
    raise cannot unless entries.is_a?(Array)

    json['ioProcessors'] = entries.to_h do |entry|
      raise cannot unless entry.is_a?(Hash) && entry['type'].is_a?(String) && (entry.keys - %w[type location]).empty?

      [entry['type'], entry.slice('location')]
    end
    raise cannot unless json['ioProcessors'].size == entries.size

    json
  end

  # The config's refusal as core wrote it, from the line the server wrote
  # on standard error before it exited 1; nil when it started.
  def refused
    started?
    nil
  rescue ServerEnded
    line = @server.stderr_lines.find { |each| each.include?('the config is refused: ') }
    raise DriverError.new(-32_603, 'internal', '', "the server ended: #{@server.stderr_lines.join}") unless line

    CBOR.encode(from_json(JSON.parse(line.split('the config is refused: ', 2).last)))
  end

  def post(message)
    result = @server.call('run.post', 'message' => base64url(message))
    handle(@server.take)
    answer = { 'status' => 0, 'notifications' => [] }
    answer['reply'] = CBOR::Bytes.new(unbase64url(result['reply'])) if result.key?('reply')
    answer
  rescue ServerError => e
    { 'status' => e.status }
  end

  def image(charts)
    sources = charts.map { |chart| { 'name' => chart['name'], 'text' => chart['text'] } }
    result = @server.call('run.addImage', 'sources' => sources)
    { 'status' => 0, 'segment' => result['segment'] }
  rescue ServerError => e
    { 'status' => e.status }
  end

  def spawn(params)
    request = { 'chart' => required(params, 'chart') }
    request['paramsCBOR'] = base64url(CBOR.encode(params['params'])) unless params['params'].nil?
    session = @server.call('run.spawn', request)['session']
    handle(@server.take)
    { 'status' => 0, 'session' => session }
  rescue ServerError => e
    { 'status' => e.status }
  end

  def send_event(params)
    request = { 'target' => required(params, 'session'), 'event' => required(params, 'event') }
    if params.key?('cbor') then request['cbor'] = base64url(params['cbor'].data)
    elsif params.key?('data') then request['cbor'] = base64url(CBOR.encode(params['data']))
    end
    request['origin'] = params['origin'] if params['origin']
    @server.call('run.send', request)
    handle(@server.take)
    { 'status' => 0 }
  rescue ServerError => e
    { 'status' => e.status }
  end

  def advance(params)
    request = { 'to' => params.fetch('monotonic', 0) }
    request['wallNs'] = params['wallNs'] if params['wallNs']
    @server.call('run.advanceTime', request)
    {}
  end

  # run.turn until idle with nothing answered; the sends to the transport,
  # and the run's status. A batch with waiting sends is held, so that a
  # send to an I/O processor is failed before its sender goes on, as a
  # binding's processor fails it in its handler.
  def run
    status = 0
    1000.times do
      result = @server.call('run.turn', 'hold' => true)
      status = RUNS.fetch(result['status'], 4)
      $stderr.write("run-conform: the run ended #{result['status']} #{result['detail']}\n") if status.between?(3, 4)
      given = handle(@server.take)
      next if result['status'] == 'held'
      break unless given && status.zero?
    end
    sends = @sends
    @sends = []
    { 'status' => status, 'sends' => sends, 'notifications' => [] }
  end

  private

  # The effects: a send to the transport kept for host.run, one to an I/O
  # processor failed (nobody serves it), an invocation failed (no service).
  # True when the host was given an input.
  def handle(notifications)
    given = false
    notifications.each do |notification|
      params = notification['params']
      case notification['method']
      when 'effect.send'
        if params['message']
          message = params['message']
          send = { 'target' => message['target'].to_s, 'event' => message['event'].to_s, 'origin' => message['origin'].to_s }
          data = value_bytes(message)
          send['data'] = CBOR::Bytes.new(data) if data
          @sends << send
        elsif params['type']
          given |= fail_send(params)
        end
      when 'effect.invoke'
        given |= quietly { @server.call('run.fail', 'target' => params['session'], 'invocation' => params['invocation']) }
      end
    end
    given
  end

  def fail_send(params) = quietly { @server.call('run.fail', 'target' => params['session'], 'sendid' => params['sendid'].to_s) }

  # True when the call was taken; a refusal (the sender ended) is told.
  def quietly
    yield
    true
  rescue ServerError => e
    $stderr.write("run-conform: #{e.message}\n")
    false
  end
end

# ---- scenarios and W3C ---------------------------------------------------------------------

# A host for one scenario or W3C test, its documents written to its
# directory so that a `src` names one by its name (driver.md, "src"): each
# under its name, and under `file:` and its name, which a `src` may write
# (the server's resolver reads a src relative to the file that names it).
class Outcome < Base
  def initialize(command, config, options, documents)
    super(command, config, options)
    documents.each do |document|
      [document['name'], "file:#{document['name']}"].each do |name|
        File.write(File.join(@directory, name), document['text'])
      end
    end
    @outcome = nil
  end

  private

  # The first Outcome log decides (driver.md, "The scenario environment").
  def judge(params)
    return unless @outcome.nil? && params['label'] == 'Outcome'

    @outcome = params['text'] == 'pass' ? 'pass' : 'fail'
  end

  # The image of documents named: their files, compiled by the server;
  # false when they do not compile.
  def compile(names)
    @server.call('run.addImage', 'files' => names)['segment']
  rescue ServerError => e
    $stderr.write("run-conform: a document does not compile: #{e.message}\n")
    raise unless e.code == -32_005

    @outcome ||= 'compile'
    false
  end
end

# scenario.run (driver.md, "The scenario environment"): a host of its own on
# the simulated clock, each step's calls, then run.turn until the host is
# idle at the current time, WAKEs answered from the store, the migration's
# end awaited.
class Scenario < Outcome
  CONFIG = { 'systemID' => 'conformance', 'system' => { 'ticksPerSecond' => 1000 } }.freeze

  # A step the host refused: the scenario fails.
  class Refused < StandardError; end

  def initialize(command, params)
    super(command, CONFIG, engine(params), required(params, 'documents'))
    @aliases = {}
    @store = {}
    @snapshots = {}
    @migrations_done = 0
  end

  def run(steps)
    steps.each do |step|
      begin
        step(step)
        settle unless @outcome
      rescue Refused, ServerError => e
        $stderr.write("run-conform: the step #{step.inspect} was refused: #{e.message}\n")
        @outcome ||= 'fail'
      end
      break if @outcome
    end
    @outcome || 'timeout'
  end

  private

  def step(step)
    if step.key?('image')
      compile(step['image'])
    elsif step.key?('spawn')
      request = { 'chart' => step['chart'], 'name' => step['spawn'] }
      request['paramsCBOR'] = value(step['params']) if step.key?('params')
      @aliases[step['spawn']] = @server.call('run.spawn', request)['session']
    elsif step.key?('send')
      @server.call('run.send', event(step['send'], step))
    elsif step.key?('sendMany')
      events = step['sendMany'].map { |each| event(each['to'], each) }
      accepted = @server.call('run.sendMany', 'events' => events)['accepted']
      raise Refused, "sendMany took #{accepted} of #{events.size}" unless accepted == events.size
    elsif step.key?('advance')
      @server.call('run.advanceTime', 'by' => step['advance'])
    elsif step.key?('hibernate')
      session = session(step['hibernate'])
      @store[session] = @server.call('run.hibernate', 'session' => session)['blob']
    elsif step.key?('snapshot')
      @snapshots[step['snapshot']] = @server.call('run.snapshot')['snapshot']
    elsif step.key?('restore')
      snapshot = @snapshots.fetch(step['restore']) { raise Refused, "no snapshot #{step['restore']}" }
      @server.call('run.restore', 'snapshot' => snapshot)
    elsif step.key?('migrate')
      migrate(step['migrate'])
    else
      raise Refused, "a step this adapter does not know: #{step.keys.join(', ')}"
    end
    handle(@server.take)
  end

  def migrate(migrate)
    request = { 'wake' => migrate['wake'] ? true : false }
    request['timeout'] = migrate['timeout'] if migrate['timeout']
    done = @migrations_done
    @server.call('run.migrate', request)
    handle(@server.take)
    100.times do
      break if @migrations_done > done || @outcome

      settle
    end
    raise Refused, 'the migration did not end' unless @migrations_done > done || @outcome
  end

  def event(alias_name, step)
    request = { 'target' => session(alias_name), 'event' => step['event'] }
    request['cbor'] = value(step['data']) if step.key?('data')
    request
  end

  def session(alias_name) = @aliases.fetch(alias_name) { raise Refused, "no actor #{alias_name}" }

  # A step's value, "@alias" at any depth that actor's address, as CBOR.
  def value(value) = base64url(CBOR.encode(addresses(value)))

  def addresses(value)
    case value
    when String
      value.start_with?('@') && @aliases.key?(value[1..]) ? "#_scxml_#{@aliases[value[1..]]}" : value
    when Array then value.map { |each| addresses(each) }
    when Hash then value.to_h { |key, each| [key, addresses(each)] }
    else value
    end
  end

  # run.turn until the host is idle with nothing left to answer, at most
  # 1,000 turns; stops at the first Outcome.
  def settle
    1000.times do
      result = @server.call('run.turn')
      given = handle(@server.take)
      return if @outcome

      case result['status']
      when 'idle' then return unless given
      when 'exhausted', 'fault'
        $stderr.write("run-conform: the run ended #{result['status']} #{result['detail']}\n")
        return
      end
    end
  end

  # True when the host was given an input.
  def handle(notifications)
    given = false
    notifications.each do |notification|
      params = notification['params']
      case notification['method']
      when 'effect.log' then judge(params)
      when 'effect.wake'
        blob = @store.delete(params['session'])
        next unless blob

        @server.call('run.wake', 'session' => params['session'], 'blob' => blob)
        given = true
      when 'effect.migrationDone' then @migrations_done += 1
      when 'effect.invoke'
        @server.call('run.fail', 'target' => params['session'], 'invocation' => params['invocation'])
        given = true
      end
    end
    given
  end
end

# w3c.run (driver.md, "The W3C environment"): the start chart booted, a
# send to BasicHTTP written as a POST and read back for its sender (core's
# codec, tinyactors-core/src/basic_http/basic_http.c, which the server does
# not offer: written again here), the clock moved to the next deadline
# whenever the host is idle.
class W3C < Outcome
  CONFIG = { 'systemID' => 'w3c', 'system' => { 'ticksPerSecond' => 1000, 'maxEvents' => 1024, 'maxActors' => 64 },
             'ioProcessors' => { BASIC_HTTP => { 'location' => LOCATION } } }.freeze
  ROUNDS = 200
  BUDGET = 20_000

  def initialize(command, params)
    @documents = required(params, 'documents')
    super(command, CONFIG, engine(params), @documents)
  end

  def run
    charts = @documents.each_with_index.select { |document, index| index.zero? || document['name'].end_with?('.scxml') }
    segment = compile(charts.map { |document, _| document['name'] })
    return @outcome unless segment

    @server.call('run.boot', 'segment' => segment)
    @posts = []
    ROUNDS.times do
      # held: the senders wait for their answers, given before the next turn acks
      result = @server.call('run.turn', 'budget' => BUDGET, 'hold' => true)
      @server.take.each { |notification| handle(notification) }
      break if @outcome

      if %w[exhausted fault].include?(result['status'])
        $stderr.write("run-conform: the run ended #{result['status']} #{result['detail']}\n")
        break
      end
      posts = @posts
      @posts = []
      posts.each { |post| deliver(post) }
      next unless posts.empty? && result['status'] == 'idle'
      break if result['nextDeadline'].nil?

      @server.call('run.advanceTime', 'to' => result['nextDeadline'])
    end
    @outcome || 'timeout'
  rescue ServerError => e
    $stderr.write("run-conform: the test's host refused it: #{e.message}\n")
    @outcome || 'fail'
  end

  private

  def handle(notification)
    params = notification['params']
    case notification['method']
    when 'effect.log' then judge(params)
    when 'effect.send' then @posts << post(params) if params['type'] == BASIC_HTTP
    end
  end

  # A send to BasicHTTP as a POST: its form, or nil when it cannot be
  # written (a target that is not the location, one without a host).
  def post(params)
    target = params['target'].to_s
    message = nil
    if target.start_with?(LOCATION)
      fields, body = form(value_bytes(params))
      message = encode_post(target, params['event'].to_s, fields, body)
    end
    { 'session' => params['session'], 'sendid' => params['sendid'].to_s, 'message' => message }
  end

  # The fields of data that is a map (each value as text: text as it is,
  # anything else in diagnostic notation), or the body of data that is text.
  def form(bytes)
    return [[], ''] unless bytes

    item = CBOR.decode(bytes)
    return [[], CBOR.value(item)] if item.major == 3
    return [[], ''] unless item.major == 5

    fields = item.children.each_slice(2).map do |key, value|
      [CBOR.value(key).to_s, value.major == 3 ? CBOR.value(value) : CBOR.diagnostic(value)]
    end
    [fields, '']
  end

  def encoded(text) = text.b.gsub(/[^A-Za-z0-9\-_.~]/n) { |byte| format('%%%02X', byte.ord) }

  # ta_http_encode_post.
  def encode_post(url, event, fields, body)
    rest = url.delete_prefix('http://')
    host, slash, path = rest.partition('/')
    return nil if host.empty?

    path = slash.empty? ? '/' : "/#{path}"
    content = if fields.empty? && event.empty? then encoded(body)
              else
                pairs = event.empty? ? [] : ["_scxmleventname=#{encoded(event)}"]
                (pairs + fields.map { |name, value| "#{encoded(name)}=#{encoded(value)}" }).join('&')
              end
    "POST #{path} HTTP/1.1\r\nHost: #{host}\r\nContent-Type: application/x-www-form-urlencoded\r\n" \
      "Content-Length: #{content.bytesize}\r\n\r\n#{content}"
  end

  def decoded(text) = text.tr('+', ' ').b.gsub(/%([0-9A-Fa-f]{2})/) { Regexp.last_match(1).hex.chr }.force_encoding('UTF-8')

  # ta_http_parse_request: the event and the form fields of a POST, and the
  # sender given them, with the location as origin and the message as raw;
  # error.communication for a send that could not be written.
  def deliver(post)
    unless post['message']
      @server.call('run.fail', 'target' => post['session'], 'sendid' => post['sendid'])
      return
    end
    message = post['message']
    body = message.split("\r\n\r\n", 2).last
    event = nil
    data = {}
    if body.include?('=')
      body.split('&', -1).each do |pair|
        name, value = pair.split('=', 2)
        name = decoded(name.to_s)
        value = decoded(value.to_s)
        if name == '_scxmleventname' then event = value
        elsif !name.empty? then data[name] = value
        end
      end
    end
    event = 'HTTP.POST' if event.nil? || event.empty?
    @server.call('run.send', 'target' => post['session'], 'event' => event, 'data' => data, 'origin' => LOCATION,
                             'origintype' => BASIC_HTTP, 'raw' => message)
  rescue ServerError => e
    $stderr.write("run-conform: the POST's answer was refused: #{e.message}\n")
  end
end

# ---- the driver ----------------------------------------------------------------------------

# The driver's methods over one server per host (docs/driver.md, "Methods").
class Adapter
  OFFERED = %w[host.create host.free host.post image.add actor.spawn actor.send clock.advance host.run scenario.run
               w3c.run].freeze

  def initialize(command)
    @command = command
    @host = nil
  end

  def close = @host&.close

  def answer(method, params)
    case method
    when 'host.create' then create(params)
    when 'host.free'
      need_host.close
      @host = nil
      {}
    when 'host.post' then need_host.post(required(params, 'message').data)
    when 'image.add' then need_host.image(required(params, 'charts'))
    when 'actor.spawn' then need_host.spawn(params)
    when 'actor.send' then need_host.send_event(params)
    when 'clock.advance' then need_host.advance(params)
    when 'host.run' then need_host.run
    when 'scenario.run' then outcome(Scenario.new(@command, params)) { |run| run.run(required(params, 'steps')) }
    when 'w3c.run' then outcome(W3C.new(@command, params), &:run)
    end
  end

  private

  def need_host = @host || raise(DriverError.new(-32_002, 'state', '', 'no host: call host.create first'))

  def create(params)
    raise DriverError.new(-32_002, 'state', '', 'a host already: call host.free first') if @host

    options = engine(params)
    config = CBOR.load(required(params, 'config').data)
    host = VectorHost.new(@command, config, options)
    refusal = host.refused
    if refusal
      host.close
      return { 'status' => STATUS['TA_ERROR_INVALID'], 'error' => CBOR::Bytes.new(refusal) }
    end
    @host = host
    { 'status' => 0, 'datamodel' => 'ecmascript' }
  end

  def outcome(run)
    { 'outcome' => yield(run) }
  rescue ServerEnded => e
    $stderr.write("run-conform: #{e.message}\n")
    { 'outcome' => 'fail' }
  ensure
    run.close
  end
end

def failure(id, error)
  { 'id' => id, 'error' => { 'code' => error.code, 'message' => error.message,
                             'data' => { 'reason' => error.reason, 'path' => error.path } } }
end

def reply(adapter, frame)
  call = begin
    CBOR.load(frame)
  rescue CBOR::Error => e
    return failure(nil, DriverError.new(-32_700, 'malformed', '', "the frame is not CBOR: #{e.message}"))
  end
  unless call.is_a?(Hash) && call['id'].is_a?(Integer)
    return failure(nil, DriverError.new(-32_600, 'type', '', 'not a call: a map with an integer id'))
  end
  unless Adapter::OFFERED.include?(call['method'])
    return failure(call['id'], DriverError.new(-32_601, 'unknown', 'method', "#{call['method']} is not offered"))
  end

  params = call['params'].is_a?(Hash) ? call['params'] : {}
  { 'id' => call['id'], 'result' => adapter.answer(call['method'], params) }
rescue DriverError => e
  failure(call['id'], e)
rescue ServerError, ServerEnded => e
  failure(call['id'], DriverError.new(-32_603, 'internal', '', "the server: #{e.message}"))
end

abort 'usage: run_adapter.rb <tinyactors>' unless ARGV.size == 1
command = [File.expand_path(ARGV.first)]
$stdin.binmode
$stdout.binmode
adapter = Adapter.new(command)
begin
  while (header = $stdin.read(4))
    raise 'the input ended inside a frame' unless header.bytesize == 4

    length = header.unpack1('N')
    frame = $stdin.read(length)
    raise 'the input ended inside a frame' unless frame && frame.bytesize == length

    body = CBOR.encode(reply(adapter, frame))
    $stdout.write([body.bytesize].pack('N') + body)
    $stdout.flush
  end
ensure
  adapter.close
end
