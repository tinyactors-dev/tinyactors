# frozen_string_literal: true

# What tinyactors-core's tasks share: its layers and their sources, the test
# programs and what they link, and how each kind of build compiles. This
# replaces the CMake build the library came with (docs/layout.md): the layers
# are static archives, built with the pinned toolchain (mise/lib/toolchain/)
# and reused while their bytes are unchanged (mise/lib/cache.rb).
require 'etc'
require 'fileutils'
require 'pathname'
require 'shellwords'
require 'cache'
require 'toolchain'
require 'platform_files'

module Core
  ROOT = Pathname.new(__dir__).join('..').expand_path
  BUILD = ROOT.join('build')

  # A layer is a static archive, libtinyactors_<name>.a. `uses` are the
  # layers it links against (docs/architecture.md section 1); `freestanding`
  # layers compile without the host's C library headers (AGENTS.md).
  Layer = Struct.new(:name, :sources, :uses, :freestanding, :flags, :libraries, keyword_init: true)

  # How QuickJS compiles. Without C11 atomics it leaves out `Atomics.wait`
  # and its threads, so it calls no pthread function: a data model is single
  # threaded, and a library that links no C library cannot count on
  # libpthread (root ADR 0001, decision 20).
  QUICKJS_FLAGS = %w[-std=gnu11 -w -fno-strict-aliasing -D_GNU_SOURCE -D__STDC_NO_ATOMICS__=1].freeze

  LAYERS = [
    Layer.new(name: 'base', uses: [], freestanding: true,
              sources: %w[address format frame json arena pool heap slab journal].map { |s| "src/base/#{s}.c" }),
    Layer.new(name: 'image', uses: %w[base], freestanding: true,
              sources: %w[src/image/image.c src/image/event_names.c]),
    # Trace values and the record schema (docs/adr/0006 in the repository root).
    Layer.new(name: 'trace', uses: %w[base], freestanding: true, sources: %w[src/trace/trace.c src/trace/record.c]),
    Layer.new(name: 'core', uses: %w[image trace], freestanding: true,
              sources: %w[core event_pool trace_step datamodel_null].map { |s| "src/core/#{s}.c" }),
    Layer.new(name: 'system', uses: %w[core], freestanding: true,
              sources: %w[system schedule route hibernate effects tracing policy].map { |s| "src/system/#{s}.c" }),
    Layer.new(name: 'compile', uses: %w[image],
              sources: %w[compile_support xml scxml link compile write].map { |s| "src/compile/#{s}.c" }),
    # Many systems as one (versions, routing, migration jobs, hibernation
    # policy). Freestanding: it allocates only through the ta_allocator its
    # configuration gives it (docs/adr/0005).
    Layer.new(name: 'host', uses: %w[system], freestanding: true, sources: %w[src/host/host.c src/host/router.c]),
    # The link protocol between systems over the caller's transports (docs/adr/0008).
    Layer.new(name: 'link', uses: %w[host], freestanding: true, sources: %w[src/link/link.c]),
    Layer.new(name: 'runtime', uses: %w[system compile], sources: %w[src/runtime/runtime.c src/runtime/runtime_hosted.c]),
    # The BasicHTTP Event I/O Processor's codec; the transport stays with the host.
    Layer.new(name: 'basic_http', uses: %w[base], freestanding: true, sources: %w[src/basic_http/basic_http.c]),
    # Trace records to OTLP protobuf and back: a codec, like basic_http (docs/adr/0006).
    Layer.new(name: 'otlp', uses: %w[trace image], freestanding: true, sources: %w[src/otlp/otlp.c src/otlp/decode.c src/otlp/batch.c]),
    # The vendored engine only: quickjs-libc (std and os modules, I/O) is not built.
    Layer.new(name: 'quickjs', uses: [], libraries: %w[-lm],
              sources: %w[quickjs libregexp libunicode dtoa].map { |s| "third_party/quickjs-ng/#{s}.c" },
              flags: QUICKJS_FLAGS),
    # The SCXML ECMAScript data model; link it like a tree-sitter language.
    Layer.new(name: 'datamodel_ecmascript', uses: %w[image quickjs],
              sources: %w[src/datamodel_ecmascript/datamodel_ecmascript.c]),
    # The tests' data model (tests/support), linked by the test programs only.
    Layer.new(name: 'test_datamodel', uses: %w[image], libraries: %w[-lm], sources: %w[tests/support/datamodel_test.c])
  ].to_h { |layer| [layer.name, layer] }.freeze

  DIST = ROOT.join('dist')

  # The libraries a release ships (root ADR 0001, decision 19), each as a
  # header, an amalgamation, a static and a shared library per native target
  # and a wasm module, all built from dist/ (mise run amalgamate):
  #
  #   tinyactors          the freestanding layers: no C library; base through
  #                       system never allocate; the host allocates only
  #                       through the ta_allocator it was given
  #   tinyactors_runtime  the compiler and the runtime conveniences, on the C
  #                       library's allocator; built on tinyactors's public
  #                       API alone
  #   tinyactors_datamodel_ecmascript
  #                       the ECMAScript data model with QuickJS inside, opted
  #                       into as a tree-sitter grammar is beside the
  #                       tree-sitter runtime; built on tinyactors's public
  #                       API alone
  #
  # `headers` are its public headers (include/tinyactors/), in the order the
  # amalgamated header has them; `uses` the libraries it builds on.
  # `libc` says whether it calls the C library: its wasm module, for
  # browsers, then compiles wasi-libc in for it and is checked to import
  # nothing, since a browser supplies no imports; tinyactors's is built for
  # wasm32-freestanding.
  #
  # `cflags` are flags its amalgamations compile with beyond a release's.
  Library = Struct.new(:name, :headers, :layers, :uses, :libc, :cflags, keyword_init: true) do
    def header = DIST.join("#{name}.h")
    def amalgamation = DIST.join("#{name}.c")
    # The amalgamation of its platform files for `suffix` (`wasm`, `hosted`).
    def platform_amalgamation(suffix) = DIST.join("#{name}_#{suffix}.c")
    def api = ROOT.join("include/#{name}.api.json")

    # What compiles its amalgamation for `platform` (nil, `hosted`, `wasm`)
    # on this machine.
    def target(platform)
      return Toolchain::Target.host unless platform == 'wasm'

      libc ? Toolchain::Target.zig('wasm32-wasi') : Toolchain::Target.release('wasm32-freestanding')
    end

    # Its C sources: its layers' and their platform files (PlatformFiles),
    # each with its platform (nil for a portable file).
    def sources
      listed = layers.flat_map { |layer| LAYERS.fetch(layer).sources }
      platform = layers.flat_map do |layer|
        PlatformFiles::SUFFIXES.flat_map { |suffix| Dir[ROOT.join("src/#{layer}/*_#{suffix}.c").to_s] }
      end
      (listed + platform.map { |path| Pathname.new(path).relative_path_from(ROOT).to_s }).uniq.sort
    end
  end

  LIBRARIES = [
    Library.new(name: 'tinyactors', layers: %w[base image trace core system host link basic_http otlp], uses: [],
                libc: false,
                headers: %w[types.h allocator.h address.h frame.h trace.h stats.h image.h datamodel.h system.h core.h host.h
                            link.h basic_http.h otlp.h wasm.h]),
    Library.new(name: 'tinyactors_runtime', layers: %w[compile runtime], uses: %w[tinyactors], libc: true,
                headers: %w[compile.h runtime.h]),
    Library.new(name: 'tinyactors_datamodel_ecmascript', layers: %w[datamodel_ecmascript quickjs], uses: %w[tinyactors],
                libc: true, headers: %w[datamodel_ecmascript.h], cflags: QUICKJS_FLAGS)
  ].freeze

  def self.library(name) = LIBRARIES.find { |library| library.name == name } || abort("unknown library #{name}")

  # The CRelease::Project of a library (mise/lib/c_release.rb): its files in
  # dist/ and include/, the version macros of types.h, and whether it uses
  # the C library.
  def self.release_project(library)
    require 'c_release'
    CRelease::Project.new(name: library.name, prefix: 'ta', header: library.header, amalgamation: library.amalgamation,
                          api: library.api, version_header: VERSION_HEADER, libc: library.libc,
                          cflags: library.cflags || [])
  end

  # The version both libraries have: TA_VERSION_* in include/tinyactors/types.h.
  VERSION_HEADER = ROOT.join('include/tinyactors/types.h')

  def self.version
    text = VERSION_HEADER.read
    %w[MAJOR MINOR PATCH].map { |part| text[/^#define TA_VERSION_#{part} (\d+)$/, 1] or abort("types.h has no TA_VERSION_#{part}") }
                         .join('.')
  end

  # A test program: its sources, the layers it links, and its defines. Each
  # runs from its own directory under the build, so programs that write files
  # can run side by side (AGENTS.md, "Tests run in parallel"). A `full` one
  # has an exhaustive mode, which `mise run test --full` asks for with
  # `--full` (AGENTS.md, "Where a new test goes").
  Program = Struct.new(:name, :sources, :uses, :defines, :arguments, :cxx, :full, keyword_init: true)

  def self.unit(name, *uses, defines: {}, full: false)
    Program.new(name: name, sources: ["tests/unit/#{name}.c"], uses: uses, defines: defines, full: full)
  end

  ECMASCRIPT = %w[datamodel_ecmascript quickjs].freeze
  # The C library functions QuickJS calls, in the ECMAScript library and in
  # the Node.js addon: allocation (malloc_usable_size, which glibc and musl
  # both have; libSystem's malloc_size on macOS), strings and numbers, the
  # clock and local time for Date, stdio for its debugging dumps, and the
  # math library, which glibc keeps in libm: a program links -lm (ADR 0001,
  # decision 20), and Node.js already has.
  ECMASCRIPT_LIBC = %w[abort acos acosh asin asinh atan atan2 atanh calloc cbrt ceil clock_gettime cos cosh exp expm1
                       floor fmod fprintf fputc free frexp fwrite gettimeofday hypot localtime_r log log10 log1p log2
                       malloc memchr memmove modf pow printf realloc round scalbn sin sinh snprintf sscanf strchr
                       strcmp strlen strrchr strstr strtod tan tanh trunc vsnprintf].freeze
  PROGRAMS = [
    unit('test_base', 'base'),
    unit('test_headers', 'base'),
    Program.new(name: 'test_headers_cxx', sources: %w[tests/unit/test_headers.cpp], uses: %w[base], cxx: true),
    unit('test_image', 'compile'),
    unit('test_compile', 'compile'),
    unit('test_compile_scale', 'compile'),
    unit('test_core', 'core', 'compile', 'test_datamodel', *ECMASCRIPT),
    unit('test_system', 'system', 'compile', 'test_datamodel'),
    unit('test_tracing', 'system', 'compile', 'test_datamodel'),
    unit('test_scenarios', 'system', 'compile', 'test_datamodel'),
    unit('test_runtime', 'runtime', 'test_datamodel', defines: { 'TA_TEST_TEMPORARY_DIRECTORY' => :scratch }),
    unit('test_datamodel_ecmascript', 'system', 'compile', *ECMASCRIPT,
         defines: { 'TA_VALUES' => ROOT.join('tests/conformance/values.json') }),
    unit('test_e2e', 'runtime', 'test_datamodel', defines: { 'TA_TEST_TEMPORARY_DIRECTORY' => :scratch }),
    unit('test_hibernate', 'system', 'compile', 'test_datamodel', *ECMASCRIPT),
    unit('test_migrate', 'system', 'compile', 'test_datamodel', *ECMASCRIPT),
    unit('test_durable', 'host', 'runtime', 'compile', *ECMASCRIPT),
    unit('test_host', 'host', 'runtime', 'compile', *ECMASCRIPT),
    unit('test_router', 'host', 'runtime', 'compile', *ECMASCRIPT),
    unit('test_link', 'link', 'runtime', 'compile', *ECMASCRIPT),
    # The host's memory: accounting, a fixed arena, and refusals (--full: each
    # allocation of the workload refused in turn).
    unit('test_host_memory', 'host', 'compile', *ECMASCRIPT, full: true),
    unit('test_address', 'base', 'quickjs', defines: { 'TA_VECTORS' => ROOT.join('tests/conformance/addresses.json') }),
    unit('test_frame', 'base'),
    unit('test_journal', 'base'),
    unit('test_otlp', 'otlp', 'system', 'compile', *ECMASCRIPT),
    unit('test_trace', 'trace', 'quickjs', defines: { 'TA_SPAN_VECTORS' => ROOT.join('tests/conformance/span_ids.json'),
                                                      'TA_TRACEPARENT_VECTORS' => ROOT.join('tests/conformance/traceparent.json') }),
    # The torture chamber's event loop and PostgreSQL wire client.
    Program.new(name: 'test_torture_loop', sources: %w[tests/unit/test_torture_loop.c tools/torture/loop.c], uses: [],
                defines: {}),
    Program.new(name: 'test_torture_postgres', uses: [], defines: {},
                sources: %w[tests/unit/test_torture_postgres.c tools/torture/postgres.c tools/torture/loop.c]),
    # Host conformance (tests/conformance/*.json) through the C host; the JS
    # drivers (tests/conformance/run_js.mjs) run the same scenarios.
    Program.new(name: 'conformance', sources: %w[tests/conformance/run_c.c], uses: ['host', 'runtime', 'compile', *ECMASCRIPT],
                defines: {}, arguments: [ROOT.join('tests/conformance').to_s])
  ].freeze

  # Programs that are built but run by their own tasks.
  TOOLS = [
    # The W3C IRP runner (mise run w3c).
    Program.new(name: 'run_w3c', sources: %w[tests/w3c/run_w3c.c], uses: %w[runtime test_datamodel basic_http] + ECMASCRIPT,
                defines: {}),
    # The torture chamber's host (tools/torture/README.md).
    Program.new(name: 'torture-bench', uses: %w[host runtime compile otlp] + ECMASCRIPT,
                sources: %w[bench.c net.c postgres.c loop.c].map { |s| "tools/torture/#{s}" },
                defines: { 'BENCH_SOURCE_DIR' => ROOT.join('tools/torture'), 'BENCH_WEB_DIR' => ROOT.join('web/torture'),
                           'BENCH_REPO_DIR' => ROOT }),
    # Examples: hosts built on the public API (each directory's README).
    Program.new(name: 'access-reconciler', sources: %w[examples/access-reconciler/host/sim.c],
                uses: %w[runtime] + ECMASCRIPT,
                defines: { 'CHART_PATH' => ROOT.join('examples/access-reconciler/charts/access.scxml') })
  ].freeze

  WARNINGS = %w[-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Wcast-align
                -fno-strict-aliasing -Werror].freeze
  C_WARNINGS = %w[-std=c11 -Wstrict-prototypes -Wmissing-prototypes].freeze
  INCLUDES = ['-Iinclude', '-Isrc', '-Itests', '-isystem', 'third_party/quickjs-ng'].freeze
  # Without the host's C library headers: clang's own stdint.h and stddef.h
  # remain, and anything else fails to compile, on every host.
  FREESTANDING = %w[-ffreestanding -nostdlibinc -fno-builtin-malloc].freeze

  # What the binding surface check compiles with: unoptimized, so the
  # compiler inlines no call away, without the sanitizers zig adds at -O0,
  # and each function and datum in a section of its own, for the linker's
  # dead-code elimination to drop what the binding does not reach.
  UNOPTIMIZED = %w[-O0 -fno-sanitize=all -ffunction-sections -fdata-sections].freeze

  # How a build compiles: `dev` is what the tests run (sanitizers, debug
  # assertions), `release` what the tools and benchmarks run. The sanitizer
  # build is -O1, as sanitizers are usually run: fast enough for the whole
  # suite, and frames stay close to a release build's.
  VARIANTS = {
    'dev' => { flags: %w[-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all -DTA_DEBUG],
               link: %w[-fsanitize=address,undefined] },
    'release' => { flags: %w[-O2 -DNDEBUG], link: [] },
    # What a shared object links (the Node-API addon): position-independent,
    # and exporting nothing but what the addon marks.
    'addon' => { flags: %w[-O2 -DNDEBUG -fvisibility=hidden], link: [], position_independent: true },
    # The test programs as WebAssembly (wasm32-wasi, the pinned zig), run
    # under wasmtime (mise run check:wasm).
    'wasi' => { flags: %w[-Os], link: [], wasi: true },
    # The Node.js addon as the binding surface check links it (mise run
    # surface): for one fixed target, x86_64 Linux ELF with the pinned zig,
    # so the answer is the same on every host.
    'surface' => { flags: UNOPTIMIZED, link: [], release: Toolchain::Target.release('x86_64-linux') }
  }.freeze


  module_function

  # The addon for a release target, `addon-<target>` (`mise run node:build
  # --target <target>`): the addon build's flags, compiled and linked by the
  # target's pinned `zig cc` from any host (docs/adr/0001).
  RELEASE_ADDON = /\Aaddon-(?<target>.+)\z/

  def addon_variant(target) = "addon-#{target}"

  def variant(name)
    settings = VARIANTS.fetch(name) do
      match = RELEASE_ADDON.match(name) or
        abort "unknown build #{name.inspect}: #{VARIANTS.keys.join(', ')}, addon-<release target>"
      { **VARIANTS.fetch('addon'), release: Toolchain::Target.release(match[:target]) }
    end
    return settings unless settings[:position_independent]

    { **settings, flags: [*settings[:flags], *target_of(settings).format.position_independent] }
  end

  # What the build compiles for: this machine, wasm32-wasi, or a release target.
  def target(variant_name) = target_of(variant(variant_name))

  def target_of(settings)
    return settings[:release] if settings[:release]

    settings[:wasi] ? Toolchain::Target.zig('wasm32-wasi') : Toolchain::Target.host
  end

  def directory(variant_name) = BUILD.join(variant_name)

  def archive_path(variant_name, layer) = directory(variant_name).join("lib/libtinyactors_#{layer}.a")

  def program_path(variant_name, program) = directory(variant_name).join("bin/#{program}")

  # The layers `uses` needs, each before the layers it uses, as a static link
  # wants them.
  def link_order(uses)
    ordered = []
    visit = lambda do |name|
      return if ordered.include?(name)

      LAYERS.fetch(name).uses.each { |used| visit.call(used) }
      ordered << name
    end
    uses.each { |name| visit.call(name) }
    ordered.reverse
  end

  def compile_command(variant_name, layer: nil, cxx: false, defines: {})
    # C++ is only the check that the public headers are valid C++: it links
    # no C++ library, so it leaves out UBSan's vptr check, which needs one.
    flags = layer&.flags || [*WARNINGS, *(cxx ? %w[-x c++ -std=c++17 -fno-sanitize=vptr] : C_WARNINGS)]
    flags = [*flags, *FREESTANDING] if layer&.freestanding
    [*target(variant_name).compile_command, *flags, *variant(variant_name)[:flags], *INCLUDES,
     *defines.map { |key, value| "-D#{key}=\"#{value}\"" }]
  end

  def compile(command, source, object)
    outcome, output = Cache.compile(command, source, object)
    if outcome == :failed
      puts (command + ['-c', source.to_s]).shelljoin
      abort output
    end
    print output
    outcome
  end

  # Compiles every layer of the build into its archive, `jobs` at a time.
  def build_layers(variant_name, layers = LAYERS.keys)
    target = target(variant_name)
    objects_root = directory(variant_name).join('objects')
    work = layers.flat_map do |name|
      layer = LAYERS.fetch(name)
      command = compile_command(variant_name, layer: layer)
      layer.sources.map { |source| [name, command, source, objects_root.join(name, "#{File.basename(source, '.c')}.o")] }
    end
    in_parallel(work) { |_name, command, source, object| compile(command, source, object) }
    layers.each do |name|
      objects = work.select { |entry| entry.first == name }.map { |entry| entry.last.to_s }
      target.archive(objects, into: archive_path(variant_name, name).to_s)
    end
  end

  # Builds `program` against the build's archives.
  def build_program(variant_name, program)
    target = target(variant_name)
    scratch = directory(variant_name).join('scratch', program.name)
    defines = (program.defines || {}).transform_values { |value| value == :scratch ? scratch : value }
    command = compile_command(variant_name, cxx: program.cxx, defines: defines)
    objects = program.sources.map do |source|
      object = directory(variant_name).join('objects', program.name, "#{File.basename(source).sub(/\.c(pp)?\z/, '')}.o")
      compile(command, source, object)
      object.to_s
    end
    order = link_order(program.uses)
    archives = order.map { |name| archive_path(variant_name, name).to_s }
    libraries = order.flat_map { |name| LAYERS.fetch(name).libraries || [] }.uniq
    link = [*objects, *archives, *libraries, *variant(variant_name)[:link]]
    target.program(link, into: program_path(variant_name, program.name).to_s)
  end

  # --- the bindings' C parts ----------------------------------------------

  # The Node.js addon (mise run node:build): its own sources, compiled with
  # the Node-API headers that ship with the pinned node, and the layers it
  # links.
  NODE_LAYERS = %w[host link runtime system compile otlp datamodel_ecmascript].freeze
  NODE_SOURCES = %w[addon values images pure datamodel quickjs system invoke persist link export trace metrics].map { |name| "bindings/node/src/#{name}.c" }.freeze

  # Builds the addon's layers and compiles its own sources for the build
  # `variant_name`; answers what links it: its objects, then the archives.
  def node_addon_inputs(variant_name)
    node_include = Pathname.new(Toolchain::Tool[:node].path).join('../../include/node').expand_path
    abort "no node_api.h in #{node_include}" unless node_include.join('node_api.h').file?

    build_layers(variant_name, link_order(NODE_LAYERS))
    # NAPI_MODULE defines the addon's exported entry points without prototypes.
    command = [*compile_command(variant_name), '-Wno-missing-prototypes', '-isystem', node_include.to_s,
               '-DNAPI_VERSION=8', '-DNODE_GYP_MODULE_NAME=tinyactors']
    objects = NODE_SOURCES.map do |source|
      object = directory(variant_name).join('objects/node', "#{File.basename(source, '.c')}.o")
      compile(command, source, object)
      object.to_s
    end
    [*objects, *link_order(NODE_LAYERS).map { |name| archive_path(variant_name, name).to_s }]
  end


  # Runs the block for each item on a thread per CPU; the block's own
  # commands run as child processes, so the threads only wait.
  def in_parallel(items, jobs: Etc.nprocessors)
    queue = Queue.new
    items.each { |item| queue << item }
    failure = nil
    Array.new([jobs, items.size].min) do
      Thread.new do
        while (item = (queue.pop(true) rescue nil))
          begin
            yield item
          rescue SystemExit, StandardError => e
            failure ||= e
          end
        end
      end
    end.each(&:join)
    raise failure if failure
  end

  # --- the W3C SCXML IRP suite ------------------------------------------

  W3C_BASE = 'https://www.w3.org/Voice/2013/scxml-irp/'

  # The suite, downloaded once into tests/w3c/irp (gitignored) and its
  # templates instantiated for ECMAScript into build/w3c/ecma, which it
  # answers. The W3C stylesheets are XSLT 2.0, which only SaxonC implements
  # among what we can pin, so that one step is tools/w3c/convert.py, run
  # with the pinned uv.
  def w3c_suite
    converted = directory('w3c').join('ecma')
    return converted if converted.join('index.txt').file?

    irp = ROOT.join('tests/w3c/irp')
    FileUtils.mkdir_p(irp)
    manifest = irp.join('manifest.xml')
    manifest.write(w3c_get('manifest.xml')) unless manifest.file?
    files = manifest.read.scan(/<(?:start|dep) uri="([^"]+)"/).flatten.uniq.sort + %w[confEcma.xsl confXPath.xsl]
    missing = files.reject { |file| irp.join(file).file? }
    missing.each do |file|
      FileUtils.mkdir_p(irp.join(file).dirname)
      irp.join(file).binwrite(w3c_get(file))
    end
    puts "w3c: #{files.size} files in #{irp.relative_path_from(ROOT)} (#{missing.size} fetched)"
    run('uv', 'run', '--quiet', '--with', 'saxonche', 'python', 'tools/w3c/convert.py', irp, converted)
    converted
  end

  # w3.org throttles bursts (HTTP 429): one request at a time, waiting as
  # long as it asks.
  def w3c_get(path, attempts: 8)
    require 'net/http'
    attempts.times do |attempt|
      response = Net::HTTP.get_response(URI(W3C_BASE + path), { 'User-Agent' => 'curl/8.5.0' })
      return response.body if response.is_a?(Net::HTTPSuccess)
      raise "#{path}: HTTP #{response.code}" unless response.code == '429' && attempt < attempts - 1

      after = response['Retry-After'].to_s
      sleep(after.match?(/\A\d+\z/) ? [after.to_i, 120].min : [2**(attempt + 1), 60].min)
    end
  end

  def run(*command, chdir: ROOT, env: {})
    pid = Process.spawn(env, *command.map(&:to_s), chdir: chdir.to_s)
    _pid, status = Process.wait2(pid)
    exit(status.exitstatus || 1) unless status.success?
  end

  # Installs the Node.js binding's development packages once.
  def node_packages
    node = ROOT.join('bindings/node')
    run('npm', 'ci', '--no-audit', '--no-fund', '--silent', chdir: node) unless node.join('node_modules/.bin/tsc').file?
    node
  end
end
