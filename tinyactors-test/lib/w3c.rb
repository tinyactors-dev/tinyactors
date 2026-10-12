# frozen_string_literal: true

require 'fileutils'
require 'pathname'
require 'tmpdir'

# The W3C SCXML IRP suite, vendored in suites/w3c/irp (its SOURCE and
# LICENSE), and its templates instantiated per language into
# build/w3c/<language>, with an index.txt the runners read. The scenarios
# (suites/scenarios) are written in the same notation and instantiated by
# the same stylesheets into build/scenarios/<language>.
#
# A language is a stylesheet: every conf*.xsl in suites/w3c (ours) and in
# suites/w3c/irp (W3C's), named by the datamodel it writes for
# conf:datamodel (confEcma.xsl: ecmascript, confPython.xsl: python). Its
# W3C tests that cannot be translated are in suites/w3c/skip-<language>.txt.
# The W3C stylesheets are XSLT 2.0, which only SaxonC implements among
# what we can pin, so that one step is suites/w3c/convert.py, run with the
# pinned uv (mise.toml).
#
# Runs share build/: both bindings' conform tasks and core's may convert
# for the same language at once. So a conversion is written into a new
# directory and published by one rename (W3C.publish): a reader sees the
# whole previous conversion or the whole new one, never half of one.
module W3C
  ROOT = Pathname.new(__dir__).join('..').expand_path
  DIRECTORY = ROOT.join('suites/w3c')
  IRP = DIRECTORY.join('irp')
  CONVERT = DIRECTORY.join('convert.py')
  SKIP = DIRECTORY.join('skip.txt')
  SCENARIOS = ROOT.join('suites/scenarios')
  SETUPS = ROOT.join('suites/protocol/setups')
  BUILD = ROOT.join('build')

  # The template that names the datamodel a stylesheet writes.
  DATAMODEL = %r{<xsl:template match="//@conf:datamodel">\s*<xsl:attribute name="datamodel">([^<]+)</xsl:attribute>}

  module_function

  # { language => stylesheet } for every stylesheet in suites/w3c and
  # suites/w3c/irp.
  def stylesheets
    [DIRECTORY, IRP].flat_map { |directory| Dir[directory.join('conf*.xsl').to_s].sort }.to_h do |path|
      language = File.read(path)[DATAMODEL, 1] or raise "w3c: #{path} has no conf:datamodel template"
      [language.strip, Pathname.new(path)]
    end
  end

  def stylesheet(language)
    stylesheets.fetch(language) { raise "w3c: no stylesheet for #{language} (#{stylesheets.keys.join(', ')})" }
  end

  # The suite converted for `language`, again when the converter, the
  # stylesheet or a file of the suite is newer than its index.
  def suite(language = 'ecmascript')
    stylesheet = stylesheet(language)
    out = BUILD.join('w3c', language)
    index = out.join('index.txt')
    sources = [CONVERT, stylesheet, *Dir[IRP.join('**/*').to_s]]
    stale = -> { !index.file? || sources.any? { |source| File.mtime(source) > index.mtime } }
    publish(out, stale) { |fresh| convert(stylesheet.to_s, IRP.to_s, fresh.to_s) }
  end

  # The scenarios' documents for `language`, converted again when a
  # template or the stylesheet is newer than a conversion.
  def scenarios(language = 'ecmascript') = templates(SCENARIOS, BUILD.join('scenarios', language), language)

  # The protocol setups' charts written as templates (suites/protocol/setups/
  # *.txml: echo), converted for `language` likewise.
  def setups(language = 'ecmascript') = templates(SETUPS, BUILD.join('setups', language), language)

  # Every .txml under `source`, as .scxml at the same place under `out`.
  def templates(source, out, language)
    stylesheet = stylesheet(language)
    stale = lambda do
      Dir[source.join('**/*.txml').to_s].any? do |template|
        converted = out.join(Pathname.new(template).relative_path_from(source)).sub_ext('.scxml')
        !converted.file? || [template, stylesheet, CONVERT].any? { |each| File.mtime(each) > converted.mtime }
      end
    end
    publish(out, stale) { |fresh| convert('--scenarios', stylesheet.to_s, source.to_s, fresh.to_s) }
  end

  # `out` written again when `stale` says so, and `out` answered. The block
  # writes the conversion into a new, empty directory beside `out`
  # (.<name>.v<random>); then `out`, a symbolic link, is pointed at it with
  # one rename, which replaces the link at once, and the conversions it no
  # longer names are deleted. A reader resolves `out` on every open, so it
  # reads either conversion whole. A lock beside `out` (.<name>.lock) makes
  # one run convert at a time; a run that waited for it asks `stale` again
  # and finds the other's conversion. A block that fails leaves `out` as it
  # was, and its directory is deleted.
  def publish(out, stale)
    out = Pathname.new(out)
    return out unless stale.call

    parent = out.dirname
    parent.mkpath
    File.open(parent.join(".#{out.basename}.lock"), File::RDWR | File::CREAT, 0o644) do |lock|
      lock.flock(File::LOCK_EX)
      next unless stale.call

      fresh = Pathname.new(Dir.mktmpdir([".#{out.basename}.v", ''], parent.to_s))
      begin
        yield fresh
        link = parent.join(".#{out.basename}.link")
        FileUtils.rm_f(link)
        File.symlink(fresh.basename.to_s, link)
        FileUtils.rm_rf(out) if out.directory? && !out.symlink? # a conversion written in place, before publish
        File.rename(link, out)
      rescue StandardError
        FileUtils.rm_rf(fresh)
        raise
      end
      Dir[parent.join(".#{out.basename}.v*").to_s].each do |old|
        FileUtils.rm_rf(old) unless File.basename(old) == fresh.basename.to_s
      end
    end
    out
  end

  def convert(*arguments)
    raise 'w3c: convert.py failed' unless saxon(CONVERT, *arguments)
  end

  # Converts the suite, the scenarios and the setups' templates for
  # `language` and checks the result with suites/w3c/check.py: well-formed, nothing of the conf:
  # notation left, and for a language it has a compile check (python),
  # every expression compiles. True when it passes.
  def check(language)
    skips = [SKIP, skip_file(language)].select(&:file?).flat_map { |path| ['--skip', path.to_s] }
    saxon(DIRECTORY.join('check.py'), language, stylesheet(language).to_s, suite(language).to_s,
          scenarios(language).to_s, setups(language).to_s, *skips)
  end

  # Runs a script of suites/w3c with SaxonC, under the pinned uv and the
  # pinned python of mise.toml. Without --python, uv takes the newest
  # interpreter it manages, which can be a free-threaded build installed
  # for another project's tests.
  def saxon(script, *arguments)
    pid = Process.spawn('uv', 'run', '--quiet', '--python', pinned_python, '--with', 'saxonche',
                        'python', script.to_s, *arguments, chdir: ROOT.to_s)
    _pid, status = Process.wait2(pid)
    status.success?
  end

  # The python version mise.toml pins for this project.
  def pinned_python
    ROOT.join('mise.toml').read[/^python\s*=\s*"([^"]+)"/, 1] or
      raise 'tinyactors-test/mise.toml pins no python'
  end

  # One test of index.txt: its id, conformance, start document and
  # dependencies (charts, and the files a src="" reads).
  Test = Struct.new(:id, :conformance, :start, :dependencies)

  def tests(directory = suite)
    directory.join('index.txt').readlines(chomp: true).map do |line|
      id, conformance, start, dependencies = line.split(' ')
      Test.new(id, conformance, start, dependencies == '-' ? [] : dependencies.split(','))
    end
  end

  # The skip list of `language`'s translation.
  def skip_file(language)
    DIRECTORY.join("skip-#{language}.txt")
  end

  # The ids skip.txt lists, with their reasons, those of `language`'s
  # skip list (tests its stylesheet cannot translate), and those of the
  # files given (the run task's --skip: an implementation's own list, in
  # the same format, such as a datamodel that implements less).
  def skipped(*files, language: 'ecmascript')
    [SKIP, skip_file(language), *files].each_with_object({}) do |path, ids|
      next if path == skip_file(language) && !path.file?

      Pathname.new(path).readlines(chomp: true).reject { |line| line.start_with?('#') || line.strip.empty? }
              .each { |line| ids.store(*line.split(' ', 2)) }
    end
  end
end
