# frozen_string_literal: true

# What tinyactors-napi's tasks share: where things are, how a task runs a
# command, and the runtimes this project pins. M1 adds the addon's build
# here (Toolchain::Target.host for the tests, Target.release for the
# packages; mise/lib/toolchain.rb).
require 'pathname'

module NodeAPI
  ROOT = Pathname.new(__dir__).join('..').expand_path
  BUILD = ROOT.join('build')
  # Core's amalgamation, built from tinyactors-core's sources into this
  # project's own build/ (ROADMAP.md, step 8, "Rules for the work").
  CORE_BUILD = BUILD.join('core')
  # The suite's adapter, `node conform.mjs` (tinyactors-test/docs/driver.md).
  ADAPTER = ROOT.join('conform.mjs')
  # The mapping table (tinyactors-core/docs/bindings.md, section 2).
  MAPPING = ROOT.join('docs/api-map.md')
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
