# frozen_string_literal: true

module Changelog
  # Everything a release of one component is written from, computed from
  # the repository at `head`: the commits since the last release, the
  # change in the public API, the notes of the ADRs the commits name, and
  # the previous release's summary. The agent reads #packet; the checks
  # read #corpus, so the summary may use exactly what the agent was given.
  class Evidence
    attr_reader :repo, :component, :head, :last_version, :commits, :delta, :adrs, :names

    def self.gather(repo, component, head: 'HEAD')
      new(repo, component, repo.git('rev-parse', head).strip)
    end

    def initialize(repo, component, head)
      @repo = repo
      @component = component
      @head = head
      @last_version = repo.last_version(component)
      if first_release?
        @commits = []
        @delta = APISurface::Delta.new(added: [], removed: [], changed: [])
        @names = APISurface.surface(repo, component, head).values.map(&:name)
      else
        base = component.tag(last_version)
        @commits = Ledger.commits(repo, component, "#{base}..#{head}")
        old = APISurface.surface(repo, component, base)
        now = APISurface.surface(repo, component, head)
        @delta = APISurface.delta(old, now)
        @names = (old.values + now.values).map(&:name)
      end
      @adrs = ADRs.notes(repo, component, commits, rev: head)
    end

    def first_release? = last_version.nil?

    def breaking_commits = commits.select(&:breaking)

    def breaking? = !breaking_commits.empty? || !delta.breaking_names.empty?

    # The lowest version this release may have, and why (docs/adr/0002):
    # below 1.0 a minor version changes the API, a patch does not; from 1.0
    # a breaking change is a major version, a feature a minor one.
    def proposed_version
      return [nil, ['the first release: its version is the maintainer\'s choice']] if first_release?

      major, minor, patch, = Release.parse_version(last_version)
      reasons = []
      reasons << "#{breaking_commits.size} breaking commit(s)" unless breaking_commits.empty?
      reasons << "#{delta.breaking_names.size} removed or changed public symbol(s)" unless delta.breaking_names.empty?
      reasons << "#{delta.added.size} added public symbol(s)" unless delta.added.empty?
      features = commits.count { |commit| commit.type == 'feat' }
      reasons << "#{features} feat commit(s)" if features.positive?
      reasons << 'fixes and internal changes only' if reasons.empty?
      version =
        if major.zero?
          delta.empty? && !breaking? ? [major, minor, patch + 1] : [major, minor + 1, 0]
        elsif breaking?
          [major + 1, 0, 0]
        elsif features.positive? || !delta.added.empty?
          [major, minor + 1, 0]
        else
          [major, minor, patch + 1]
        end
      [version.join('.'), reasons]
    end

    def previous_summary
      text = repo.show(head, component.changelog_path)
      return unless text

      Document.sections(text).first&.summary
    end

    def packet
      lines = []
      version, reasons = proposed_version
      lines << "# Evidence for #{component.dir}"
      lines << ''
      if first_release?
        lines << "No release of #{component.dir} exists. This is its first release: write a summary only. " \
                 'Its history since the start of the repository is not a changelog, so no commits are listed.'
      else
        lines << "Last release: #{last_version} (tag #{component.tag(last_version)}). " \
                 "Range: #{component.tag(last_version)}..#{head[0, 12]}, #{commits.size} commit(s) for this component."
        lines << "Proposed version: #{version} (#{reasons.join('; ')}; the minimum this release may have)."
      end
      lines.concat(commit_section, api_section, adr_section, thin_section, previous_section)
      "#{lines.join("\n").rstrip}\n"
    end

    # The text a summary may draw identifiers from.
    def corpus = "#{packet}\n#{names.join("\n")}"

    private

    def commit_section
      return [] if commits.empty?

      out = ['', '## Commits (newest first)']
      commits.each do |commit|
        out << ''
        out << "### #{commit.short} #{commit.subject}#{commit.breaking ? '  [BREAKING]' : ''}"
        out << commit.body[0, 900] unless commit.body.empty?
        out << "Files: #{files_summary(commit)}"
      end
      out
    end

    def files_summary(commit)
      ours = commit.files.select { |file| file.start_with?(component.path_prefix) }
      ours = commit.files if ours.empty?
      shown = ours.first(6).join(', ')
      ours.size > 6 ? "#{shown} (and #{ours.size - 6} more; #{commit.files.size} in the commit)" : shown
    end

    def api_section
      return [] if first_release?

      out = ['', "## API delta (#{(component.api_json ? Array(component.api_json) : component.api_files).join(', ')})"]
      if delta.empty?
        out << '' << 'No public declaration was added, removed or changed.'
        return out
      end
      out.concat(entity_list('Removed', delta.removed), entity_list('Added', delta.added))
      unless delta.changed.empty?
        out << '' << "Changed (#{delta.changed.size}):"
        delta.changed.each do |entity, old|
          out << '' << "- #{entity.kind} `#{entity.name}` (#{entity.file})"
          APISurface.line_diff(entity, old).first(30).each { |line| out << "    #{line}" }
        end
      end
      out
    end

    def entity_list(heading, entities)
      return [] if entities.empty?

      ['', "#{heading} (#{entities.size}):", *entities.map { |entity| "- #{entity.kind} `#{entity.name}` (#{entity.file})" }]
    end

    def adr_section
      return [] if adrs.empty?

      ['', '## Notes of the ADRs the commits name', *adrs.flat_map { |note| ['', "### #{note.label} (#{note.path})", note.text] }]
    end

    def thin_section
      thin = Ledger.thin(commits)
      return [] if thin.empty?

      ['', '## Commits whose subject says little (read their diffs before describing them)',
       *thin.map { |commit| "- #{commit.short} #{commit.subject}" }]
    end

    def previous_section
      text = previous_summary
      return [] unless text

      ['', "## Previous summary (for voice and continuity; do not repeat it)", '', text]
    end
  end
end
