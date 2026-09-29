# frozen_string_literal: true

require 'fileutils'
require 'json'
require 'pathname'
require 'shellwords'
require 'cache'
require 'toolchain'

# The one compile command for the addon's C sources, the core amalgamation and
# the native tests. Every translation unit is compiled once to its own object
# under build/obj (Cache.compile reuses it while its inputs are unchanged).
module NapiCompile
  ROOT = Cache::ROOT
  BUILD = ROOT.join('build')
  # TNA_VALUES_MODEL=table builds value boxes as a handle table (ADR 0011
  # model B) instead of one napi_ref each, for the A/B measurement, into its
  # own object directory. The addon it links replaces the default one.
  VALUES_TABLE = ENV['TNA_VALUES_MODEL'] == 'table'
  OBJECTS = BUILD.join(VALUES_TABLE ? 'obj-table' : 'obj')
  CORE_DIR = ROOT.join('../tinyactors-core/include').expand_path
  CORE_SOURCE = CORE_DIR.join('tinyactors.c')
  # The SCXML codec and the XML reader it runs on, linked into the addon so
  # define() can import SCXML documents (napi ADR 0019). Both amalgamations
  # are committed, like core's.
  SCXML_DIR = ROOT.join('../tinyactors-scxml').expand_path
  XML_DIR = ROOT.join('../tinyxml/include').expand_path
  CODEC_SOURCES = [SCXML_DIR.join('include/ta_scxml.c'), XML_DIR.join('txml.c')].freeze
  # The host kernel (kernel ADR 0001): libc-free C that every host needs,
  # compiled into the addon with the addon's own command. Its private
  # headers are on the include path while napi still reaches into them
  # (kernel ADR 0001, step 2 notes).
  KERNEL_DIR = ROOT.join('../tinyactors-host').expand_path

  module_function

  # The pinned Node-API headers (vendor/node-api-headers), not the ones of
  # the Node mise installed: every target compiles against the same
  # declarations, at NAPI_VERSION 10.
  NODE_API_INCLUDE = ROOT.join('vendor/node-api-headers/include')

  def version = JSON.parse(ROOT.join('package.json').read).fetch('version')

  # The warnings every build of the addon holds itself to.
  WARNINGS = %w[-std=c11 -Wall -Wextra -Werror -pedantic].freeze

  def defines_and_includes
    [
      '-DNAPI_VERSION=10',
      "-DTINYACTORS_NAPI_VERSION=\"#{version}\"",
      "-I#{NODE_API_INCLUDE}",
      "-I#{CORE_DIR}",
      "-I#{SCXML_DIR.join('include')}",
      "-I#{XML_DIR}",
      "-I#{ROOT.join('src')}",
      "-I#{KERNEL_DIR.join('include')}",
      "-I#{KERNEL_DIR.join('src')}"
    ]
  end

  # The compile command: for the host with the pinned clang and $CFLAGS, as development
  # and the tests build; for a release target (mise/lib/toolchain/) with
  # the pinned `zig cc` and that target's fixed flags. A Linux target
  # compiles against musl's headers (ADR 0001 decision 10).
  def command(target = nil)
    @commands ||= {}
    @commands[target&.name] ||=
      if target.nil?
        Toolchain::Target.host.compile_command(
          *Shellwords.split(ENV.fetch('CFLAGS', "#{WARNINGS.join(' ')} -O2")),
          *(VALUES_TABLE ? ['-DTNA_VALUES_TABLE'] : []),
          '-fPIC',
          *defines_and_includes
        )
      else
        target.compile_command(*WARNINGS, *defines_and_includes)
      end
  end

  # The addon's own sources, adapters included, and the host kernel's.
  def sources = Cache.files('src/**/*.c') + kernel_sources

  def kernel_sources = Dir[KERNEL_DIR.join('src/*.c').to_s].sort.map { |path| Pathname.new(path) }

  # Where a release target's objects and addon go: build/<target>/.
  def target_dir(target) = BUILD.join(target.name)

  def object_for(source, target = nil)
    source = Pathname.new(source)
    base = target.nil? ? OBJECTS : target_dir(target).join('obj')
    return base.join('core/tinyactors.o') if source == CORE_SOURCE
    return base.join('codec', source.basename.sub_ext('.o')) if CODEC_SOURCES.include?(source)
    return base.join('kernel', source.basename.sub_ext('.o')) if source.to_s.start_with?("#{KERNEL_DIR}/")

    base.join(source.relative_path_from(ROOT).sub_ext('.o'))
  end

  # Compiles each source for `target` (nil: the host), reusing cached
  # objects, and returns the objects.
  def objects(sources, target = nil)
    sources.map do |source|
      object = object_for(source, target)
      state, output = Cache.compile(command(target), source, object)
      if state == :failed
        warn output
        abort "tinyactors-napi: compiling #{source.relative_path_from(ROOT)} failed"
      end
      puts "compiled #{source.relative_path_from(ROOT)}" if state == :compiled
      object
    end
  end

  def core_object(target = nil) = objects([CORE_SOURCE], target).first

  def codec_objects(target = nil) = objects(CODEC_SOURCES, target)

  # Every object of the addon for `target`: its own sources, core and the
  # codec.
  def addon_objects(target = nil) = objects(sources, target) + [core_object(target)] + codec_objects(target)

  # Links the addon for a release target into build/<target>/tinyactors.node,
  # a plugin whose Node-API functions the loading process provides
  # (Toolchain::Target#plugin). macOS links libSystem; Linux links no C
  # library (`-nostdlib`, no DT_NEEDED), so its libc functions bind to the
  # libc Node itself runs on, glibc or musl (ADR 0001 decision 10).
  def link_target(target)
    out = target.plugin(addon_objects(target), into: target_dir(target).join('tinyactors.node')).path
    puts "linked #{out.relative_path_from(ROOT)}"
    out
  end

  # Links `objects` into `output`, unless the same objects were linked the same
  # way last time and the output is still there.
  def link(objects, output, flags: [])
    compiler = Toolchain::Target.host.compile_command
    link_command = [*compiler, *flags, *objects.map(&:to_s), '-o', output.to_s]
    name = "link-#{output.relative_path_from(BUILD).to_s.tr('/', '_')}"
    key = Cache.digest(objects, link_command.join(' '), Cache.tool_id(compiler.first(1)))
    return if output.file? && Cache.stamped?(name, key)

    Cache.unstamp(name)
    FileUtils.rm_f(output)
    FileUtils.mkdir_p(output.dirname)
    puts link_command.shelljoin
    pid = Process.spawn(*link_command, chdir: ROOT.to_s)
    _pid, status = Process.wait2(pid)
    exit(status.exitstatus || 1) unless status.success?
    Cache.stamp(name, key)
  end
end
