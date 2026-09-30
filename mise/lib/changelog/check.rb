# frozen_string_literal: true

module Changelog
  # What makes a changelog section fit to release. The prose is an agent's,
  # so the checks hold it to the evidence: nothing it cites may be missing
  # from the repository, and nothing that breaks a caller may go
  # unmentioned.
  #
  # A check that fails says what to change.
  module Check
    MAX_SUMMARY_WORDS = 400
    PLACEHOLDER = /\b(TODO|FIXME|TBD|XXX)\b/
    HASH = /\b(?=[0-9a-f]*\d)(?=[0-9a-f]*[a-f])[0-9a-f]{7,40}\b/
    IDENTIFIER = /\A[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*(?:\(\))?\z/

    module_function

    # The component's CHANGELOG.md as it is on disk, or nil.
    def changelog_text(repo, component)
      file = repo.root.join(component.changelog_path)
      file.file? ? file.read : nil
    end

    # The problems of the section for `version` in the component's
    # CHANGELOG.md (`text`) against the evidence at `head`; none means it
    # may be released.
    def problems(repo, component, version, text: changelog_text(repo, component), head: 'HEAD')
      unless text
        return ["#{component.changelog_path} does not exist: write the section with " \
                "`mise run changelog:write #{component.name} #{version}`"]
      end

      section = Document.section(text, version)
      unless section
        return ["#{component.changelog_path} has no section \"## [#{version}] - YYYY-MM-DD\""]
      end

      evidence = Evidence.gather(repo, component, head: head)
      found = []
      found.concat(version_problems(evidence, version))
      found.concat(prose_problems(section))
      found.concat(generated_problems(evidence, section))
      return found if evidence.first_release?

      found.concat(breaking_problems(evidence, section))
      found.concat(citation_problems(repo, evidence, section))
      found.concat(identifier_problems(evidence, section))
      found
    end

    def version_problems(evidence, version)
      proposed, reasons = evidence.proposed_version
      return [] unless proposed

      if Release.compare_versions(version, evidence.last_version) <= 0
        return ["#{version} is not higher than the last release, #{evidence.last_version}"]
      end
      return [] if Release.compare_versions(version, proposed) >= 0

      ["#{version} is lower than the proposed #{proposed} (#{reasons.join('; ')})"]
    end

    def prose_problems(section)
      found = []
      summary = section.summary.to_s
      found << 'the Summary is empty' if summary.empty?
      words = summary.split.size
      found << "the Summary is #{words} words; keep it under #{MAX_SUMMARY_WORDS}" if words > MAX_SUMMARY_WORDS
      written = "#{summary}\n#{section.upgrading}"
      found << "the Summary or Upgrading holds a placeholder (#{written[PLACEHOLDER]})" if written.match?(PLACEHOLDER)
      found
    end

    def generated_problems(evidence, section)
      expected = Document.render_generated(evidence)
      return [] if section.generated.to_s.strip == expected.strip

      ['the generated part of the section is stale or was edited: run `mise run changelog:write` again ' \
       '(a commit for this component landed since, or the lists were edited by hand)']
    end

    # Every breaking commit is cited by hash; every removed or changed
    # public symbol is named, and the release has an Upgrading section.
    def breaking_problems(evidence, section)
      found = []
      written = "#{section.summary}\n#{section.upgrading}"
      cited = possible_hashes(written)
      evidence.breaking_commits.each do |commit|
        next if cited.any? { |hash| commit.hash.start_with?(hash) }

        found << "the breaking commit #{commit.short} (#{commit.description}) is not cited: write its hash in the Summary or Upgrading"
      end
      return found unless evidence.breaking?

      if section.upgrading.to_s.empty?
        found << 'the release breaks callers but has no "### Upgrading" section'
        return found
      end
      evidence.delta.breaking_names.each do |name|
        next if section.upgrading.match?(/(?<![A-Za-z0-9_])#{Regexp.escape(name)}(?![A-Za-z0-9_])/)

        found << "Upgrading does not mention the removed or changed `#{name}`"
      end
      found
    end

    # Every hash the prose cites is a commit of the release range.
    def citation_problems(repo, evidence, section)
      written = "#{section.summary}\n#{section.upgrading}"
      base = evidence.component.tag(evidence.last_version)
      known = repo.git('rev-list', "#{base}..#{evidence.head}").lines.map(&:strip)
      cited_hashes(written).filter_map do |hash|
        "the Summary or Upgrading cites #{hash}, which is not a commit since #{evidence.last_version}" unless known.any? { |full| full.start_with?(hash) }
      end
    end

    # Every identifier the prose puts in backticks is one the evidence
    # mentions: a function, type, macro or option that does not exist is
    # how an agent's summary goes wrong.
    def identifier_problems(evidence, section)
      written = "#{section.summary}\n#{section.upgrading}"
      corpus = evidence.corpus
      written.scan(/`([^`\n]+)`/).flatten.uniq.filter_map do |code|
        next unless code.match?(IDENTIFIER)

        name = code.delete_suffix('()')
        next if name.split('.').all? { |part| corpus.match?(/(?<![A-Za-z0-9_])#{Regexp.escape(part)}(?![A-Za-z0-9_])/) }

        "`#{code}` is not in the evidence (commits, API, ADR notes): remove it, or check the name against the repository"
      end
    end

    # The hashes a text cites, to validate: a hex token that mixes digits
    # and letters, so that "defaced" and "4194304" are not taken for one.
    def cited_hashes(text) = text.scan(HASH).uniq

    # Every hex token of seven characters or more: what may cite a breaking
    # commit, whose hash is all digits one time in twenty-seven.
    def possible_hashes(text) = text.scan(/\b[0-9a-f]{7,40}\b/).uniq
  end
end
