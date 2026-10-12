# frozen_string_literal: true

# What `mise run bench-engines` needs besides StaticJS::Bench: the dump
# helper built from bench/engines/dump.c, the pinned quickjs-wasi installed
# from bench/engines/package-lock.json, child processes under a timeout,
# and where the records and the report go (bench/results/README.md).
require 'fileutils'
require 'json'
require 'timeout'
require_relative 'bench'

module StaticJS
  module Engines
    DIR = ROOT.join('bench', 'engines')
    BUILD = StaticJS::BUILD.join('engines')
    DUMP = BUILD.join('dump')
    # The stable path of the latest stored report.
    LATEST = DIR.join('REPORT.md')

    module_function

    # Builds bench/engines/dump.c (which includes bench/bench.c) with the
    # release bench's flags against build/release/libstaticjs.a, which
    # StaticJS::Bench.build must have built.
    def build_dump
      FileUtils.mkdir_p(BUILD)
      command = [*Bench.target.compile_command, *WARNINGS, *Bench::RELEASE, "-I#{ROOT.join('include')}"]
      object = BUILD.join('dump.o')
      outcome, output = Cache.compile(command, DIR.join('dump.c').expand_path, object)
      abort "staticjs: bench/engines/dump.c does not build against bench/bench.c:\n#{output}" if outcome == :failed

      Bench.target.program([object.to_s, Bench::RELEASE_BUILD.join('libstaticjs.a').to_s], into: DUMP.to_s)
      DUMP
    end

    # The version package-lock.json pins for `name`, and the one installed.
    def pinned(name) = JSON.parse(DIR.join('package-lock.json').read).dig('packages', "node_modules/#{name}", 'version')

    def installed(name)
      file = DIR.join('node_modules', name, 'package.json')
      file.exist? ? JSON.parse(file.read)['version'] : nil
    end

    # Installs the locked packages into bench/engines/node_modules/ (ignored
    # by git) unless they are there already: the first run needs the
    # network, later ones do not.
    def install
      return if installed('quickjs-wasi') == pinned('quickjs-wasi')

      puts "staticjs: installing quickjs-wasi #{pinned('quickjs-wasi')} (npm ci in bench/engines/)"
      run!(%w[npm ci --prefer-offline --no-audit --no-fund], timeout: 600, chdir: DIR)
      abort 'staticjs: npm ci did not install the pinned quickjs-wasi' unless installed('quickjs-wasi') == pinned('quickjs-wasi')
    end

    # Runs a command, its output passing through, and returns whether it
    # succeeded; kills its process group and fails after `timeout` seconds.
    def run(command, timeout:, chdir: ROOT, out: $stdout)
      pid = Process.spawn(*command.map(&:to_s), chdir: chdir.to_s, out: out, pgroup: true)
      begin
        _pid, status = Timeout.timeout(timeout) { Process.wait2(pid) }
        status.success?
      rescue Timeout::Error
        Process.kill('KILL', -pid)
        Process.wait(pid)
        warn "staticjs: #{command.first(3).join(' ')} ... timed out after #{timeout} s"
        false
      end
    end

    def run!(command, timeout:, chdir: ROOT, out: $stdout)
      abort "staticjs: #{command.join(' ')} failed" unless run(command, timeout: timeout, chdir: chdir, out: out)
    end

    def results(signature) = Bench::RESULTS.join(signature['hash'])
  end
end
