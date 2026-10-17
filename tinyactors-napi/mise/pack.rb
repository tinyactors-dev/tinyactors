# frozen_string_literal: true

# The npm packages (docs/adr/0001, decisions 10, 11, 14 and 15): one addon
# per native release target, built with the root's zig
# (Toolchain::Target.release) and checked by its symbols, each in a
# platform package, and the main package that lists them. The pack task
# (mise/tasks/pack) and the release task (mise/tasks/release) both build
# through this file.
#
# - @tinyactors/node: index.mjs, lib/ (the direct layer and the
#   convenience layer), index.d.ts, README.md, LICENSE and a
#   package.json made from this directory's, with optionalDependencies on
#   every platform package at exactly its own version. They live only in
#   the staged copy: the repository's package.json cannot name packages
#   that are not published.
# - @tinyactors/node-<os>-<cpu>: tinyactors.node, with `os` and `cpu` and
#   no `libc` field (a Linux addon links no C library, so one file serves
#   glibc and musl), a README.md and LICENSE.
#
# Each is packed with the pinned node's `npm pack`, which writes every
# entry with the same time and mode, so the same commit gives the same
# bytes.
require 'cache'
require 'fileutils'
require 'json'
require 'tmpdir'
require 'release'
require_relative 'napi'

module NodeAPI
  module Pack
    NAME = '@tinyactors/node'
    # The release line's tags (the changelog component `napi`).
    TAG_PREFIX = 'tinyactors-napi/'
    PACKAGE_JSON = ROOT.join('package.json')
    LICENSE = ROOT.join('../LICENSE').expand_path
    # Where the release addons and their objects go, and the tarballs.
    RELEASE = BUILD.join('release')
    PACKAGES = BUILD.join('packages')
    # Each native release target: npm's os and cpu (process.platform and
    # process.arch, which lib/direct.mjs names the package by).
    TARGETS = {
      'aarch64-macos' => %w[darwin arm64],
      'x86_64-macos' => %w[darwin x64],
      'x86_64-linux' => %w[linux x64],
      'aarch64-linux' => %w[linux arm64]
    }.freeze
    # The main package's files, beside its package.json.
    FILES = %w[index.mjs lib/direct.mjs lib/convenience.mjs index.d.ts README.md LICENSE].freeze
    # What the addon exports: the runtime that loads it finds it by these
    # names (NAPI_MODULE_INIT, Node.js's include/node/node_api.h).
    EXPORTS = %w[napi_register_module_v1 node_api_module_get_api_version_v1].freeze
    # The C library functions it calls, which the runtime's process has:
    # each has the same name in glibc and musl, so a Linux addon that links
    # no C library binds to either. The math functions (quickjs's) are in
    # glibc's libm, which node and bun link. On macOS libSystem has them,
    # malloc_size in place of malloc_usable_size; Darwin's clang calls bzero
    # and the fortified __memcpy_chk and __memmove_chk, and dyld binds
    # lazily. A new one fails the build until it is checked and listed (ADR
    # 0001, "Consequences").
    LIBC = %w[abort acos acosh asin asinh atan atan2 atanh bcmp calloc cbrt ceil clock_gettime cos cosh exp expm1
              fclose floor fmod fopen fprintf fputc fread free frexp fseek ftell fwrite gettimeofday hypot
              localtime_r log log10 log1p log2 malloc malloc_usable_size memchr memcmp memcpy memmove memset pow
              printf qsort realloc round scalbn sin sinh snprintf sscanf strchr strcmp strlen strncmp strrchr
              strstr strtod tan tanh trunc vsnprintf].freeze
    DARWIN = %w[bzero __bzero __memcpy_chk __memmove_chk malloc_size dyld_stub_binder].freeze
    # Node-API's functions, which the runtime provides.
    NODE_API_SYMBOL = /\A(napi_|node_api_)/

    module_function

    def fail!(message)
      abort "pack: #{message}"
    end

    def target_names = TARGETS.keys

    def manifest = JSON.parse(PACKAGE_JSON.read)

    def platform_name(target) = "#{NAME}-#{TARGETS.fetch(target).join('-')}"

    def names = [NAME, *target_names.map { |target| platform_name(target) }]

    # npm pack's file name for `name` at `version`.
    def tarball_name(name, version) = "#{name.delete_prefix('@').tr('/', '-')}-#{version}.tgz"

    # zig cc writes debug information unless told not to, and the Mach-O
    # linker then records each object's absolute path and time in the addon
    # (its OSO stabs), so two builds of one commit differed: the packages
    # carry no debug information (-g0), on every target alike.
    RELEASE_FLAGS = %w[-g0].freeze

    def compile(target, source, object, flags)
      command = target.compile_command(*RELEASE_FLAGS, *flags)
      outcome, output = Cache.compile(command, source, object)
      return object unless outcome == :failed

      warn output
      fail!("#{Pathname.new(source).relative_path_from(ROOT)} does not compile for #{target}")
    end

    # The addon for one release target, build/release/<target>/tinyactors.node:
    # core's amalgamation, txml and src/ compiled with zig for the target
    # and linked as a module the runtime loads (Target#plugin), then
    # checked. Answers its path.
    def build(name)
      target = Toolchain::Target.release(name)
      dir = RELEASE.join(name)
      core = CORE_SOURCES.map do |file, flags|
        compile(target, CORE_BUILD.join(file), dir.join('objects/core', file.sub(/\.c\z/, '.o')),
                [*flags, "-I#{CORE_BUILD}"])
      end
      txml = compile(target, TXML.join('txml.c'), dir.join('objects/txml/txml.o'), [*TXML_FLAGS, "-I#{TXML}"])
      generated = NodeAPI.embed_realm.dirname
      own = NodeAPI.sources.map do |source|
        object = dir.join('objects/addon', source.relative_path_from(ROOT.join('src')).sub_ext('.o'))
        compile(target, source, object, [*ADDON_FLAGS, "-I#{CORE_BUILD}", "-I#{TXML}", "-I#{generated}",
                                         "-I#{NodeAPI.node_headers}"])
      end
      addon = dir.join('tinyactors.node')
      target.plugin([*core, txml, *own].map(&:to_s), into: addon.to_s)
      verify!(target, addon)
      addon
    end

    # The addon is for its target, loads no shared library (the C library
    # and Node-API are the process's, which is what lets one Linux file
    # serve glibc and musl), exports only the module's entry points, and
    # leaves undefined only Node-API and the listed C library functions.
    def verify!(target, addon)
      binary = Toolchain::Binary.new(addon)
      binary.must_be_for!(target)
      binary.must_load_only!([])
      binary.must_export_exactly!(EXPORTS)
      libc = target.format.links_libc ? LIBC + DARWIN : LIBC
      binary.must_leave_undefined_only!(binary.undefined.to_a.grep(NODE_API_SYMBOL) + libc)
      node_api = binary.undefined.count { |symbol| symbol.match?(NODE_API_SYMBOL) }
      puts "pack: #{target}: exports #{EXPORTS.join(', ')}; #{node_api} Node-API and " \
           "#{binary.undefined.size - node_api} C library imports"
    rescue Toolchain::Binary::Mismatch => e
      fail!("#{addon.relative_path_from(ROOT)}: #{e.message}. A C library function joins LIBC (mise/pack.rb) " \
            'once it has the same name, arguments and types in glibc and musl, and exists in libSystem')
    end

    # The fields every package shares, from this directory's package.json.
    def shared(manifest, version) = { 'version' => version, **manifest.slice('license', 'homepage', 'repository', 'bugs', 'engines') }

    # Stages the main package in `dir`: its files and a package.json listing
    # every platform package at `version`.
    def stage_main(dir, version)
      FileUtils.mkdir_p(dir)
      FILES.each do |file|
        FileUtils.mkdir_p(dir.join(file).dirname)
        FileUtils.cp(file == 'LICENSE' ? LICENSE : ROOT.join(file), dir.join(file))
      end
      staged = manifest.merge(
        'version' => version,
        'files' => FILES,
        'optionalDependencies' => target_names.to_h { |target| [platform_name(target), version] }
      )
      dir.join('package.json').write("#{JSON.pretty_generate(staged)}\n")
    end

    # Stages the platform package of `target` in `dir`, with `addon`.
    def stage_platform(dir, target, addon, version)
      os, cpu = TARGETS.fetch(target)
      name = platform_name(target)
      FileUtils.mkdir_p(dir)
      FileUtils.cp(addon, dir.join('tinyactors.node'))
      FileUtils.cp(LICENSE, dir.join('LICENSE'))
      package = {
        'name' => name,
        'description' => "The #{os}-#{cpu} addon of #{NAME}",
        **shared(manifest, version),
        'os' => [os],
        'cpu' => [cpu],
        'main' => 'tinyactors.node',
        'files' => %w[tinyactors.node LICENSE README.md]
      }
      dir.join('package.json').write("#{JSON.pretty_generate(package)}\n")
      dir.join('README.md').write(<<~README)
        # #{name}

        The #{os}-#{cpu} addon of [#{NAME}](https://www.npmjs.com/package/#{NAME}).
        Install `#{NAME}`, not this package: it lists the addon of each
        platform as an optional dependency, and npm, pnpm and Bun install only
        the one that matches.
      README
    end

    # npm pack of the staged directory `dir` into `out`; answers the tarball.
    def npm_pack(dir, out)
      name = JSON.parse(dir.join('package.json').read).values_at('name', 'version')
      # it prints the tarball's name, which build_all prints with its size
      ok = system(NodeAPI.pinned('npm'), 'pack', '--silent', '--pack-destination', out.to_s, dir.to_s,
                  chdir: ROOT.to_s, out: File::NULL)
      fail!("npm pack #{dir} failed") unless ok
      tarball = out.join(tarball_name(*name))
      fail!("npm pack did not write #{tarball.basename}") unless tarball.file?
      tarball
    end

    # The packages for `version`: every target of `names` built and
    # checked, its platform package and the main package packed into
    # `out`, platform packages first (the order they are published in).
    # Answers {target name or NAME => tarball}.
    def build_all(names, version:, out:)
      Release.parse_version(version)
      fail!("#{LICENSE} is missing") unless LICENSE.file?
      addons = names.to_h { |name| [name, build(name)] }
      FileUtils.rm_rf(out)
      FileUtils.mkdir_p(out)
      Dir.mktmpdir('tinyactors-pack') do |staging|
        staging = Pathname.new(staging)
        tarballs = addons.to_h do |name, addon|
          dir = staging.join(name)
          stage_platform(dir, name, addon, version)
          [name, npm_pack(dir, out)]
        end
        stage_main(staging.join('main'), version)
        tarballs[NAME] = npm_pack(staging.join('main'), out)
        tarballs.each_value { |tarball| puts format('pack: %8.1f KiB  %s', tarball.size / 1024.0, tarball.relative_path_from(ROOT)) }
        tarballs
      end
    end

    # The smoke tests resolve every symbol when the addon is loaded, not
    # when a function is first called: a C library function the runtime's
    # process lacks fails the import, not a later run. macOS's dyld ignores it.
    BIND_NOW = { 'LD_BIND_NOW' => '1' }.freeze

    # The main package and this machine's platform package, installed by
    # path into a fresh project outside the repository, once with npm and
    # run by node, once with bun and run by bun: tools/smoke.mjs there. The
    # other platform packages are optional and skipped, as in a real
    # install on this machine.
    def smoke_here!(tarballs)
      here = Toolchain::Host.current.target.name
      platform = tarballs[here] or fail!("this machine's target (#{here}) was not packed: no smoke test")
      main = tarballs.fetch(NAME)
      npm = NodeAPI.pinned('npm')
      [
        [[npm, 'install', '--no-audit', '--no-fund', '--silent'], NodeAPI.pinned('node')],
        [[NodeAPI.pinned('bun'), 'add', '--silent'], NodeAPI.pinned('bun')]
      ].each do |install, runtime|
        Dir.mktmpdir('tinyactors-smoke') do |dir|
          File.write(File.join(dir, 'package.json'), "{\"name\": \"smoke\", \"private\": true, \"type\": \"module\"}\n")
          FileUtils.cp(ROOT.join('tools/smoke.mjs'), dir)
          NodeAPI.run(*install, main.to_s, platform.to_s, chdir: dir)
          NodeAPI.run(runtime, 'smoke.mjs', env: BIND_NOW, chdir: dir)
        end
      end
    end

    # The containers a Linux package is smoke-tested in: Node.js on glibc
    # and musl, and Bun on Debian, on each architecture (the other one
    # under emulation, a smoke test only: decision 14). Bun on musl x64 is
    # left out: its build needs AVX, which an emulated x64 lacks.
    CONTAINERS = {
      'node-bookworm' => ['node:22-bookworm-slim', :npm],
      'node-alpine' => ['node:22-alpine', :npm],
      'bun-debian' => ['oven/bun:1', :bun],
      'bun-alpine' => ['oven/bun:1-alpine', :bun]
    }.freeze

    def smoke_in_containers!(tarballs)
      main = tarballs.fetch(NAME)
      tarballs.each do |name, tarball|
        next if name == NAME

        platform = Toolchain::Target.release(name).container_platform or next
        CONTAINERS.each do |label, (image, engine)|
          next if label == 'bun-alpine' && platform == 'linux/amd64'

          install = engine == :npm ? 'npm install --no-audit --no-fund --silent' : 'bun add --silent'
          run = engine == :npm ? 'node smoke.mjs' : 'bun smoke.mjs'
          script = 'set -e; mkdir /work; cd /work; echo \'{"name":"smoke","private":true,"type":"module"}\' > package.json; ' \
                   "cp /t/smoke.mjs .; #{install} /p/#{main.basename} /p/#{tarball.basename}; #{run}"
          NodeAPI.run('docker', 'run', '--rm', '--platform', platform, '-e', 'LD_BIND_NOW=1',
                      '-v', "#{tarball.dirname}:/p:ro", '-v', "#{ROOT.join('tools')}:/t:ro", image, 'sh', '-c', script)
          puts "pack: #{name} in #{label} (#{image}, #{platform}): ok"
        end
      end
    end
  end
end
