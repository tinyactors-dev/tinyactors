# frozen_string_literal: true

require 'fileutils'
require 'json'
require 'open3'
require 'pathname'
require 'tmpdir'
require 'toolchain'

# The workspace's Ruby build scripts, the libraries under mise/lib/ and the
# file tasks under mise/tasks/ of the root and of every project, the
# libraries beside a project's tasks (<project>/mise/*.rb), and their
# static analysis with ast-grep (sgconfig.yml, mise/ast-grep/rules/).
#
# ast-grep chooses a file's language by its name, and a mise file task has
# no extension, so BuildScripts.scan copies every script into a temporary
# tree, a task under its path plus `.rb`, runs `ast-grep scan` there, and
# reports each finding at the script's own path and line.
module BuildScripts
  ROOT = Toolchain::ROOT
  DIRECTORIES = %w[mise/lib mise/tasks].freeze
  # What a project's tasks share, beside its tasks (tinyactors-core/mise/core.rb,
  # tinyactors-cpython/mise/cpython.rb, tinyactors-napi/mise/napi.rb).
  PROJECT_LIBRARIES = %w[*/mise/*.rb].freeze

  # A finding: where, which rule, and what to do instead.
  Finding = Data.define(:path, :line, :column, :rule, :message, :text) do
    def to_s = "#{path}:#{line}:#{column}: #{rule}: #{message}\n    #{text.lines.first.strip}"
  end

  module_function

  # The tracked Ruby build scripts, as paths relative to the root; with
  # `within`, a project's directory, only those under it (a binding's lint
  # checks its own scripts).
  def paths(within = nil)
    patterns = DIRECTORIES.flat_map { |directory| [directory, "*/#{directory}/*"] } + PROJECT_LIBRARIES
    out, status = Open3.capture2('git', 'ls-files', '-z', '--', *patterns, chdir: ROOT.to_s)
    raise Toolchain::Error, 'git ls-files failed' unless status.success?

    prefix = within && "#{within.to_s.chomp('/')}/"
    out.split("\0").select { |path| (prefix.nil? || path.start_with?(prefix)) && ruby?(ROOT.join(path)) }.sort
  end

  def ruby?(file)
    return false unless file.file?
    return true if file.extname == '.rb'

    file.extname.empty? && file.open(&:gets).to_s.start_with?('#!') && file.open(&:gets).include?('ruby')
  end

  # The name a script has in the scanned copy.
  def scanned_name(path) = File.extname(path) == '.rb' ? path : "#{path}.rb"

  # Every finding of the rules in sgconfig.yml, sorted by place; with
  # `within`, in the scripts under that directory only.
  def scan(within = nil)
    by_copy = paths(within).to_h { |path| [scanned_name(path), path] }
    Dir.mktmpdir('build-scripts') do |dir|
      by_copy.each do |copy, path|
        FileUtils.mkdir_p(File.dirname(File.join(dir, copy)))
        FileUtils.cp(ROOT.join(path), File.join(dir, copy))
      end
      out, err, status = Toolchain::Tool[:ast_grep].capture('scan', '--config', ROOT.join('sgconfig.yml'),
                                                            '--json=compact', '.', chdir: dir)
      raise Toolchain::Error, "ast-grep scan failed:\n#{err}" if out.strip.empty? && !status.success?

      JSON.parse(out).map do |match|
        start = match.fetch('range').fetch('start')
        Finding.new(path: by_copy.fetch(match.fetch('file').delete_prefix('./')), line: start.fetch('line') + 1,
                    column: start.fetch('column') + 1, rule: match.fetch('ruleId'), message: match.fetch('message'),
                    text: match.fetch('text'))
      end.sort_by { |finding| [finding.path, finding.line, finding.column] }
    end
  end

  # The rules' own tests (mise/ast-grep/tests/): each rule reports what it
  # should and nothing else. Answers [passed, output].
  def test_rules
    out, err, status = Toolchain::Tool[:ast_grep].capture('test', '--config', ROOT.join('sgconfig.yml'),
                                                          '--skip-snapshot-tests', chdir: ROOT)
    [status.success?, "#{out}#{err}"]
  end
end
