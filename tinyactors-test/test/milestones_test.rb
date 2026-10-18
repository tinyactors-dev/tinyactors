# frozen_string_literal: true

require 'minitest/autorun'
require 'fileutils'
require 'open3'
require 'stringio'
require 'tmpdir'
require 'milestones'
require 'runner'

# The run task's --milestone (suites/milestones.txt): cumulative sets, the
# suite's file placing every case, and a runner that runs only the set.
class MilestonesTest < Minitest::Test
  FAKE = ['ruby', File.expand_path('fake_implementation.rb', __dir__)].freeze
  RUN = File.expand_path('../mise/tasks/run', __dir__)

  def setup
    @directory = Dir.mktmpdir('tinyactors-test-milestones')
    @out = StringIO.new
  end

  def teardown
    FileUtils.rm_rf(@directory)
  end

  def test_a_milestone_runs_its_lines_and_those_above_it
    file = milestones("A protocol requests\nA scenarios basics\nB w3c\nC scenarios invoke\nC protocol values\nD\n")
    assert_equal %w[A B C D], Milestones.names(file)
    a = Milestones.select('A', file)
    assert_equal [%w[protocol scenarios], %w[requests], %w[basics]], [a.suites, a.protocol, a.scenarios]
    b = Milestones.select('B', file)
    assert_equal %w[protocol scenarios w3c], b.suites
    assert_equal %w[basics], b.scenarios
    c = Milestones.select('C', file)
    assert_equal [%w[requests values], %w[basics invoke]], [c.protocol, c.scenarios]
    assert_equal c.to_h.except(:milestone), Milestones.select('D', file).to_h.except(:milestone)
    assert_equal 'A: protocol (requests), scenarios (basics)', a.to_s
  end

  def test_unknown_milestones_and_bad_lines
    file = milestones("A protocol requests\n")
    error = assert_raises(Milestones::Error) { Milestones.select('M9', file) }
    assert_includes error.message, 'no milestone "M9"'
    assert_raises(Milestones::Error) { Milestones.read(milestones("A effects x\n")) }
    assert_raises(Milestones::Error) { Milestones.read(milestones("A scenarios\n")) }
    assert_raises(Milestones::Error) { Milestones.read(milestones("A w3c 144\n")) }
  end

  def test_the_suites_file_places_every_case_once_it_is_reached
    assert_empty Milestones.problems(Milestones::FILE, protocol: Runner::PROTOCOL, scenarios: Scenarios::ROOT)
    assert_equal %w[M1 M2 M3 M4 M5], Milestones.names
    m1 = Milestones.select('M1')
    assert_equal %w[protocol scenarios], m1.suites
    assert_equal %w[requests config values], m1.protocol
    assert_equal %w[basics parallel-history send-errors outcome-compile outcome-fail outcome-timeout], m1.scenarios
    assert_includes Milestones.select('M2').suites, 'w3c'
  end

  def test_a_case_in_no_milestone_and_a_name_not_in_the_suite
    FileUtils.mkdir_p(File.join(@directory, 'protocol'))
    File.write(File.join(@directory, 'protocol/requests.txt'), '')
    File.write(File.join(@directory, 'protocol/values.txt'), '')
    %w[basics invoke].each { |name| scenario(name, 'pass') }
    file = milestones("A protocol requests\nA scenarios basics gone\n")
    problems = Milestones.problems(file, protocol: File.join(@directory, 'protocol'),
                                         scenarios: File.join(@directory, 'scenarios'))
    assert_equal ['protocol values: in no milestone', 'scenarios gone: not in suites/scenarios',
                  'scenarios invoke: in no milestone'], problems.map { |problem| problem.sub(/ \(.*\)\z/, '') }.sort
  end

  def test_the_runner_runs_only_the_milestones_cases
    scenario('basics', 'pass')
    scenario('invoke', 'fail')
    vectors('requests.txt', "# r\n\nrequest echo\nsend #{hex('{"id":1,"method":"echo","params":[]}')}\n" \
                            "reply #{hex('{"id":1,"result":[]}')}\n")
    vectors('values.txt', "# v\n\nroundtrip same\ncbor #{hex('[1]')}\n")
    file = milestones("A protocol requests\nA scenarios basics\nB scenarios invoke\nB protocol values\n")
    runner = runner(Milestones.select('A', file))
    assert runner.run(%w[protocol scenarios]), @out.string
    assert_equal [%w[protocol requests.txt "echo"], %w[scenarios basics]],
                 runner.results.map { |result| [result.suite, *result.name.split(' ', 2)] }
    everything = runner(Milestones.select('B', file))
    refute everything.run(%w[protocol scenarios])
    assert_equal %w[basics invoke], everything.results.select { |result| result.suite == 'scenarios' }.map(&:name)
  end

  def test_the_run_task_refuses_a_suite_outside_the_milestone
    out, status = Open3.capture2e('ruby', RUN, '--milestone', 'M1', '--suite', 'w3c', *FAKE)
    refute status.success?
    assert_includes out, 'M1 does not run w3c'
    out, status = Open3.capture2e('ruby', RUN, '--milestone', 'M0', *FAKE)
    refute status.success?
    assert_includes out, 'no milestone "M0"'
  end

  private

  def milestones(text)
    path = File.join(@directory, "milestones-#{text.hash.abs}.txt")
    File.write(path, "# a test's milestones\n\n#{text}")
    path
  end

  def hex(diagnostic) = CBOR.from_diagnostic(diagnostic).unpack1('H*')

  def vectors(file, text)
    FileUtils.mkdir_p(File.join(@directory, 'protocol'))
    File.write(File.join(@directory, 'protocol', file), text)
  end

  # A scenario whose one document tells the fake its outcome.
  def scenario(name, outcome)
    FileUtils.mkdir_p(File.join(@directory, 'scenarios', name))
    FileUtils.mkdir_p(File.join(@directory, 'converted', name))
    File.write(File.join(@directory, 'scenarios', name, 'steps.txt'), "image judge\nspawn j judge\n")
    File.write(File.join(@directory, 'scenarios', name, 'judge.txml'), '<scxml/>')
    File.write(File.join(@directory, 'converted', name, 'judge.scxml'), "<scxml/><!-- outcome: #{outcome} -->")
  end

  def runner(only)
    setups = File.join(@directory, 'setups')
    FileUtils.mkdir_p(setups)
    File.write(File.join(setups, 'echo.scxml'), '<scxml name="echo"/>')
    Runner.new(FAKE, out: @out, setups: setups, protocol: File.join(@directory, 'protocol'),
                     scenarios: File.join(@directory, 'scenarios'), documents: File.join(@directory, 'converted'),
                     only: only)
  end
end
