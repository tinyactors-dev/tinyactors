# frozen_string_literal: true

require 'fileutils'
require 'pathname'
require_relative 'cache'

# The test binaries and their arguments, shared by the tasks that run them.
module Suite
  # name => arguments, in the order they run.
  BINARIES = {
    'header_test' => [],
    'arena_test' => [],
    'text_test' => [],
    'cursor_test' => [],
    'doc_test' => [],
    'incremental_test' => [],
    'writer_test' => [],
    'stream_test' => ['tests/spec'],
    'spec_test' => ['tests/spec']
  }.freeze

  # Tests that include private src/ headers, and so can only be linked against
  # the modules; every other test uses the public API alone and is also linked
  # against the amalgamation (mise run amalgamate-test).
  PRIVATE = %w[doc_test].freeze
  # Never linked: header_test needs no library, and its binary is built by build.
  UNLINKED = %w[header_test].freeze

  def self.public_api_tests = BINARIES.keys - PRIVATE - UNLINKED
end

module Suite
  LOGS = Cache::ROOT.join('build/logs')

  # Runs each test binary that is not already known to pass, all at once (they
  # are separate processes over separate memory), and reports in the order given.
  # `entries` is [[label, binary, arguments], ...].
  #
  # A binary is skipped when its *bytes*, its arguments and the test data
  # (tests/spec/**) are those of its last passing run; the record is written only
  # for a run that passed, and removed before one starts. A test binary that a
  # comment edit relinked to identical bytes therefore does not run again.
  # TXML_FORCE=1 runs everything. Returns the labels that failed.
  def self.run_all(entries)
    data = Cache.digest(Cache.files('tests/spec/**/*'))
    FileUtils.mkdir_p(LOGS)
    jobs = entries.map do |label, binary, arguments|
      name = "run-#{label}"
      key = Cache.digest(arguments.join(' '), data, binary)
      next { label: label, name: name, key: key, cached: true } if Cache.stamped?(name, key)

      Cache.unstamp(name)
      log = LOGS.join("#{label.tr('/', '_')}.log")
      pid = Process.spawn(binary.to_s, *arguments, chdir: Cache::ROOT.to_s, out: log.to_s, err: %i[child out])
      { label: label, name: name, key: key, pid: pid, log: log }
    end

    failed = []
    ran = 0
    reused = 0
    jobs.each do |job|
      if job[:cached]
        reused += 1
        puts "#{job[:label]}: cached (binary and test data unchanged since its last pass)"
        next
      end
      _pid, status = Process.wait2(job[:pid])
      ran += 1
      output = job[:log].read
      if status.success?
        Cache.stamp(job[:name], job[:key])
        puts output
      else
        failed << job[:label]
        warn output
        warn "#{job[:label]}: FAILED (#{status.exitstatus ? "exit #{status.exitstatus}" : "signal #{status.termsig}"})"
      end
    end
    puts "suite: #{ran} test binaries ran, #{reused} unchanged" unless entries.empty?
    failed
  end
end
