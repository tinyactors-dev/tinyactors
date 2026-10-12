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

  # Statuses of tinyactors-core/include/tinyactors/types.h, and the routes
  # of host.h that envelopes.txt expects (TA_ROUTE_RELAYED, TA_ROUTE_REFUSED).
  OK = 0
  INVALID = -3
  NO_MEMORY = -4
  ROUTES = { 'relay' => 1, 'refused' => 3 }.freeze
  DELIVERED = 0

  # The echo setup's peer, and the error envelope it gets back for a value
  # the echo's datamodel cannot read (values.txt, malformed).
  ECHO_ORIGIN = 'tinyactors://p/#_scxml_9'
  MALFORMED = CBOR.from_diagnostic(%({"error":"malformed","sendid":"s1","origin":"#{ECHO_ORIGIN}","msgid":"m1"}))
                  .unpack1('H*')

  # The wall clock a setup's runs see (suites/protocol/README.md, "Setups").
  WALL_NS = 1_700_000_000_000_000_000

  # The setups' system ids (suites/protocol/README.md, "Setups").
  SYSTEMS = { 'empty' => 'h', 'pinged' => 'h', 'pinged2' => 'h', 'pinger' => 'a', 'relay' => 'b' }.freeze

  # The methods of docs/driver.md, and those each kind of implementation
  # must offer (docs/driver.md, "Kinds"): the reference all of them, a
  # binding all but the C-level ones, which call core below the API a
  # binding exposes (it sends a binding no `capacity` either).
  METHODS = %w[scenario.run w3c.run host.create host.free host.request image.add actor.spawn actor.send
               clock.advance host.run peer.add envelope.read envelope.deliver value.encode].freeze
  C_LEVEL = %w[envelope.read value.encode].freeze
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
    %w[requests config envelopes values].each do |kind|
      path = @protocol.join("#{kind}.txt")
      next unless path.file?
      next if @only && !@only.protocol.include?(kind)

      Vectors.read(path).each do |vector|
        run_case('protocol', vector.to_s) { send("#{kind}_case", vector) }
      end
    end
  end

  # Every send with the reply after it, in order, on one host; a capacity
  # line applies to the send after it.
  def requests_case(vector)
    setup(vector.get('setup') || 'empty')
    capacity = nil
    request = nil
    asked = 0
    vector.lines.each do |key, value|
      case key
      when 'capacity' then capacity = Integer(value)
      when 'send' then request = value
      when 'reply'
        raise Failed, 'a reply line without a send' unless request

        ask(request, value, capacity)
        request = capacity = nil
        asked += 1
      end
    end
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

  def envelopes_case(vector)
    kind = vector.kind
    routed = ROUTES.key?(kind)
    setup(vector.get('setup') || (routed ? 'relay' : 'empty'))
    envelope = bytes(vector.get('cbor').to_s)
    case kind
    when 'send' then envelopes!(vector)
    when 'relay', 'refused'
      read = call('envelope.deliver', 'peer' => @peers.fetch('a'), 'cbor' => envelope)
      status!(read, OK, 'ta_host_deliver')
      raise Failed, "route #{read['route']}, expected #{ROUTES[kind]} (#{kind})" unless read['route'] == ROUTES[kind]

      envelopes!(vector)
    when 'roundtrip', 'error-roundtrip'
      read = call('envelope.read', 'cbor' => envelope, 'error' => kind.start_with?('error'))
      status!(read, OK, 'reading it')
      same!('written back', vector.get('back') || vector.get('cbor'), read['back'])
    when 'invalid', 'error-invalid'
      status!(call('envelope.read', 'cbor' => envelope, 'error' => kind.start_with?('error')), INVALID, 'reading it')
    else raise Failed, "unknown kind #{kind}"
    end
  end

  # On the datamodel under test; ECMAScript expressions and the cases named
  # "ECMAScript: ..." only on an ECMAScript one, those named "not
  # ECMAScript: ..." only on another (values.txt, its header).
  def values_case(vector)
    created = create(config('h'), @datamodel)
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

  # The value of a roundtrip or malformed case, delivered in an envelope
  # from the peer "p" to the echo setup's session, through the calls a
  # binding's API makes (ta_host_envelope_read, ta_host_deliver): its
  # datamodel's from_wire reads it, and the PEER envelope the echo sends
  # back carries what its to_wire wrote. A malformed value is refused, and
  # the error envelope back to "p" says so (host.h, ta_host_deliver).
  def echoed!(vector)
    value = CBOR::Tag.new(24, CBOR::Bytes.new(hex(vector.get('cbor'))))
    envelope = CBOR::Map.new([['target', "tinyactors://h/#_scxml_#{@echo}"], ['name', 'v'], ['data', value],
                              ['sendid', 's1'], ['origin', ECHO_ORIGIN], ['msgid', 'm1']])
    delivered = call('envelope.deliver', 'peer' => @peers.fetch('p'), 'cbor' => CBOR::Bytes.new(CBOR.encode(envelope)))
    status!(delivered, OK, 'ta_host_deliver')
    malformed = vector.kind == 'malformed'
    route = malformed ? ROUTES['refused'] : DELIVERED
    raise Failed, "route #{delivered['route'].inspect}, expected #{route}" unless delivered['route'] == route

    call('clock.advance', 'monotonic' => 0, 'wallNs' => WALL_NS)
    sent = call('host.run').fetch('envelopes', [])
    raise Failed, "#{sent.size} envelopes back, expected 1" unless sent.size == 1
    return same!('the error envelope', MALFORMED, sent.first) if malformed

    back = CBOR.decode(data(sent.first))
    data = back.major == 5 && back.children.each_slice(2).find { |key, _| CBOR.value(key) == 'data' }&.last
    unless data&.major == 6 && data.argument == 24 && data.children.first.major == 2
      raise Failed, "the envelope back has no data as tag 24 on bytes\n  got  #{CBOR.diagnostic(back)}"
    end

    same!('the data sent back', vector.get('back') || vector.get('cbor'), CBOR.payload(data.children.first))
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

  def config(system)
    CBOR.encode({ 'systemID' => system,
                  'system' => { 'maxActors' => 8, 'ticksPerSecond' => 1000, 'traceSeed' => 1 },
                  'maxSubscriptions' => 1 })
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
    when 'pinger'
      chart('pinger')
      session = spawn_actor('pinger')
      peer('b')
      sent = call('actor.send', 'session' => session, 'event' => 'go',
                                'data' => { 'to' => 'tinyactors://b/@echo', 'n' => 1 })
      status!(sent, OK, 'setup pinger: ta_host_send')
    when 'relay'
      @peers = { 'a' => peer('a'), 'c' => peer('c') }
    end
  end

  # The echo setup on a host already created as `empty`: the echo chart in
  # the datamodel's language, one session of it, and the peer "p".
  def echo(language)
    unless W3C.stylesheets.key?(language)
      raise Failed, "the datamodel is #{language.inspect}, and no stylesheet writes the echo chart in it"
    end

    text = Pathname.new(@setups || W3C.setups(language)).join('echo.scxml').read
    status!(call('image.add', 'charts' => [{ 'name' => 'echo.scxml', 'text' => text }]), OK, 'image echo')
    @echo = spawn_actor('echo')
    @peers = { 'p' => peer('p') }
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

  def peer(id)
    added = call('peer.add', 'id' => id, 'trusted' => true)
    status!(added, OK, "peer #{id}")
    added['peer']
  end

  def settle
    call('clock.advance', 'monotonic' => 0, 'wallNs' => WALL_NS)
    call('host.run')
  end

  # ---- checks --------------------------------------------------------------------

  # One request and its expected reply ("none": a notification's). With a
  # capacity: first refused in that many bytes and measured; the retry in
  # the length it asked for is the reply.
  def ask(request, want, capacity)
    params = { 'request' => bytes(request) }
    raise Skipped, "host.request's capacity is C-level, and the command is a binding" if capacity && @kind == 'binding'

    if capacity
      refused = call('host.request', params.merge('capacity' => capacity))
      unless refused['status'] == NO_MEMORY && refused['length'].to_i > capacity
        raise Failed, "a reply larger than #{capacity} bytes is not TA_ERROR_NO_MEMORY with its length " \
                      "(status #{refused['status']}, length #{refused['length'].inspect})"
      end
      capacity = refused['length']
      params['capacity'] = capacity
    end
    answered = call('host.request', params)
    status!(answered, OK, 'ta_host_request')
    reply = data(answered['reply'])
    if capacity && reply.bytesize != capacity
      raise Failed, "the retry's reply is #{reply.bytesize} bytes, not the #{capacity} TA_ERROR_NO_MEMORY asked for"
    end
    return same!('the reply', want, answered['reply']) unless want == 'none'
    return if reply.empty?

    raise Failed, "a notification got a reply\n  want (nothing)\n  got  #{diagnostic(reply)}"
  end

  # The envelopes of the host's next run, against the vector's envelope
  # lines in order.
  def envelopes!(vector)
    call('clock.advance', 'monotonic' => 0, 'wallNs' => WALL_NS)
    got = call('host.run').fetch('envelopes', [])
    want = vector.all('envelope')
    got.each_with_index do |envelope, index|
      raise Failed, "an envelope more than expected\n  got  #{diagnostic(envelope)}" unless want[index]

      same!("envelope #{index + 1}", want[index], envelope)
    end
    raise Failed, "#{got.size} envelopes, expected #{want.size}" if got.size < want.size
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
