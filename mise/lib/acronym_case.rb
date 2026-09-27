# frozen_string_literal: true

# Checks that listed acronyms are spelled ALL CAPITALS or all lowercase in
# code, never Titlecase or mixedCase (AGENTS.md, "Acronyms"; the list is
# acronyms.txt at the repository root).
#
# An identifier is split where camelCase and snake_case split it, and a
# listed acronym is flagged only when it fills a whole segment:
#
# - `metricsToOtlp`: segments metrics, To, Otlp. `Otlp` is OTLP in mixed
#   case, so it is flagged.
# - `OTLPExporter`: a run of capitals ends where the next Titlecase word
#   begins, so the segments are OTLP and Exporter. Nothing is flagged.
# - `Idle`, `Iota`, `Identity`: `Id` and `Io` are followed by lowercase
#   letters, so they are the start of a longer word, not a segment.
# - `actorIds`: a trailing `s` after an acronym is its plural, so `Ids` is
#   ID in mixed case and is flagged (`actorIDs` or `actorids` pass).
#
# Only a spelling that mixes capitals and lowercase letters can be wrong,
# so all-capital and all-lowercase occurrences are never examined further.
module AcronymCase
  ROOT = File.expand_path('../..', __dir__)
  LIST = File.join(ROOT, 'acronyms.txt')
  ALLOW = File.join(ROOT, 'acronyms-allow.txt')

  CODE_EXTENSIONS = %w[.c .h .js .cjs .mjs .ts .rb].freeze

  # Paths the check never reads, as prefixes or path segments. Each entry
  # says why.
  EXCLUDED = [
    %r{(\A|/)vendor/},                     # third-party sources we copy in
    %r{(\A|/)node_modules/},               # installed packages
    %r{(\A|/)build/},                      # build output
    %r{(\A|/)dist/},                       # release output
    %r{\A\.claude/},                       # agent worktrees and settings
    %r{\Atinyactors-core/include/tinyactors\.c\z}, # generated from src/ (amalgamation)
    %r{(\A|/)docs/adr/},                   # ADRs are an append-only record
    %r{(\A|/)docs/research/},              # quotes other projects' names
    %r{\Amise/tasks/check-acronyms-test\z}, # the checker's own cases, wrong on purpose
    %r{\Amise/lib/acronym_case\.rb\z}     # the checker, whose comments show wrong spellings
  ].freeze

  Finding = Struct.new(:path, :line, :word, :spelling, :acronym, :fixed) do
    def to_s
      "#{path}:#{line}: #{spelling} → #{acronym} or #{acronym.downcase} (in #{word})"
    end
  end

  module_function

  def acronyms(path = LIST)
    File.readlines(path, chomp: true).map(&:strip).reject { |l| l.empty? || l.start_with?('#') }
  end

  # Words that may keep a mixed spelling: names we do not own, such as an
  # OpenTelemetry SDK field or a compiler flag. Each line of
  # acronyms-allow.txt is `WORD` or `WORD PATH-GLOB`, then `# reason`; with
  # a glob, the word is allowed only in matching paths.
  def allowed(path = ALLOW)
    return [] unless File.file?(path)

    File.readlines(path, chomp: true).filter_map do |line|
      word, glob = line.sub(/#.*/, '').split
      [word, glob] if word
    end
  end

  def allowed_word?(allow, word, path)
    allow.any? do |entry, glob|
      entry == word && (glob.nil? || File.fnmatch?(glob, path, File::FNM_PATHNAME | File::FNM_EXTGLOB))
    end
  end

  def pattern(list)
    alternatives = list.sort_by { |a| -a.length }.map { |a| Regexp.escape(a) }
    /\G(?:#{alternatives.join('|')})/i
  end

  def upper?(c) = c && c.match?(/[A-Z]/)
  def lower?(c) = c && c.match?(/[a-z]/)
  def letter?(c) = upper?(c) || lower?(c)

  # The mixed-case acronym segments in one identifier, as
  # [position, spelling, acronym].
  def check_word(word, regex, by_upcase)
    found = []
    i = 0
    while i < word.length
      m = regex.match(word, i)
      unless m
        i += 1
        next
      end
      text = m[0]
      j = i + text.length
      if text.match?(/[A-Z]/) && text.match?(/[a-z]/) && left_boundary?(word, i) && right_boundary?(word, j)
        spelling = word[j] == 's' && !right_boundary_plain?(word, j) ? "#{text}s" : text
        found << [i, spelling, by_upcase.fetch(text.upcase)]
      end
      i += 1
    end
    found
  end

  # The identifier with each flagged segment in capitals: `metricsToOtlp`
  # becomes `metricsToOTLP`, `actorIds` becomes `actorIDs`. A segment that
  # starts the identifier in lowercase cannot be flagged, so capitals never
  # turn a camelCase name into a Titlecase one.
  def fix_word(word, found)
    fixed = word.dup
    found.each { |i, _spelling, acronym| fixed[i, acronym.length] = acronym }
    fixed
  end

  # A segment starts at `i`: at the start, after a non-letter, or at a
  # capital (after lowercase, `toOtlp`; after a run of capitals, the
  # Titlecase word that ends it, `XMLHttp`).
  def left_boundary?(word, i)
    i.zero? || !letter?(word[i - 1]) || upper?(word[i])
  end

  def right_boundary_plain?(word, j)
    j >= word.length || !letter?(word[j]) || upper?(word[j])
  end

  # A segment ends at `j`, or one plural `s` later.
  def right_boundary?(word, j)
    return true if right_boundary_plain?(word, j)

    word[j] == 's' && right_boundary_plain?(word, j + 1)
  end

  # The text of `path` that the check reads: all of it for code, the code
  # spans and fenced blocks for Markdown (prose capitalizes as English does).
  def checked_text(path, text)
    return text unless path.end_with?('.md')

    lines = text.lines
    fenced = false
    lines.map do |line|
      if line.lstrip.start_with?('```', '~~~')
        fenced = !fenced
        "\n"
      elsif fenced
        line
      else
        spans = line.scan(/`([^`]+)`/).flatten
        "#{spans.join(' ')}\n"
      end
    end.join
  end

  def code_file?(path, root)
    return true if CODE_EXTENSIONS.include?(File.extname(path)) || path.end_with?('.md')
    return false unless path.include?('mise/tasks/') && File.extname(path).empty?

    first = File.open(File.join(root, path), &:gets).to_s
    first.start_with?('#!') && first.include?('ruby')
  end

  def excluded?(path)
    EXCLUDED.any? { |re| path.match?(re) }
  end

  # `memo` maps each word already checked to its segments, so a word that
  # repeats is split once.
  def check_text(path, text, regex, by_upcase, allow, memo)
    anywhere = unanchored(regex)
    findings = []
    checked_text(path, text).each_line.with_index(1) do |line, number|
      line.scan(/[A-Za-z0-9_$]+/) do |word|
        found = memo.fetch(word) { memo[word] = candidate?(word, anywhere) ? check_word(word, regex, by_upcase) : [] }
        next if found.empty? || allowed_word?(allow, word, path)

        fixed = fix_word(word, found)
        found.each do |_i, spelling, acronym|
          findings << Finding.new(path, number, word, spelling, acronym, fixed)
        end
      end
    end
    findings
  end

  # Only a word with both capitals and lowercase letters, holding a listed
  # acronym in any case, can have a mixed-case segment; most words have not.
  def candidate?(word, anywhere)
    word.match?(/[A-Z]/) && word.match?(/[a-z]/) && word.match?(anywhere)
  end

  # pattern() without its \G anchor: a listed acronym anywhere in a word.
  def unanchored(regex)
    (@unanchored ||= {})[regex] ||= Regexp.new(regex.source.delete_prefix('\\G'), regex.options)
  end

  # Every finding in `paths` (relative to `root`).
  def check(paths, root: ROOT, list: acronyms, allow: allowed)
    regex = pattern(list)
    by_upcase = list.to_h { |a| [a.upcase, a] }
    memo = {}
    paths.flat_map do |path|
      next [] if excluded?(path) || !code_file?(path, root)

      full = File.join(root, path)
      next [] unless File.file?(full)

      check_text(path, File.read(full, encoding: 'UTF-8', invalid: :replace, undef: :replace),
                 regex, by_upcase, allow, memo)
    end
  end

  # Rewrites every finding in place: each flagged word, on its line, becomes
  # its fixed form. Markdown lines are rewritten as a whole, so a prose
  # occurrence of the same word on that line is fixed too.
  def fix(findings, root: ROOT)
    findings.group_by(&:path).each do |path, in_file|
      full = File.join(root, path)
      lines = File.read(full).lines
      in_file.group_by(&:line).each do |number, on_line|
        on_line.map { |f| [f.word, f.fixed] }.uniq.each do |word, fixed|
          lines[number - 1] = lines[number - 1].gsub(/(?<![A-Za-z0-9_$])#{Regexp.escape(word)}(?![A-Za-z0-9_$])/, fixed)
        end
      end
      File.write(full, lines.join)
    end
  end
end
