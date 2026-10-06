# frozen_string_literal: true

require 'digest'
require 'fileutils'
require 'json'
require 'open3'
require 'pathname'
require 'toolchain'
require 'platform_files'
require 'release'

# The C libraries' release (docs/adr/0001-packaging-and-releases.md,
# decisions 4, 6, 9 and 16): every target built with `zig cc` from the
# amalgamation, its symbols verified, tested on this host and under qemu,
# and packed into deterministic tarballs. A project's release task only
# describes the project and calls CRelease.release.
#
# Each native library is one relocatable object: the amalgamation joined
# with zig's compiler-rt, which defines memcpy, memset, memmove, memcmp and
# bcmp (the compiler emits calls to them for aggregate copies whatever the
# flags). Only the public API stays global: everything else, the memory
# functions included, is made local, so a program that links the archive
# keeps its own C library's memcpy and no internal name can clash with its
# own (decision 16, as Ghostty does).
module CRelease
  NATIVE_TARGETS = Toolchain::Target.native_releases.map(&:name).freeze
  MEMORY = %w[memcpy memset memmove memcmp bcmp].freeze
  # zig 0.16's compiler-rt memset is scalar; this one replaces it in every
  # native release object (decision 16: 24% of core's time before, 4% after).
  MEMSET_SOURCE = Pathname.new(__dir__).join('memset.c')

  module_function

  def release_fail!(message)
    abort "release: #{message}"
  end

  # Runs `command`; answers its stdout, and fails the release, with its
  # output, when it fails.
  def capture!(*command, chdir: nil)
    options = chdir ? { chdir: chdir.to_s } : {}
    out, err, status = Open3.capture3(*command.map(&:to_s), **options)
    release_fail!("#{command.join(' ')} failed:\n#{out}#{err}") unless status.success?
    out
  end

  # The pinned tools (Toolchain::Tool), resolved before anything is built,
  # and docker for the container smoke tests.
  def require_tools!(containers:)
    Toolchain::Tool.each(&:path)
    return if !containers || system('sh', '-c', 'command -v docker', out: File::NULL)

    release_fail!('docker is missing; it runs the Linux smoke tests (or pass --skip-containers)')
  end

  # The functions a binary built for `target` exports: every public,
  # implemented, non-inline function of the API description, and those of
  # the target's platform (a `platform` field, decision 9); nil is a
  # native target's. `project` may be an API description's path instead
  # (a dependency's).
  def api_functions(project, target: nil)
    path = project.respond_to?(:api) ? project.api : Pathname.new(project)
    platforms = target ? PlatformFiles.suffixes_for(target) : []
    JSON.parse(path.read).fetch('functions').select do |function|
      function['public'] && !function['static_inline'] && function['implemented'] &&
        (function['platform'].nil? || platforms.include?(function['platform']))
    end.map { |function| function['name'] }.sort
  end

  # MAJOR.MINOR.PATCH from the <MACRO>_VERSION_* macros of its version header.
  def header_version(project)
    text = project.version_header.read
    %w[MAJOR MINOR PATCH].map do |part|
      text[/^#define #{project.macro}_VERSION_#{part} (\d+)$/, 1] or
        release_fail!("#{project.version_header.basename} has no #{project.macro}_VERSION_#{part}")
    end.join('.')
  end

  def set_header_version!(project, version)
    numbers = version.split('.').first(3)
    text = %w[MAJOR MINOR PATCH].zip(numbers).reduce(project.version_header.read) do |header, (part, number)|
      header.sub(/^#define #{project.macro}_VERSION_#{part} \d+$/, "#define #{project.macro}_VERSION_#{part} #{number}")
    end
    project.version_header.write(text)
  end

  # ------------------------------------------------------------------
  # Dependencies (root ADR 0001, "Dependencies between releases")
  #
  # A library that builds on others (the codec on core and txml) ships only
  # its own code: its libraries leave exactly the functions of its
  # dependencies' APIs undefined, and a program links the dependencies'
  # released libraries beside it. The release builds against the
  # dependencies' published releases: it downloads them, and refuses to go
  # on unless their header, amalgamation and API description at HEAD are
  # byte-identical to what was published, so it never ships against an
  # unreleased library.

  # A dependency: `name` its library's name (`txml`), `directory` its
  # project (`tinyxml`), `macro` its version macros' prefix (`TXML`).
  Dependency = Data.define(:name, :directory, :macro) do
    def header = Release::REPO.join(directory, 'include', "#{name}.h")
    def sources = %W[#{name}.h #{name}.c #{name}.api.json].map { |file| Release::REPO.join(directory, 'include', file) }
  end

  # The version of `dependency` at HEAD, from its header.
  def dependency_version(dependency)
    text = dependency.header.read
    %w[MAJOR MINOR PATCH].map do |part|
      text[/^#define #{dependency.macro}_VERSION_#{part} (\d+)$/, 1] or
        release_fail!("#{dependency.header.basename} has no #{dependency.macro}_VERSION_#{part}")
    end.join('.')
  end

  # Downloads `dependency`'s published release of its version at HEAD into
  # <out>/dependencies/<name>/, checks every file against its SHA256SUMS,
  # unpacks it, and fails unless the published header, amalgamation and API
  # description equal HEAD's. Answers { name:, version:, api:, targets: {
  # target name => unpacked directory } }.
  def fetch_dependency!(dependency, out)
    version = dependency_version(dependency)
    tag = "#{dependency.directory}/v#{version}"
    base = "#{dependency.name}-#{version}"
    dir = out.join('dependencies', dependency.name)
    FileUtils.rm_rf(dir)
    FileUtils.mkdir_p(dir)
    wanted = ["#{base}.json", 'SHA256SUMS', "#{base}-src.tar.gz", *NATIVE_TARGETS.map { |name| "#{base}-#{name}.tar.gz" }]
    capture!('gh', 'release', 'download', tag, '--dir', dir, *wanted.flat_map { |file| ['--pattern', file] })
    sums = dir.join('SHA256SUMS').read.lines.to_h { |line| line.split.reverse }
    wanted.reject { |file| file == 'SHA256SUMS' }.each do |file|
      expected = sums[file] or release_fail!("#{tag}: SHA256SUMS does not list #{file}")
      actual = Digest::SHA256.file(dir.join(file)).hexdigest
      release_fail!("#{tag}: #{file} does not match SHA256SUMS") unless actual == expected
    end
    wanted.select { |file| file.end_with?('.tar.gz') }.each { |file| Toolchain::Tool[:tar].run('-xzf', dir.join(file), '-C', dir) }
    published = dir.join("#{base}-src")
    dependency.sources.each do |source|
      next if published.join(source.basename).file? && published.join(source.basename).binread == source.binread

      release_fail!("#{dependency.directory} at HEAD differs from its published #{version} (#{source.basename}); " \
                    "release #{dependency.directory} first")
    end
    puts "release: builds against #{dependency.name} #{version}: #{tag} matches HEAD"
    { name: dependency.name, version: version, api: published.join("#{dependency.name}.api.json"),
      targets: NATIVE_TARGETS.to_h { |name| [name, dir.join("#{base}-#{name}")] } }
  end

  # The released libraries of `dependencies` for `target`: include
  # directories, static archives and shared libraries.
  def dependency_libraries(dependencies, target)
    dependencies.map do |dependency|
      dir = dependency[:targets].fetch(target.name)
      shared = Dir[dir.join('lib', target.format.shared_library_glob).to_s].first or
        release_fail!("#{dir.basename} has no shared library")
      { include: dir.join('include'), archive: dir.join('lib', "lib#{dependency[:name]}.a"), shared: Pathname.new(shared) }
    end
  end

  # zig's compiler-rt for `target`, as one object, built once into
  # build/release/compiler-rt/<target>/. `zig cc` cannot bundle it into an
  # archive; `zig build-lib -fcompiler-rt` does, and the object is taken
  # out of that archive.
  def compiler_rt(project, target)
    dir = project.root.join('build/release/compiler-rt', target.name)
    out = dir.join('compiler_rt.o')
    return out if out.file?

    work = dir.join('work')
    FileUtils.rm_rf(work)
    FileUtils.mkdir_p(work)
    stub = work.join('stub.c')
    stub.write("int release_compiler_rt_stub;\n")
    Toolchain::Tool[:zig].run('build-lib', '-target', target.triple, '-OReleaseFast', *target.format.position_independent,
                              '-fcompiler-rt', stub, "-femit-bin=#{work.join('bundle.a')}", chdir: work)
    Toolchain::Tool[:llvm_ar].run('x', work.join('bundle.a'), chdir: work)
    member = Dir[work.join('**/compiler_rt.o').to_s].first or release_fail!("no compiler_rt.o for #{target.name}")
    FileUtils.chmod(0o644, member)
    FileUtils.mv(member, out)
    FileUtils.rm_rf(work)
    out
  end

  # The amalgamations `target` builds for `project`: the portable one, and
  # its platform amalgamations for the target's suffixes (PlatformFiles), of
  # those it has.
  def amalgamations(project, target)
    platform = PlatformFiles.suffixes_for(target).map { |suffix| project.platform_amalgamation(suffix) }.select(&:file?)
    [project.amalgamation, *platform]
  end

  # The library's one release object for `target`: its amalgamations
  # compiled with the API at default visibility, joined with compiler-rt,
  # stripped of debug info, and with only the API left global.
  def release_object(project, target, dir)
    FileUtils.mkdir_p(dir)
    api = api_functions(project, target: target)
    flags = %w[-g0 -ffunction-sections -fdata-sections]
    compiled = amalgamations(project, target).map do |source|
      target.compile(source, into: dir.join("#{source.basename('.c')}.o"),
                             includes: [project.header.dirname, *project.includes],
                             defines: ["#{project.api_macro}=__attribute__((visibility(\"default\")))"], flags: flags)
    end
    # The vectorized memset takes the place of compiler-rt's weak, scalar one
    # (MEMSET_SOURCE; decision 16).
    memset = target.compile(MEMSET_SOURCE, into: dir.join('memset.o'), flags: flags)
    # Rooted at the API: only the compiler-rt functions the library calls
    # stay, where the format's linker collects garbage.
    joined = target.combine([*compiled, memset, compiler_rt(project, target)], into: dir.join("#{project.name}-joined.o"),
                                                                             keep: api)
    exports = dir.join('exports.txt')
    exports.write(api.map { |name| "#{target.format.symbol(name)}\n" }.join)
    object = dir.join("#{project.name}.o")
    Toolchain::Tool[:llvm_objcopy].run('--strip-debug', "--keep-global-symbols=#{exports}", joined, object)
    [object, exports]
  end

    # Builds `target`'s static and shared library into <out>/<target>/:
    # include/<name>.h, lib/lib<name>.a, and lib<name>.so.<major>.<minor>
    # with a lib<name>.so symlink (Linux) or lib<name>.<major>.<minor>.dylib
    # (macOS). The shared library's name follows the minor version on 0.x.
    # A library with `dependencies` (fetch_dependency!) leaves their functions
    # undefined in its archive, and links its shared library against their
    # released shared libraries, so it names them (their sonames, or their
    # @rpath install names on macOS) and a program that links it needs
    # nothing else to find them.
    def build_native(project, target_name, version, out, dependencies: [])
      target = Toolchain::Target.release(target_name)
      needed = dependency_libraries(dependencies, target).map { |other| other[:shared] }
      dir = out.join(target_name)
      FileUtils.rm_rf(dir)
      object, exports = release_object(project, target, dir.join('work'))
      lib = dir.join('lib')
      archive = target.archive([object], into: lib.join("lib#{project.name}.a"))
      major, minor, = version.split('.')
      shared = target.shared_library([object, *needed], library: project.name, version: "#{major}.#{minor}", dir: lib,
                                                        exports: exports)
      FileUtils.mkdir_p(dir.join('include'))
      FileUtils.cp(project.header, dir.join('include'))
      { target: target, dir: dir, object: object, archive: archive.path, shared: shared.path,
        shared_name: shared.basename.to_s, needed: needed.map { |path| path.basename.to_s },
        cflags: target.cflags + %w[-g0 -ffunction-sections -fdata-sections] }
    end

    def expect_equal!(what, actual, expected)
      return if actual == expected

      release_fail!("#{what}:\n  unexpected: #{(actual - expected).join(' ')}\n  missing:    #{(expected - actual).join(' ')}")
    end

    # The symbol checks of decision 16 for one target's libraries. `external`
    # are the dependencies' API functions, which a dependent's libraries
    # leave undefined and nothing else may. What the platform's runtime adds
    # (libSystem, ld64's symbols) is the object format's to allow.
  # `libc` are the C library functions a `libc` library may import on this
  # target (Library#libc_imports): a Linux library links no C library, so they
  # bind to the program's, glibc or musl (decision 10's rule, as the Node
  # addon follows it); each must have the same name and signature in both.
  def verify_native!(project, build, external: [], libc: [])
    target = build[:target]
    expected = api_functions(project, target: target)
    allowed = external + libc
    Toolchain::Binary.new(build[:archive]).must_export_exactly!(expected).must_leave_undefined_only!(allowed)
                     .must_keep_local!(MEMORY)
    Toolchain::Binary.new(build[:shared]).must_export_exactly!(expected).must_leave_undefined_only!(allowed)
                     .must_load_only!(build[:needed]).must_be_named!(build[:shared_name]).must_be_for!(target)
    # The archive holds exactly the release object, which is checked.
    Toolchain::Binary.new(build[:object]).must_be_for!(target)
    puts "release: #{target.name}: #{project.name} exports the #{expected.size} API functions; undefined symbols allowed" \
         "#{external.empty? ? '' : " (the dependencies' #{external.size} API functions among them)"}" \
         "#{libc.empty? ? '' : " and #{libc.size} C library functions"}; memory functions local; architecture right"
  end

  # Links each [label, inputs, arguments] test (inputs: its objects, and any
  # library flags it needs) against the packed static and shared library of
  # this machine's target, with its C compiler, and runs it from the project
  # root.
  def host_tests!(project, build, tests, dependencies: [])
    Toolchain::Host.current.must_run!(build[:target], 'the host tests')
    dir = build[:dir].join('host-tests')
    FileUtils.mkdir_p(dir)
    lib = build[:archive].dirname
    others = dependency_libraries(dependencies, build[:target])
    failed = []
    { 'static' => [build[:archive].to_s, *others.map { |other| other[:archive].to_s }],
      # By path: the linker records the library's soname (or @rpath
      # install name), and the rpath finds it.
      'shared' => [build[:shared].to_s, "-Wl,-rpath,#{lib}",
                   *others.flat_map { |other| [other[:shared].to_s, "-Wl,-rpath,#{other[:shared].dirname}"] }] }.each do |kind, library|
      tests.each do |label, inputs, arguments|
        binary = Toolchain::Target.host.program([*Array(inputs), *library], into: dir.join("#{label}-#{kind}"))
        # A dependent's shared library names its dependencies; the loader
        # finds them on the library path, as it would installed ones.
        paths = [lib, *others.map { |other| other[:shared].dirname }].join(':')
        out, status = Open3.capture2e({ 'LD_LIBRARY_PATH' => paths }, binary.to_s, *arguments, chdir: project.root.to_s)
        next if status.success?

        warn out
        failed << "#{label} (#{kind})"
      end
    end
    release_fail!("tests failed against the packed library: #{failed.join(', ')}") unless failed.empty?
    with = dependencies.empty? ? '' : " with those of #{dependencies.map { |d| "#{d[:name]} #{d[:version]}" }.join(' and ')}"
    puts "release: #{tests.size} tests passed against the packed #{build[:target]} static and shared library#{with}"
  end

  # Builds `smoke` (one C file using the public API) for `build`'s Linux
  # target against its static and its shared library, and runs both in a
  # container of that architecture: under qemu for the other one. Docker
  # must be usable by the current user.
  def container_smoke!(build, smoke, dependencies: [])
    target = build[:target]
    dir = build[:dir].join('smoke')
    FileUtils.mkdir_p(dir)
    include = build[:dir].join('include')
    others = dependency_libraries(dependencies, target)
    includes = [include, *others.map { |other| other[:include] }].map { |path| "-I#{path}" }
    target.program([smoke, build[:archive], *others.map { |other| other[:archive] }], into: dir.join('smoke-static'),
                                                                                       flags: ['-O2', '-static', *includes])
    # The dependencies' shared libraries go beside the program, which finds
    # them through $ORIGIN inside the container.
    others.each { |other| FileUtils.cp(other[:shared], dir) }
    # zig's linker has no -l:<file>; the path records the soname all the same.
    target.program([smoke, build[:shared], *others.map { |other| dir.join(other[:shared].basename) }],
                   into: dir.join('smoke-shared'), flags: ['-O2', *includes, '-Wl,-rpath,$ORIGIN/../lib', '-Wl,-rpath,$ORIGIN'])
    platform = target.container_platform
    out = capture!('docker', 'run', '--rm', '--platform', platform, '-v', "#{build[:dir]}:/r:ro", 'alpine:3',
                   'sh', '-c', '/r/smoke/smoke-static && LD_LIBRARY_PATH=/r/lib:/r/smoke /r/smoke/smoke-shared')
    puts "release: #{target.name} in a #{platform} container, static and shared: #{out.lines.map(&:strip).uniq.join('; ')}"
  end

  # ------------------------------------------------------------------
  # WebAssembly (decisions 4 and 6)

  WASM = 'wasm32-freestanding'

  # The standalone module <out>/wasm32-freestanding/<name>.wasm: the
  # amalgamation and the platform amalgamation <name>_wasm.c (the module's
  # allocator among it), linked with no entry point, exporting the API, the
  # platform's functions included, and its memory, and importing nothing.
  # zig's default wasm CPU has bulk memory, so copies need no memcpy
  # (decision 16).
  #
  # A library that builds on others in the same release (`uses`, Projects)
  # ships one module with them, since a module cannot call into another's
  # memory: their portable amalgamations, its own, and its wasm platform
  # amalgamation, which stands in for theirs (decision 19). A `libc`
  # library's module is compiled against wasi-libc and linked as a WASI
  # reactor; it still imports nothing (verify_wasm!), and exports
  # _initialize beside the API.
  def build_wasm(project, out, uses: [])
    api_target = Toolchain::Target.release(WASM)
    target = project.libc ? Toolchain::Target.zig('wasm32-wasi') : api_target
    dir = out.join(api_target.name, project.name)
    FileUtils.rm_rf(dir)
    work = dir.join('work')
    platform = project.platform_amalgamation('wasm')
    release_fail!("#{platform.basename} is missing: run mise run amalgamate") unless platform.file?
    sources = [*uses.map(&:amalgamation), project.amalgamation, platform]
    objects = sources.map do |source|
      target.compile(source, into: work.join("#{source.basename('.c')}.o"),
                             includes: [project.header.dirname, *project.includes, *uses.map { |used| used.header.dirname }],
                             defines: ["#{project.api_macro}=__attribute__((visibility(\"default\")))"], flags: %w[-g0])
    end
    exports = [*uses, project].flat_map { |library| api_functions(library, target: api_target) }.uniq.sort
    wasm = target.wasm_module(objects, into: dir.join("#{project.name}.wasm"), exports: exports,
                                       flags: project.libc ? %w[-mexec-model=reactor] : []).path
    FileUtils.mkdir_p(dir.join('include'))
    [*uses, project].each { |library| FileUtils.cp(library.header, dir.join('include')) }
    { target: api_target, dir: dir, wasm: wasm, extra: [wasm], asset: 'wasm32', exports: exports,
      reactor: project.libc, triple: target.triple, cflags: target.cflags + %w[-g0] }
  end

  # The imports and exports of a wasm module, as [[module, name, kind]] and
  # [[name, kind]], read from its binary: the import section (2) and the
  # export section (7).
  def wasm_interface(path)
    bytes = path.binread.bytes
    release_fail!("#{path} is not a wasm module") unless bytes[0, 4] == [0, 0x61, 0x73, 0x6d]
    at = 8
    leb = lambda do
      value = 0
      shift = 0
      loop do
        byte = bytes[at]
        at += 1
        value |= (byte & 0x7f) << shift
        shift += 7
        break if byte < 0x80
      end
      value
    end
    name = lambda do
      length = leb.call
      text = bytes[at, length].pack('C*').force_encoding('UTF-8')
      at += length
      text
    end
    kinds = %w[function table memory global tag]
    imports = []
    exports = []
    while at < bytes.size
      id = bytes[at]
      at += 1
      size = leb.call
      finish = at + size
      if id == 2
        leb.call.times do
          module_name = name.call
          field = name.call
          kind = bytes[at]
          at += 1
          imports << [module_name, field, kinds[kind]]
          at = finish # the rest of an import's type is not needed to know it exists
          break
        end
      elsif id == 7
        leb.call.times do
          export = name.call
          kind = bytes[at]
          at += 1
          leb.call
          exports << [export, kinds[kind]]
        end
      end
      at = finish
    end
    [imports, exports]
  end

  # The module imports nothing and exports exactly the API (its own and that
  # of the libraries it holds), the platform's functions, its memory, and a
  # reactor's _initialize; then `test`, a lambda from the module's path to a
  # command, runs it.
  def verify_wasm!(build, test: nil)
    imports, exports = wasm_interface(build[:wasm])
    release_fail!("#{build[:wasm].basename} imports #{imports.map { |i| i.first(2).join('.') }.join(', ')}") unless imports.empty?
    expected = build[:exports].map { |name| [name, 'function'] } + [%w[memory memory]]
    expected << %w[_initialize function] if build[:reactor]
    expect_equal!("#{build[:wasm].basename} exports", exports.sort, expected.sort)
    puts "release: #{build[:wasm].basename}: imports nothing, exports the #{build[:exports].size} functions and memory" \
         "#{build[:reactor] ? ' (and _initialize, a WASI reactor)' : ''}"
    capture!(*test.call(build[:wasm])).lines.each { |line| puts "release: #{line.strip}" } if test
  end

  # ------------------------------------------------------------------
  # Packing

  # A deterministic .tar.gz of `dirname` under `parent`: sorted entries,
  # one mtime, owner and group 0, no timestamp or name in the gzip header.
  def tarball(parent, dirname, out, epoch)
    tar = Toolchain::Tool[:tar].run('--sort=name', "--mtime=@#{epoch}", '--owner=0', '--group=0', '--numeric-owner',
                                    '--mode=u=rwX,go=rX', '--format=gnu', '-C', parent, '-cf', '-', dirname, binary: true)
    out.binwrite(Toolchain::Tool[:gzip].run('-n', '-9', stdin: tar, binary: true))
    out
  end

  def sha256_tree(dir)
    Dir[dir.join('**/*').to_s].sort.select { |path| File.file?(path) && !File.symlink?(path) }.to_h do |path|
      [Pathname.new(path).relative_path_from(dir).to_s, Digest::SHA256.file(path).hexdigest]
    end
  end

  # Stages and packs one library's assets into `assets`: its source
  # tarball, a tarball per build, and its manifest last. The release writes
  # one SHA256SUMS for every library's assets.
  def pack(project, version:, builds:, sources:, readme:, assets:, commit:, epoch:, dependencies: [])
    staging = assets.join('staging', project.name)
    FileUtils.mkdir_p(staging)
    base = "#{project.name}-#{version}"
    packed = []
    manifest_targets = {}

    source_dir = staging.join("#{base}-src")
    FileUtils.mkdir_p(source_dir)
    (sources + [Release::REPO.join('LICENSE'), readme]).each { |file| FileUtils.cp(file, source_dir) }
    packed << tarball(staging, source_dir.basename, assets.join("#{base}-src.tar.gz"), epoch)

    builds.each do |build|
      target = build[:target]
      label = build[:asset] || target.name
      staged = staging.join("#{base}-#{label}")
      FileUtils.mkdir_p(staged)
      FileUtils.cp_r(build[:dir].join('include'), staged)
      FileUtils.cp_r(build[:dir].join('lib'), staged) if build[:dir].join('lib').directory?
      build[:extra]&.each { |file| FileUtils.cp(file, staged) }
      FileUtils.cp(Release::REPO.join('LICENSE'), staged)
      packed << tarball(staging, staged.basename, assets.join("#{base}-#{label}.tar.gz"), epoch)
      manifest_targets[label] = {
        'triple' => build[:triple] || target.triple,
        'cflags' => build.fetch(:cflags),
        'files' => sha256_tree(staged)
      }
    end

    manifest = assets.join("#{base}.json")
    manifest.write("#{JSON.pretty_generate(
      'name' => project.name,
      'version' => version,
      'commit' => commit,
      'source_date_epoch' => epoch.to_i,
      'zig' => Toolchain::Tool[:zig].identity,
      'api' => Digest::SHA256.file(project.api).hexdigest,
      **(dependencies.empty? ? {} : { 'dependencies' => dependencies.to_h { |d| [d[:name], d[:version]] } }),
      'targets' => manifest_targets,
      'assets' => packed.to_h { |file| [file.basename.to_s, Digest::SHA256.file(file).hexdigest] }
    )}\n")
    packed << manifest
    FileUtils.rm_rf(staging)
    packed
  end

  # ------------------------------------------------------------------
  # The command

  # One library of a release: `project`, its `tests` against the packed
  # library ([label, inputs, arguments], host_tests!), its container
  # `smoke` program, the `sources` of its source tarball, the libraries of
  # the same release it builds on (`uses`, by name), and, when it ships a
  # wasm module, `wasm_test`, a lambda from the module's path to the command
  # that tests it (true for none).
  #
  # `libc_imports` is a lambda from a target to the C library functions a
  # `libc` library may import there (verify_native!).
  Library = Data.define(:project, :tests, :smoke, :sources, :uses, :wasm_test, :libc_imports) do
    def initialize(project:, tests:, smoke:, sources:, uses: [], wasm_test: nil, libc_imports: ->(_target) { [] })
      super
    end

    def name = project.name
  end

  # `mise run release -- <version> [--publish] [--skip-containers]` for a C
  # project's libraries, released together under one tag. The stages (ADR
  # 0001, "The release command"):
  #
  # 1. check: a clean main in step with origin, the tag free, the changelog,
  #    the project's `checks` (its amalgamations and test suite);
  # 2. version: the version macros set and `regenerate` run when they
  #    differ; with --publish, committed as release(<dir>): v<version> and
  #    tagged; a dry run restores the files instead;
  # 3. build every native target of each library, and its wasm module;
  # 4. verify: symbols, architectures, `tests` against the packed libraries
  #    of this machine's target, `smoke` in a container for each Linux
  #    target, the wasm module's interface and its test;
  # 5. pack: deterministic tarballs, a manifest per library and one
  #    SHA256SUMS in build/release/<version>/assets/;
  # 6. publish (--publish): push the commit and tag, then the GitHub release.
  #
  # A failure after the commit and tag, before anything is pushed, undoes
  # them. The first library's version macros are the release's.
  #
  # A library that `uses` another of the same release is built against it:
  # its libraries leave that one's API functions undefined, its shared
  # library names that one's, and its tests and smoke link both (decision
  # 17's rules, without a download). `dependencies` (CRelease::Dependency)
  # are libraries of other projects, released before: fetched and matched
  # against HEAD in the check stage, and treated the same way.
  def release(libraries:, directory:, title:, argv:, checks:, regenerate:, readme:, dependencies: [])
    publish = !argv.delete('--publish').nil?
    containers = argv.delete('--skip-containers').nil?
    release_fail!("usage: mise run release -- <version> [--publish] [--skip-containers]") unless argv.size == 1
    version = argv.first
    prefix = "#{directory}/"
    first = libraries.first.project
    require_tools!(containers: containers)

    # 1. Check.
    Release.check_tree!(scope: [directory, 'mise'])
    tag = Release.check_version!(prefix, version)
    release_fail!("#{version} is a prerelease; C releases are MAJOR.MINOR.PATCH") if Release.parse_version(version)[3]
    Release.check_changelog!(prefix: prefix, version: version)
    checks.each { |command| Release.run(*command, chdir: first.root) }

    out = first.root.join('build/release', version)
    fetched = dependencies.map { |dependency| fetch_dependency!(dependency, out) }
    notes = out.join('notes.md')
    FileUtils.mkdir_p(out)
    notes.write(Release.notes(prefix: prefix, version: version, title: title))
    subject = "release(#{directory}): v#{version}"
    changed = []
    committed = false
    tagged = false
    pushed = false
    begin
      # 2. Version.
      if header_version(first) != version
        set_header_version!(first, version)
        regenerate.each { |command| Release.run(*command, chdir: first.root) }
        changed = Release.git('status', '--porcelain').to_s.lines.map { |line| line[3..].strip }
      end
      if publish
        if changed.empty?
          Release.run('git', 'tag', '--annotate', '--cleanup=verbatim', tag, '--file', notes.to_s)
        else
          Release.commit_and_tag!(paths: changed.map { |path| Release::REPO.join(path) }, subject: subject,
                                  tag: tag, notes_file: notes)
          committed = true
        end
        tagged = true
      end
      commit = Release.git('rev-parse', 'HEAD').to_s.strip
      epoch = Release.git('show', '-s', '--format=%ct', 'HEAD').to_s.strip

      # 3. Build, 4. verify, 5. pack: each library after those it uses.
      assets = out.join('assets')
      FileUtils.rm_rf(assets)
      packed = []
      built = {}
      libraries.each do |library|
        project = library.project
        used = library.uses.map { |name| built.fetch(name) { release_fail!("#{library.name} uses #{name}, which comes later") } }
        others = fetched + used.map { |entry| entry[:dependency] }
        external = others.flat_map { |other| api_functions(other[:api]) }.uniq.sort
        targets = out.join('targets', project.name)
        natives = NATIVE_TARGETS.map { |name| build_native(project, name, version, targets, dependencies: others) }
        natives.each do |build|
          verify_native!(project, build, external: external, libc: library.libc_imports.call(build[:target]))
        end
        host = natives.find { |build| build[:target] == Toolchain::Host.current.target }
        host_tests!(project, host, library.tests, dependencies: others) unless library.tests.empty?
        if containers
          natives.select { |build| build[:target].container_platform }
                 .each { |build| container_smoke!(build, library.smoke, dependencies: others) }
        else
          puts "release: --skip-containers: no Linux container smoke test of #{project.name} ran"
        end
        builds = natives
        if library.wasm_test
          wasm = build_wasm(project, out.join('targets'), uses: used.map { |entry| entry[:project] })
          verify_wasm!(wasm, test: library.wasm_test == true ? nil : library.wasm_test)
          builds += [wasm]
        end
        packed += pack(project, version: version, builds: builds, sources: library.sources, readme: readme,
                                assets: assets, commit: commit, epoch: epoch, dependencies: others)
        built[library.name] = {
          project: project,
          dependency: { name: project.name, version: version, api: project.api,
                        targets: natives.to_h { |build| [build[:target].name, build[:dir]] } }
        }
      end
      FileUtils.rm_rf(assets.join('staging'))
      sums = Release.write_sha256sums(packed.map(&:to_s), assets.join('SHA256SUMS'))
      assets_list = packed + [Pathname.new(sums)]

      unless publish
        puts <<~DRY
          release: dry run for #{libraries.map(&:name).join(' and ')} #{version} passed; nothing was committed, tagged or uploaded.
            would tag:     #{tag}
            assets:        #{assets_list.map { |file| file.basename.to_s }.join(', ')}
            in:            #{assets.relative_path_from(Release::REPO)}
            notes:         #{notes.relative_path_from(Release::REPO)}
          Publish with: mise run //#{directory}:release -- #{version} --publish
        DRY
        return
      end

      # 6. Publish.
      Release.push_and_release!(tag: tag, title: title, notes_file: notes, assets: assets_list)
      pushed = true
      puts "release: published #{libraries.map(&:name).join(' and ')} #{version}, tagged #{tag}"
    ensure
      if tagged && !pushed
        if committed
          Release.undo_commit_and_tag!(subject: subject, tag: tag)
        else
          Release.git('tag', '--delete', tag)
        end
        warn "release: undid the local #{committed ? 'commit and ' : ''}tag #{tag}; nothing was pushed"
      end
      unless committed
        changed.each { |path| Release.git('checkout', '--', path) }
      end
    end
  rescue Toolchain::Error => e
    release_fail!(e.message)
  end
end
