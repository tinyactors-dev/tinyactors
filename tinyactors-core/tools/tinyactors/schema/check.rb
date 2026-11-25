# frozen_string_literal: true

# The schema's checks against a reference validator (mise run run:schema):
#
# - run.schema.json is a valid JSON Schema 2020-12, and the generator takes it;
# - its methods are the run.* methods and every method of docs/requests.md,
#   each with its params and result, and each notification has its params;
# - every document of the fixture cases (tests/<case>/) is accepted or
#   refused by json_schemer as its expectation says, the refusal at the path
#   the expectation names (or around it: a validator reports a missing member
#   at its object), so the generated checker (test_schema_check, against the
#   same expectations) and a reference validator agree;
# - the generator refuses each unsupported schema (a case with an
#   expected-refusal.txt) with its JSON pointer;
# - the generator itself: $refs resolved to their definition's node, the
#   discriminators it picks, and output that is the same bytes each time.
#
# json_schemer 2.5.0, pinned in this directory's Gemfile.lock, validates
# 2020-12; its patterns are read as ECMA-262 reads them, as the checker's.
require 'json'
require 'json_schemer'
require 'open3'
require 'pathname'
require 'rbconfig'
require 'tmpdir'
require_relative 'generate'

module SchemaCheck
  HERE = Pathname.new(__dir__)
  CORE = HERE.join('../../..').expand_path
  RUN_SCHEMA = HERE.join('../run.schema.json')
  REQUESTS = CORE.join('docs/requests.md')
  TESTS = HERE.join('tests')

  @failures = []
  @checks = 0

  def self.check(condition, message)
    @checks += 1
    @failures << message unless condition
  end

  def self.validator(schema) = JSONSchemer.schema(schema, regexp_resolver: 'ecma')

  def self.read(path) = JSON.parse(path.read, allow_duplicate_key: false)

  # The JSON pointer of a path the checker writes ("params.items[0].sku"),
  # from `root` (which it leaves out).
  def self.pointer(path, root)
    path = path.delete_prefix(root).delete_prefix('.')
    tokens = path.scan(/\[(\d+)\]|([^.\[\]]+)/).map { |index, name| index || name }
    tokens.map { |token| "/#{token.gsub('~', '~0').gsub('/', '~1')}" }.join
  end

  def self.run_schema
    schema = read(RUN_SCHEMA)
    errors = JSONSchemer.validate_schema(schema).to_a
    check(errors.empty?, "run.schema.json is not a valid 2020-12 schema: #{errors.first(3).map { |e| e['error'] }}")
    compiled = Schema::Compiled.new(schema, file: 'run.schema.json')
    methods = compiled.methods.map(&:name)
    documented = REQUESTS.read.scan(/^### `([a-z]+\.[A-Za-z.]+)`$/).flatten
    check(!documented.empty?, 'docs/requests.md lists no method')
    forwarded = methods.reject { |name| name.start_with?('run.') }
    (documented - forwarded).each { |name| check(false, "docs/requests.md has #{name}, the schema has no request for it") }
    (forwarded - documented).each { |name| check(false, "the schema forwards #{name}, which docs/requests.md does not list") }
    compiled.methods.each do |entry|
      check(entry.params && schema['$defs'].key?("#{entry.name}.params"), "#{entry.name} has no $defs/#{entry.name}.params")
      check(entry.result, "#{entry.name} has no $defs/#{entry.name}.result")
    end
    check(compiled.notifications.size >= 13, 'the schema has fewer notifications than the effects')
    compiled.notifications.each do |entry|
      check(entry.name.start_with?('effect.'), "a notification #{entry.name} outside effect.*")
      check(schema['$defs'].key?("#{entry.name}.params"), "#{entry.name} has no $defs/#{entry.name}.params")
    end
    examples(schema)
  end

  # Lines of the protocol, valid or not as a client or the server writes them.
  def self.examples(schema)
    client = validator(schema).ref('#/$defs/clientMessage')
    server = validator(schema).ref('#/$defs/serverMessage')
    good_client = [
      { 'jsonrpc' => '2.0', 'id' => 1, 'method' => 'run.spawn', 'params' => { 'chart' => 'order', 'name' => 'order-17', 'params' => { 'total' => 4200 } } },
      { 'jsonrpc' => '2.0', 'id' => 3, 'method' => 'run.turn', 'params' => { 'hold' => true } },
      { 'jsonrpc' => '2.0', 'id' => 6, 'method' => 'run.turn' },
      { 'jsonrpc' => '2.0', 'id' => 10, 'method' => 'run.advanceTime', 'params' => { 'by' => '30s' } },
      { 'jsonrpc' => '2.0', 'id' => 12, 'method' => 'actor.state', 'params' => { 'session' => 2 } },
      { 'jsonrpc' => '2.0', 'method' => 'run.fail', 'params' => { 'target' => 2, 'sendid' => 'pay' } },
      [{ 'jsonrpc' => '2.0', 'id' => 7, 'method' => 'run.spawn', 'params' => { 'chart' => 'order', 'params' => { 'total' => 990 } } },
       { 'jsonrpc' => '2.0', 'id' => 9, 'method' => 'run.turn' }]
    ]
    bad_client = [
      [],
      { 'jsonrpc' => '2.0', 'id' => 1, 'method' => 'run.spawn', 'params' => {} },
      { 'jsonrpc' => '2.0', 'id' => 1, 'method' => 'run.send', 'params' => { 'target' => 1, 'event' => 'x', 'data' => 1, 'cbor' => 'AQ' } },
      { 'jsonrpc' => '1.0', 'id' => 1, 'method' => 'run.turn' }
    ]
    good_server = [
      { 'jsonrpc' => '2.0', 'id' => 1, 'result' => { 'session' => 1, 'address' => 'tinyactors://tinyactors-run/#_scxml_1' } },
      { 'jsonrpc' => '2.0', 'method' => 'effect.send', 'params' => { 'batch' => 1, 'index' => 0, 'session' => 1, 'type' => 'payments', 'target' => 'charges', 'event' => 'charge', 'sendid' => 'pay', 'data' => { 'amount' => 4200 }, 'waiting' => true } },
      { 'jsonrpc' => '2.0', 'id' => 4, 'error' => { 'code' => -32002, 'message' => 'a batch is held', 'data' => { 'reason' => 'state', 'path' => '' } } },
      [{ 'jsonrpc' => '2.0', 'id' => 8, 'result' => {} }]
    ]
    bad_server = [
      { 'jsonrpc' => '2.0', 'method' => 'effect.log', 'params' => { 'batch' => 1, 'index' => 0 } },
      { 'jsonrpc' => '2.0', 'id' => 1, 'result' => {}, 'error' => { 'code' => -32602, 'message' => 'x' } }
    ]
    good_client.each { |line| check(client.valid?(line), "a client line is refused: #{JSON.generate(line)}") }
    bad_client.each { |line| check(!client.valid?(line), "a client line is taken: #{JSON.generate(line)}") }
    good_server.each { |line| check(server.valid?(line), "a server line is refused: #{JSON.generate(line)}") }
    bad_server.each { |line| check(!server.valid?(line), "a server line is taken: #{JSON.generate(line)}") }
    transcripts(client, server)
  end

  TRANSCRIPTS = CORE.join('tests/run')
  # The codes a server gives to a line the schema refuses: not JSON, not a
  # request, no such method, params refused.
  REFUSED = [-32700, -32600, -32601, -32602].freeze

  # Every line of tinyactors run's transcripts (tests/run): the server's
  # against serverMessage, and the client's against clientMessage unless the
  # server refused it as the schema would.
  def self.transcripts(client, server)
    cases = TRANSCRIPTS.glob('*/transcript.jsonl').sort
    check(cases.size >= 3, 'fewer transcripts than expected in tests/run')
    cases.each do |path|
      name = path.dirname.basename.to_s
      turns = []
      path.read.each_line(chomp: true) do |line|
        if line.start_with?('> ') then turns << [line.delete_prefix('> '), []]
        elsif line.start_with?('< ') then turns.last&.last&.push(line.delete_prefix('< '))
        else check(false, "#{name}: a line that is neither the client's nor the server's: #{line[0, 60]}")
        end
      end
      turns.each do |sent, answers|
        responses = answers.map { |text| JSON.parse(text, allow_duplicate_key: false) }
        responses.each { |message| check(server.valid?(message), "#{name}: a server line is refused: #{JSON.generate(message)[0, 200]}") }
        codes = responses.flat_map { |message| Array(message.is_a?(Array) ? message : [message]) }
                         .filter_map { |message| message.dig('error', 'code') }
        next if codes.intersect?(REFUSED)

        check(client.valid?(JSON.parse(sent, allow_duplicate_key: false)), "#{name}: a client line is refused: #{sent[0, 200]}")
      end
    end
  end

  def self.fixtures
    cases = Schema.cases(TESTS)
    check(cases.size >= 8, 'fewer fixture cases than expected')
    cases.each do |test_case|
      refusal = test_case.directory.join('expected-refusal.txt')
      schema = read(test_case.schema)
      if refusal.file?
        expected = refusal.read.strip
        begin
          Schema::Compiled.new(schema, file: 'schema.json')
          check(false, "#{test_case.name}: the generator took a schema it should refuse")
        rescue Schema::Unsupported => e
          check(e.message.include?(expected), "#{test_case.name}: refused with #{e.message.inspect}, not #{expected.inspect}")
        end
        refused_by_the_command(test_case, expected)
        next
      end
      errors = JSONSchemer.validate_schema(schema).to_a
      check(errors.empty?, "#{test_case.name}: schema.json is not a valid 2020-12 schema")
      check(!test_case.documents.empty?, "#{test_case.name}: no documents")
      test_case.documents.each { |document| fixture(test_case, schema, document) }
    end
  end

  # The command the build runs fails, and says where.
  def self.refused_by_the_command(test_case, expected)
    Dir.mktmpdir do |out|
      message, status = Open3.capture2e(RbConfig.ruby, HERE.join('generate.rb').to_s, test_case.schema.to_s, out, 'refused')
      check(!status.success? && message.include?(expected.delete_prefix('schema.json')),
            "#{test_case.name}: generate.rb exited #{status.exitstatus} with #{message.inspect}")
      check(Dir.empty?(out), "#{test_case.name}: generate.rb wrote files for a schema it refused")
    end
  end

  def self.fixture(test_case, schema, document)
    label = "#{test_case.name}/#{document.name}"
    if document.method
      instance = { 'method' => document.method, 'params' => document.value }
      schemer = validator(schema).ref('#/$defs/request')
      root = 'params'
    else
      instance = document.value
      schemer = validator(schema)
      root = ''
    end
    errors = schemer.validate(instance).to_a
    unless document.error
      check(errors.empty?, "#{label}: json_schemer refuses it: #{errors.first(2).map { |e| e['error'] }}")
      return
    end
    check(!errors.empty?, "#{label}: json_schemer accepts it")
    expected = (root.empty? ? '' : "/#{root}") + pointer(document.error['path'], root)
    around = [expected]
    around << expected.sub(%r{/[^/]*\z}, '') while around.last != '' && around.size < 32
    check(errors.any? { |error| around.include?(error['data_pointer']) },
          "#{label}: json_schemer reports #{errors.map { |e| e['data_pointer'] }.uniq}, not around #{expected.inspect}")
    check(document.error['code'] == -32602, "#{label}: a refusal is -32602")
  end

  # The generator's own: references, discriminators, determinism.
  def self.generator
    schema = read(RUN_SCHEMA)
    compiled = Schema::Compiled.new(schema, file: 'run.schema.json')
    nodes = compiled.nodes
    check(nodes[0].types.zero? && nodes[0].members.empty?, 'node 0 is the root, which allows anything')
    session = compiled.node_of('session')
    spawn = nodes[compiled.node_of('run.spawn.result')]
    check(spawn.members.find { |member| member.name == 'session' }.node == session, 'a $ref is its definition\'s node')
    check(compiled.node_of('run.ack.params') == compiled.node_of('noParams'), 'a definition that is only a $ref is its target')
    request = nodes[compiled.node_of('request')]
    check(request.choices.all? { |choice| choice.key == 'method' && choice.value }, 'requests are told apart by method')
    target = nodes[compiled.node_of('target')]
    check(target.choices.map(&:types) == [Schema::TYPE_BITS['integer'], Schema::TYPE_BITS['string']] &&
          target.choices.none?(&:key), 'a target is told apart by type')
    image = nodes[compiled.node_of('run.addImage.params')]
    check(image.choices.map(&:key) == %w[files sources image] && image.choices.none?(&:value),
          'run.addImage is told apart by the member only one form takes')
    server = nodes[compiled.node_of('serverMessage')]
    check(server.choices.map(&:key) == ['id', 'method', nil], 'a server message: a response by id, a notification by method, a batch by type')
    Dir.mktmpdir do |first|
      Dir.mktmpdir do |second|
        Schema.generate(RUN_SCHEMA, first, 'schema_run')
        Schema.generate(RUN_SCHEMA, second, 'schema_run')
        %w[schema_run.h schema_run.c].each do |file|
          check(File.binread(File.join(first, file)) == File.binread(File.join(second, file)), "#{file} differs between runs")
        end
        check(File.read(File.join(first, 'schema_run.h')).include?('TOOL_SCHEMA_RUN_METHOD_RUN_ADD_IMAGE = 0'),
              'the methods are numbered in the schema\'s order')
      end
    end
  end

  def self.main
    run_schema
    fixtures
    generator
    @failures.each { |failure| warn "FAIL #{failure}" }
    abort "run:schema: #{@failures.size} of #{@checks} checks failed" unless @failures.empty?
    puts "run:schema: #{@checks} checks pass (json_schemer #{Gem.loaded_specs['json_schemer'].version})"
  end
end

SchemaCheck.main if $PROGRAM_NAME == __FILE__
