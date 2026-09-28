# frozen_string_literal: true

require 'fileutils'
require 'pathname'
require 'cache'
require 'platform_files'
require 'toolchain'

# The freestanding check (AGENTS.md hard constraint 1). The tasks under
# mise/tasks/freestanding/ call this; mise schedules them (depends, --jobs) and
# decides when they are fresh (sources), so nothing here does either.
#
# 1. Includes. Every `#include <...>` in src/ and in the amalgamation must be
#    stdint.h, stddef.h or stdbool.h; a quoted include must name a project
#    header.
# 2. Symbols. The compiler can call memset, memcpy or memmove behind the code's
#    back (zero-initialising or copying a large struct, or turning a byte loop
#    into a call), and only the object files show it. For one optimisation
#    level and target (src/*.c, or the amalgamation include/txml.c), the
#    sources are compiled for the check target (Toolchain::Target.check: the
#    pinned clang, x86_64 Linux ELF, so every host checks the same code) with
#    `-ffreestanding -fno-builtin -fno-stack-protector`, combined into one
#    relocatable object, and it must leave no symbol undefined. Each
#    (target, level) is its own mise task so mise can run them in parallel.
#
# Caching (mise/lib/cache.rb). Each source is compiled to its own object under
# build/freestanding/<target>/<level>/ and reused while its contents and those
# of the headers it included are unchanged; the combine-and-read step reruns only
# when the bytes of some object changed, so a touch or a comment edit costs a few
# hash computations. Every (target, level) writes a one-line record under
# build/freestanding/results/, which `freestanding:full` prints as its report.
# `TXML_FORCE=1` (or `rm -rf build`) redoes everything.
#
# `-fno-stack-protector` removes __stack_chk_fail, which is the distribution's
# default flags rather than the code.
module Freestanding
  ALLOWED_HEADERS = %w[stdint.h stddef.h stdbool.h].freeze
  BASE = %w[-std=c11 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror -pedantic].freeze

  # A set of translation units built and checked together.
  Target = Struct.new(:name, :sources, :headers, :include_dirs)

  def self.targets(root)
    sources = PlatformFiles.sources(root.join('src')).map { |path| Pathname.new(path) }
    headers = sources + PlatformFiles.headers(root.join('src')).map { |path| Pathname.new(path) }
    amalgamation = root.join('include/txml.c')
    [Target.new('src', sources, headers, [root.join('include'), root.join('src')]),
     Target.new('amalgamation', [amalgamation], [amalgamation], [root.join('include')])]
  end

  def self.now = Process.clock_gettime(Process::CLOCK_MONOTONIC)

  # One line per (target, level) for `freestanding:full` to report.
  def self.record(root, name, status, seconds, detail)
    directory = root.join('build/freestanding/results')
    FileUtils.mkdir_p(directory)
    directory.join(name).write("#{status}\t#{format('%.1f', seconds)}\t#{detail}\n")
  end

  # The include check, for src/ and the amalgamation.
  def self.check_includes(root)
    started = now
    all = targets(root).flat_map(&:headers)
    key = Cache.digest(all, root.join('mise/lib/freestanding.rb'))
    status = Cache.cached('freestanding-includes', key) do
      failures = []
      targets(root).each do |target|
        target.headers.each do |path|
          path.each_line.with_index(1) do |line, number|
            next unless (match = line.match(/^\s*#\s*include\s*([<"])([^>"]+)[>"]/))

            kind = match[1]
            name = match[2]
            where = "#{path.relative_path_from(root)}:#{number}"
            if kind == '<'
              failures << "#{where}: #include <#{name}> is not allowed (only #{ALLOWED_HEADERS.join(', ')})" unless ALLOWED_HEADERS.include?(name)
            elsif target.include_dirs.none? { |dir| dir.join(name).file? || path.dirname.join(name).file? }
              failures << "#{where}: #include \"#{name}\" is not a project header"
            end
          end
        end
      end
      finish(failures, 'freestanding:includes', '')
    end
    detail = "#{all.size} files include only project headers and #{ALLOWED_HEADERS.join(', ')}"
    record(root, 'includes', status, now - started, detail)
    puts "freestanding:includes: #{detail} [#{status == :cached ? 'cached: contents unchanged since the last pass' : 'checked'}]"
  end

  # One optimisation level, e.g. '-O2', for one target ('src' or 'amalgamation').
  def self.check_level(root, level, target_name)
    started = now
    short = level.delete_prefix('-')
    label = "freestanding:#{target_name}:#{short}"
    # clang lowers every aggregate copy and compound-literal initialiser through
    # memcpy/memset when it is not optimising, and there is no flag that turns
    # that off. Unoptimised clang therefore needs the toolchain's mem*
    # functions, the contract clang documents for -ffreestanding, and only
    # optimised builds are held to the rule.
    if level == '-O0'
      puts "#{label}: skipped with clang (it lowers struct copies through memcpy when not optimising)"
      record(root, "#{target_name}.#{short}", 'skipped', now - started, 'clang lowers struct copies through memcpy at -O0')
      return
    end

    target = targets(root).find { |candidate| candidate.name == target_name }
    directory = root.join('build/freestanding', target.name, short)
    command = Toolchain::Target.check.compile_command(*BASE, level,
                                                     *target.include_dirs.flat_map { |dir| ['-I', dir.relative_path_from(root).to_s] })

    compiled = 0
    reused = 0
    failures = []
    objects = target.sources.map do |source|
      object = directory.join("#{source.basename('.c')}.o")
      outcome, output = Cache.compile(command, source, object)
      case outcome
      when :failed then failures << "#{source.relative_path_from(root)} #{level}: does not compile freestanding:\n#{output}"
      when :compiled then compiled += 1
      else reused += 1
      end
      object
    end
    finish(failures, label, '') unless failures.empty?

    # The undefined-symbol check depends on the bytes of the objects, so an
    # object that a comment edit recompiled to the same bytes costs nothing here.
    key = Cache.digest(level, target_name, Toolchain::Tool[:zig].identity, Toolchain::Tool[:llvm_nm].identity, objects,
                       root.join('mise/lib/freestanding.rb'))
    symbols = Cache.cached("freestanding-#{target_name}-#{short}", key) do
      finish(undefined_symbols(target, level, objects), label, '')
    end
    detail = "#{objects.size} objects: #{compiled} compiled, #{reused} reused; symbol check #{symbols == :cached ? 'reused' : 'ran'}"
    seconds = now - started
    record(root, "#{target_name}.#{short}", compiled.zero? && symbols == :cached ? 'cached' : 'ran', seconds, detail)
    puts "#{label}: #{level}, no undefined symbols (#{detail}) #{format('%.1f', seconds)}s"
  end

  # Combines one target's objects and requires zero undefined symbols.
  def self.undefined_symbols(target, level, objects)
    undefined = Toolchain::Target.check.combine(objects, into: objects.first.dirname.join('library.o')).undefined
    return [] if undefined.empty?

    culprits = undefined.map do |symbol|
      users = objects.select { |object| Toolchain::Binary.new(object).undefined.include?(symbol) }
      "#{symbol} (from #{users.map { |object| object.basename('.o') }.join(', ')})"
    end
    ["#{target.name} #{level}: the library references symbols it does not define: #{culprits.join(', ')}"]
  rescue Toolchain::Error => e
    ["#{target.name} #{level}: #{e.message}"]
  end

  def self.finish(failures, name, summary)
    unless failures.empty?
      warn failures.join("\n")
      abort "#{name}: #{failures.size} problem(s)"
    end
    puts "#{name}: #{summary}" unless summary.empty?
  end
end
