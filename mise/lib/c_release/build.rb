# frozen_string_literal: true

require 'digest'
require 'fileutils'
require 'json'
require 'open3'
require 'pathname'
require 'shellwords'
require 'toolchain'
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
  NATIVE_TARGETS = %w[aarch64-macos x86_64-linux aarch64-linux].freeze
  MEMORY = %w[memcpy memset memmove memcmp bcmp].freeze
  # What a macOS library may leave undefined: libSystem, which every macOS
  # process has. The first two come from compiler-rt; Darwin's clang lowers
  # zeroing to bzero whatever the flags (-fno-builtin-bzero does not stop
  # it), and compiler-rt does not define it.
  MACOS_UNDEFINED = %w[__availability_version_check _bzero _sys_icache_invalidate].freeze
  MACOS_DYLIB_UNDEFINED = (MACOS_UNDEFINED + %w[dyld_stub_binder]).freeze
  # Symbols ld64 defines in every dylib.
  MACOS_SYNTHESIZED = %w[___dso_handle __mh_dylib_header].freeze
  # Tools the release needs beyond the pinned zig. llvm's tools read every
  # object format the targets produce; GNU tar packs deterministically.
  TOOLS = %w[llvm-ar llvm-nm llvm-objcopy llvm-objdump llvm-readobj tar gzip cc docker].freeze
  # llvm-readobj --file-headers: ELF's Machine and Mach-O's CpuType.
  ARCHITECTURES = {
    x86_64: /EM_X86_64|CpuType: X86-64/,
    aarch64: /EM_AARCH64|CpuType: Arm64/
  }.freeze

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

  def require_tools!(tools = TOOLS)
    missing = tools.reject { |tool| system('sh', '-c', "command -v #{tool.shellescape}", out: File::NULL) }
    release_fail!("missing tools: #{missing.join(', ')} (llvm's binutils, GNU tar and gzip)") unless missing.empty?
    return if capture!('tar', '--version').include?('GNU tar')

    release_fail!('tar is not GNU tar, which the release needs for --sort, --mtime and --owner')
  end

  # The functions a binary built for `platform` exports: every public,
  # implemented, non-inline function of the API description, and those
  # of the platform's own (a `platform` field, decision 9).
  def api_functions(project, platform: nil)
    JSON.parse(project.api.read).fetch('functions').select do |function|
      function['public'] && !function['static_inline'] && function['implemented'] &&
        (function['platform'].nil? || function['platform'] == platform)
    end.map { |function| function['name'] }.sort
  end

  # MAJOR.MINOR.PATCH from the header's <MACRO>_VERSION_* macros.
  def header_version(project)
    text = project.header.read
    %w[MAJOR MINOR PATCH].map do |part|
      text[/^#define #{project.macro}_VERSION_#{part} (\d+)$/, 1] or
        release_fail!("#{project.header.basename} has no #{project.macro}_VERSION_#{part}")
    end.join('.')
  end

  def set_header_version!(project, version)
    numbers = version.split('.').first(3)
    text = %w[MAJOR MINOR PATCH].zip(numbers).reduce(project.header.read) do |header, (part, number)|
      header.sub(/^#define #{project.macro}_VERSION_#{part} \d+$/, "#define #{project.macro}_VERSION_#{part} #{number}")
    end
    project.header.write(text)
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
    capture!(Toolchain.zig, 'build-lib', '-target', target.triple, '-OReleaseFast',
             *(target.wasm? ? [] : ['-fPIC']), '-fcompiler-rt', stub, "-femit-bin=#{work.join('bundle.a')}", chdir: work)
    capture!('llvm-ar', 'x', work.join('bundle.a'), chdir: work)
    member = Dir[work.join('**/compiler_rt.o').to_s].first or release_fail!("no compiler_rt.o for #{target.name}")
    FileUtils.chmod(0o644, member)
    FileUtils.mv(member, out)
    FileUtils.rm_rf(work)
    out
  end

  # The symbol names `names` take in `target`'s object format.
  def mangle(target, names) = target.macos? ? names.map { |name| "_#{name}" } : names

  # The library's one release object for `target`: the amalgamation
  # compiled with the API at default visibility, joined with compiler-rt,
  # stripped of debug info, and with only the API left global.
  def release_object(project, target, dir)
    FileUtils.mkdir_p(dir)
    api = api_functions(project, platform: target.wasm? ? 'wasm' : nil)
    compiled = dir.join("#{project.name}-amalgamation.o")
    Toolchain.compile(target, project.amalgamation, compiled,
                      includes: [project.header.dirname],
                      defines: ["#{project.macro}_API=__attribute__((visibility(\"default\")))"],
                      flags: %w[-g0 -ffunction-sections -fdata-sections])
    joined = dir.join("#{project.name}-joined.o")
    runtime = compiler_rt(project, target)
    if target.macos?
      # zig has no ld64.lld to garbage-collect a relocatable link; the
      # dylib's -dead_strip drops what the API does not reach.
      capture!(Toolchain.zig, 'cc', '-target', target.triple, '-r', compiled, runtime, '-o', joined)
    else
      # Rooted at the API: only the compiler-rt functions the library calls
      # stay.
      capture!(Toolchain.zig, 'ld.lld', '-r', '--gc-sections', *api.flat_map { |name| ['-u', name] },
               compiled, runtime, '-o', joined)
    end
    exports = dir.join('exports.txt')
    exports.write(mangle(target, api).map { |name| "#{name}\n" }.join)
    object = dir.join("#{project.name}.o")
    capture!('llvm-objcopy', '--strip-debug', "--keep-global-symbols=#{exports}", joined, object)
    [object, exports]
  end

  # Builds `target`'s static and shared library into <out>/<target>/:
  # include/<name>.h, lib/lib<name>.a, and lib<name>.so.<major>.<minor>
  # with a lib<name>.so symlink (Linux) or lib<name>.<major>.<minor>.dylib
  # (macOS). The shared library's name follows the minor version on 0.x.
  def build_native(project, target_name, version, out)
    target = Toolchain.target(target_name)
    dir = out.join(target_name)
    FileUtils.rm_rf(dir)
    object, exports = release_object(project, target, dir.join('work'))
    lib = dir.join('lib')
    FileUtils.mkdir_p(lib)
    archive = Toolchain.archive(target, [object], lib.join("lib#{project.name}.a"))
    major, minor, = version.split('.')
    if target.macos?
      name = "lib#{project.name}.#{major}.#{minor}.dylib"
      Toolchain.shared(target, [object], lib.join(name), name: name, libc: true,
                                                          flags: ['-Wl,-dead_strip', "-Wl,-exported_symbols_list,#{exports}"])
    else
      name = "lib#{project.name}.so.#{major}.#{minor}"
      Toolchain.shared(target, [object], lib.join(name), name: name, flags: %w[-Wl,--gc-sections -Wl,-S])
      File.symlink(name, lib.join("lib#{project.name}.so"))
    end
    FileUtils.mkdir_p(dir.join('include'))
    FileUtils.cp(project.header, dir.join('include'))
    { target: target, dir: dir, object: object, archive: archive, shared: lib.join(name), shared_name: name,
      cflags: Toolchain.cflags(target) + %w[-g0 -ffunction-sections -fdata-sections] }
  end

  def symbols(file, *flags)
    capture!('llvm-nm', *flags, '--format=just-symbols', file).lines.map(&:strip)
                                                          .reject { |line| line.empty? || line.end_with?(':') }.uniq.sort
  end

  def expect_equal!(what, actual, expected)
    return if actual == expected

    release_fail!("#{what}:\n  unexpected: #{(actual - expected).join(' ')}\n  missing:    #{(expected - actual).join(' ')}")
  end

  def expect_subset!(what, actual, allowed)
    extra = actual - allowed
    release_fail!("#{what}: #{extra.join(' ')} (allowed: #{allowed.empty? ? 'none' : allowed.join(' ')})") unless extra.empty?
  end

  def expect_architecture!(file, target)
    headers = capture!('llvm-readobj', '--file-headers', file)
    wanted = ARCHITECTURES.fetch(target.arch)
    other = ARCHITECTURES.reject { |arch, _| arch == target.arch }.values
    return if headers.match?(wanted) && other.none? { |pattern| headers.match?(pattern) }

    release_fail!("#{file} is not #{target.arch}:\n#{headers}")
  end

  # The symbol checks of decision 16 for one target's libraries.
  def verify_native!(project, build)
    target = build[:target]
    expected = mangle(target, api_functions(project))
    archive = build[:archive]
    shared = build[:shared]
    expect_equal!("#{target.name}: libraries export exactly the API; #{archive.basename}",
                  symbols(archive, '-g', '--defined-only'), expected)
    expect_subset!("#{target.name}: #{archive.basename} leaves undefined", symbols(archive, '-u'),
                   target.macos? ? MACOS_UNDEFINED : [])
    global_memory = symbols(archive, '-g') & mangle(target, MEMORY)
    release_fail!("#{archive.basename}: #{global_memory.join(' ')} must be local") unless global_memory.empty?
    if target.macos?
      expect_equal!("#{shared.basename} exports", symbols(shared, '-g', '--defined-only') - MACOS_SYNTHESIZED, expected)
      expect_subset!("#{shared.basename} leaves undefined", symbols(shared, '-u'), MACOS_DYLIB_UNDEFINED)
      id = capture!('llvm-objdump', '--macho', '--dylib-id', shared)
      release_fail!("#{shared.basename}: install name is not @rpath/#{build[:shared_name]}") unless id.include?("@rpath/#{build[:shared_name]}")
    else
      expect_equal!("#{shared.basename} exports", symbols(shared, '-D', '--defined-only'), expected)
      expect_subset!("#{shared.basename} leaves undefined", symbols(shared, '-D', '-u'), [])
      dynamic = capture!('llvm-readobj', '--dynamic-table', shared)
      release_fail!("#{shared.basename} names a library it needs:\n#{dynamic}") if dynamic.include?('NEEDED')
      release_fail!("#{shared.basename}: soname is not #{build[:shared_name]}") unless dynamic.include?("[#{build[:shared_name]}]")
    end
    # The archive holds exactly the release object, which is checked.
    [build[:object], shared].each { |file| expect_architecture!(file, target) }
    puts "release: #{target.name}: exports are the #{expected.size} API functions, undefined symbols allowed, " \
         'memory functions local, architecture right'
  end

  def host_linux_x86_64? = RUBY_PLATFORM.start_with?('x86_64-linux')

  # Links each [label, object, arguments] test against the packed static and
  # shared library of the host's target, with the host's C compiler, and
  # runs it from the project root.
  def host_tests!(project, build, tests)
    unless host_linux_x86_64? && build[:target].name == 'x86_64-linux'
      release_fail!('the host tests need an x86_64 Linux host')
    end
    dir = build[:dir].join('host-tests')
    FileUtils.mkdir_p(dir)
    lib = build[:archive].dirname
    failed = []
    { 'static' => [build[:archive].to_s],
      'shared' => ["-L#{lib}", "-l:#{build[:shared_name]}", "-Wl,-rpath,#{lib}"] }.each do |kind, library|
      tests.each do |label, object, arguments|
        binary = dir.join("#{label}-#{kind}")
        capture!('cc', object, *library, '-o', binary)
        out, status = Open3.capture2e(binary.to_s, *arguments, chdir: project.root.to_s)
        next if status.success?

        warn out
        failed << "#{label} (#{kind})"
      end
    end
    release_fail!("tests failed against the packed library: #{failed.join(', ')}") unless failed.empty?
    puts "release: #{tests.size} tests passed against the packed x86_64-linux static and shared library"
  end

  # Builds `smoke` (one C file using the public API) for `build`'s Linux
  # target against its static and its shared library, and runs both in a
  # container of that architecture: under qemu for the other one. Docker
  # must be usable by the current user.
  def container_smoke!(build, smoke)
    target = build[:target]
    dir = build[:dir].join('smoke')
    FileUtils.mkdir_p(dir)
    include = build[:dir].join('include')
    capture!(Toolchain.zig, 'cc', '-target', target.triple, '-O2', '-static', "-I#{include}", smoke, build[:archive],
             '-o', dir.join('smoke-static'))
    # zig's linker has no -l:<file>; the path records the soname all the same.
    capture!(Toolchain.zig, 'cc', '-target', target.triple, '-O2', "-I#{include}", smoke, build[:shared],
             '-Wl,-rpath,$ORIGIN/../lib', '-o', dir.join('smoke-shared'))
    platform = { aarch64: 'linux/arm64', x86_64: 'linux/amd64' }.fetch(target.arch)
    out = capture!('docker', 'run', '--rm', '--platform', platform, '-v', "#{build[:dir]}:/r:ro", 'alpine:3',
                   'sh', '-c', '/r/smoke/smoke-static && /r/smoke/smoke-shared')
    puts "release: #{target.name} in a #{platform} container, static and shared: #{out.lines.map(&:strip).uniq.join('; ')}"
  end

  # ------------------------------------------------------------------
  # WebAssembly (decisions 4 and 6)

  WASM = 'wasm32-freestanding'

  # The standalone module <out>/wasm32-freestanding/<name>.wasm: the
  # amalgamation and the platform amalgamation include/<name>_wasm.c (the
  # module's allocator among it), linked with no entry point, exporting the
  # API, the platform's functions included, and its memory, and importing
  # nothing. zig's default wasm CPU has bulk memory, so copies need no
  # memcpy (decision 16).
  def build_wasm(project, out)
    target = Toolchain.target(WASM)
    dir = out.join(target.name)
    FileUtils.rm_rf(dir)
    work = dir.join('work')
    platform = project.root.join("include/#{project.name}_wasm.c")
    release_fail!("#{platform.basename} is missing: run mise run amalgamate") unless platform.file?
    objects = [project.amalgamation, platform].map do |source|
      Toolchain.compile(target, source, work.join("#{source.basename('.c')}.o"),
                        includes: [project.header.dirname],
                        defines: ["#{project.macro}_API=__attribute__((visibility(\"default\")))"], flags: %w[-g0])
    end
    wasm = Toolchain.wasm_module(target, objects, dir.join("#{project.name}.wasm"),
                                 exports: api_functions(project, platform: 'wasm'))
    FileUtils.mkdir_p(dir.join('include'))
    FileUtils.cp(project.header, dir.join('include'))
    { target: target, dir: dir, wasm: wasm, extra: [wasm], asset: 'wasm32', cflags: Toolchain.cflags(target) + %w[-g0] }
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

  # The module imports nothing and exports exactly the API, the platform's
  # functions and its memory; then `test`, a lambda from the module's path
  # to a command, runs it.
  def verify_wasm!(project, build, test: nil)
    imports, exports = wasm_interface(build[:wasm])
    release_fail!("#{build[:wasm].basename} imports #{imports.map { |i| i.first(2).join('.') }.join(', ')}") unless imports.empty?
    expected = api_functions(project, platform: 'wasm').map { |name| [name, 'function'] } + [%w[memory memory]]
    expect_equal!("#{build[:wasm].basename} exports", exports.sort, expected.sort)
    puts "release: #{build[:wasm].basename}: imports nothing, exports the #{expected.size - 1} functions and memory"
    capture!(*test.call(build[:wasm])).lines.each { |line| puts "release: #{line.strip}" } if test
  end

  # ------------------------------------------------------------------
  # Packing

  # A deterministic .tar.gz of `dirname` under `parent`: sorted entries,
  # one mtime, owner and group 0, no timestamp or name in the gzip header.
  def tarball(parent, dirname, out, epoch)
    tar = capture_binary!('tar', '--sort=name', "--mtime=@#{epoch}", '--owner=0', '--group=0', '--numeric-owner',
                          '--mode=u=rwX,go=rX', '--format=gnu', '-C', parent, '-cf', '-', dirname)
    gzip, status = Open3.capture2('gzip', '-n', '-9', stdin_data: tar, binmode: true)
    release_fail!('gzip failed') unless status.success?
    out.binwrite(gzip)
    out
  end

  def capture_binary!(*command)
    out, err, status = Open3.capture3(*command.map(&:to_s), binmode: true)
    release_fail!("#{command.join(' ')} failed:\n#{err}") unless status.success?
    out
  end

  def sha256_tree(dir)
    Dir[dir.join('**/*').to_s].sort.select { |path| File.file?(path) && !File.symlink?(path) }.to_h do |path|
      [Pathname.new(path).relative_path_from(dir).to_s, Digest::SHA256.file(path).hexdigest]
    end
  end

  # Stages and packs every asset into `assets`; answers the asset paths,
  # the manifest and SHA256SUMS last.
  def pack(project, version:, builds:, sources:, readme:, assets:, commit:, epoch:)
    FileUtils.rm_rf(assets)
    staging = assets.join('staging')
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
        'triple' => target.triple,
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
      'zig' => capture!(Toolchain.zig, 'version').strip,
      'api' => Digest::SHA256.file(project.api).hexdigest,
      'targets' => manifest_targets,
      'assets' => packed.to_h { |file| [file.basename.to_s, Digest::SHA256.file(file).hexdigest] }
    )}\n")
    packed << manifest
    sums = Release.write_sha256sums(packed.map(&:to_s), assets.join('SHA256SUMS'))
    FileUtils.rm_rf(staging)
    packed + [Pathname.new(sums)]
  end

  # ------------------------------------------------------------------
  # The command

  # `mise run release -- <version> [--publish] [--skip-containers]` for one
  # C project. The stages (ADR 0001, "The release command"):
  #
  # 1. check: a clean main in step with origin, the tag free, the
  #    project's `checks` (its amalgamation and test suite);
  # 2. version: the header's version macros set and `regenerate` run when
  #    they differ; with --publish, committed as release(<dir>): v<version>
  #    and tagged; a dry run restores the files instead;
  # 3. build every native target (and `extra_build`'s, such as wasm);
  # 4. verify: symbols, architectures, `tests` against the packed host
  #    library, `smoke` in a container for each Linux target, `extra_verify`;
  # 5. pack: deterministic tarballs, the manifest and SHA256SUMS in
  #    build/release/<version>/assets/;
  # 6. publish (--publish): push the commit and tag, then the GitHub release.
  #
  # A failure after the commit and tag, before anything is pushed, undoes
  # them.
  def release(project:, directory:, title:, summary:, argv:, checks:, regenerate:, tests:, smoke:,
              readme:, sources:, extra_build: nil, extra_verify: nil)
    publish = !argv.delete('--publish').nil?
    containers = argv.delete('--skip-containers').nil?
    release_fail!("usage: mise run release -- <version> [--publish] [--skip-containers]") unless argv.size == 1
    version = argv.first
    prefix = "#{directory}/"
    require_tools!

    # 1. Check.
    Release.check_tree!(scope: [directory, 'mise'])
    tag = Release.check_version!(prefix, version)
    release_fail!("#{version} is a prerelease; C releases are MAJOR.MINOR.PATCH") if Release.parse_version(version)[3]
    checks.each { |command| Release.run(*command, chdir: project.root) }

    out = project.root.join('build/release', version)
    notes = out.join('notes.md')
    FileUtils.mkdir_p(out)
    notes.write(Release.notes(prefix: prefix, path: prefix, title: title, summary: summary))
    subject = "release(#{directory}): v#{version}"
    changed = []
    committed = false
    tagged = false
    pushed = false
    begin
      # 2. Version.
      if header_version(project) != version
        set_header_version!(project, version)
        regenerate.each { |command| Release.run(*command, chdir: project.root) }
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

      # 3. Build.
      targets = out.join('targets')
      builds = NATIVE_TARGETS.map { |name| build_native(project, name, version, targets) }
      builds += Array(extra_build&.call(targets))

      # 4. Verify.
      natives = builds.first(NATIVE_TARGETS.size)
      natives.each { |build| verify_native!(project, build) }
      host_tests!(project, natives.find { |build| build[:target].name == 'x86_64-linux' }, tests)
      if containers
        natives.select { |build| build[:target].linux? }.each { |build| container_smoke!(build, smoke) }
      else
        puts 'release: --skip-containers: no Linux container smoke test ran'
      end
      extra_verify&.call(builds)

      # 5. Pack.
      assets = pack(project, version: version, builds: builds, sources: sources, readme: readme,
                             assets: out.join('assets'), commit: commit, epoch: epoch)

      unless publish
        puts <<~DRY
          release: dry run for #{project.name} #{version} passed; nothing was committed, tagged or uploaded.
            would tag:     #{tag}
            assets:        #{assets.map { |file| file.basename.to_s }.join(', ')}
            in:            #{out.join('assets').relative_path_from(Release::REPO)}
            notes:         #{notes.relative_path_from(Release::REPO)}
          Publish with: mise run //#{directory}:release -- #{version} --publish
        DRY
        return
      end

      # 6. Publish.
      Release.push_and_release!(tag: tag, title: title, notes_file: notes, assets: assets)
      pushed = true
      puts "release: published #{project.name} #{version}, tagged #{tag}"
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
  end
end
