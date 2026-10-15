# frozen_string_literal: true

require 'pathname'
require_relative 'driver'
require_relative 'milestones'
require_relative 'scenarios'
require_relative 'vectors'
require_relative 'w3c'

# Runs the suites against one implementation through docs/driver.md and
# reports each case: pass, fail (with why) or skip (a method the command
# does not offer). Nothing here schedules or interprets charts: the
# implementation runs them and answers outcomes; the runner compares only
# the protocol vectors' bytes.
class Runner
  SUITES = %w[protocol scenarios w3c].freeze
  PROTOCOL = Pathname.new(__dir__).join('../suites/protocol').expand_path

  # Statuses of tinyactors-core/include/tinyactors/types.h.
  OK = 0
  INVALID = -3
  NO_MEMORY = -4

  # The echo setup's sender in another system, and its remote I/O processor
  # (suites/protocol/README.md, "Setups").
  ECHO_ORIGIN = 'tinyactors://p/#_scxml_9'
  REMOTE = 'urn:tinyactors:test:remote'

  # The wall clock a setup's runs see (suites/protocol/README.md, "Setups").
  WALL_NS = 1_700_000_000_000_000_000

  # The setups' system ids (suites/protocol/README.md, "Setups").
  SYSTEMS = { 'empty' => 'h', 'pinged' => 'h', 'pinged2' => 'h' }.freeze

  # The methods of docs/driver.md, and those each kind of implementation
  # must offer (docs/driver.md, "Kinds"): the reference all of them, a
  # binding all but the C-level ones, which call core below the API a
  # binding exposes (it sends a binding no `refuse` either).
  METHODS = %w[scenario.run w3c.run host.create host.free host.post image.add actor.spawn actor.send
               clock.advance host.run value.encode].freeze
  C_LEVEL = %w[value.encode].freeze
  REQUIRED = { 'reference' => METHODS, 'binding' => METHODS - C_LEVEL }.freeze

  Result = Struct.new(:suite, :name, :verdict, :detail)

  # A case that cannot pass, and why.
  class Failed < StandardError; end

  # A case that is not run here, and why.
  class Skipped < StandardError; end

  attr_reader :results

  # `kind`: "reference" or "binding", the methods the command must offer
  # (REQUIRED). `scenarios`: the templates and steps; `documents`: their
  # conversion (default: converted now for the datamodel's language);
  # `w3c`: the converted W3C suite and `setups` the setups' templates
  # (default: likewise); `skip`: files of W3C test ids and reasons skipped
  # besides suites/w3c/skip.txt and the language's skip list; `only`: a
  # Milestones::Selection, the vector files and scenarios a milestone runs
  # (default: all of them).
  def initialize(command, datamodel: 'quickjs', realm: nil, kind: 'binding', timeout: 60, out: $stdout, chdir: Dir.pwd,
                 protocol: PROTOCOL, scenarios: Scenarios::ROOT, documents: nil, w3c: nil, setups: nil, skip: [],
                 only: nil)
    raise ArgumentError, "kind #{kind.inspect}: #{REQUIRED.keys.join(' or ')}" unless REQUIRED.key?(kind)

    @driver = Driver.new(command, timeout: timeout, chdir: chdir)
    @datamodel = datamodel
    @realm = realm
    @kind = kind
    @out = out
    @protocol = Pathname.new(protocol)
    @scenarios = scenarios
    @documents = documents
    @w3c = w3c
    @setups = setups
    @skip = skip
    @only = only
    @unsupported = []
    @results = []
  end

  # True when no case failed, every suite run passed a case, and the
  # command offered every method its kind must offer that the run called.
  def run(suites = SUITES)
    suites.each { |suite| send(suite) }
    summary(suites)
  ensure
    @driver.close
  end

  # The chart language of the datamodel under test: the name its vtable
  # gives (`datamodel` in host.create's result, docs/driver.md), which is
  # the datamodel a stylesheet writes ("ecmascript", "python"). The charts
  # are converted for it, so a run that asks for a native datamodel gets
  # charts only that datamodel runs. Nil, with the suite's case failed,
  # when host.create refuses or names a datamodel no stylesheet writes.
  def language(suite)
    return @language if @language

    created = create(config('h'), @datamodel)
    free
    raise Failed, "host.create: status #{created['status'].inspect}" unless created['status'] == OK

    name = created['datamodel'].to_s
    unless W3C.stylesheets.key?(name)
      raise Failed, "the datamodel #{@datamodel} is #{name.inspect}, and no stylesheet writes it " \
                    "(#{W3C.stylesheets.keys.join(', ')})"
    end
    @language = name
  rescue Failed, Skipped => e
    record(suite, 'language', :fail, "the chart language from host.create: #{e.message}")
    nil
  rescue Driver::Crashed => e
    @driver.restart
    @host = false
    record(suite, 'language', :fail, "the chart language from host.create: #{e.message}")
    nil
  end

  # ---- the protocol vectors --------------------------------------------------

  def protocol
    %w[requests config values].each do |kind|
      path = @protocol.join("#{kind}.txt")
      next unless path.file?
      next if @only && !@only.protocol.include?(kind)

      Vectors.read(path).each do |vector|
        run_case('protocol', vector.to_s) do
          reference_only!(vector)
          send("#{kind}_case", vector)
        end
      end
    end
  end

  # A case marked "only reference" is envelope-level: its bytes are no
  # request or config a binding's API makes (request(method, params)
  # writes the map and its id; the config is encoded from a language value),
  # so only the reference runs it (suites/protocol/README.md,
  # "Envelope-level cases").
  def reference_only!(vector)
    only = vector.get('only')
    return unless only

    raise Failed, "only #{only}: a case can be limited to the reference alone" unless only == 'reference'
    raise Skipped, 'envelope-level: reference only' unless @kind == 'reference'
  end

  # Every send with the reply after it, in order, on one host; a refused
  # line applies to the send after it. An event line sends an event, and a
  # run line runs the host: the notifications it answers are the
  # notification lines after it, in order (none when there are none).
  def requests_case(vector)
    setup(vector.get('setup') || 'empty')
    refused = false
    request = nil
    asked = 0
    ran = nil
    vector.lines.each do |key, value|
      unless %w[notification diag].include?(key) || ran.nil?
        notifications!(ran)
        ran = nil
      end
      case key
      when 'refused' then refused = true
      when 'send' then request = value
      when 'reply'
        raise Failed, 'a reply line without a send' unless request

        ask(request, value, refused)
        request = nil
        refused = false
        asked += 1
      when 'event' then event(value)
      when 'run' then ran = { got: run_host.fetch('notifications', []), want: [] }
      when 'notification'
        raise Failed, 'a notification line without a run before it' unless ran

        ran[:want] << value
      end
    end
    notifications!(ran) if ran
    raise Failed, 'each send needs a reply line (or reply none)' if asked.zero? || request
  end

  def config_case(vector)
    config = vector.get('cbor')
    want = vector.get('error')
    raise Failed, 'needs cbor, and error or ok' unless config && (want || vector.key?('ok'))

    result = create(hex(config), 'quickjs')
    if want
      status!(result, INVALID, 'ta_host_create')
      same!('the error', want, result['error'])
    else
      raise Failed, "ta_host_create refused the config: #{diagnostic(result['error'])}" unless result['status'] == OK

      vector.all('send').zip(vector.all('reply')).each { |request, reply| ask(request, reply, nil) }
    end
  end

  # On the datamodel under test; ECMAScript expressions and the cases named
  # "ECMAScript: ..." only on an ECMAScript one, those named "not
  # ECMAScript: ..." only on another (values.txt, its header).
  def values_case(vector)
    created = create(config('h', remote: true), @datamodel)
    status!(created, OK, 'ta_host_create')
    ecmascript = created['datamodel'] == 'ecmascript'
    case vector.kind
    when 'roundtrip', 'malformed'
      raise Skipped, "for ECMAScript; the datamodel is #{created['datamodel']}" if vector.name.start_with?('ECMAScript:') && !ecmascript
      raise Skipped, 'not for ECMAScript' if vector.name.start_with?('not ECMAScript:') && ecmascript

      echo(created['datamodel'])
      echoed!(vector)
    when 'encode', 'invalid'
      raise Skipped, "an ECMAScript expression; the datamodel is #{created['datamodel']}" unless ecmascript

      encoded = call('value.encode', 'expression' => vector.get('ecmascript'))
      if vector.kind == 'invalid'
        status!(encoded, INVALID, 'to_wire')
      else
        status!(encoded, OK, 'to_wire')
        same!('to_wire', vector.get('cbor'), encoded['cbor'])
      end
    else raise Failed, "unknown kind #{vector.kind}"
    end
  end

  # The value of a roundtrip or malformed case, given to the echo setup's
  # session as the data of an event from another system (actor.send with
  # `cbor` and `origin`): the target's datamodel reads it, the echo sends it
  # back to its origin, and that send leaves through the remote I/O
  # processor (host.run's `sends`), its data written by the implementation.
  # A malformed value is refused at the input (TA_ERROR_INVALID).
  def echoed!(vector)
    sent = call('actor.send', 'session' => @echo, 'event' => 'v', 'origin' => ECHO_ORIGIN,
                              'cbor' => bytes(vector.get('cbor')))
    return status!(sent, INVALID, 'the input of a malformed value') if vector.kind == 'malformed'

    status!(sent, OK, 'ta_host_send')
    sends = run_host.fetch('sends', [])
    raise Failed, "#{sends.size} sends to the remote processor, expected 1" unless sends.size == 1

    back = sends.first
    unless back['target'] == ECHO_ORIGIN && back['event'] == 'back'
      raise Failed, "the echo sent #{back['event'].inspect} to #{back['target'].inspect}, expected back to #{ECHO_ORIGIN}"
    end
    raise Failed, 'the send back has no data' unless back['data']

    same!('the data sent back', vector.get('back') || vector.get('cbor'), back['data'])
  end

  # ---- scenarios and W3C: outcomes -----------------------------------------------

  def scenarios
    language = language('scenarios') or return
    directory = Pathname.new(@documents || W3C.scenarios(language))
    Scenarios.all(@scenarios).each do |scenario|
      next if @only && !@only.scenarios.include?(scenario.name)

      run_case('scenarios', scenario.name) do
        documents = scenario.documents.map do |name|
          { 'name' => "#{name}.scxml", 'text' => directory.join(scenario.name, "#{name}.scxml").read }
        end
        expected = scenario.expected
        outcome!(call('scenario.run', with_datamodel('documents' => documents, 'steps' => scenario.steps)), expected)
      end
    end
  end

  def w3c
    language = language('w3c') or return
    directory = Pathname.new(@w3c || W3C.suite(language))
    skip = W3C.skipped(*@skip, language: language)
    W3C.tests(directory).each do |test|
      next record('w3c', test.id, :skip, skip[test.id]) if skip.key?(test.id)

      run_case('w3c', test.id) do
        documents = [test.start, *test.dependencies].map do |path|
          { 'name' => File.basename(path), 'text' => directory.join(path).read }
        end
        outcome!(call('w3c.run', with_datamodel('documents' => documents)))
      end
    end
  end

  private

  # ---- one case ------------------------------------------------------------------

  def run_case(suite, name)
    @host = false
    said = @driver.stderr_count
    yield
    record(suite, name, :pass)
  rescue Skipped => e
    record(suite, name, :skip, e.message)
  rescue Failed => e
    stderr = @driver.stderr_since(said)
    record(suite, name, :fail, stderr.empty? ? e.message : "#{e.message}\nstandard error:\n#{stderr}")
  rescue Driver::Crashed => e
    stderr = @driver.stderr_since(said)
    @driver.restart
    @host = false
    record(suite, name, :fail, "#{e.message}\nstandard error:\n#{stderr}")
  ensure
    free
  end

  def free
    return unless @host

    @host = false
    @driver.call('host.free')
  rescue Driver::RPCError, Driver::Crashed
    nil
  end

  # A method the command answers -32601 is skipped from then on.
  def call(method, params = {})
    raise Skipped, "#{method} is not offered" if @unsupported.include?(method)

    @driver.call(method, params)
  rescue Driver::RPCError => e
    if e.code == -32_601
      @unsupported << method
      raise Skipped, "#{method} is not offered"
    end
    raise Failed, "#{method}: #{e.message}"
  end

  def record(suite, name, verdict, detail = nil)
    result = Result.new(suite, name, verdict, detail)
    @results << result
    quiet = verdict == :skip && @results.count { |each| each.suite == suite && each.detail == detail } > 1
    return if quiet

    @out.puts "#{verdict.to_s.ljust(4)} #{suite} #{name}#{verdict == :skip ? ": #{detail}" : ''}"
    @out.puts detail.to_s.gsub(/^/, '     ') if verdict == :fail
    if verdict == :skip && detail.to_s.end_with?('not offered')
      @out.puts '     (later cases that need it are skipped without a line)'
    end
  end

  # The counts of each suite, and why the run fails though no case did: a
  # suite that passed nothing proves nothing, and a method the kind must
  # offer was not.
  def summary(suites)
    empty = suites.select do |suite|
      results = @results.select { |each| each.suite == suite }
      counts = %i[pass fail skip].map { |verdict| "#{results.count { |each| each.verdict == verdict }} #{verdict}" }
      @out.puts "#{suite}: #{counts.join(', ')}"
      results.none? { |each| each.verdict == :pass }
    end
    @out.puts "not offered: #{@unsupported.join(', ')}" unless @unsupported.empty?
    missing = @unsupported & REQUIRED.fetch(@kind)
    @out.puts "FAIL no case passed in #{empty.join(', ')}: a run that passes nothing proves nothing" unless empty.empty?
    @out.puts "FAIL not offered, but a #{@kind} must offer: #{missing.join(', ')} (docs/driver.md, \"Kinds\")" unless missing.empty?
    @results.none? { |result| result.verdict == :fail } && empty.empty? && missing.empty?
  end

  # ---- setups (suites/protocol/README.md) -----------------------------------------

  def config(system, remote: false)
    config = { 'systemID' => system, 'system' => { 'maxActors' => 8, 'ticksPerSecond' => 1000, 'traceSeed' => 1 } }
    config['ioProcessors'] = [{ 'type' => REMOTE, 'remote' => true }] if remote
    CBOR.encode(config)
  end

  def create(config, datamodel)
    params = { 'config' => CBOR::Bytes.new(config), 'datamodel' => datamodel }
    params['realm'] = @realm if @realm && datamodel == 'quickjs'
    result = call('host.create', params)
    @host = result['status'] == OK
    result
  end

  def setup(name)
    system = SYSTEMS.fetch(name) { raise Failed, "unknown setup #{name}" }
    created = create(config(system), 'quickjs')
    raise Failed, "setup #{name}: ta_host_create refused the config: #{diagnostic(created['error'])}" unless created['status'] == OK

    case name
    when 'pinged', 'pinged2'
      chart('pinged')
      (name == 'pinged2' ? 2 : 1).times { spawn_actor('pinged') }
      settle
    end
  end

  # The echo setup on a host created with the remote I/O processor: the
  # echo chart in the datamodel's language and one session of it.
  def echo(language)
    unless W3C.stylesheets.key?(language)
      raise Failed, "the datamodel is #{language.inspect}, and no stylesheet writes the echo chart in it"
    end

    text = Pathname.new(@setups || W3C.setups(language)).join('echo.scxml').read
    status!(call('image.add', 'charts' => [{ 'name' => 'echo.scxml', 'text' => text }]), OK, 'image echo')
    @echo = spawn_actor('echo')
    settle
  end

  def chart(name)
    text = @protocol.join('setups', "#{name}.scxml").read
    status!(call('image.add', 'charts' => [{ 'name' => "#{name}.scxml", 'text' => text }]), OK, "image #{name}")
  end

  def spawn_actor(chart)
    spawned = call('actor.spawn', 'chart' => chart)
    status!(spawned, OK, "spawn #{chart}")
    spawned['session']
  end

  def settle
    run_host
  end

  # host.run with the setups' wall clock; its result.
  def run_host
    call('clock.advance', 'monotonic' => 0, 'wallNs' => WALL_NS)
    call('host.run')
  end

  # "event <name> <session>": an event without data.
  def event(value)
    name, session = value.to_s.split(' ', 2)
    raise Failed, "an event line is \"event <name> <session>\", not #{value.inspect}" unless session.to_s.match?(/\A\d+\z/)

    status!(call('actor.send', 'session' => Integer(session), 'event' => name), OK, "event #{name}")
  end

  # ---- checks --------------------------------------------------------------------

  # One message posted and its expected REPLY ("none": a message without an
  # id gets no REPLY). Refused: first posted while the host's allocator
  # refuses, which is TA_ERROR_NO_MEMORY with nothing applied and no REPLY;
  # the post after it is the reply. A post publishes no notification here:
  # the runs before it answered theirs.
  def ask(request, want, refused)
    params = { 'message' => bytes(request) }
    raise Skipped, "host.post's refuse is C-level, and the command is a binding" if refused && @kind == 'binding'

    if refused
      answer = call('host.post', params.merge('refuse' => true))
      unless answer['status'] == NO_MEMORY
        raise Failed, "a message the host has no room for is not TA_ERROR_NO_MEMORY (status #{answer['status']})"
      end
    end
    answered = call('host.post', params)
    status!(answered, OK, 'ta_host_post')
    unexpected = answered.fetch('notifications', [])
    unless unexpected.empty?
      raise Failed, "the post published notifications no run answered\n  got  #{unexpected.map { |each| diagnostic(each) }.join("\n       ")}"
    end
    return same!('the reply', want, answered['reply']) unless want == 'none'
    return if answered['reply'].nil?

    raise Failed, "a message without an id got a reply\n  want (nothing)\n  got  #{diagnostic(answered['reply'])}"
  end

  # The notifications a run answered (ran[:got]) against the notification
  # lines after it (ran[:want]), in order.
  def notifications!(ran)
    got = ran[:got]
    want = ran[:want]
    got.each_with_index do |notification, index|
      raise Failed, "a notification more than expected\n  got  #{diagnostic(notification)}" unless want[index]

      same!("notification #{index + 1}", want[index], notification)
    end
    raise Failed, "#{got.size} notifications after the run, expected #{want.size}" if got.size < want.size
  end

  def outcome!(result, expected = 'pass')
    return if result['outcome'] == expected

    raise Failed, "outcome #{result['outcome'].inspect}#{expected == 'pass' ? '' : ", expected #{expected.inspect}"}"
  end

  def with_datamodel(params)
    params['datamodel'] = @datamodel
    params['realm'] = @realm if @realm
    params
  end

  def status!(result, want, what)
    return if result['status'] == want

    detail = result['error'] ? ": #{diagnostic(result['error'])}" : ''
    raise Failed, "#{what}: status #{result['status'].inspect}, expected #{want}#{detail}"
  end

  # The bytes `actual` holds against the vector's hex, byte for byte but
  # where the expected side holds a wildcard (suites/protocol/README.md).
  def same!(what, want, actual)
    expected = CBOR.decode(hex(want))
    got = data(actual)
    item = begin
      CBOR.decode(got)
    rescue CBOR::Error => e
      raise Failed, "#{what} is not CBOR (#{e.message})\n  want #{CBOR.diagnostic(expected)}\n  got  #{got.unpack1('H*')}"
    end
    where = CBOR.mismatch(expected, item) or return
    raise Failed, "#{what} differs at #{where}\n  want #{CBOR.diagnostic(expected)}\n  got  #{CBOR.diagnostic(item)}\n" \
                  "  got  #{got.unpack1('H*')}"
  end

  def data(value) = value.is_a?(CBOR::Bytes) ? value.data.b : value.to_s.b

  def hex(text) = [text.to_s.delete(' ')].pack('H*')

  def bytes(text) = CBOR::Bytes.new(hex(text))

  def diagnostic(value)
    bytes = data(value)
    return '(nothing)' if bytes.empty?

    CBOR.diagnostic(bytes)
  rescue CBOR::Error
    "(not CBOR) #{bytes.unpack1('H*')}"
  end
end
