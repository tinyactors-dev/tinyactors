# frozen_string_literal: true

require 'digest'
require 'fileutils'
require 'json'
require 'pathname'
require 'cache'
require 'toolchain'

# The C libraries' build artifacts (docs/adr/0001-packaging-and-releases.md).
# A C project (tinyxml, tinyactors-core) is described once, by name, header,
# amalgamation, API description and symbol prefix, and built from its
# amalgamation: the one translation unit whose non-public functions are
# static, so its object exports exactly the public API, whereas a src/*.c
# object gives internal helpers external linkage.
#
# `dist` is the host case: it publishes, for consumers inside the repository,
#
#   dist/include/<name>.h           the authored public header
#   dist/include/<name>.c           the amalgamation (source embedders)
#   dist/include/<name>.api.json    the API description, for a consumer that
#                                   generates bindings from it
#   dist/lib/<profile>/lib<name>.a  an archive of ONE object: the amalgamation
#                                   compiled once, under the profile's flags
#   dist/<name>.json                a manifest a consumer keys its cache on
#
# Reproducibility rules, which the release build keeps too:
# - The flags are fixed here, per profile; $CFLAGS is never read. A dist
#   artifact must be a function of this definition, not of whoever runs it:
#   `CFLAGS=-O2 mise run dist` would otherwise publish an archive a later
#   plain run silently replaces. The compiler is the pinned clang
#   (Toolchain::Target.host), so the archive is a function of the pinned
#   toolchain and this definition.
# - The manifest is a pure function of content: no timestamps, commit, paths
#   or counters, so an unchanged library never churns a consumer's build.
# - The archive is llvm-ar's deterministic one (Toolchain::Target#archive),
#   a function of the object's bytes on every host.
# - Every published file is written only when its bytes differ, so an
#   unchanged artifact keeps its mtime and a consumer using mise's mtime-based
#   `sources` sees no reason to rebuild.
# - Gated on content (mise/lib/cache.rb), one gate per profile, keyed on the
#   profile's flags: a hand-edited or deleted archive is rebuilt, and one
#   profile's run never perturbs another's cache record.
module CRelease
  # One C library. `prefix` is its symbol prefix (`ta`, `txml`, `ta_scxml`).
  # `includes` are the directories its header and amalgamation need beyond
  # their own: those of the libraries it builds on (the codec's core and
  # txml).
  # `api_macro` is the macro its header declares public functions with
  # (TXML_API, TA_API); the codec uses core's TA_API.
  Project = Data.define(:name, :prefix, :includes, :api_macro) do
    def initialize(name:, prefix:, includes: [], api_macro: nil)
      super(name: name, prefix: prefix, includes: includes.map { |dir| Pathname.new(dir).expand_path },
            api_macro: api_macro || "#{prefix.upcase}_API")
    end

    def root = Cache::ROOT
    # The prefix of the header's macros: TXML for txml, TA for core.
    def macro = prefix.upcase
    def header = root.join("include/#{name}.h")
    def amalgamation = root.join("include/#{name}.c")
    def api = root.join("include/#{name}.api.json")
  end

  # Host profiles for dist; -fPIC in every one.
  PROFILES = {
    'release' => %w[-std=c11 -Wall -Wextra -Werror -pedantic -O2 -fPIC],
    'debug' => %w[-std=c11 -Wall -Wextra -Werror -pedantic -O0 -g -fPIC]
  }.freeze
  DEFAULT_PROFILE = 'release'

  module_function

  # Builds lib<name>.a from the amalgamation into `out`, and answers `out`.
  # `target` nil is the host with the pinned clang and a dist profile's flags; a
  # release target's name builds with `zig cc` and the toolchain's fixed
  # flags (the release adds compiler-rt and checks symbols; ADR 0001
  # decision 16).
  def archive(project, out:, target: nil, profile: DEFAULT_PROFILE)
    out = Pathname.new(out)
    FileUtils.mkdir_p(out.dirname)
    temp = out.dirname.join("#{out.basename}.tmp#{Process.pid}")
    FileUtils.rm_f(temp)
    if target.nil?
      object = project.root.join("build/dist/#{profile}/#{project.name}.o")
      outcome, output = Cache.compile(host_compile_command(profile, project), project.amalgamation, object)
      fail!("include/#{project.name}.c does not compile:\n#{output}") if outcome == :failed
      Toolchain::Target.host.archive([object], into: temp)
    else
      release = Toolchain::Target.release(target)
      object = release.compile(project.amalgamation, into: project.root.join("build/release/#{target}/#{project.name}.o"),
                                                     includes: [project.header.dirname, *project.includes])
      release.archive([object], into: temp)
    end
    replace_if_changed(temp, out)
    out
  end

  # The host compile command of a dist profile: the pinned clang, never $CFLAGS.
  def host_compile_command(profile, project)
    Toolchain::Target.host.compile_command(*PROFILES.fetch(profile), '-Iinclude', *project.includes.map { |dir| "-I#{dir}" })
  end

  # `mise run dist [profile]` for `project`: the archive for the profile, the
  # header, amalgamation and API description, and the manifest listing every
  # profile published on disk.
  def dist(project, argv)
    abort "usage: mise run dist [#{PROFILES.keys.join('|')}]" if argv.size > 1
    profile = argv.first || DEFAULT_PROFILE
    fail!("unknown profile #{profile.inspect} (known: #{PROFILES.keys.join(', ')})") unless PROFILES.key?(profile)

    dist = project.root.join('dist')
    published = {
      project.header => dist.join("include/#{project.name}.h"),
      project.amalgamation => dist.join("include/#{project.name}.c"),
      project.api => dist.join("include/#{project.name}.api.json")
    }
    archive_path = dist.join(archive_name(project, profile))
    command = host_compile_command(profile, project)
    Cache.gate("dist-#{profile}",
               inputs: Cache.files("include/#{project.name}.h", "include/#{project.name}.c",
                                   "include/#{project.name}.api.json", 'mise/tasks/dist'),
               outputs: [*published.values, archive_path],
               extra: [profile, PROFILES.fetch(profile).join(' '), command.join(' '),
                       Cache.tool_id(command.first(1)), Toolchain::Tool[:llvm_ar].identity,
                       Digest::SHA256.file(__FILE__).hexdigest])

    archive(project, out: archive_path, profile: profile)
    published.each { |source, path| write_if_changed(path, File.binread(source)) }

    profiles = PROFILES.keys.select { |name| dist.join(archive_name(project, name)).file? }.to_h do |name|
      [name, { 'flags' => PROFILES.fetch(name), 'archive' => archive_name(project, name),
               'digest' => Cache.digest(dist.join(archive_name(project, name))) }]
    end
    manifest = {
      'name' => project.name,
      'abi' => Cache.digest(project.header, project.api),
      'header' => "include/#{project.name}.h",
      'api' => "include/#{project.name}.api.json",
      'profiles' => profiles
    }
    write_if_changed(dist.join("#{project.name}.json"), "#{JSON.pretty_generate(manifest)}\n")
    puts "dist: published dist/ profile=#{profile} (abi #{manifest['abi'][0, 12]}, " \
         "archive #{profiles[profile]['digest'][0, 12]})"
  end

  def archive_name(project, profile) = "lib/#{profile}/lib#{project.name}.a"

  # Writes `contents` to `path` only when they differ, keeping its mtime.
  def write_if_changed(path, contents)
    FileUtils.mkdir_p(path.dirname)
    return if path.file? && File.binread(path) == contents

    path.binwrite(contents)
  end

  # Moves `temp` over `out` only when the bytes differ, keeping out's mtime.
  def replace_if_changed(temp, out)
    if out.file? && File.binread(out) == File.binread(temp)
      FileUtils.rm_f(temp)
    else
      File.rename(temp, out)
    end
  end

  def fail!(message)
    abort "dist: #{message}"
  end
end

require_relative 'c_release/build'
