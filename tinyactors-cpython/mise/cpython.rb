# frozen_string_literal: true

# What tinyactors-cpython's tasks share: where things are, how a task runs
# a command, and the interpreters this project pins. M1 adds the
# extension's build here (Toolchain::Target.host for the tests,
# Target.release for the wheel; mise/lib/toolchain.rb).
require 'pathname'

module CPython
  ROOT = Pathname.new(__dir__).join('..').expand_path
  BUILD = ROOT.join('build')
  # Core's amalgamation, built from tinyactors-core's sources into this
  # project's own build/ (ROADMAP.md, step 8, "Rules for the work").
  CORE_BUILD = BUILD.join('core')
  # The suite's adapter, `python -m conform` (tinyactors-test/docs/driver.md).
  ADAPTER = ROOT.join('conform')
  # The mapping table (tinyactors-core/docs/bindings.md, section 2).
  MAPPING = ROOT.join('docs/api-map.md')

  module_function

  # Runs a command in the project's directory; a failure ends the task with
  # its status.
  def run(*command, env: {})
    pid = Process.spawn(env, *command, chdir: ROOT.to_s)
    _pid, status = Process.wait2(pid)
    exit(status.exitstatus || 1) unless status.success?
  end

  # The path of a program this project's mise.toml pins (python, uv). A
  # command another project's task runs gets it by path: that project's
  # pins come first on its PATH (tinyactors-test pins python 3.13).
  def pinned(program)
    path = IO.popen(['mise', 'which', program], chdir: ROOT.to_s, err: File::NULL, &:read).strip
    abort "#{program} is not installed: run `mise install` in #{ROOT}" if path.empty?
    path
  end

  # The free-threaded interpreter of mise.toml's TA_PYTHON_FREE_THREADED,
  # installed by the pinned uv when it is missing.
  def free_threaded_python
    request = ENV.fetch('TA_PYTHON_FREE_THREADED')
    uv = pinned('uv')
    run(uv, 'python', 'install', '--quiet', request)
    path = IO.popen([uv, 'python', 'find', '--managed-python', request], chdir: ROOT.to_s, &:read).strip
    abort "uv found no interpreter for #{request}" if path.empty?
    path
  end
end
