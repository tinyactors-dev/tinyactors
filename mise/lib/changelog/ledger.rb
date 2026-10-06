# frozen_string_literal: true

module Changelog
  # The commits of a release range that belong to one component, read from
  # the Conventional Commits.
  #
  # A commit belongs to a component when its scopes name the component, or
  # when it names no component at all and touches the component's
  # directory (`feat(test-host)` that edits core is core's; `feat(napi)`
  # that edits a call site in core is not). A commit that only edits
  # changelogs is left out: the changelog is committed before the release,
  # and must not list itself.
  module Ledger
    Commit = Struct.new(:hash, :type, :scopes, :breaking, :description, :subject, :body, :files, keyword_init: true) do
      def short = hash[0, 7]
    end

    SUBJECT = /\A(?<type>[a-z]+)(?:\((?<scopes>[^)]*)\))?(?<bang>!)?:\s*(?<description>.*)\z/m
    BREAKING_FOOTER = /^BREAKING[ -]CHANGE:/

    SECTIONS = [
      ['Added', %w[feat]],
      ['Fixed', %w[fix]],
      ['Performance', %w[perf]],
      ['Changed', %w[refactor revert]]
    ].freeze

    module_function

    # The component's commits in `range` ("<tag>..HEAD", or "HEAD" for all),
    # newest first.
    def commits(repo, component, range)
      format = '%x1e%H%x1f%s%x1f%b%x1f'
      log = repo.git('log', '--no-merges', "--format=#{format}", '--name-only', range)
      log.split("\x1e").filter_map do |record|
        next if record.strip.empty?

        hash, subject, body, files = record.split("\x1f", 4)
        commit = parse(hash, subject, body.to_s.strip, files.to_s.lines.map(&:strip).reject(&:empty?))
        commit if commit.type != 'release' && belongs?(commit, component)
      end
    end

    def parse(hash, subject, body, files)
      body = body.lines.reject { |line| line.match?(/\A(Co-Authored-By|Signed-off-by):/i) }.join.strip
      match = SUBJECT.match(subject)
      breaking = body.match?(BREAKING_FOOTER)
      if match
        scopes = match[:scopes].to_s.split(/\s*,\s*/).reject(&:empty?)
        Commit.new(hash: hash, type: match[:type], scopes: scopes, breaking: breaking || !match[:bang].nil?,
                   description: match[:description].strip, subject: subject, body: body, files: files)
      else
        Commit.new(hash: hash, type: 'other', scopes: [], breaking: breaking, description: subject.strip,
                   subject: subject, body: body, files: files)
      end
    end

    def belongs?(commit, component)
      return false if !commit.files.empty? && commit.files.all? { |file| File.basename(file) == 'CHANGELOG.md' }

      named = commit.scopes & Changelog.component_scopes
      return !(named & component.scopes).empty? unless named.empty?

      commit.files.any? { |file| file.start_with?(component.path_prefix) }
    end

    # Sections in the order a release lists them: what each commit is, after
    # the breaking ones, which are listed first whatever their type.
    def grouped(commits)
      breaking, rest = commits.partition(&:breaking)
      changes = SECTIONS.filter_map do |heading, types|
        found = rest.select { |commit| types.include?(commit.type) }
        [heading, found] unless found.empty?
      end
      listed = SECTIONS.flat_map(&:last)
      internal = rest.reject { |commit| listed.include?(commit.type) }
      { breaking: breaking, changes: changes, internal: internal }
    end

    # Commits whose subject says too little to summarize: what is left
    # after removing ADR pointers is shorter than three words.
    def thin(commits)
      commits.select do |commit|
        next false if commit.type == 'docs' || commit.type == 'test'

        text = commit.description.gsub(/\(?(?:[a-z]+ )?ADRs?\s[^)]*\)?/i, '').gsub(/[^A-Za-z ]/, ' ')
        text.split.size < 3
      end
    end

    def line(commit) = "- #{commit.description} (#{commit.short})"
  end
end
