# frozen_string_literal: true

# What `mise run bench` and `mise run bench-compare` share: the optimized
# build of the library and of bench/bench.c, the machine signature, running
# the program, and storing and printing its records (bench/results/README.md).
# Every record carries throughput and memory together; a report never gives
# one without the other.
require 'cache'
require 'digest'
require 'fileutils'
require 'json'
require 'open3'
require 'time'
require_relative 'staticjs'

module StaticJS
  module Bench
    # The optimization flags of a release build (Toolchain::Target::Zig#flags),
    # with the pinned clang against this machine's C library: zig's musl
    # malloc would dominate the cases that allocate, and differ between
    # Linux and macOS. Never -flto: every call into the library stays opaque.
    RELEASE = %w[-O2 -fno-stack-protector -fvisibility=hidden].freeze
    RELEASE_BUILD = BUILD.join('release')
    RESULTS = ROOT.join('bench', 'results')
    PROGRAM = ROOT.join('bench', 'bench.c')
    # What a build of the bench depends on, for `dirty?`.
    MEASURED = %w[staticjs/src staticjs/include staticjs/bench/bench.c staticjs/bench/modules].freeze

    module_function

    def target = Toolchain::Target.host

    # Compiles source_root/src/*.c optimized into into/objects/, archives
    # them into into/libstaticjs.a, and links into/bench from `bench_source`.
    # Returns nil when it builds, or the compiler's output when bench_source
    # does not compile against source_root's staticjs.h.
    def build(source_root:, bench_source:, into:)
      source_root = Pathname.new(source_root)
      into = Pathname.new(into)
      objects = into.join('objects')
      FileUtils.mkdir_p(objects)
      includes = ["-I#{source_root.join('include')}", "-I#{source_root.join('src')}"]
      library_command = [*target.compile_command, *WARNINGS, *FREESTANDING, *RELEASE, *includes].freeze
      library = PlatformFiles.sources(source_root.join('src')).map do |path|
        source = Pathname.new(path)
        object = objects.join("#{source.basename('.c')}.o")
        compile!(library_command, source, object)
        object.to_s
      end
      archive = into.join('libstaticjs.a').to_s
      FileUtils.rm_f(archive)
      target.archive(library, into: archive)
      bench_command = [*target.compile_command, *WARNINGS, *RELEASE, "-I#{source_root.join('include')}"].freeze
      bench_object = objects.join('bench.o')
      outcome, output = Cache.compile(bench_command, Pathname.new(bench_source).expand_path, bench_object)
      return output if outcome == :failed

      target.program([bench_object.to_s, archive], into: into.join('bench').to_s)
      nil
    end

    def compile!(command, source, object)
      outcome, output = Cache.compile(command, source.expand_path, object)
      abort "staticjs: compiling #{source} failed:\n#{output}" if outcome == :failed
      puts output unless output.empty?
    end

    # { 'hash' => 12 hex digits, 'fields' => { ... } }: the machine as
    # Toolchain::Host describes it, with the compiler and the flags.
    def signature
      host = Toolchain::Host.current
      fields = host.hardware.merge(host.power).merge('compiler' => Toolchain::Tool[:clang].identity,
                                                     'flags' => RELEASE.join(' ')).sort.to_h
      { 'hash' => Digest::SHA256.hexdigest(JSON.generate(fields))[0, 12], 'fields' => fields }
    end

    # Time-varying conditions, recorded with each run and never hashed.
    def conditions = Toolchain::Host.current.conditions

    def git(*args)
      output, status = Open3.capture2('git', *args, chdir: ROOT.to_s)
      status.success? ? output.strip : nil
    end

    def repository = git('rev-parse', '--show-toplevel')

    def commit = git('rev-parse', '--short=7', 'HEAD')

    def dirty? = !git('-C', repository, 'status', '--porcelain', '--', *MEASURED).to_s.empty?

    # Runs `binary` with `args` from staticjs/ (it reads bench/modules/
    # there, so both sides of a comparison read the same modules) and
    # returns { 'meta', 'records', 'status' }. Its stderr passes through.
    def run(binary, args)
      output, status = Open3.capture2(binary.to_s, '--jsonl', *args, chdir: ROOT.to_s)
      lines = output.lines.filter_map do |line|
        JSON.parse(line)
      rescue JSON::ParserError
        nil
      end
      meta = lines.find { |line| line.key?('meta') }&.fetch('meta')
      { 'meta' => meta, 'records' => lines.reject { |line| line.key?('meta') }, 'status' => status.exitstatus }
    end

    # The names and sizes `binary --list` gives: { name => [n, ...] }.
    def list(binary, args = [])
      output, status = Open3.capture2(binary.to_s, '--list', *args, chdir: ROOT.to_s)
      return nil unless status.success?

      output.lines.to_h do |line|
        name, _unit, sizes = line.chomp.split("\t")
        [name, sizes.to_s.split(',').map(&:to_i)]
      end
    end

    # One stored record per (case, n): the signature, the build, the
    # conditions around the run, and the program's throughput and memory.
    def records(result, signature:, commit:, dirty:, before:, after:)
      time = Time.now.utc.iso8601
      result['records'].map do |record|
        {
          'machine' => signature['hash'],
          'commit' => commit,
          'dirty' => dirty,
          'time' => time,
          'compiler' => signature['fields']['compiler'],
          'flags' => signature['fields']['flags'],
          'version' => result.dig('meta', 'version'),
          'conditionsBefore' => before,
          'conditionsAfter' => after
        }.merge(record)
      end
    end

    # Appends records to bench/results/<signature>/<commit>.jsonl, and
    # writes the signature's readable fields beside them once.
    def store(records, signature)
      dir = RESULTS.join(signature['hash'])
      FileUtils.mkdir_p(dir)
      machine = dir.join('machine.json')
      File.write(machine, "#{JSON.pretty_generate(signature['fields'])}\n") unless machine.exist?
      records.group_by { |record| record['commit'] }.each do |commit, group|
        File.open(dir.join("#{commit}.jsonl"), 'a') { |file| group.each { |record| file.puts(JSON.generate(record)) } }
      end
      dir
    end

    UNITS = { 'byte' => 'MB/s', 'eval' => 'M eval/s', 'element' => 'M elem/s', 'op' => 'M op/s' }.freeze

    # A number with thousands separators, or "-" when absent.
    def num(value, digits = 0)
      return '-' if value.nil?

      whole, fraction = format("%.#{digits}f", value).split('.')
      sign = whole.start_with?('-') ? '-' : ''
      grouped = whole.delete_prefix('-').reverse.scan(/\d{1,3}/).join(',').reverse
      fraction ? "#{sign}#{grouped}.#{fraction}" : "#{sign}#{grouped}"
    end

    # About four significant digits, with k, M or G.
    def short(value)
      return '-' if value.nil?

      scaled, suffix = if value >= 1e9 then [value / 1e9, ' G']
                       elsif value >= 1e6 then [value / 1e6, ' M']
                       elsif value >= 1e4 then [value / 1e3, ' k']
                       else [value, '']
                       end
      text = if scaled >= 100 then format('%.1f', scaled)
             elsif scaled >= 1 then format('%.2f', scaled)
             else format('%.3g', scaled)
             end
      "#{text}#{suffix}"
    end

    def throughput(record)
      return nil if record['unitsPerSecond'].nil?

      "#{short(record['unitsPerSecond'] / 1e6)} #{UNITS.fetch(record['unit'], record['unit'])}"
    end

    COLUMNS = [
      ['n', ->(r) { num(r['n']) }],
      ['throughput', ->(r) { Bench.throughput(r) }],
      ['ops/s', ->(r) { Bench.short(r['opsPerSecond']) }],
      ['ns/unit', ->(r) { Bench.num(r.dig('nsPerOp', 'median').to_f / [r['unitsPerOp'].to_f, 1e-9].max, 2) }],
      ['spread', ->(r) { format('%.1f%%', r['spreadPercent']) }],
      ['fuel/op', ->(r) { Bench.num(r['fuelPerOp']) }],
      ['peak B', ->(r) { Bench.num(r.dig('memory', 'peakBytes')) }],
      ['alloc B/op', ->(r) { Bench.num(r.dig('memory', 'allocatedBytesPerOp')) }],
      ['allocs/op', ->(r) { Bench.num(r.dig('memory', 'allocsPerOp')) }],
      ['held B', ->(r) { Bench.num(r.dig('memory', 'heldBytes')) }]
    ].freeze

    # Throughput and memory side by side, one row per (case, n).
    def table(records)
      rows = records.map do |record|
        next [record['bench'], num(record['n']), "could not run: #{record['error']}"] if record['error']

        [record['bench'], *COLUMNS.map { |_, value| value.call(record) }]
      end
      layout(['benchmark', *COLUMNS.map(&:first)], rows)
    end

    # Columns padded to their widest cell: the first and a row's last
    # message left-aligned, numbers right-aligned. A row with fewer cells
    # than the header ends in a message, which is not padded.
    def layout(header, rows)
      widths = header.each_index.map do |at|
        ([header] + rows.select { |row| row.length == header.length }).map { |row| row[at].to_s.length }.max
      end
      [header, *rows].map do |row|
        row.each_with_index.map do |cell, at|
          if at.zero? then cell.to_s.ljust(widths[at])
          elsif row.length < header.length && at == row.length - 1 then cell.to_s
          else cell.to_s.rjust(widths[at])
          end
        end.join('  ').rstrip
      end.join("\n")
    end

    # "machine 3f9c..., (cpu, 8 cores, os), clang 22.1.x, -O2 ...": who
    # measured, for a report's header.
    def describe(signature)
      fields = signature['fields']
      "machine #{signature['hash']} (#{fields['cpu']}, #{fields['cores']} cores, #{fields['os']}), " \
        "clang #{fields['compiler'].to_s[/\d+\.\d+(\.\d+)?/]}, #{fields['flags']}"
    end
  end
end
