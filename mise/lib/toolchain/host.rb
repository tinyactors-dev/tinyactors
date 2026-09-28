# frozen_string_literal: true

require 'open3'
require 'rbconfig'

module Toolchain
  # The machine the build runs on. Host.current is chosen once, here, from
  # RbConfig; a build script asks it to do what differs between machines
  # instead of reading RUBY_PLATFORM itself
  # (mise/ast-grep/rules/host-detection.yml).
  class Host
    def self.current
      @current ||= begin
        arch = case RbConfig::CONFIG.fetch('host_cpu')
               when /\A(x86_64|amd64)\z/ then :x86_64
               when /\A(aarch64|arm64)/ then :aarch64
               else raise Error, "the build runs on x86_64 and aarch64, not #{RbConfig::CONFIG['host_cpu']}"
               end
        case RbConfig::CONFIG.fetch('host_os')
        when /darwin/ then Mac.new(arch)
        when /linux/ then Linux.new(arch)
        else raise Error, "the build runs on macOS and Linux, not #{RbConfig::CONFIG['host_os']}"
        end
      end
    end

    attr_reader :arch

    def initialize(arch)
      @arch = arch
    end

    # The release target whose programs run natively here.
    def target = Target.release("#{@arch}-#{os}")

    def to_s = target.name

    # Raises unless `target`'s programs run natively here; `why` says what
    # needs them to.
    def must_run!(target, why)
      return self if target == self.target

      raise Error, "#{why}: only on an #{target.name} host; this one is #{self.target.name}"
    end

    # Target.host: this machine, compiling with `compiler`.
    def development_target(compiler)
      Target.new(name: 'host', triple: target.triple, os: os, arch: @arch, format: target.format, compiler: compiler)
    end

    # What decides how fast the same build runs here, for a benchmark's
    # machine signature (tinyactors-napi/mise/lib/napi/machine.rb).
    def hardware = raise(NotImplementedError)

    # What can change the speed of a laptop between runs.
    def power = {}

    # Time-varying conditions, recorded with a run and never hashed.
    def conditions = raise(NotImplementedError)

    private

    def run(*command)
      output, status = Open3.capture2e(*command)
      status.success? ? output.strip : nil
    rescue SystemCallError
      nil
    end

    class Mac < Host
      def os = :macos

      def hardware
        {
          'cpu' => sysctl('machdep.cpu.brand_string'),
          'cores' => sysctl('hw.physicalcpu')&.to_i,
          'performanceCores' => sysctl('hw.perflevel0.physicalcpu')&.to_i,
          'efficiencyCores' => sysctl('hw.perflevel1.physicalcpu')&.to_i,
          'ramBytes' => sysctl('hw.memsize')&.to_i,
          'os' => "macOS #{run('sw_vers', '-productVersion')}",
          'arch' => run('uname', '-m')
        }
      end

      def power
        source = run('pmset', '-g', 'ps')&.[](/Now drawing from '([^']+)'/, 1)
        low = run('pmset', '-g')&.[](/^\s*lowpowermode\s+(\d+)/, 1)
        { 'powerSource' => source, 'lowPowerMode' => low == '1' }
      end

      def conditions
        {
          'loadAverage' => sysctl('vm.loadavg')&.scan(/[\d.]+/)&.first(3)&.map(&:to_f),
          'cpuSpeedLimit' => run('pmset', '-g', 'therm')&.[](/CPU_Speed_Limit\s*=\s*(\d+)/, 1)&.to_i
        }
      end

      private

      def sysctl(name) = run('sysctl', '-n', name)
    end

    class Linux < Host
      def os = :linux

      def hardware
        cpuinfo = read('/proc/cpuinfo')
        release = read('/etc/os-release')[/^PRETTY_NAME="?([^"\n]*)/, 1]
        {
          'cpu' => cpuinfo[/^model name\s*:\s*(.*)$/, 1],
          'cores' => run('nproc')&.to_i,
          'ramBytes' => read('/proc/meminfo')[/^MemTotal:\s*(\d+)/, 1]&.then { |kb| kb.to_i * 1024 },
          'os' => "#{release || run('uname', '-s')} (kernel #{run('uname', '-r')})",
          'arch' => run('uname', '-m')
        }
      end

      def conditions
        load = read('/proc/loadavg').split.first(3).map(&:to_f)
        { 'loadAverage' => load.empty? ? nil : load, 'cpuSpeedLimit' => nil }
      end

      private

      def read(path) = File.exist?(path) ? File.read(path) : ''
    end
  end
end
