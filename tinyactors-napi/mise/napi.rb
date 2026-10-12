# frozen_string_literal: true

# What tinyactors-napi's tasks share: where things are, how a task runs a
# command, and the runtimes this project pins. The addon's build belongs
# here too (Toolchain::Target.host for the tests, Target.release for the
# packages; mise/lib/toolchain.rb).
require 'pathname'

module NodeAPI
  ROOT = Pathname.new(__dir__).join('..').expand_path
  BUILD = ROOT.join('build')
  # Core's amalgamation, built from tinyactors-core's sources into this
  # project's own build/ (AGENTS.md, "Tasks").
  CORE_BUILD = BUILD.join('core')
  # The suite's adapter, `node conform.mjs` (tinyactors-test/docs/driver.md).
  ADAPTER = ROOT.join('conform.mjs')
  # The mapping table (tinyactors-core/docs/bindings.md, section 2).
  MAPPING = ROOT.join('docs/api-map.md')
  # The suite's set `mise run test` runs (`--milestone`, a set of
  # tinyactors-test/suites/milestones.txt), named here and nowhere else.
  # Which set the binding is held to now is in ROADMAP.md.
  MILESTONE = 'M1'
  # The runtimes the package serves, each running the same addon.
  RUNTIMES = %w[node bun].freeze

  module_function

  # Runs a command in the project's directory; a failure ends the task with
  # its status.
  def run(*command, env: {})
    pid = Process.spawn(env, *command, chdir: ROOT.to_s)
    _pid, status = Process.wait2(pid)
    exit(status.exitstatus || 1) unless status.success?
  end

  # The path of a program this project's mise.toml pins (node, bun, pnpm).
  # A command another project's task runs gets it by path: that project's
  # pins come first on its PATH.
  def pinned(program)
    path = IO.popen(['mise', 'which', program], chdir: ROOT.to_s, err: File::NULL, &:read).strip
    abort "#{program} is not installed: run `mise install` in #{ROOT}" if path.empty?
    path
  end
end
