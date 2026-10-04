# frozen_string_literal: true

require 'English'
require 'etc'
require 'fileutils'
require 'pathname'
require 'shellwords'
require 'cache'
require 'json'
require 'toolchain'

# What tinyparse's tasks share: its sources and flags, the files generated
# from its data (the embedded manual, grammars and amalgamation), compiling
# by content (Cache.compile) and linking by content, and the targets it
# ships for.
#
# The library (src/*.c but cli.c) is freestanding C11: it is compiled with
# -ffreestanding -fno-builtin and may leave undefined only the memory
# functions a compiler emits calls to. The CLI (src/cli.c), the test harness
# and the examples are hosted programs.
module Tinyparse
  ROOT = Cache::ROOT
  BUILD = ROOT.join('build')
  DIST = ROOT.join('dist')
  GENERATED = BUILD.join('generated')
  HEADER = ROOT.join('include/tinyparse.h')

  # The release targets tinyparse ships, as they were before the move into
  # the monorepo: Linux on x86_64 and macOS on Apple silicon.
  RELEASE_TARGETS = %w[x86_64-linux aarch64-macos].freeze

  # What the freestanding library may leave undefined: the memory functions
  # a compiler emits calls to for aggregate copies and zeroing.
  MEMORY = %w[memcpy memset memmove memcmp].freeze

  COMMON_FLAGS = %w[-std=c11 -O2 -Wall -Wextra -pedantic -Werror].freeze
  LIBRARY_FLAGS = [*COMMON_FLAGS, '-ffreestanding', '-fno-builtin', '-Iinclude'].freeze
  PROGRAM_FLAGS = [*COMMON_FLAGS, '-Iinclude', '-Isrc', "-I#{GENERATED.relative_path_from(ROOT)}"].freeze

  # Defined for a shared library, so the API alone is exported.
  API_VISIBILITY = 'TINYPARSE_API=__attribute__((visibility("default")))'

  module_function

  def version = ROOT.join('VERSION').binread.strip

  # "0.1" for 0.1.0: a shared library's name follows the minor version on 0.x.
  def shared_version = version.split('.').first(2).join('.')

  def library_sources = Dir[ROOT.join('src/*.c').to_s].sort.map { |path| Pathname.new(path) }.reject { |path| path.basename.to_s == 'cli.c' }
  def cli_source = ROOT.join('src/cli.c')

  # The public API: every function tinyparse.h declares with TINYPARSE_API,
  # read from the pinned clang's AST of the header with the macro defined as
  # an annotation. (ast-grep's C parser, which CHeader uses, cannot tell
  # `TINYPARSE_API tinyparse_result tinyparse_finish(tinyparse_parser *p);`
  # from an expression, since every name in it is a typedef it cannot see.)
  def api
    @api ||= begin
      ast = Toolchain::Tool[:clang].run('-x', 'c', '-std=c11', '-fsyntax-only', '-Xclang', '-ast-dump=json',
                                        '-DTINYPARSE_API=__attribute__((annotate("tinyparse_api")))', HEADER)
      JSON.parse(ast).fetch('inner').select do |declaration|
        declaration['kind'] == 'FunctionDecl' && (declaration['inner'] || []).any? { |node| node['kind'] == 'AnnotateAttr' }
      end.map { |declaration| declaration.fetch('name') }.uniq.sort
    end
  end

  def relative(path) = Pathname.new(path).relative_path_from(ROOT).to_s

  # Writes `text` to `path` only when it differs, so a regenerated file with
  # the same bytes keeps every object built from it.
  def write_if_changed(path, text)
    path = Pathname.new(path)
    return false if path.file? && path.binread == text

    FileUtils.mkdir_p(path.dirname)
    path.binwrite(text)
    true
  end

  # --- compiling and linking by content ----------------------------------

  # Compiles each source with `command` into `dir`, as many at once as this
  # machine has processors; aborts with the compiler's output on a failure.
  # Answers the objects, in the order of `sources`.
  def compile_all(command, sources, dir, label)
    Cache.tool_id(command.first(1)) # once, before the threads share it
    queue = Queue.new
    sources.each_with_index { |source, index| queue << [source, index] }
    queue.close
    results = Array.new(sources.size)
    Array.new([Etc.nprocessors, sources.size].min) do
      Thread.new do
        while (item = queue.pop)
          source, index = item
          object = dir.join("#{File.basename(source.to_s, '.c')}.o")
          results[index] = [object, *Cache.compile(command, relative(ROOT.join(source.to_s)), object), source]
        end
      end
    end.each(&:join)
    failed = results.find { |_object, outcome| outcome == :failed }
    if failed
      puts [*command, '-c', relative(ROOT.join(failed.last.to_s))].shelljoin
      abort "#{label}: #{failed[2]}"
    end
    results.each { |_object, _outcome, output| puts output unless output.empty? }
    compiled = results.count { |_object, outcome| outcome == :compiled }
    puts "#{label}: #{compiled} compiled, #{results.size - compiled} reused"
    results.map(&:first)
  end

  # Runs `block` to produce `out` unless the last successful run had the
  # same command and the same input bytes, and `out` is still there.
  def link(name, out, command, inputs)
    key = Cache.digest(command.map { |part| part.to_s.delete_prefix("#{ROOT}/") }.join(' '),
                       inputs.map { |input| Pathname.new(input) })
    return :reused if Pathname.new(out).exist? && Cache.stamped?("link-#{name}", key)

    Cache.unstamp("link-#{name}")
    yield
    Cache.stamp("link-#{name}", key)
    :linked
  end

  # --- the host build (Toolchain::Target.host) ---------------------------

  def host = Toolchain::Target.host

  def host_library = BUILD.join('lib/libtinyparse.a')
  def host_cli = BUILD.join('bin/tinyparse')
  def harness = BUILD.join('bin/harness')

  # --- the generated headers ---------------------------------------------

  # A C array initializer of `bytes`, NUL-terminated so the bytes are also a
  # C string; a length beside it is what a reader uses.
  def byte_array(name, bytes)
    numbers = bytes.bytes.map(&:to_s)
    lines = numbers.each_slice(24).map { |slice| "    #{slice.join(',')}," }
    "static const unsigned char #{name}[] = {\n#{lines.join("\n")}\n    0\n};\n"
  end

  def generated_header(file, guard, description, body)
    <<~C
      /*
      ** #{file} -- #{description}
      ** GENERATED by `mise run generate` (tinyparse/mise/lib/tinyparse.rb).
      ** Do not edit by hand; it is a build artifact under build/generated/.
      */
      #ifndef #{guard}
      #define #{guard}

      #{body}
      #endif /* #{guard} */
    C
  end

  # The rendered manual page tinyparse.1, which the CLI's `man` prints.
  def manpage_header
    body = byte_array('tinyparse_manpage', ROOT.join('tinyparse.1').binread) +
           "static const unsigned long tinyparse_manpage_length =\n    sizeof tinyparse_manpage - 1;\n"
    generated_header('manpage_data.h', 'TINYPARSE_MANPAGE_DATA_H', 'the rendered tinyparse(1) manual, embedded.', body)
  end

  # The standard grammar packages under grammars/std/, by the name their
  # manifest gives (std.json): `tinyparse grammars` lists them and a grammar
  # name falls back to them.
  def standard_packages
    Dir[ROOT.join('grammars/std/*/').to_s].sort.filter_map do |dir|
      dir = Pathname.new(dir)
      manifest = dir.join('package.scm')
      grammar = dir.join('grammar.scm')
      next unless manifest.file? && grammar.file?

      name = manifest.binread[/(?:\A|\s)name:\s+(\S+)/, 1] || dir.basename.to_s
      [name, manifest, grammar]
    end
  end

  def grammars_header
    arrays = []
    entries = []
    standard_packages.each_with_index do |(name, manifest, grammar), index|
      arrays << byte_array("tp_embed_pkg_#{index}", manifest.binread)
      arrays << byte_array("tp_embed_grm_#{index}", grammar.binread)
      entries << "    { \"#{name}\", tp_embed_pkg_#{index}, sizeof tp_embed_pkg_#{index} - 1, " \
                 "tp_embed_grm_#{index}, sizeof tp_embed_grm_#{index} - 1 },"
    end
    body = <<~C
      typedef struct tinyparse_embedded_package {
          const char          *name;
          const unsigned char *package_bytes;
          unsigned long        package_length;
          const unsigned char *grammar_bytes;
          unsigned long        grammar_length;
      } tinyparse_embedded_package;

      #{arrays.join("\n")}
      static const tinyparse_embedded_package tinyparse_embedded_packages[] = {
      #{entries.join("\n")}
          { 0, 0, 0, 0, 0 }
      };
    C
    generated_header('grammars_data.h', 'TINYPARSE_GRAMMARS_DATA_H', 'the embedded standard grammar registry.', body)
  end

  # The library amalgamation, embedded in the CLI so `init` and `vendor`
  # seed a project with the whole engine.
  def amalgamation_header
    body = byte_array('tinyparse_amalgamation_c_bytes', library_amalgamation) +
           "static const unsigned long tinyparse_amalgamation_c_length =\n    sizeof tinyparse_amalgamation_c_bytes - 1;\n\n" +
           byte_array('tinyparse_amalgamation_h_bytes', HEADER.binread) +
           "static const unsigned long tinyparse_amalgamation_h_length =\n    sizeof tinyparse_amalgamation_h_bytes - 1;\n"
    generated_header('amalgamation_data.h', 'TINYPARSE_AMALGAMATION_DATA_H',
                     'the embedded tinyparse library amalgamation (tinyparse.c and tinyparse.h).', body)
  end

  GENERATED_HEADERS = {
    'manpage_data.h' => :manpage_header,
    'grammars_data.h' => :grammars_header,
    'amalgamation_data.h' => :amalgamation_header
  }.freeze

  # The inputs the generated headers are made from.
  def generated_inputs
    Cache.files('tinyparse.1', 'grammars/std/*/package.scm', 'grammars/std/*/grammar.scm', 'include/tinyparse.h',
                'src/*.c', 'src/*.h', 'VERSION', 'mise/lib/tinyparse.rb')
  end

  # Writes build/generated/*.h; answers the files that changed.
  def generate!
    GENERATED_HEADERS.filter_map do |file, method|
      path = GENERATED.join(file)
      path if write_if_changed(path, send(method))
    end
  end

  # --- the amalgamation ---------------------------------------------------

  # The #include lines of files the amalgamation folds in.
  INLINED_INCLUDE = /^\s*#\s*include\s+"(tinyparse\.h|internal\.h|manpage_data\.h)"[^\n]*\n/

  # One translation unit of the library: the public header, internal.h and
  # every library source in name order, with default visibility so a
  # shared library built from it with -fvisibility=hidden exports the API.
  def library_amalgamation
    pieces = library_sources.map do |source|
      "\n/* ===== src/#{source.basename} ============================================ */\n" +
        source.binread.gsub(INLINED_INCLUDE, '')
    end
    banner = <<~C
      /*
      ** tinyparse.c -- single-file amalgamation of the tinyparse library.
      **
      ** GENERATED by `mise run amalgamate` for tinyparse #{version}.
      ** Do not edit by hand. It folds together, in one translation unit:
      **   - the tinyparse public header;
      **   - the private internal.h contract;
      **   - every library source (each C file in src/ except the CLI).
      **
      ** Build:  cc -c tinyparse.c
      **         cc -shared -fPIC -fvisibility=hidden tinyparse.c -o libtinyparse.so
      */

      /*
      ** The tinyparse library is built with default visibility so its public
      ** symbols are exported even when the whole file is compiled with
      ** -fvisibility=hidden (the recommended shared-library flag).
      */
      #pragma GCC visibility push(default)

      /*
      ** internal.h's rule-call helpers for generated parsers are unused here, so
      ** this translation unit leaves them out (see internal.h).
      */
      #define TINYPARSE_AMALGAMATION 1

      /* ===== tinyparse public header ========================================== */
    C
    banner + HEADER.binread + "\n/* ===== tinyparse internal header ======================================== */\n" +
      ROOT.join('src/internal.h').binread.gsub(INLINED_INCLUDE, '') + pieces.join + "\n#pragma GCC visibility pop\n"
  end

  # The CLI as one file: src/cli.c with the generated headers inlined, built
  # beside tinyparse.c.
  def cli_amalgamation
    text = cli_source.binread.gsub(/^#include "(\w+_data\.h)"\n/) do
      file = Regexp.last_match(1)
      "\n/* --- inlined #{file} --- */\n#{GENERATED.join(file).binread}/* --- end #{file} --- */\n"
    end
    banner = <<~C
      /*
      ** tinyparse_cli.c -- the tinyparse command-line shell (amalgamated).
      **
      ** GENERATED by `mise run amalgamate` for tinyparse #{version}.
      ** It keeps #include "tinyparse.h" (shipped alongside) and links against
      ** tinyparse.c / libtinyparse. The generated headers (the manual page, the
      ** standard grammars and the library amalgamation) are inlined below.
      **
      ** Build:  cc tinyparse.c tinyparse_cli.c -o tinyparse
      */
    C
    banner + text
  end

  # --- what the tests are given ------------------------------------------

  TOOLS = BUILD.join('tools')

  # A one-line shell script in build/tools that runs `command`, so a test
  # (a shell command in a .test file, or examples/embed/embed-lib.sh) runs
  # the pinned toolchain by one quoted path.
  def tool_script(name, command)
    path = TOOLS.join(name)
    write_if_changed(path, "#!/bin/sh\nexec #{command.shelljoin} \"$@\"\n")
    FileUtils.chmod(0o755, path)
    path
  end

  # The environment of the suite's cases and the benchmarks: the built
  # CLI and library, the amalgamation, the examples, and the pinned
  # toolchain: TP_CC (the pinned clang for this machine), TP_LINUX_CC (zig cc
  # for x86_64 Linux, whose objects read the same from any host), TP_NM
  # (llvm-nm), TP_AR (llvm-ar), TP_TAR and TP_GZIP (GNU tar and
  # gzip), TP_AST_GREP. TP and TP_CLI both name the CLI.
  def test_environment
    {
      'TP' => host_cli.to_s,
      'TP_CLI' => host_cli.to_s,
      'TP_ROOT' => ROOT.to_s,
      'TP_LIB' => host_library.to_s,
      'TP_AMALGAMATION' => DIST.join('amalgamation').to_s,
      'TP_EXAMPLES' => BUILD.join('examples').to_s,
      'TP_CC' => tool_script('cc', host.compile_command).to_s,
      'TP_LINUX_CC' => tool_script('x86_64-linux-cc', Toolchain::Target.release('x86_64-linux').compile_command).to_s,
      'TP_NM' => tool_script('nm', Toolchain::Tool[:llvm_nm].command).to_s,
      'TP_AR' => tool_script('ar', Toolchain::Tool[:llvm_ar].command).to_s,
      'TP_TAR' => tool_script('tar', Toolchain::Tool[:tar].command).to_s,
      'TP_GZIP' => tool_script('gzip', Toolchain::Tool[:gzip].command).to_s,
      'TP_AST_GREP' => Toolchain::Tool[:ast_grep].path
    }
  end
end

module Tinyparse
  LOGS = BUILD.join('logs')

  module_function

  # Runs each [label, command] at once, each with its output in
  # build/logs/<label>.log, and reports them in the order given, with each
  # one's time and the last line of its log. Answers the labels that failed.
  def run_all(jobs, environment)
    FileUtils.mkdir_p(LOGS)
    clock = -> { Process.clock_gettime(Process::CLOCK_MONOTONIC) }
    running = jobs.map do |label, command|
      log = LOGS.join("#{label}.log")
      started = clock.call
      pid = Process.spawn(environment, *command, chdir: ROOT.to_s, out: log.to_s, err: %i[child out])
      { label: label, log: log, pid: pid, started: started }
    end
    by_pid = running.to_h { |job| [job[:pid], job] }
    until by_pid.values.all? { |job| job[:status] }
      pid, status = Process.wait2(-1)
      next unless (job = by_pid[pid])

      job[:status] = status
      job[:seconds] = clock.call - job[:started]
    end
    running.reject do |job|
      ok = job[:status].success?
      lines = job[:log].binread.lines.map(&:strip).reject(&:empty?)
      results = lines.grep(/\A(RESULT|PROGRESS|STATIC_MUTABLE)/)
      summary = results.empty? ? lines.last.to_s : results.join('  ')
      puts format('%-4s %6.1fs  %-18s %s', ok ? 'ok' : 'FAIL', job[:seconds], job[:label], summary)
      unless ok
        puts "     #{Tinyparse.relative(job[:log])}:"
        job[:log].binread.lines.last(25).each { |line| puts "       #{line}" }
      end
      ok
    end.map { |job| job[:label] }
  end
end

module Tinyparse
  module_function

  # Runs bench/<script>, a benchmark or corpus count in bash, with the
  # environment of the suite (test_environment) and BENCH=1, which the
  # scripts ask for so that nothing runs them by accident. They are opt-in
  # tasks (`mise run bench:<name>`), never part of `mise run test`: they
  # measure time, so they run alone, and need the references of
  # reference/PINS.txt (`mise run bench:fetch`) and the network for the
  # tree-sitter runtime and the npm packages they compare against.
  def bench(script, *arguments)
    exec(test_environment.merge('BENCH' => '1'), 'bash', ROOT.join('bench', script).to_s, *arguments, chdir: ROOT.to_s)
  end
end
