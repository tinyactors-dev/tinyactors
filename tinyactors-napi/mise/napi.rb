# frozen_string_literal: true

# What tinyactors-napi's tasks share: where things are, how a task runs a
# command, the runtimes this project pins, and how the addon is built
# (Toolchain::Target.host for the tests, Target.release for the packages
# when packaging is built; mise/lib/toolchain.rb).
require 'digest'
require 'pathname'
require 'toolchain'

module NodeAPI
  ROOT = Pathname.new(__dir__).join('..').expand_path
  BUILD = ROOT.join('build')
  # Core's amalgamation, built from tinyactors-core's sources into this
  # project's own build/ (AGENTS.md, "Tasks").
  CORE_BUILD = BUILD.join('core')
  # The objects the addon links, and the addon itself, which index.mjs loads.
  OBJECTS = BUILD.join('objects')
  ADDON = BUILD.join('tinyactors.node')
  # The suite's adapter, `node conform.mjs` (tinyactors-test/docs/driver.md).
  ADAPTER = ROOT.join('conform.mjs')
  # The mapping table (tinyactors-core/docs/bindings.md, section 2).
  MAPPING = ROOT.join('docs/api-map.md')
  # The suite's set `mise run test` runs (`--milestone`, a set of
  # tinyactors-test/suites/milestones.txt), named here and nowhere else.
  # Which set the binding is held to now is in ROADMAP.md.
  MILESTONE = 'M1'
  # The runtimes the package serves, each running the same addon.
  RUNTIMES = %w[node bun].freeze
  # What of that set `mise run test` runs now: the suites (`--suite`) and
  # the datamodels (the differential run). It grows with the work
  # (ROADMAP.md, M1, "Plan and progress"): M1 is done when it is every
  # suite of the set on both datamodels, native and quickjs, and both
  # pass. The protocol suite waits for a request to core (ROADMAP.md,
  # "Requests to core").
  GATE = { suites: %w[scenarios], datamodels: %w[native quickjs] }.freeze
  # The datamodels `mise run conform` runs the suite on: both, unless
  # TA_NAPI_DATAMODELS names some (the test task's gate does).
  DATAMODELS = %w[native quickjs].freeze

  # Core's libraries the addon links, from its amalgamation: the host and
  # the layers below it, the runtime (the compiler, ta_runtime_malloc) with
  # its hosted platform file, and the ECMAScript datamodel on quickjs-ng,
  # each with the flags tinyactors-core's amalgamate task compiles it with
  # (tinyactors-core/mise/core.rb, QUICKJS_FLAGS for quickjs).
  CORE_SOURCES = {
    'tinyactors.c' => %w[-std=c11],
    'tinyactors_runtime.c' => %w[-std=c11],
    'tinyactors_runtime_hosted.c' => %w[-std=c11],
    'tinyactors_datamodel_ecmascript.c' => %w[-std=gnu11 -w -fno-strict-aliasing -D_GNU_SOURCE
                                              -D__STDC_NO_ATOMICS__=1]
  }.freeze
  # Every object is linked into a loadable module, so it is position
  # independent, and optimized: the suite runs thousands of charts.
  OPTIMIZE = %w[-O2 -fPIC].freeze
  # The addon's own C: warnings are errors, and Node-API version 8 (type
  # tags) is the oldest it uses.
  ADDON_FLAGS = %w[-std=c11 -Wall -Wextra -Werror -DNAPI_VERSION=8].freeze

  module_function

  # Runs a command in the project's directory; a failure ends the task with
  # its status.
  def run(*command, env: {})
    pid = Process.spawn(env, *command, chdir: ROOT.to_s)
    _pid, status = Process.wait2(pid)
    exit(status.exitstatus || 1) unless status.success?
  end

  # The path of a program this project's mise.toml pins (node, bun, pnpm).
  # A command another project's task runs gets it by path: that project's
  # pins come first on its PATH.
  def pinned(program)
    path = IO.popen(['mise', 'which', program], chdir: ROOT.to_s, err: File::NULL, &:read).strip
    abort "#{program} is not installed: run `mise install` in #{ROOT}" if path.empty?
    path
  end

  # Node-API's headers (node_api.h), from the pinned node: the oldest
  # Node.js the package claims, so the addon uses nothing newer.
  def node_headers = Pathname.new(pinned('node')).dirname.parent.join('include/node')

  # The binding's own tests: each a program that exits 1 when a case fails
  # (test/harness.mjs), run under every runtime.
  def tests = ROOT.glob('test/*.test.mjs').sort

  # The addon's C sources.
  def sources = ROOT.glob('src/**/*.c').sort

  # Compiles `source` into `object` unless it was compiled from the same
  # bytes of it and of `inputs` (the headers it reads) with the same flags:
  # the amalgamate task writes its files again on every run, so their
  # times say nothing.
  def compile(source, object, flags:, inputs: [])
    stamp = object.sub_ext('.digest')
    digest = Digest::SHA256.hexdigest([flags.join(' '), *[source, *inputs].map { |path| Digest::SHA256.file(path).hexdigest }].join("\n"))
    return object if object.exist? && stamp.exist? && stamp.read == digest

    Toolchain::Target.host.compile(source.to_s, into: object.to_s, flags: flags)
    stamp.write(digest)
    object
  end

  # Builds build/tinyactors.node from core's amalgamation and src/.
  def build_addon
    core = CORE_SOURCES.map do |name, flags|
      compile(CORE_BUILD.join(name), OBJECTS.join('core', name.sub(/\.c\z/, '.o')),
              flags: [*flags, *OPTIMIZE, "-I#{CORE_BUILD}"], inputs: CORE_BUILD.glob('*.h'))
    end
    headers = [*ROOT.glob('src/**/*.h'), *CORE_BUILD.glob('*.h')]
    own = sources.map do |source|
      object = OBJECTS.join('addon', source.relative_path_from(ROOT.join('src')).sub_ext('.o'))
      compile(source, object, flags: [*ADDON_FLAGS, *OPTIMIZE, "-I#{CORE_BUILD}", "-I#{node_headers}"],
                              inputs: headers)
    end
    Toolchain::Target.host.plugin([*core, *own].map(&:to_s), into: ADDON.to_s)
  end
end
