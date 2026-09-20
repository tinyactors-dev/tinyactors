# frozen_string_literal: true

require 'English'
require 'digest'
require 'fileutils'
require 'pathname'
require 'shellwords'

# Content-based skipping for the txml tasks.
#
# mise decides whether a task is fresh from `sources` by modification time: a
# `touch`, or an editor that rewrites a file with the same bytes, reruns the
# task. mise still schedules (`depends`, `--jobs`) and there is nothing here that
# schedules; what mise cannot do, and this module does, is ask whether the
# *contents* an outcome depends on have changed. Three tools:
#
# Stamps. A task names the files its result depends on. `Cache.gate` hashes
# them (and any outputs it is responsible for); when the hash equals the one
# written by the last *successful* run, the task prints "cached" and exits. The
# old stamp is deleted before any work starts and the new one is written only
# when the process exits with status 0, so a failing run can never leave a
# success record behind.
#
# Compile cache. `Cache.compile` compiles one translation unit to its own object
# (with -MMD, so the object's header list is known) and reuses the object while
# the compile command, the compiler, the source and every header it included
# have the same contents.
#
# Preprocessed addressing. When a source's bytes changed but only in comments or
# layout, the compiler would produce the same object. `Cache.compile` therefore
# falls back, before compiling, to the digest of the preprocessed translation
# unit with each line stripped and blank lines dropped (`cc -E -P`, which has no
# comments and no line markers); if that matches the one the existing object was
# built from the object is kept. Only whitespace at the ends of lines and blank
# lines are ignored, never whitespace inside a line, so a change to a string
# literal or a `__LINE__` expansion still recompiles.
#
# Content addressing. Everything downstream (linking, the undefined-symbol
# check, running a test binary) keys on the *bytes* of what it consumes, not on
# when it was written. An object that a comment edit recompiled to identical
# bytes therefore invalidates nothing after it.
#
# TXML_FORCE=1 ignores every stamp and every cached object, for a from-scratch
# run; `rm -rf build` does the same. (`mise run --force` only defeats mise's own
# `sources` check, and mise does not tell a task it was given.)
module Cache
  ROOT = Pathname.new(__dir__).join('../..').expand_path
  STAMPS = ROOT.join('build/stamps')

  module_function

  def force? = ENV['TXML_FORCE'] == '1'

  # SHA-256 over a mix of files (path and contents) and strings.
  def digest(*items)
    sha = Digest::SHA256.new
    items.flatten.each do |item|
      if item.is_a?(Pathname)
        sha << "file\0#{item.relative_path_from(ROOT)}\0"
        item.file? ? sha << Digest::SHA256.file(item.to_s).digest : sha << 'missing'
      else
        sha << "text\0#{item}\0"
      end
    end
    sha.hexdigest
  end

  # The files under `patterns` (globs relative to the project root), sorted.
  def files(*patterns)
    patterns.flatten.flat_map { |pattern| Dir[ROOT.join(pattern).to_s] }.select { |path| File.file?(path) }.sort.map { |path| Pathname.new(path) }
  end

  def stamp_path(name) = STAMPS.join(name.tr('/', '_'))

  def stamped?(name, key)
    path = stamp_path(name)
    !force? && path.file? && path.read == key
  end

  def unstamp(name) = FileUtils.rm_f(stamp_path(name))

  def stamp(name, key)
    FileUtils.mkdir_p(STAMPS)
    temporary = stamp_path("#{name}.tmp#{Process.pid}")
    temporary.write(key)
    File.rename(temporary, stamp_path(name))
  end

  # Runs the block unless `key` is the one recorded by the last successful run.
  # Returns :cached or :ran. The record is dropped first and written last, so a
  # block that raises or exits leaves none.
  def cached(name, key)
    return :cached if stamped?(name, key)

    unstamp(name)
    yield
    stamp(name, key)
    :ran
  end

  # For a task script whose whole body is the work: call once after the
  # requires. When the inputs (and outputs, for a generator) hash to the recorded
  # key the process exits at once; otherwise the body runs and a stamp is written
  # if, and only if, it exits with status 0. `outputs` are hashed again at exit,
  # so a generator's own result is part of what "fresh" means and an output that
  # was edited by hand makes the next run do the work.
  def gate(name, inputs:, outputs: [], extra: [])
    input_key = digest(inputs, extra)
    fresh = lambda do
      digest(input_key, outputs)
    end
    if stamped?(name, fresh.call)
      puts "#{name}: cached (inputs unchanged)"
      exit 0
    end
    unstamp(name)
    at_exit do
      stamp(name, fresh.call) if $!.nil? || ($!.is_a?(SystemExit) && $!.success?)
    end
  end

  # The compiler's identity, so a new compiler invalidates every object.
  def tool_id(command)
    @tool_ids ||= {}
    @tool_ids[command] ||= begin
      IO.popen([*command, '--version'], err: %i[child out], &:read).to_s.lines.first.to_s.strip
    rescue SystemCallError
      "#{command.first} (unavailable)"
    end
  end

  # Header paths from a -MMD file (make syntax, `\` continuations).
  def dependencies(path)
    text = path.read.gsub(/\\\n/, ' ')
    _target, rest = text.split(/(?<!\\): /, 2)
    (rest || '').split(/(?<!\\)\s+/).reject(&:empty?).map { |entry| entry.gsub('\ ', ' ') }
  end

  # Digest of the preprocessed translation unit, ignoring comments (the
  # preprocessor drops them), line markers (-P) and blank lines and indentation.
  # nil when preprocessing fails; the compile then reports the real error.
  def preprocessed_digest(command, source)
    source = ROOT.join(source.to_s)
    text = IO.popen([*command, '-E', '-P', source.relative_path_from(ROOT).to_s], chdir: ROOT.to_s, err: File::NULL, &:read)
    return nil unless $CHILD_STATUS.success?

    Digest::SHA256.hexdigest(text.lines.map(&:strip).reject(&:empty?).join("\n"))
  end

  # Compile `source` to `object` with `command` (compiler and flags). Returns
  # [:cached | :compiled, output]; raises nothing: a failing compile returns
  # [:failed, compiler output] and leaves no key behind.
  def compile(command, source, object)
    object = Pathname.new(object)
    dep = object.sub_ext('.d')
    key_file = object.sub_ext('.key')
    source = ROOT.join(source.to_s)

    key = lambda do |headers|
      digest(command.join(' '), tool_id(command.first(1)), source, headers.map { |header| ROOT.join(header) })
    end

    unit_file = object.sub_ext('.unit')
    if !force? && object.file? && dep.file? && key_file.file?
      headers = dependencies(dep).reject { |header| ROOT.join(header) == source }
      return [:cached, ''] if key_file.read == key.call(headers)

      # Something it reads changed. If only comments or layout did, the object
      # is still right: record the new contents and keep it.
      if unit_file.file? && (unit = preprocessed_digest(command, source)) && unit_file.read == unit
        key_file.write(key.call(headers))
        return [:cached, '']
      end
    end

    FileUtils.rm_f([key_file, unit_file])
    FileUtils.mkdir_p(object.dirname)
    output = IO.popen([*command, '-MMD', '-MF', dep.to_s, '-c', source.relative_path_from(ROOT).to_s, '-o', object.to_s], chdir: ROOT.to_s, err: %i[child out], &:read)
    unless $CHILD_STATUS.success?
      FileUtils.rm_f([object, dep])
      return [:failed, output]
    end
    headers = dependencies(dep).reject { |header| ROOT.join(header) == source }
    key_file.write(key.call(headers))
    unit = preprocessed_digest(command, source)
    unit_file.write(unit) if unit
    [:compiled, output]
  end
end
