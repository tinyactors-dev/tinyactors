# frozen_string_literal: true

require 'pathname'

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
module W3C
  ROOT = Pathname.new(__dir__).join('..').expand_path
  DIRECTORY = ROOT.join('suites/w3c')
  IRP = DIRECTORY.join('irp')
  CONVERT = DIRECTORY.join('convert.py')
  SKIP = DIRECTORY.join('skip.txt')
  SCENARIOS = ROOT.join('suites/scenarios')
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
    return out if index.file? && sources.all? { |source| File.mtime(source) <= index.mtime }

    convert(stylesheet.to_s, IRP.to_s, out.to_s)
    out
  end

  # The scenarios' documents for `language`, converted again when a
  # template or the stylesheet is newer than a conversion.
  def scenarios(language = 'ecmascript')
    stylesheet = stylesheet(language)
    out = BUILD.join('scenarios', language)
    stale = Dir[SCENARIOS.join('**/*.txml').to_s].any? do |template|
      converted = out.join(Pathname.new(template).relative_path_from(SCENARIOS)).sub_ext('.scxml')
      !converted.file? || [template, stylesheet, CONVERT].any? { |source| File.mtime(source) > converted.mtime }
    end
    return out unless stale

    convert('--scenarios', stylesheet.to_s, SCENARIOS.to_s, out.to_s)
    out
  end

  def convert(*arguments)
    pid = Process.spawn('uv', 'run', '--quiet', '--with', 'saxonche', 'python', CONVERT.to_s, *arguments,
                        chdir: ROOT.to_s)
    _pid, status = Process.wait2(pid)
    raise "w3c: convert.py failed (#{status.exitstatus})" unless status.success?
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
