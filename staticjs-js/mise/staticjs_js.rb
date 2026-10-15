# frozen_string_literal: true

# What the package's tasks share: where things are, and how a command runs.
require 'pathname'
require 'toolchain'

module StaticJSPackage
  ROOT = Pathname.new(__dir__).parent
  STATICJS = ROOT.parent.join('staticjs')
  BUILD = ROOT.join('build')
  # The module the package ships, beside index.mjs.
  WASM = ROOT.join('staticjs.wasm')

  module_function

  def run(*command, chdir: ROOT)
    pid = Process.spawn(*command.map(&:to_s), chdir: chdir.to_s)
    _pid, status = Process.wait2(pid)
    exit(status.exitstatus || 1) unless status.success?
  end
end
