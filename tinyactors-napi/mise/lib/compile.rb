# frozen_string_literal: true

require 'English'
require 'fileutils'
require 'json'
require 'pathname'
require 'rbconfig'
require 'shellwords'
require 'cache'

# The one compile command for the addon's C sources, the core amalgamation and
# the native tests. Every translation unit is compiled once to its own object
# under build/obj (Cache.compile reuses it while its inputs are unchanged), so
# `build` and `test-native` share objects instead of compiling core twice.
module NapiCompile
  ROOT = Cache::ROOT
  BUILD = ROOT.join('build')
  # TNA_SANITIZE=1 builds with AddressSanitizer and UBSan, into separate
  # object and binary directories so the two modes never evict each other.
  SANITIZE = ENV['TNA_SANITIZE'] == '1'
  SANITIZE_FLAGS = %w[-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g].freeze
  # TNA_VALUES_MODEL=table builds value boxes as a handle table (ADR 0011
  # model B) instead of one napi_ref each, for the A/B measurement, into its
  # own object directory. The addon it links replaces the default one.
  VALUES_TABLE = ENV['TNA_VALUES_MODEL'] == 'table'
  OBJECTS = BUILD.join(SANITIZE ? 'obj-asan' : (VALUES_TABLE ? 'obj-table' : 'obj'))
  NATIVE = BUILD.join(SANITIZE ? 'native-asan' : 'native')
  CORE_DIR = ROOT.join('../tinyactors-core/include').expand_path
  CORE_SOURCE = CORE_DIR.join('tinyactors.c')

  module_function

  def capture(*command)
    output = IO.popen(command, chdir: ROOT.to_s, err: %i[child out], &:read)
    abort output unless $CHILD_STATUS.success?
    output.strip
  end

  def node_include_dir
    @node_include_dir ||= begin
      node = Pathname.new(capture('mise', 'which', 'node'))
      dir = node.dirname.parent.join('include/node')
      abort "Node-API headers not found under #{dir}" unless dir.join('node_api.h').file?
      dir
    end
  end

  def version = JSON.parse(ROOT.join('package.json').read).fetch('version')

  def compiler = Shellwords.split(ENV.fetch('CC', 'cc'))

  def command
    @command ||= [
      *compiler,
      *Shellwords.split(ENV.fetch('CFLAGS', '-std=c11 -Wall -Wextra -Werror -pedantic -O2')),
      *(SANITIZE ? SANITIZE_FLAGS : []),
      *(VALUES_TABLE ? ['-DTNA_VALUES_TABLE'] : []),
      '-fPIC',
      '-DNAPI_VERSION=10',
      "-DTINYACTORS_NAPI_VERSION=\"#{version}\"",
      "-I#{node_include_dir}",
      "-I#{CORE_DIR}",
      "-I#{ROOT.join('src')}"
    ]
  end

  # The addon's own sources, adapters included.
  def sources = Cache.files('src/**/*.c')

  def object_for(source)
    source = Pathname.new(source)
    return OBJECTS.join('core/tinyactors.o') if source == CORE_SOURCE

    OBJECTS.join(source.relative_path_from(ROOT).sub_ext('.o'))
  end

  # Compiles each source (reusing cached objects) and returns the objects.
  def objects(sources)
    sources.map do |source|
      object = object_for(source)
      state, output = Cache.compile(command, source, object)
      if state == :failed
        warn output
        abort "tinyactors-napi: compiling #{source.relative_path_from(ROOT)} failed"
      end
      puts "compiled #{source.relative_path_from(ROOT)}" if state == :compiled
      object
    end
  end

  def core_object = objects([CORE_SOURCE]).first

  # Links `objects` into `output`, unless the same objects were linked the same
  # way last time and the output is still there.
  def link(objects, output, flags: [])
    link_command = [*compiler, *(SANITIZE ? SANITIZE_FLAGS : []), *flags, *objects.map(&:to_s), '-o', output.to_s]
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

  # Whether the object's translation unit included node_api.h, directly or
  # through another header. Such an object cannot link outside a Node process.
  def uses_node_api?(object)
    dep = object.sub_ext('.d')
    Cache.dependencies(dep).any? { |header| File.basename(header) == 'node_api.h' }
  end

  # The only sources allowed to include node_api.h, directly or through a
  # header. Every other source is portable plumbing for a future host kernel
  # (docs/proposals/host-kernel.md) and must link, and be tested, without Node.
  NODE_API_SOURCES = %w[
    src/addon.c
    src/gen/ta_napi.c
    src/napi_datamodel.c
    src/napi_services.c
    src/napi_system.c
    src/pump.c
    src/runtime.c
    src/runtime_codecs.c
    src/values.c
  ].freeze

  # Splits the addon's objects into [portable, node_api] and aborts if the
  # split differs from NODE_API_SOURCES in either direction.
  def partition_node_api(sources)
    node_api, portable = sources.zip(objects(sources)).partition { |_source, object| uses_node_api?(object) }
    found = node_api.map { |source, _| source.relative_path_from(ROOT).to_s }.sort
    unexpected = found - NODE_API_SOURCES
    stale = NODE_API_SOURCES - found
    unless unexpected.empty?
      abort "tinyactors-napi: these sources include node_api.h but are not in " \
            "NapiCompile::NODE_API_SOURCES (mise/lib/compile.rb):\n  #{unexpected.join("\n  ")}\n" \
            'Keep Node-API out of the portable plumbing, or add the file to the list on purpose.'
    end
    unless stale.empty?
      abort "tinyactors-napi: these sources in NODE_API_SOURCES (mise/lib/compile.rb) no longer " \
            "include node_api.h; remove them from the list:\n  #{stale.join("\n  ")}"
    end
    [portable.map(&:last), node_api.map(&:last)]
  end

  def addon_link_flags
    case RbConfig::CONFIG.fetch('host_os')
    when /darwin/ then %w[-bundle -undefined dynamic_lookup]
    when /linux|bsd/ then %w[-shared]
    else abort 'tinyactors-napi: the build supports macOS, Linux, and BSD'
    end
  end
end
