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
              ['request a notification', 'send {"id":1,"method":"echo"}', 'reply none']
            ])
    ok, results = run_suites('protocol')
    refute ok
    assert_equal({ 'requests.txt "echo"' => :pass, 'requests.txt "wrong"' => :fail,
                   'requests.txt "a notification"' => :fail }, verdicts(results))
    assert_includes @out.string, 'the reply differs at {"result"}{"n"}'
    assert_includes @out.string, 'want {"id":1,"result":{"n":1}}'
    assert_includes @out.string, 'got  {"id":1,"result":{"n":2}}'
    assert_includes @out.string, "got  #{CBOR.from_diagnostic('{"id":1,"result":{"n":2}}').unpack1('H*')}"
    assert_includes @out.string, 'a notification got a reply'
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

  def test_a_capacity_line_measures_first
    vectors('requests.txt', [['request big', 'capacity 10', 'send {"id":1,"method":"big"}', 'reply 39999("any")']])
    ok, = run_suites('protocol')
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
    ok, results = run_suites('protocol')
    assert ok
    assert_equal %i[skip skip], results.map(&:verdict)
    assert_equal 1, @out.string.scan('envelope.read is not offered').size
    assert_includes @out.string, 'not offered: envelope.read'
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
    out = StringIO.new
    runner = Runner.new(FAKE, out: out, w3c: w3c, skip: { '145' => 'a reason' })
    assert runner.run(['w3c'])
    assert_equal [%w[144 skip], %w[145 skip]], runner.results.map { |result| [result.name, result.verdict.to_s] }
    assert_includes out.string, 'w3c.run is not offered'
    assert_includes out.string, 'skip w3c 145: a reason'
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

  def run_suites(suite, timeout: 10)
    runner = Runner.new(FAKE, out: @out, timeout: timeout, protocol: File.join(@directory, 'protocol'),
                              scenarios: File.join(@directory, 'scenarios'), documents: File.join(@directory, 'converted'))
    [runner.run([suite]), runner.results]
  end

  def verdicts(results) = results.to_h { |result| [result.name, result.verdict] }
end
