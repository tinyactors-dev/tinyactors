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
  # (ROADMAP.md, M1, "Plan and progress"): M1 is every suite of the set,
  # protocol and scenarios, on both datamodels, native and quickjs, and
  # both pass under node and bun (docs/plan.xml, p1-conform).
  GATE = { suites: %w[protocol scenarios], datamodels: %w[native quickjs] }.freeze
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
  # txml, the project's XML library (tinyxml/include/txml.c, its
  # amalgamation; freestanding, allocating through the allocator it is
  # given), compiled into the addon: the native datamodel parses XML
  # content with it (docs/plan.xml, decision d-dom).
  TXML = ROOT.join('../tinyxml/include').expand_path
  TXML_FLAGS = %w[-std=c11 -w].freeze
  # The realm's script of the native datamodel, embedded as a C string
  # (src/datamodel/realms.c includes it), as the Python binding embeds
  # its support.py.
  REALM = ROOT.join('src/datamodel/realm.js')
  GENERATED = BUILD.join('generated')
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

  # A gate over the application's store (README.md, "Hibernation" and
  # "Migration"): tools/stress.mjs, the hibernation gate (N actors of a
  # counter, at most R resident, the rest in each runtime's SQLite,
  # node:sqlite or bun:sqlite), or tools/migration.mjs, the migration gate
  # (the same population moved to a new chart version while events come),
  # on both datamodels under both runtimes, each run a process of its own.
  # A failed check (a count or version wrong, more than R resident after an
  # eviction, an actor left behind) ends the task.
  def stress(actors:, resident:, events:, tool: 'stress')
    RUNTIMES.each do |runtime|
      DATAMODELS.each do |datamodel|
        puts "#{tool}: #{runtime} #{datamodel}, #{actors} actors, #{resident} resident, #{events} events"
        run(pinned(runtime), *(runtime == 'node' ? ['--disable-warning=ExperimentalWarning'] : []), "tools/#{tool}.mjs",
            '--datamodel', datamodel, '--actors', actors.to_s, '--resident', resident.to_s, '--events', events.to_s)
      end
    end
  end

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

  # The realm's script as the lines of a C string literal, written only
  # when they change (the digest of each compile reads it).
  def embed_realm
    GENERATED.mkpath
    lines = REALM.read.lines.map { |line| "    \"#{line.chomp.gsub(/[\\"]/) { |c| "\\#{c}" }}\\n\"\n" }
    text = "/* GENERATED by mise/napi.rb from src/datamodel/realm.js. */\n#{lines.join}"
    target = GENERATED.join('realm.inc')
    target.write(text) unless target.exist? && target.read == text
    target
  end

  # Builds build/tinyactors.node from core's amalgamation, txml and src/.
  def build_addon
    core = CORE_SOURCES.map do |name, flags|
      compile(CORE_BUILD.join(name), OBJECTS.join('core', name.sub(/\.c\z/, '.o')),
              flags: [*flags, *OPTIMIZE, "-I#{CORE_BUILD}"], inputs: CORE_BUILD.glob('*.h'))
    end
    txml = compile(TXML.join('txml.c'), OBJECTS.join('txml', 'txml.o'),
                   flags: [*TXML_FLAGS, *OPTIMIZE, "-I#{TXML}"], inputs: [TXML.join('txml.h')])
    headers = [*ROOT.glob('src/**/*.h'), *CORE_BUILD.glob('*.h'), TXML.join('txml.h'), embed_realm]
    own = sources.map do |source|
      object = OBJECTS.join('addon', source.relative_path_from(ROOT.join('src')).sub_ext('.o'))
      compile(source, object, flags: [*ADDON_FLAGS, *OPTIMIZE, "-I#{CORE_BUILD}", "-I#{TXML}", "-I#{GENERATED}",
                                      "-I#{node_headers}"],
                              inputs: headers)
    end
    Toolchain::Target.host.plugin([*core, txml, *own].map(&:to_s), into: ADDON.to_s)
  end
end
