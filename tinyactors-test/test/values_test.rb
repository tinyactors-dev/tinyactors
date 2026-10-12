# frozen_string_literal: true

require 'minitest/autorun'
require 'open3'
require 'vectors'

# values.txt's `python` lines: no runner method reads them (value.encode is
# ECMAScript's), so they are checked here: each belongs to an encode or
# invalid case beside its `ecmascript` line, and compiles as a Python
# expression (the pinned python, mise.toml).
class ValuesTest < Minitest::Test
  VALUES = File.expand_path('../suites/protocol/values.txt', __dir__)

  def cases = Vectors.read(VALUES).select { |vector| vector.key?('python') }

  def test_python_lines_sit_beside_an_ecmascript_line
    refute_empty cases
    cases.each do |vector|
      assert_includes %w[encode invalid], vector.kind, "#{vector}: a python line in a #{vector.kind} case"
      assert vector.key?('ecmascript'), "#{vector}: a python line without an ecmascript line"
    end
  end

  def test_python_lines_compile
    source = cases.map { |vector| vector.get('python') }
    check = 'import sys; [compile(line, "<values.txt>", "eval") for line in sys.stdin.read().splitlines()]'
    output, status = Open3.capture2e('python3', '-I', '-c', check, stdin_data: source.join("\n"))
    assert status.success?, output
  end
end
