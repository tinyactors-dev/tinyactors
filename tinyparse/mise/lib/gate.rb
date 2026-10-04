# frozen_string_literal: true

require 'etc'
require 'open3'
require 'pathname'
require 'tmpdir'
require_relative 'tinyparse'

module Tinyparse
  # A gate checks one property of tinyparse across many inputs or builds: a
  # differential (two engines build the same tree), a crash bound, an export
  # surface. Each is a file task in mise/tasks/gate/ that `mise run test`
  # depends on, written as
  #
  #   Tinyparse::Gate.run('cc-diff', 'compiled-parser differential') do |gate|
  #     gate.compare(...)            # counts a match or prints the mismatch
  #     gate.check(ok, 'what failed') # a property that must hold
  #   end
  #
  # A gate works in its own temporary directory (gate.scratch) and writes
  # nothing else, so gates run side by side. `--full` (which `mise run test
  # --full` passes) asks for the exhaustive version; the fast one checks the
  # same property on fewer or smaller inputs. The gate prints RESULT=m/n
  # (the comparisons that matched) and exits 0 only when nothing failed.
  class Gate
    # One run of a program: its output, and how it ended.
    Run = Struct.new(:out, :err, :status) do
      def success? = status.success?

      # The exit code as a shell reports it: 128 + N for a death by signal N.
      def code = status.exitstatus || (128 + status.termsig.to_i)
      def signaled? = status.signaled?
    end

    def self.run(name, title)
      gate = new(name)
      puts "== tinyparse #{title}#{gate.full? ? ' (--full)' : ''} =="
      Dir.mktmpdir("tinyparse-#{name}-") do |dir|
        gate.scratch = Pathname.new(dir)
        begin
          yield gate
        rescue Toolchain::Error => e
          gate.fail(e.message)
        end
      end
      gate.finish
    end

    attr_accessor :scratch
    attr_reader :name

    def initialize(name)
      @name = name
      @full = ARGV.include?('--full')
      @failures = 0
      @total = 0
      @matched = 0
      @lock = Mutex.new
      @parsers = {}
    end

    def full? = @full
    def failed? = @failures.positive?

    # --- what fails a gate ------------------------------------------------

    def fail(message)
      @lock.synchronize do
        @failures += 1
        puts "  FAIL: #{message}"
      end
      false
    end

    def ok(message)
      @lock.synchronize { puts "  ok: #{message}" }
      true
    end

    # Fails with `message` unless `condition`; answers the condition.
    def check(condition, message) = condition ? true : fail(message)

    # Counts one comparison of `expected` and `got` (each a Run, or a
    # string): it matches when their stdout, their stderr (for a Run made
    # with `separate: true`) and their exit codes are the same.
    def compare(label, expected, got)
      same = parts(expected) == parts(got)
      @lock.synchronize do
        @total += 1
        @matched += 1 if same
      end
      return true if same

      fail("#{label}: differs (exit #{code(expected)} expected, #{code(got)} got)\n#{Gate.difference(output(expected), output(got))}")
    end

    def counts = [@matched, @total]

    def result(label = nil)
      puts "RESULT#{label ? "[#{label}]" : ''}=#{@matched}/#{@total}"
      @matched = @total = 0
    end

    def finish
      if failed?
        puts "== #{@name}: RED (#{@failures} failure#{@failures == 1 ? '' : 's'}) =="
        exit 1
      end
      puts "== #{@name}: GREEN =="
      exit 0
    end

    # The first lines where `expected` and `got` differ, for a report.
    def self.difference(expected, got, context: 20)
      a = expected.to_s.lines
      b = got.to_s.lines
      first = (0...[a.size, b.size].max).find { |index| a[index] != b[index] } || 0
      lines = ["      first difference at line #{first + 1}:"]
      a[first, context].to_a.each { |line| lines << "      - #{line.chomp}" }
      b[first, context].to_a.each { |line| lines << "      + #{line.chomp}" }
      lines.join("\n")
    end

    # --- running ----------------------------------------------------------

    # Runs `command`, stdout and stderr in one stream as `2>&1` gives them
    # (`separate: true` keeps them apart in Run#out and Run#err). `stdin` is
    # a file to read from; `stack` limits the C stack, in bytes.
    def execute(command, env: {}, stdin: nil, separate: false, stack: nil)
      options = { binmode: true, chdir: ROOT.to_s }
      options[:stdin_data] = stdin ? Pathname.new(stdin).binread : ''
      options[:rlimit_stack] = stack if stack
      command = command.map(&:to_s)
      if separate
        out, err, status = Open3.capture3(env, *command, **options)
      else
        out, status = Open3.capture2e(env, *command, **options)
        err = ''
      end
      Run.new(out, err, status)
    end

    # The built CLI, with `args`.
    def cli(*args, **options) = execute([Tinyparse.host_cli, *args], **options)

    # The CLI's tree of `input` (a file) under `grammar`.
    def vm_tree(grammar, input, *extra, **options) = cli('run', grammar, '--input', input, '--dump-tree', *extra, **options)

    # Runs the CLI and raises on a failure, answering its output.
    def cli!(*args)
      run = cli(*args)
      raise Toolchain::Tool::Failed, "tinyparse #{args.join(' ')} failed (exit #{run.code}):\n#{run.out}" unless run.success?

      run.out
    end

    # A standalone parser generated from `grammar` (a path or a package name)
    # with `tinyparse c --parser`, built for this machine against the
    # library; one per grammar per gate. The generated translation unit is
    # machine output that includes src/internal.h, so it is compiled without
    # the warning flags, as `mise run examples` compiles it.
    def parser(grammar)
      key = grammar.to_s.delete_prefix("#{ROOT}/").gsub(/[^A-Za-z0-9.]+/, '_')
      @lock.synchronize { @parsers[key] ||= Mutex.new }.synchronize do
        binary = scratch.join("parser_#{key}")
        return binary if binary.file?

        source = scratch.join("parser_#{key}.c")
        cli!('c', grammar, '--parser', '--out', source)
        program([source, Tinyparse.host_library], binary)
      end
    end

    # A program for this machine of `inputs` (C files, objects, archives),
    # with the public header and src/ on the include path.
    def program(inputs, binary, flags: [])
      Tinyparse.host.program(inputs.map(&:to_s), into: binary.to_s,
                                                 flags: [*COMPILED_FLAGS, *flags])
      binary
    end

    COMPILED_FLAGS = ['-std=c11', '-O2', "-I#{ROOT.join('include')}", "-I#{ROOT.join('src')}"].freeze

    # Runs the block for each item, as many at once as this machine has
    # processors; answers the block's results in the order of `items`.
    def each_parallel(items)
      queue = Queue.new
      items.each_with_index { |item, index| queue << [item, index] }
      queue.close
      results = Array.new(items.size)
      errors = []
      Array.new([Etc.nprocessors, items.size].min) do
        Thread.new do
          while (entry = queue.pop)
            begin
              results[entry[1]] = yield entry[0]
            rescue Toolchain::Error => e
              @lock.synchronize { errors << e }
            end
          end
        end
      end.each(&:join)
      errors.each { |error| fail(error.message) }
      results
    end

    # The global symbols a program defines, as C names, from its full
    # symbol table. (Toolchain::Binary#exports reads a position-independent
    # executable's dynamic table, which lists none of its own functions.)
    def defined_symbols(program)
      format = Toolchain::Binary.new(program).format
      Toolchain::Tool[:llvm_nm].run('-g', '--defined-only', '--format=just-symbols', program.to_s)
                              .lines.map(&:strip).reject(&:empty?).map { |symbol| format.c_name(symbol) }
    end

    # Writes `bytes` to a file in the scratch directory; answers its path.
    def write(name, bytes)
      path = scratch.join(name)
      path.dirname.mkpath
      path.binwrite(bytes)
      path
    end

    private

    def parts(run) = run.is_a?(Run) ? [run.out, run.err, run.code] : [run.to_s, '', 0]
    def output(run) = parts(run).first(2).join
    def code(run) = parts(run).last
  end
end

module Tinyparse
  # The inputs the differential gates (vm-diff, cc-diff) run both engines
  # on.
  module Corpus
    module_function

    # A spread of the json family, by grammar: scalars, nesting, escapes,
    # the empty cases, each dialect's own features (comments in jsonc;
    # identifier keys and trailing commas in json5), and inputs that must
    # fail the same way in both engines. There is no committed json corpus.
    JSON_FAMILY = {
      'std.json' => {
        'scalar_int' => '123', 'scalar_neg' => '-42', 'scalar_float' => '3.14', 'scalar_exp' => '-1.5e3',
        'scalar_exp2' => '6.022E+23', 'scalar_true' => 'true', 'scalar_false' => 'false', 'scalar_null' => 'null',
        'scalar_string' => '"hello world"', 'string_escape' => '"a\"b\\\\c\n\t"', 'empty_object' => '{}',
        'empty_array' => '[]', 'array_simple' => '[1,2,3]', 'array_mixed' => '[1,true,null,"x",[2,3]]',
        'object_simple' => '{"a":1}', 'object_multi' => '{"a":1,"b":2,"c":3}',
        'nested' => '{"a":[1,{"b":[2,3]},true],"c":{"d":null}}',
        'whitespace' => '  {  "a" : [ 1 , 2 ]  ,  "b" : true  }  ', 'deep' => '[[[[[[[1]]]]]]]',
        'bignum' => '123456789012345678901234567890',
        'err_trailing' => '1 2', 'err_unterminated' => '{"a":', 'err_garbage' => '@@@', 'err_empty' => ''
      },
      'std.jsonc' => {
        'line_comment' => "{\"a\":1 // trailing\n}", 'block_comment' => '{"a":/* note */1}',
        'lead_comment' => "// header\n{\"a\":1}", 'comment_array' => '[1, /* two */ 2, 3] // done',
        'plain_json' => '{"a":[1,2,3]}', 'err_open_block' => '{"a":/* unterminated 1}'
      },
      'std.json5' => {
        'ident_key' => '{a:1}', 'ident_keys' => '{a:1,b:2,c:3}', 'trailing_comma' => '{a:1,}',
        'trailing_array' => '{a:[1,2,3],}', 'mixed_keys' => '{"q":1, x:2, _y:3, $z:4}', 'dollar_ident' => '{$id:1}',
        'nested_json5' => '{a:{b:1,},c:[1,2,],}', 'err_bad_key' => '{1:2}'
      }
    }.freeze

    # The committed corpora of the suite, by family: each input names its
    # grammar by extension.
    FAMILIES = { 'jsfamily' => 'std.javascript', 'tsfamily' => 'std.typescript', 'shellfamily' => 'std.shell' }.freeze
    EXTENSIONS = { '.tsx' => 'std.tsx', '.ts' => 'std.typescript', '.js' => 'std.javascript' }.freeze

    # [grammar, path] for every input of a family's corpus or, unless `all`,
    # the first two of each extension, which the fast pass runs.
    def family(name, all:)
      inputs = Dir[ROOT.join("test/suite/#{name}/inputs/*").to_s].sort.select { |path| File.file?(path) }
      inputs = inputs.group_by { |path| File.extname(path) }.values.flat_map { |paths| paths.first(2) } unless all
      inputs.map { |path| [EXTENSIONS.fetch(File.extname(path), FAMILIES.fetch(name)), path] }
    end

    # Writes the json family into `gate`'s scratch; answers [grammar, path].
    def json_family(gate)
      JSON_FAMILY.flat_map do |grammar, inputs|
        inputs.map { |name, bytes| [grammar, gate.write("json/#{grammar}/#{name}.input", bytes)] }
      end
    end
  end
end

module Tinyparse
  class Gate
    # A shared object built by the embedding recipe users follow,
    # examples/embed/embed-lib.sh: the library amalgamation and a parser
    # generated with --parser-lib --name PREFIX for each `prefix => grammar`,
    # linked to export each PREFIX_load and PREFIX_run and the public
    # tinyparse_* API alone. It is compiled at -O0 in the fast pass and at
    # the recipe's -O2 with --full; the exports are the same.
    def embedded_library(file, parsers)
      library = scratch.join(file)
      environment = {
        'CC' => Tinyparse.tool_script('cc', Tinyparse.host.compile_command).to_s, 'CFLAGS' => full? ? '-O2' : '-O0',
        'TP_CLI' => Tinyparse.host_cli.to_s, 'TP_AMALGAMATION' => DIST.join('amalgamation').to_s
      }
      run = execute(['bash', ROOT.join('examples/embed/embed-lib.sh'), library,
                     *parsers.map { |prefix, grammar| "#{prefix}:#{grammar}" }], env: environment)
      raise Toolchain::Tool::Failed, "embed-lib.sh #{file} failed (exit #{run.code}):\n#{run.out}" unless run.success?

      library
    end

    # Checks that `library` exports `entries` and the public API (the
    # functions include/tinyparse.h declares with TINYPARSE_API), and
    # nothing else: no tp_* internal, no other tinyparse_* name, no foreign
    # symbol.
    def must_export_only(library, entries)
      exports = Toolchain::Binary.new(library).exports.to_a
      expected = (Tinyparse.api + entries).sort
      label = library.basename
      extra = exports - expected
      check(extra.empty?, "#{label} exports #{extra.size} symbols beyond its API: #{extra.first(10).join(' ')}")
      missing = expected - exports
      check(missing.empty?, "#{label} does not export #{missing.first(10).join(' ')}")
      return unless extra.empty? && missing.empty?

      ok("#{label} exports #{entries.join(' ')} and the #{Tinyparse.api.size} API functions, nothing else")
    end
  end
end
