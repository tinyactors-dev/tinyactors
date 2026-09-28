# frozen_string_literal: true

require 'digest'
require 'json'
require 'open3'
require 'toolchain'

# The machine a benchmark ran on, as a signature: the fields that decide how
# fast the same build runs, and a short hash of them that keys stored
# results (bench/results/<hash>/). The same build measured about 30% apart
# on two days on one machine, so figures are only comparable under one
# signature, and best side by side (`mise run bench-compare`).
#
# Hashed: CPU model, core counts (performance and efficiency where the OS
# says), RAM, OS and version, architecture, the runtime and its version,
# and on macOS the power source and low-power mode. Not hashed, recorded per
# run: load average and thermal state (NapiMachine.conditions).
module NapiMachine
  module_function

  # The runtime's version, as `mise exec` resolves it in `dir`.
  def runtime_version(runtime, dir)
    output, status = Open3.capture2e('mise', 'exec', '--', runtime, '--version', chdir: dir.to_s)
    status.success? ? output.strip : nil
  rescue SystemCallError
    nil
  end

  # { 'hash' => 12 hex digits, 'fields' => { ... } } for `runtime` in `dir`.
  # What this machine reports is Toolchain::Host's to find out.
  def signature(runtime, dir)
    host = Toolchain::Host.current
    fields = host.hardware.merge(host.power).merge('runtime' => runtime, 'runtimeVersion' => runtime_version(runtime, dir))
    canonical = JSON.generate(fields.sort.to_h)
    { 'hash' => Digest::SHA256.hexdigest(canonical)[0, 12], 'fields' => fields.sort.to_h }
  end

  # Time-varying conditions, recorded with each run and never hashed.
  def conditions = Toolchain::Host.current.conditions
end
