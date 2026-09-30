# frozen_string_literal: true

module Changelog
  # The ADRs a release's commits name, and what each says about how it was
  # implemented: the notes are what the agent reads to explain a change.
  module ADRs
    Note = Struct.new(:label, :path, :text, keyword_init: true)

    # "core ADR 0094", "ADRs 0090 and 0091", "kernel ADR 0002 section 2".
    REFERENCE = /(?:\b(?<qualifier>core|napi|kernel|host|scxml|tinyxml)\s+)?\bADRs?\s+(?<numbers>\d{4}(?:(?:,\s*|\s+and\s+|\s*,\s*and\s+)\d{4})*)/i
    QUALIFIERS = { 'kernel' => 'host' }.freeze
    LIMIT = 3000

    module_function

    # The notes of every ADR the commits name, each once, in the order the
    # commits (newest first) name them.
    def notes(repo, component, commits, rev: 'HEAD')
      references = commits.flat_map { |commit| references_in("#{commit.subject}\n#{commit.body}") }.uniq
      references.filter_map { |qualifier, number| note(repo, component, qualifier, number, rev) }.uniq(&:path)
    end

    def references_in(text)
      text.to_enum(:scan, REFERENCE).map { Regexp.last_match }.flat_map do |match|
        qualifier = match[:qualifier]&.downcase
        match[:numbers].scan(/\d{4}/).map { |number| [QUALIFIERS.fetch(qualifier, qualifier), number] }
      end
    end

    def note(repo, component, qualifier, number, rev)
      own = Changelog.component(qualifier || component.name)
      dirs = [own.adr_dir] + (qualifier ? [] : COMPONENTS.map(&:adr_dir) + ['docs/adr'])
      dirs.uniq.each do |dir|
        path = repo.files(rev, dir).find { |file| File.basename(file).start_with?("#{number}-") }
        next unless path

        owner = COMPONENTS.find { |candidate| candidate.adr_dir == dir }&.name || 'repository'
        return Note.new(label: "#{owner} ADR #{number}", path: path, text: implementation_notes(repo.show(rev, path), path))
      end
      nil
    end

    # The title, the status, and the "Implementation notes" section.
    def implementation_notes(text, path)
      title = text.lines.first.to_s.strip
      status = text.lines.find { |line| line.start_with?('**Status:**') }.to_s.strip
      section = text[/^## Implementation notes\b.*?(?=^## |\z)/m].to_s.strip
      body = [title, status, section].reject(&:empty?).join("\n\n")
      body = "#{body[0, LIMIT].rstrip}\n[... cut; read #{path}]" if body.size > LIMIT
      body
    end
  end
end
