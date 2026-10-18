# frozen_string_literal: true

# The WebAssembly module of @tinyactors/wasm (docs/browser.md): core's
# amalgamation (the host and the layers below it, the compiler and the
# runtime with its wasm platform file, ta_wasm_alloc over wasi-libc), txml,
# and the C layer of wasm/src/, compiled by the pinned zig for wasm32-wasi
# as a reactor (Toolchain::Target.zig; never a compiler from PATH), at
# -O2 with no debug information, its stack size set, its custom sections
# stripped. Its imports are the layer's own (ta_web.*) and its exports the
# layer's functions (TA_WEB_EXPORT in wasm/src/), memory and _initialize,
# each checked exactly after the link.
require 'English'
require 'json'
require_relative 'napi'

module WebModule
  ROOT = NodeAPI::ROOT.join('wasm')
  SOURCES = ROOT.join('src')
  BUILD = NodeAPI::BUILD.join('wasm')
  # The module the loaders find beside the package's entries
  # (wasm/lib/module.mjs: new URL('../tinyactors.wasm', import.meta.url)).
  MODULE = ROOT.join('tinyactors.wasm')
  # The same module with its custom sections (names, target_features), for
  # the report; never shipped.
  FULL = BUILD.join('tinyactors.full.wasm')
  # The build with the stack probe (TA_WEB_STACK_PROBE), for `mise run
  # wasm:stack` only.
  PROBE = BUILD.join('tinyactors.probe.wasm')
  CORE_SOURCES = %w[tinyactors.c tinyactors_runtime.c tinyactors_runtime_wasm.c].freeze
  # The shadow stack (wasm-ld's -z stack-size): measured with `mise run
  # wasm:stack` (docs/browser.md, "Exceptions, ..."; the measurement and the
  # margin are in docs/log.xml, l-p5-build-stack).
  STACK_SIZE = 256 * 1024
  # The C layer's own flags: warnings are errors.
  LAYER_FLAGS = %w[-std=c11 -Wall -Wextra -Werror].freeze
  CORE_FLAGS = %w[-std=c11].freeze
  TXML_FLAGS = %w[-std=c11 -w].freeze
  # What the module may import: the layer's imports (wasm/src/web.h).
  IMPORTS = %w[ta_web.clock ta_web.datamodel ta_web.effect ta_web.output ta_web.resolve].freeze

  module_function

  def target = Toolchain::Target.zig('wasm32-wasi')

  def layer_sources = SOURCES.glob('*.c').sort

  # The layer's exports: every function its sources mark TA_WEB_EXPORT.
  def exports
    layer_sources.flat_map { |source| source.read.scan(/^TA_WEB_EXPORT\s[^;{]*?\b(ta_web_\w+)\s*\(/).flatten }.uniq.sort
  end

  # Compiles `source` into `object` unless it was compiled from the same
  # bytes with the same flags (NodeAPI.compile's rule, for this target).
  def compile(source, object, flags:, inputs: [])
    stamp = object.sub_ext('.digest')
    digest = Digest::SHA256.hexdigest([target.cflags.join(' '), flags.join(' '),
                                       *[source, *inputs].map { |path| Digest::SHA256.file(path).hexdigest }].join("\n"))
    return object if object.exist? && stamp.exist? && stamp.read == digest

    target.compile(source.to_s, into: object.to_s, flags: flags)
    stamp.write(digest)
    object
  end

  def objects(probe: false)
    core_headers = NodeAPI::CORE_BUILD.glob('*.h')
    core = CORE_SOURCES.map do |name|
      compile(NodeAPI::CORE_BUILD.join(name), BUILD.join('objects/core', name.sub(/\.c\z/, '.o')),
              flags: [*CORE_FLAGS, '-g0', "-I#{NodeAPI::CORE_BUILD}"], inputs: core_headers)
    end
    txml = compile(NodeAPI::TXML.join('txml.c'), BUILD.join('objects/txml/txml.o'),
                   flags: [*TXML_FLAGS, '-g0', "-I#{NodeAPI::TXML}"], inputs: [NodeAPI::TXML.join('txml.h')])
    variant = probe ? 'probe' : 'layer'
    headers = [*SOURCES.glob('*.h'), *core_headers, NodeAPI::TXML.join('txml.h')]
    own = layer_sources.map do |source|
      compile(source, BUILD.join("objects/#{variant}", source.basename.sub_ext('.o')),
              flags: [*LAYER_FLAGS, '-g0', *(probe ? %w[-DTA_WEB_STACK_PROBE] : []), "-I#{NodeAPI::CORE_BUILD}",
                      "-I#{NodeAPI::TXML}"],
              inputs: headers)
    end
    [*core, txml, *own].map(&:to_s)
  end

  def link(objects, into:, exports:, strip:)
    flags = ['-mexec-model=reactor', "-Wl,-z,stack-size=#{STACK_SIZE}", *(strip ? %w[-Wl,--strip-all] : [])]
    target.wasm_module(objects, into: into.to_s, exports: exports, flags: flags)
  end

  # Builds MODULE (stripped) and FULL (with its custom sections), checks
  # both modules' imports and exports, and answers the report of
  # tools/wasm-module.mjs (sizes, sections, features) as a Hash.
  def build
    NodeAPI.run('mise', 'run', '//tinyactors-core:amalgamate', '--', '--out', NodeAPI::CORE_BUILD.to_s)
    BUILD.mkpath
    list = exports
    objects = self.objects
    link(objects, into: FULL, exports: list, strip: false)
    link(objects, into: MODULE, exports: list, strip: true)
    report = inspect_module(MODULE, exports: list)
    full = inspect_module(FULL, exports: list)
    report.merge('features' => full['features'], 'fullBytes' => full['bytes'])
  end

  # The stack probe's build (TA_WEB_STACK_PROBE: two more exports that
  # paint the stack and read its high-water mark), for `mise run wasm:stack`.
  def build_probe
    NodeAPI.run('mise', 'run', '//tinyactors-core:amalgamate', '--', '--out', NodeAPI::CORE_BUILD.to_s)
    BUILD.mkpath
    list = [*exports, 'ta_web_stack_paint', 'ta_web_stack_used', 'ta_web_stack_size'].sort
    link(objects(probe: true), into: PROBE, exports: list, strip: true)
    inspect_module(PROBE, exports: list)
    PROBE
  end

  # The module's imports and exports checked exactly (wasm/tools/module.mjs
  # reads them with WebAssembly.Module), and its report.
  def inspect_module(path, exports:)
    out = IO.popen([NodeAPI.pinned('node'), ROOT.join('tools/module.mjs').to_s, path.to_s], &:read)
    abort "wasm: #{path} could not be read" unless $CHILD_STATUS.success?
    report = JSON.parse(out)
    unless report['imports'].sort == IMPORTS
      abort "wasm: #{path.basename} imports #{report['imports'].sort.inspect}, not #{IMPORTS.inspect}"
    end
    expected = [*exports, '_initialize', 'memory'].sort
    unless report['exports'].sort == expected
      abort "wasm: #{path.basename} exports #{(report['exports'] - expected).inspect} beyond the layer's, " \
            "and lacks #{(expected - report['exports']).inspect}"
    end
    report
  end
end
