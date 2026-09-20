# frozen_string_literal: true

require 'English'
require 'fileutils'
require 'pathname'
require 'shellwords'
require 'tmpdir'

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
#    sources are compiled with `-ffreestanding -fno-builtin -fno-stack-protector`,
#    combined with `ld -r`, and `nm -u` on the result must list nothing. Each
#    (target, level) is its own mise task so mise can run them in parallel.
#
# `-fno-stack-protector` removes __stack_chk_fail, which is the distribution's
# default flags rather than the code. GCC can still recognise a byte loop as
# memset at -O2/-O3 (-ftree-loop-distribute-patterns); the check therefore also
# passes `-fno-tree-loop-distribute-patterns` when the compiler accepts it.
module Freestanding
  ALLOWED_HEADERS = %w[stdint.h stddef.h stdbool.h].freeze
  BASE = %w[-std=c11 -ffreestanding -fno-builtin -fno-stack-protector -Wall -Wextra -Werror -pedantic].freeze

  # A set of translation units built and checked together.
  Target = Struct.new(:name, :sources, :headers, :include_dirs)

  def self.compiler = Shellwords.split(ENV.fetch('CC', 'cc'))

  def self.run(*command)
    output = IO.popen(command, err: %i[child out], &:read)
    [$CHILD_STATUS.success?, output]
  end

  def self.accepted?(flag)
    Dir.mktmpdir('txml-flag') do |directory|
      source = File.join(directory, 'probe.c')
      File.write(source, "int probe(void) { return 0; }\n")
      ok, output = run(*compiler, flag, '-Werror', '-c', source, '-o', File.join(directory, 'probe.o'))
      ok && !output.include?('unknown') && !output.include?('unrecognized')
    end
  end

  def self.targets(root)
    sources = Dir[root.join('src/*.c').to_s].sort.map { |path| Pathname.new(path) }
    headers = sources + Dir[root.join('src/*.h').to_s].sort.map { |path| Pathname.new(path) }
    amalgamation = root.join('include/txml.c')
    [Target.new('src', sources, headers, [root.join('include'), root.join('src')]),
     Target.new('amalgamation', [amalgamation], [amalgamation], [root.join('include')])]
  end

  # The include check, for src/ and the amalgamation.
  def self.check_includes(root)
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
    finish(failures, 'freestanding:includes', "#{targets(root).sum { |t| t.headers.size }} files include only project headers and #{ALLOWED_HEADERS.join(', ')}")
  end

  # One optimisation level, e.g. '-O2', for one target ('src' or 'amalgamation').
  def self.check_level(root, level, target_name)
    # clang lowers every aggregate copy and compound-literal initialiser through
    # memcpy/memset when it is not optimising, and there is no flag that turns
    # that off; GCC does not for this code. Unoptimised clang therefore needs
    # the toolchain's mem* functions, the same contract both compilers document
    # for -ffreestanding, and only optimised clang builds are held to the rule.
    version = IO.popen([*compiler, '--version'], err: %i[child out], &:read).to_s
    if level == '-O0' && version.include?('clang')
      puts "freestanding:#{target_name}:O0: skipped with clang (it lowers struct copies through memcpy when not optimising)"
      return
    end

    flags = BASE.dup
    flags << '-fno-tree-loop-distribute-patterns' if accepted?('-fno-tree-loop-distribute-patterns')
    failures = []
    targets(root).select { |target| target.name == target_name }.each do |target|
      directory = root.join('build/freestanding', target.name, level.delete_prefix('-'))
      FileUtils.rm_rf(directory)
      FileUtils.mkdir_p(directory)
      objects = []
      target.sources.each do |source|
        object = directory.join("#{source.basename('.c')}.o")
        ok, output = run(*compiler, *flags, level, *target.include_dirs.flat_map { |dir| ['-I', dir.to_s] },
                         '-c', source.to_s, '-o', object.to_s)
        if ok
          objects << object
        else
          failures << "#{source.relative_path_from(root)} #{level}: does not compile freestanding:\n#{output}"
        end
      end
      failures.concat(undefined_symbols(target, level, objects)) if objects.size == target.sources.size
    end
    finish(failures, "freestanding:#{target_name}:#{level.delete_prefix('-')}", "#{level}, no undefined symbols")
  end

  # ld -r one target's objects together and require zero undefined symbols.
  def self.undefined_symbols(target, level, objects)
    combined = objects.first.dirname.join('library.o')
    ok, output = run('ld', '-r', *objects.map(&:to_s), '-o', combined.to_s)
    return ["#{target.name} #{level}: ld -r failed:\n#{output}"] unless ok

    ok, output = run('nm', '--undefined-only', '--format=posix', combined.to_s)
    return ["#{target.name} #{level}: nm failed:\n#{output}"] unless ok

    undefined = output.lines.map { |line| line.split.first }.compact.uniq
    return [] if undefined.empty?

    culprits = undefined.map do |symbol|
      users = objects.select do |object|
        _ok, listing = run('nm', '--undefined-only', '--format=posix', object.to_s)
        listing.lines.any? { |line| line.split.first == symbol }
      end
      "#{symbol} (from #{users.map { |object| object.basename('.o') }.join(', ')})"
    end
    ["#{target.name} #{level}: the library references symbols it does not define: #{culprits.join(', ')}"]
  end

  def self.finish(failures, name, summary)
    unless failures.empty?
      warn failures.join("\n")
      abort "#{name}: #{failures.size} problem(s)"
    end
    puts "#{name}: #{summary}"
  end
end
