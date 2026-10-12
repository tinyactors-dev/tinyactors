# frozen_string_literal: true

require 'minitest/autorun'
require 'fileutils'
require 'stringio'
require 'tmpdir'
require 'runner'

# The runner against test/fake_implementation.rb, on suites written here:
# vectors that pass and fail, wildcards, outcomes, a crash, a hang and a
# method the command does not offer.
class RunnerTest < Minitest::Test
  FAKE = ['ruby', File.expand_path('fake_implementation.rb', __dir__)].freeze

  def setup
    @directory = Dir.mktmpdir('tinyactors-test-runner')
    @out = StringIO.new
  end

  def teardown
    FileUtils.rm_rf(@directory)
  end

  # ---- protocol -------------------------------------------------------------

  def test_vectors_pass_and_fail_with_a_diff
    vectors('requests.txt', [
              ['request echo', 'send {"id":1,"method":"echo","params":{"n":1}}', 'reply {"id":1,"result":{"n":1}}'],
              ['request wrong', 'send {"id":1,"method":"wrong"}', 'reply {"id":1,"result":{"n":1}}'],
              ['request a notification', 'send {"id":1,"method":"echo"}', 'reply none'],
              ['request no id', 'send {"method":"echo"}', 'reply none']
            ])
    ok, results = run_suites('protocol')
    refute ok
    assert_equal({ 'requests.txt "echo"' => :pass, 'requests.txt "wrong"' => :fail,
                   'requests.txt "a notification"' => :fail, 'requests.txt "no id"' => :pass }, verdicts(results))
    assert_includes @out.string, 'the reply differs at {"result"}{"n"}'
    assert_includes @out.string, 'want {"id":1,"result":{"n":1}}'
    assert_includes @out.string, 'got  {"id":1,"result":{"n":2}}'
    assert_includes @out.string, "got  #{CBOR.from_diagnostic('{"id":1,"result":{"n":2}}').unpack1('H*')}"
    assert_includes @out.string, 'a message without an id got a reply'
  end

  def test_notifications_after_a_run
    subscribe = ['send {"id":1,"method":"subscribe","params":{"session":1}}', 'reply {"id":1,"result":{}}']
    changed = 'notification {"method":"actor.changed","params":{"session":1,"value":"t"}}'
    vectors('requests.txt', [
              ['request one after an event', *subscribe, 'run', 'event ping 1', 'run', changed, 'run'],
              ['request expected but none', *subscribe, 'run', changed],
              ['request one more than expected', *subscribe, 'event ping 1', 'run'],
              ['request another value', *subscribe, 'event ping 1', 'run',
               'notification {"method":"actor.changed","params":{"session":1,"value":"s"}}'],
              ['request a notification without a run', *subscribe, changed]
            ])
    ok, results = run_suites('protocol')
    refute ok
    assert_equal({ 'requests.txt "one after an event"' => :pass, 'requests.txt "expected but none"' => :fail,
                   'requests.txt "one more than expected"' => :fail, 'requests.txt "another value"' => :fail,
                   'requests.txt "a notification without a run"' => :fail }, verdicts(results))
    assert_includes @out.string, '0 notifications after the run, expected 1'
    assert_includes @out.string, 'a notification more than expected'
    assert_includes @out.string, 'notification 1 differs at {"params"}{"value"}'
    assert_includes @out.string, 'a notification line without a run before it'
  end

  def test_wildcards_match_by_type
    error = '{"code":-32601,"message":39999("%s"),"data":{"reason":"unknown","path":""}}'
    vectors('requests.txt', [
              ['request text', 'send {"id":1,"method":"nope"}', "reply {\"id\":1,\"error\":#{format(error, 'text')}}"],
              ['request any', 'send {"id":1,"method":"echo","params":[1]}', 'reply 39999("any")'],
              ['request not uint', 'send {"id":1,"method":"nope"}', "reply {\"id\":1,\"error\":#{format(error, 'uint')}}"]
            ])
    _ok, results = run_suites('protocol')
    assert_equal({ 'requests.txt "text"' => :pass, 'requests.txt "any"' => :pass,
                   'requests.txt "not uint"' => :fail }, verdicts(results))
    assert_includes @out.string, 'differs at {"error"}{"message"}'
  end

  def test_a_refused_line_is_refused_first
    vectors('requests.txt', [['request big', 'refused', 'send {"id":1,"method":"big"}', 'reply 39999("any")']])
    ok, = run_suites('protocol', kind: 'reference')
    assert ok, @out.string
  end

  def test_configs_refused_and_taken
    vectors('config.txt', [
              ['config refused', 'cbor {}',
               'error {"error":{"code":-32602,"message":39999("text"),"data":{"reason":"missing","path":"systemID"}}}'],
              ['config taken', 'cbor {"systemID":"h"}', 'ok', 'send {"id":1,"method":"echo","params":[]}',
               'reply {"id":1,"result":[]}'],
              ['config refused, expected taken', 'cbor 1', 'ok']
            ])
    _ok, results = run_suites('protocol')
    assert_equal({ 'config.txt "refused"' => :pass, 'config.txt "taken"' => :pass,
                   'config.txt "refused, expected taken"' => :fail }, verdicts(results))
  end

  def test_a_method_not_offered_is_skipped
    vectors('envelopes.txt', [['roundtrip one', 'cbor {"target":"tinyactors://h/@a","name":"x"}'],
                              ['roundtrip two', 'cbor {"target":"tinyactors://h/@a","name":"y"}']])
    vectors('requests.txt', [['request echo', 'send {"id":1,"method":"echo","params":[]}', 'reply {"id":1,"result":[]}']])
    ok, results = run_suites('protocol')
    assert ok, @out.string
    assert_equal %i[pass skip skip], results.map(&:verdict)
    assert_equal 1, @out.string.scan('envelope.read is not offered').size
    assert_includes @out.string, 'not offered: envelope.read'
  end

  def test_values_go_through_the_echo_setup
    vectors('values.txt', [['roundtrip same', 'cbor [1,"a"]'],
                           ['roundtrip other', 'cbor [1]', 'back [2]'],
                           ['malformed truncated', 'cbor 8201']])
    File.write(File.join(@directory, 'protocol/values.txt'),
               File.read(File.join(@directory, 'protocol/values.txt')).sub(/^cbor .*\ndiag 8201$/, 'cbor 8201'))
    ok, results = run_suites('protocol')
    refute ok
    assert_equal({ 'values.txt "same"' => :pass, 'values.txt "other"' => :fail, 'values.txt "truncated"' => :pass },
                 verdicts(results))
    assert_includes @out.string, 'the data sent back differs'
  end

  # ---- runs that prove nothing (docs/driver.md, "Kinds") -----------------------

  def test_a_suite_that_passes_nothing_fails_the_run
    vectors('envelopes.txt', [['roundtrip one', 'cbor {"target":"tinyactors://h/@a","name":"x"}']])
    ok, results = run_suites('protocol')
    refute ok
    assert_equal %i[skip], results.map(&:verdict)
    assert_includes @out.string, 'FAIL no case passed in protocol'
  end

  def test_the_reference_must_offer_the_c_level_methods
    vectors('envelopes.txt', [['roundtrip one', 'cbor {"target":"tinyactors://h/@a","name":"x"}']])
    vectors('requests.txt', [['request echo', 'send {"id":1,"method":"echo","params":[]}', 'reply {"id":1,"result":[]}']])
    ok, = run_suites('protocol', kind: 'reference')
    refute ok
    assert_includes @out.string, 'FAIL not offered, but a reference must offer: envelope.read'
  end

  def test_a_binding_is_sent_no_refuse
    vectors('requests.txt', [['request big', 'refused', 'send {"id":1,"method":"big"}', 'reply 39999("any")'],
                             ['request echo', 'send {"id":1,"method":"echo","params":[]}', 'reply {"id":1,"result":[]}']])
    ok, results = run_suites('protocol')
    assert ok, @out.string
    assert_equal({ 'requests.txt "big"' => :skip, 'requests.txt "echo"' => :pass }, verdicts(results))
    assert_includes @out.string, "host.post's refuse is C-level"
  end

  def test_an_envelope_level_case_is_the_references
    vectors('requests.txt', [['request id: text', 'only reference', 'send {"id":"a","method":"echo","params":[]}',
                              'reply {"id":"a","result":[]}'],
                             ['request echo', 'send {"id":1,"method":"echo","params":[]}', 'reply {"id":1,"result":[]}']])
    vectors('config.txt', [['config marked', 'only reference', 'cbor {"systemID":"h"}', 'ok']])
    ok, results = run_suites('protocol')
    assert ok, @out.string
    assert_equal({ 'requests.txt "id: text"' => :skip, 'requests.txt "echo"' => :pass, 'config.txt "marked"' => :skip },
                 verdicts(results))
    assert_includes @out.string, 'skip protocol requests.txt "id: text": envelope-level: reference only'

    @out = StringIO.new
    _ok, results = run_suites('protocol', kind: 'reference')
    assert_equal({ 'requests.txt "id: text"' => :pass, 'requests.txt "echo"' => :pass, 'config.txt "marked"' => :pass },
                 verdicts(results))
  end

  def test_only_names_the_reference
    vectors('requests.txt', [['request odd', 'only binding', 'send {"id":1,"method":"echo","params":[]}',
                              'reply {"id":1,"result":[]}']])
    _ok, results = run_suites('protocol')
    assert_equal({ 'requests.txt "odd"' => :fail }, verdicts(results))
    assert_includes @out.string, 'only binding: a case can be limited to the reference alone'
  end

  def test_a_method_a_binding_must_offer
    scenario('a', 'pass')
    w3c = File.join(@directory, 'w3c')
    FileUtils.mkdir_p(File.join(w3c, '144'))
    File.write(File.join(w3c, 'index.txt'), "144 mandatory 144/test144.scxml -\n")
    File.write(File.join(w3c, '144/test144.scxml'), '<scxml/>')
    runner = Runner.new(FAKE, out: @out, w3c: w3c, scenarios: File.join(@directory, 'scenarios'),
                              documents: File.join(@directory, 'converted'))
    refute runner.run(%w[scenarios w3c])
    assert_includes @out.string, 'FAIL not offered, but a binding must offer: w3c.run'
  end

  # ---- the chart language (docs/driver.md, "Datamodels") -------------------------

  def test_the_language_is_the_datamodel_host_create_reports
    scenario('a', 'pass')
    runner = Runner.new(FAKE, out: @out, datamodel: 'native', scenarios: File.join(@directory, 'scenarios'),
                              documents: File.join(@directory, 'converted'))
    assert runner.run(['scenarios']), @out.string
    assert_equal 'python', runner.language('scenarios')
  end

  def test_a_datamodel_no_stylesheet_writes_fails
    scenario('a', 'pass')
    runner = Runner.new(FAKE, out: @out, datamodel: 'cobol', scenarios: File.join(@directory, 'scenarios'),
                              documents: File.join(@directory, 'converted'))
    refute runner.run(['scenarios'])
    assert_equal [%w[language fail]], runner.results.map { |result| [result.name, result.verdict.to_s] }
    assert_includes @out.string, 'the datamodel cobol is "cobol", and no stylesheet writes it'
  end

  # ---- scenarios ---------------------------------------------------------------

  def test_scenario_outcomes
    %w[pass fail compile timeout].each { |outcome| scenario(outcome, outcome) }
    ok, results = run_suites('scenarios')
    refute ok
    assert_equal({ 'compile' => :fail, 'fail' => :fail, 'pass' => :pass, 'timeout' => :fail }, verdicts(results))
    assert_includes @out.string, 'outcome "compile"'
    assert_includes @out.string, 'fake: the outcome is timeout'
  end

  def test_a_canary_passes_only_with_the_outcome_it_expects
    scenario('fails', 'fail', "expect fail\nimage judge\nspawn j judge\n")
    scenario('passes', 'pass', "expect fail\nimage judge\nspawn j judge\n")
    scenario('times-out', 'timeout', "expect timeout\nimage judge\nspawn j judge\n")
    ok, results = run_suites('scenarios')
    refute ok
    assert_equal({ 'fails' => :pass, 'passes' => :fail, 'times-out' => :pass }, verdicts(results))
    assert_includes @out.string, 'outcome "pass", expected "fail"'
  end

  def test_an_expect_line_is_not_a_step
    scenario('a', 'pass', "# a comment\nexpect compile\nimage judge\nspawn j judge\n")
    path = File.join(@directory, 'scenarios/a/steps.txt')
    assert_equal 'compile', Scenarios.expected(path)
    assert_equal [{ 'image' => ['judge.scxml'] }, { 'spawn' => 'j', 'chart' => 'judge' }], Scenarios.steps(path)
    File.write(path, "expect maybe\nimage judge\n")
    assert_raises(Scenarios::Error) { Scenarios.expected(path) }
  end

  def test_the_suite_has_a_canary_for_each_outcome_but_pass
    expected = Scenarios.all.map(&:expected)
    assert_equal %w[compile fail timeout], (expected - ['pass']).sort
  end

  def test_a_crash_fails_its_case_and_the_next_one_runs
    scenario('a', 'crash')
    scenario('b', 'pass')
    _ok, results = run_suites('scenarios')
    assert_equal({ 'a' => :fail, 'b' => :pass }, verdicts(results))
    assert_includes @out.string, 'the command ended (exit 3)'
    assert_includes @out.string, 'fake: crashing as asked'
  end

  def test_a_command_that_hangs
    scenario('a', 'hang')
    scenario('b', 'pass')
    _ok, results = run_suites('scenarios', timeout: 1)
    assert_equal({ 'a' => :fail, 'b' => :pass }, verdicts(results))
    assert_includes @out.string, 'no reply in 1 s'
  end

  def test_scenario_steps_reach_the_implementation
    scenario('a', 'pass', "image a\nspawn x a {\"Var1\": [1, 2]}\nsend-many\n  x go\n  x stop {\"why\": \"@x\"}\nmigrate wake timeout 5\n")
    steps = Scenarios.steps(File.join(@directory, 'scenarios/a/steps.txt'))
    assert_equal [{ 'image' => ['a.scxml'] }, { 'spawn' => 'x', 'chart' => 'a', 'params' => CBOR::Map.new([['Var1', [1, 2]]]) },
                  { 'sendMany' => [{ 'to' => 'x', 'event' => 'go' },
                                   { 'to' => 'x', 'event' => 'stop', 'data' => CBOR::Map.new([%w[why @x]]) }] },
                  { 'migrate' => { 'wake' => true, 'timeout' => 5 } }], steps
  end

  def test_the_suites_scenarios_parse
    Scenarios.all.each do |scenario|
      documents = scenario.documents.map { |name| "#{name}.scxml" }
      images = scenario.steps.flat_map { |step| step.fetch('image', []) }
      assert_empty images - documents, "#{scenario.name}: an image names a document that is not there"
      assert_empty documents - images, "#{scenario.name}: a document no image adds"
    end
  end

  # ---- W3C ----------------------------------------------------------------------

  def test_w3c_not_offered_and_skip_list
    w3c = File.join(@directory, 'w3c')
    FileUtils.mkdir_p(File.join(w3c, '144'))
    File.write(File.join(w3c, '144/test144.scxml'), '<scxml/>')
    File.write(File.join(w3c, 'index.txt'), "144 mandatory 144/test144.scxml -\n145 mandatory 145/test145.scxml -\n")
    skip = File.join(@directory, 'skip.txt')
    File.write(skip, "145 a reason\n")
    out = StringIO.new
    runner = Runner.new(FAKE, out: out, w3c: w3c, skip: [skip])
    refute runner.run(['w3c'])
    assert_equal [%w[144 skip], %w[145 skip]], runner.results.map { |result| [result.name, result.verdict.to_s] }
    assert_includes out.string, 'w3c.run is not offered'
    assert_includes out.string, 'skip w3c 145: a reason'
  end

  def test_skip_files_add_to_the_suites_list
    path = File.join(@directory, 'skip.txt')
    File.write(path, "# a datamodel's own list\n\n145 a reason\n")
    skipped = W3C.skipped(path)
    assert_equal 'a reason', skipped['145']
    assert_equal W3C.skipped.size + 1, skipped.size
  end

  def test_a_language_is_a_stylesheet_with_its_own_skip_list
    assert_equal W3C::IRP.join('confEcma.xsl'), W3C.stylesheet('ecmascript')
    assert_equal W3C::DIRECTORY.join('confPython.xsl'), W3C.stylesheet('python')
    assert_raises(RuntimeError) { W3C.stylesheet('cobol') }
    python = W3C.skipped(language: 'python')
    assert_includes python.keys, '567'
    refute_includes W3C.skipped(language: 'ecmascript').keys, '567'
    assert_equal W3C.skipped.size, W3C.skipped(language: 'cobol').size
  end

  private

  # A vector file from cases written as lines; a hex line is written from
  # its diagnostic notation.
  def vectors(file, cases)
    FileUtils.mkdir_p(File.join(@directory, 'protocol'))
    text = cases.map do |lines|
      lines.map do |line|
        key, value = line.split(' ', 2)
        next line unless Vectors::HEX_KEYS.include?(key) && value != 'none'

        "#{key} #{CBOR.from_diagnostic(value).unpack1('H*')}\ndiag #{value}"
      end.join("\n")
    end
    File.write(File.join(@directory, 'protocol', file), "# written by runner_test.rb\n\n#{text.join("\n\n")}\n")
  end

  # A scenario whose one document tells the fake its outcome.
  def scenario(name, outcome, steps = "image judge\nspawn j judge\n")
    FileUtils.mkdir_p(File.join(@directory, 'scenarios', name))
    FileUtils.mkdir_p(File.join(@directory, 'converted', name))
    File.write(File.join(@directory, 'scenarios', name, 'steps.txt'), steps)
    document = steps[/image (\w+)/, 1]
    File.write(File.join(@directory, 'scenarios', name, "#{document}.txml"), '<scxml/>')
    File.write(File.join(@directory, 'converted', name, "#{document}.scxml"), "<scxml/><!-- outcome: #{outcome} -->")
  end

  def run_suites(suite, timeout: 10, kind: 'binding')
    setups = File.join(@directory, 'setups')
    FileUtils.mkdir_p(setups)
    File.write(File.join(setups, 'echo.scxml'), '<scxml name="echo"/>')
    runner = Runner.new(FAKE, out: @out, timeout: timeout, kind: kind, setups: setups, protocol: File.join(@directory, 'protocol'),
                              scenarios: File.join(@directory, 'scenarios'), documents: File.join(@directory, 'converted'))
    [runner.run([suite]), runner.results]
  end

  def verdicts(results) = results.to_h { |result| [result.name, result.verdict] }
end
