# frozen_string_literal: true

require 'minitest/autorun'
require 'fileutils'
require 'stringio'
require 'tmpdir'
require 'mcp'

# The MCP runner (docs/mcp.md) against test/fake_mcp_server.rb, on cases
# written here: expectations that hold and that do not, matchers, saved
# values, errors, and a server that dies.
class MCPTest < Minitest::Test
  FAKE = ['ruby', File.expand_path('fake_mcp_server.rb', __dir__)].freeze

  def setup
    @suite = Dir.mktmpdir('tinyactors-test-mcp')
    FileUtils.mkdir_p(File.join(@suite, 'fixtures'))
    FileUtils.mkdir_p(File.join(@suite, 'cases'))
    File.write(File.join(@suite, 'fixtures', 'f.json'), '{"system": "s", "options": {}}')
    @out = StringIO.new
  end

  def teardown
    FileUtils.rm_rf(@suite)
  end

  def kase(name, steps)
    File.write(File.join(@suite, 'cases', "#{name}.json"), JSON.generate({ 'name' => name, 'fixture' => 'f', 'steps' => steps }))
  end

  def run_suite = MCP.run(FAKE, out: @out, timeout: 10, suite: @suite)

  def test_expectations_saved_values_and_errors_pass
    kase('ok', [
           { 'fixture' => 'spawn', 'params' => {}, 'save' => { 'actor' => 'session' } },
           { 'call' => 'echo', 'arguments' => { 'a' => { 'b' => [1, 2] }, 'who' => '$actor', 'text' => 'actor ${actor}' },
             'expect' => { 'a' => { 'b' => [1, { '$gt' => 1 }] }, 'who' => 1, 'text' => { '$match' => '^actor 1$' } } },
           { 'call' => 'refuse', 'isError' => true, 'error' => 'refused' }
         ])
    assert run_suite, @out.string
    assert_match(/^pass mcp ok$/, @out.string)
  end

  def test_a_result_that_differs_fails_and_names_its_path
    kase('wrong', [{ 'call' => 'echo', 'arguments' => { 'list' => [1, 2] }, 'expect' => { 'list' => [1] } }])
    refute run_suite
    assert_match(/fail mcp wrong: step 1 \(echo\)\.list: 2 items, not 1/, @out.string)
  end

  def test_an_error_not_expected_and_one_expected_but_missing_fail
    kase('a', [{ 'call' => 'refuse' }])
    kase('b', [{ 'call' => 'echo', 'isError' => true }])
    refute run_suite
    assert_match(/fail mcp a: step 1 \(refuse\): an error/, @out.string)
    assert_match(/fail mcp b: step 1 \(echo\): no error/, @out.string)
  end

  def test_a_server_that_dies_fails_its_case_and_the_next_runs
    kase('a', [{ 'call' => 'die' }])
    kase('b', [{ 'call' => 'echo', 'arguments' => {}, 'expect' => {} }])
    refute run_suite
    assert_match(/fail mcp a: the command ended/, @out.string)
    assert_match(/pass mcp b/, @out.string)
  end

  def test_the_catalog_step_compares_every_tool
    kase('tools', [{ 'tools' => 'catalog' }])
    refute run_suite
    assert_match(/fail mcp tools: step 1: tools \["echo"\], not/, @out.string)
  end

  def test_matchers
    MCP.match({ '$length' => 2, '$type' => 'list' }, [1, 2])
    MCP.match({ '$any' => true }, nil)
    MCP.match({ '$ge' => 3 }, 3)
    assert_raises(MCP::Mismatch) { MCP.match({ '$gt' => 3 }, 3) }
    assert_raises(MCP::Mismatch) { MCP.match({ 'k' => 1 }, {}) }
    assert_raises(MCP::Mismatch) { MCP.match({ '$nothing' => 1 }, 1) }
    assert_equal 2, MCP.dig({ 'a' => [{ 'b' => 2 }] }, 'a[0].b')
  end
end
