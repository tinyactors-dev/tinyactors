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

    # Target.host: this machine, compiling with `compiler` against its C
    # library.
    def development_target(compiler)
      must_have_c_library!
      Target.new(name: 'host', triple: target.triple, os: os, arch: @arch, format: target.format, compiler: compiler)
    end

    private

    # The pinned clang builds this machine's programs against its C library.
    def must_have_c_library! = self

    def run(*command)
      output, status = Open3.capture2e(*command)
      status.success? ? output.strip : nil
    rescue SystemCallError
      nil
    end

    class Mac < Host
      def os = :macos

      private

      # The pinned clang reads the macOS SDK of the Command Line Tools.
      def must_have_c_library!
        return self if run('xcrun', '--sdk', 'macosx', '--show-sdk-path')&.then { |path| File.directory?(path) }

        raise Error, 'the pinned clang builds against the macOS SDK, which is missing: run `xcode-select --install`'
      end
    end

    class Linux < Host
      def os = :linux
    end
  end
end
