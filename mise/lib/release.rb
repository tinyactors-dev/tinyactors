# frozen_string_literal: true

require 'digest'
require 'fileutils'
require 'open3'
require 'pathname'
require 'tmpdir'

# What every project's release task shares (docs/adr/0001-packaging-and-releases.md):
# the checks before a release, versions and tags, release notes from the
# Conventional Commits, SHA256SUMS, the npm and PyPI credentials and the GitHub
# release. A project's task says what to build and publish; this module
# knows nothing about any one project.
#
# Development happens in this repository, which keeps every release's tag
# (`tinyxml/v0.2.0`). A project's GitHub releases are in a repository of
# its own, named after its directory (tinyactors-dev/tinyxml), which holds
# one snapshot commit of the directory per release, tagged `v0.2.0`
# (docs/adr/0016-a-release-repository-per-component.md).
module Release
  REPO = Pathname.new(__dir__).join('../..').expand_path

  # The GitHub organization of this repository and of the release
  # repositories.
  ORG = 'tinyactors-dev'
  MONOREPO = "#{ORG}/tinyactors"

  # The commit author's trailer on every commit a release makes.
  TRAILER = 'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>'

  module_function

  def fail!(message)
    abort "release: #{message}"
  end

  # Runs `command`, streaming its output; fails the release when it fails.
  # A URL's credentials are left out of what is printed.
  def run(*command, chdir: REPO, env: {})
    shown = redact(command.join(' '))
    puts "$ #{shown}"
    pid = Process.spawn(env, *command, chdir: chdir.to_s)
    _pid, status = Process.wait2(pid)
    fail!("#{shown} failed") unless status.success?
  end

  # `text` without the user and password of any URL in it
  # (`https://x-access-token:...@github.com/...`).
  def redact(text) = text.gsub(%r{(\w+://)[^/@\s]+@}, '\1')

  # `git` in the repository; answers stdout, or nil when git fails.
  def git(*args)
    out, status = Open3.capture2('git', *args, chdir: REPO.to_s, err: File::NULL)
    status.success? ? out : nil
  end

  # A version is MAJOR.MINOR.PATCH, with an optional -prerelease.
  VERSION = /\A(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z.-]+))?\z/

  def parse_version(text)
    match = VERSION.match(text.to_s) or fail!("#{text.inspect} is not a version (MAJOR.MINOR.PATCH)")
    [match[1].to_i, match[2].to_i, match[3].to_i, match[4]]
  end

  # Semver precedence: a prerelease sorts before its release.
  def compare_versions(a, b)
    a = parse_version(a)
    b = parse_version(b)
    numbers = a.first(3) <=> b.first(3)
    return numbers unless numbers.zero?
    return 0 if a[3] == b[3]
    return 1 if a[3].nil?
    return -1 if b[3].nil?

    a[3] <=> b[3]
  end

  # The versions of the tags `<prefix>v<version>`, oldest first.
  def released_versions(prefix)
    (git('tag', '--list', "#{prefix}v*") || '').lines.map { |tag| tag.strip.delete_prefix("#{prefix}v") }
                                                 .select { |version| VERSION.match?(version) }
                                                 .sort { |a, b| compare_versions(a, b) }
  end

  # The development version of HEAD (ADR 0001, decision 15):
  # <next minor>-dev.<YYYYMMDD>.<N>.g<sha>, where the date is HEAD's commit
  # date in UTC and N counts the commits since the last `prefix` release
  # (all commits before the first). Semver compares prerelease identifiers
  # left to right and numeric ones as numbers, so on linear main a later
  # commit always sorts after an earlier one: the date first, then N. The
  # sha only names the commit; the `g` keeps an all-digit sha with a leading
  # zero from being an invalid numeric identifier. The same commit always
  # gets the same version.
  def dev_version(prefix)
    latest = released_versions(prefix).last
    major, minor, = latest ? parse_version(latest) : [0, 0, 0]
    range = latest ? "#{prefix}v#{latest}..HEAD" : 'HEAD'
    count = git('rev-list', '--count', range).to_s.strip
    date = with_utc { git('show', '-s', '--format=%cd', '--date=format-local:%Y%m%d', 'HEAD') }.to_s.strip
    sha = git('rev-parse', '--short=7', 'HEAD').to_s.strip
    fail!('cannot describe HEAD for a development version') if [count, date, sha].any?(&:empty?)

    "#{major}.#{minor + 1}.0-dev.#{date}.#{count}.g#{sha}"
  end

  def with_utc
    zone = ENV['TZ']
    ENV['TZ'] = 'UTC'
    yield
  ensure
    zone.nil? ? ENV.delete('TZ') : ENV['TZ'] = zone
  end

  # The tree is clean, on main, and in step with origin/main. With `scope`
  # (directories relative to the repository), only changes inside it count:
  # other sessions share the working tree (AGENTS.md), a release builds only
  # from its own directories, and its commit stages only the files it
  # versions, so an edit elsewhere can neither reach the release nor be
  # committed by it.
  def check_tree!(scope: nil)
    git('fetch', '--quiet', '--tags', 'origin') or fail!('git fetch origin failed')
    status = git('status', '--porcelain') or fail!('git status failed')
    if scope
      status = status.lines.select do |line|
        scope.any? { |dir| line[3..].start_with?("#{dir.chomp('/')}/") }
      end.join
    end
    fail!("the working tree is not clean:\n#{status}") unless status.empty?
    branch = git('rev-parse', '--abbrev-ref', 'HEAD').to_s.strip
    fail!("on #{branch}, not main") unless branch == 'main'
    head = git('rev-parse', 'HEAD').to_s.strip
    upstream = git('rev-parse', 'origin/main').to_s.strip
    fail!('main is not in step with origin/main; pull or push first') unless head == upstream
  end

  # `version` is new, higher than every earlier release, and its tag is free
  # here and on origin.
  def check_version!(prefix, version)
    parse_version(version)
    tag = "#{prefix}v#{version}"
    latest = released_versions(prefix).last
    if latest && compare_versions(version, latest) <= 0
      fail!("#{version} is not higher than the last release, #{latest}")
    end
    fail!("the tag #{tag} exists") if git('rev-parse', '--quiet', '--verify', "refs/tags/#{tag}")
    remote = git('ls-remote', '--tags', 'origin', "refs/tags/#{tag}") or fail!('git ls-remote origin failed')
    fail!("the tag #{tag} exists on origin") unless remote.strip.empty?
    tag
  end

  # Release notes: the project's section of CHANGELOG.md for `version`
  # (docs/adr/0002-changelogs.md). The release stops unless the section
  # passes `mise run changelog:check`: an agent writes its summary from the
  # evidence `changelog:draft` prints, and the release-changelog skill
  # (.agents/skills/release-changelog) is how it gets there.
  def notes(prefix:, version:, title:)
    component, repo = check_changelog!(prefix: prefix, version: version)
    section = Changelog::Document.section(Changelog::Check.changelog_text(repo, component), version)
    "# #{title}\n\n#{Changelog::Document.release_body(section)}"
  end

  # Fails the release unless the project's CHANGELOG.md has a section for
  # `version` that passes `changelog:check`. A release task calls it in its
  # check stage, before it builds anything; `notes` calls it again.
  def check_changelog!(prefix:, version:)
    require 'changelog' # changelog/repo.rb requires this file
    component = Changelog.component(prefix)
    repo = Changelog::Repo.new(REPO)
    problems = Changelog::Check.problems(repo, component, version)
    unless problems.empty?
      fail!("#{component.changelog_path} is not fit to release #{version}:\n  #{problems.join("\n  ")}\n" \
            "Write it with the release-changelog skill; `mise run changelog:check #{component.name} #{version}` repeats this check.")
    end
    [component, repo]
  end

  # Writes SHA256SUMS for `files` into `out`, one "<sha256>  <name>" per file.
  def write_sha256sums(files, out)
    lines = files.sort_by { |file| File.basename(file) }
                 .map { |file| "#{Digest::SHA256.file(file).hexdigest}  #{File.basename(file)}" }
    File.write(out, "#{lines.join("\n")}\n")
    out
  end

  # Whether fnox has a non-empty NPM_TOKEN. The token is never read here:
  # the shell only tests it.
  def npm_token?
    system('fnox', 'exec', '--', 'sh', '-c', 'test -n "$NPM_TOKEN"', out: File::NULL, err: File::NULL)
  end

  # Yields an npm userconfig outside the tree that names the token by
  # reference, `${NPM_TOKEN}`, which npm expands from the environment fnox
  # sets. The file never holds the token, and is deleted afterwards.
  def with_npm_userconfig
    Dir.mktmpdir('release-npmrc') do |dir|
      path = File.join(dir, 'npmrc')
      File.write(path, "//registry.npmjs.org/:_authToken=${NPM_TOKEN}\n")
      yield path
    end
  end

  # Whether the registry has `name@version`: false for a 404, which includes
  # a package that was never published.
  def npm_published?(name, version)
    out, err, status = Open3.capture3('npm', 'view', "#{name}@#{version}", 'version')
    return !out.strip.empty? if status.success?
    return false if err.include?('E404')

    fail!("npm view #{name}@#{version} failed:\n#{err}")
  end

  # Publishes the tarballs in order. On a failure it stops and prints what
  # was published and what is left: npm cannot unpublish freely, so nothing
  # is retried.
  # Publishes `tarballs` in order, under `dist_tag`, and stops at the first
  # failure. Answers the tarballs published, and the one that failed (nil
  # when all went), so the caller can undo its commit and tag when nothing
  # was published.
  # A failure is never retried: npm cannot unpublish freely.
  def npm_publish!(tarballs, dist_tag: 'latest')
    published = []
    with_npm_userconfig do |userconfig|
      tarballs.each do |tarball|
        command = ['fnox', 'exec', '--', 'npm', 'publish', tarball.to_s, '--access', 'public',
                   '--tag', dist_tag, '--userconfig', userconfig]
        puts "$ npm publish #{File.basename(tarball)} --access public --tag #{dist_tag}"
        return [published, tarball] unless system(*command, chdir: REPO.to_s)

        published << tarball
      end
    end
    [published, nil]
  end

  # Whether the registry accepts NPM_TOKEN (`npm whoami`). It cannot tell
  # whether a publish will ask for a one-time password: a token for
  # releases must bypass two-factor authentication (ADR 0001).
  def npm_authenticated?
    with_npm_userconfig do |userconfig|
      system('fnox', 'exec', '--', 'npm', 'whoami', '--userconfig', userconfig, out: File::NULL, err: File::NULL)
    end
  end

  # How a command gets PYPI_TOKEN: through fnox where it is installed (a
  # maintainer's machine), else from the environment as it is (a cloud
  # session whose environment holds it). Either way only the shell that
  # runs the command sees it.
  def pypi_secrets
    system('sh', '-c', 'command -v fnox', out: File::NULL) ? %w[fnox exec --] : []
  end

  # Whether PYPI_TOKEN is set and not empty (ADR 0001, decision 24), tested
  # by the shell, never read here.
  def pypi_token?
    system(*pypi_secrets, 'sh', '-c', 'test -n "$PYPI_TOKEN"', out: File::NULL, err: File::NULL)
  end

  # Whether PyPI has `name` at `version`: false for a 404, which includes a
  # project that was never published.
  def pypi_published?(name, version)
    require 'net/http'
    response = Net::HTTP.get_response(URI("https://pypi.org/pypi/#{name}/#{version}/json"))
    return true if response.is_a?(Net::HTTPSuccess)
    return false if response.code == '404'

    fail!("PyPI answered #{response.code} for #{name} #{version}")
  end

  # Uploads `files` to PyPI with `uv` (a path), the token passed from
  # PYPI_TOKEN (pypi_secrets) to uv's UV_PUBLISH_TOKEN inside the shell, so it never
  # reaches this process, a file or a command line. --check-url skips the
  # files PyPI has already, so a publish that stopped half way runs again.
  # Answers whether it succeeded; PyPI never takes a file name back, so a
  # failure is not undone.
  def pypi_publish!(uv, files)
    puts "$ uv publish #{files.map { |file| File.basename(file) }.join(' ')}"
    script = 'UV_PUBLISH_TOKEN="$PYPI_TOKEN" exec "$0" publish --check-url https://pypi.org/simple/ "$@"'
    system(*pypi_secrets, 'sh', '-c', script, uv.to_s, *files.map(&:to_s), chdir: REPO.to_s)
  end

  # Undoes commit_and_tag! when nothing was published: deletes the tag and
  # drops the release commit, which only HEAD may be.
  def undo_commit_and_tag!(subject:, tag:)
    fail!("HEAD is not #{subject.inspect}; not undoing it") unless git('log', '-1', '--format=%s')&.strip == subject

    run('git', 'tag', '--delete', tag)
    run('git', 'reset', '--quiet', '--hard', 'HEAD~1')
  end

  # Commits `paths` as `subject` and tags the commit, annotated with `notes`.
  def commit_and_tag!(paths:, subject:, tag:, notes_file:)
    run('git', 'add', '--', *paths.map(&:to_s))
    run('git', 'commit', '--quiet', '-m', subject, '-m', TRAILER)
    run('git', 'tag', '--annotate', '--cleanup=verbatim', tag, '--file', notes_file.to_s)
  end

  # Whether origin has `tag`, so a release whose push went through is not
  # undone: nil when origin cannot be asked.
  def pushed?(tag)
    remote = git('ls-remote', '--tags', 'origin', "refs/tags/#{tag}") or return nil
    !remote.strip.empty?
  end

  # Pushes `tag` to origin, with main when the tag's commit is not on
  # origin's main yet, in one atomic push: both refs or neither. When it is
  # (a publish run again after another session pushed to main), only the
  # tag goes, so a local main that is behind is not pushed.
  def push_to_origin!(tag)
    git('fetch', '--quiet', 'origin', 'main') or fail!('git fetch origin main failed')
    if system('git', 'merge-base', '--is-ancestor', "#{tag}^{commit}", 'origin/main', chdir: REPO.to_s)
      run('git', 'push', 'origin', "refs/tags/#{tag}")
    else
      run('git', 'push', '--atomic', 'origin', 'main', "refs/tags/#{tag}")
    end
  end

  # Whether `repository` has the GitHub release `release_tag`. A draft,
  # which `gh release create` leaves when an upload fails, fails the
  # release: its files are not all there.
  def released?(repository, release_tag)
    # The list, not releases/tags/<tag>, which leaves drafts out.
    out, status = Open3.capture2('gh', 'api', '--paginate', "repos/#{repository}/releases",
                                 '--jq', ".[] | select(.tag_name == \"#{release_tag}\") | .draft")
    fail!("gh api repos/#{repository}/releases failed") unless status.success?
    return false if out.strip.empty?
    return true unless out.lines.map(&:strip).include?('true')

    fail!("#{repository}'s release #{release_tag} is a draft, left by a stopped upload; delete it " \
          "(gh release delete #{release_tag} --repo #{repository}) and run the publish again")
  end

  # A release tag of this repository, `<directory>/v<version>`, split into
  # its directory and version.
  def split_tag(tag)
    directory, version = tag.split(%r{/v(?=[^/]*\z)}, 2)
    fail!("#{tag.inspect} is not a release tag (<directory>/v<version>)") if version.nil? || directory.empty?
    parse_version(version)
    [directory, version]
  end

  # The repository that holds the GitHub releases of `directory`.
  def release_repository(directory) = "#{ORG}/#{directory}"

  # The git URL of `repository`, written as origin's is (HTTPS or SSH), so
  # it uses the same credentials.
  def repository_url(repository)
    origin = git('remote', 'get-url', 'origin').to_s.strip
    url = origin.sub(%r{#{Regexp.escape(MONOREPO)}(\.git)?/?\z}) { "#{repository}#{Regexp.last_match(1)}" }
    fail!("origin is #{origin.inspect}, not #{MONOREPO}") if url == origin

    url
  end

  # Creates `repository`, private like this one (ADR 0001, decision 13),
  # without issues or a wiki: those stay in this repository. Nothing when
  # it exists.
  def ensure_release_repository!(repository, directory)
    return if system('gh', 'api', "repos/#{repository}", '--silent', out: File::NULL, err: File::NULL)

    run('gh', 'repo', 'create', repository, '--private', '--disable-issues', '--disable-wiki',
        '--description', "Releases of #{directory}/ from #{MONOREPO}, where it is developed")
  end

  # The tags of the repository at `url`, as { name => commit }.
  def remote_tags(url)
    out = git('ls-remote', '--tags', url) or fail!("git ls-remote #{redact(url)} failed")
    out.lines.each_with_object({}) do |line, tags|
      sha, ref = line.split
      name = ref.delete_prefix('refs/tags/')
      peeled = name.end_with?('^{}')
      name = name.delete_suffix('^{}')
      tags[name] = sha if peeled || !tags.key?(name)
    end
  end

  # Pushes the snapshot of `tag` (`<directory>/v<version>`) to the
  # directory's release repository, creating it when it is missing: one
  # commit whose tree is the directory's tree at the tag and whose parent
  # is the release repository's main (none for the first), with the tagged
  # commit's author, committer and dates, pushed as main and tagged
  # `v<version>`. Answers the repository and that tag.
  #
  # It can run again: when the release repository has the tag with the same
  # tree, nothing is pushed, and with another tree it fails. A version not
  # above the release repository's latest is refused, so its main only
  # moves forward.
  def push_snapshot!(tag)
    directory, version = split_tag(tag)
    repository = release_repository(directory)
    release_tag = "v#{version}"
    commit = git('rev-parse', '--verify', '--quiet', "#{tag}^{commit}").to_s.strip
    fail!("there is no tag #{tag}") if commit.empty?
    tree = git('rev-parse', '--verify', '--quiet', "#{commit}:#{directory}").to_s.strip
    fail!("#{tag} has no directory #{directory}/") if tree.empty?

    ensure_release_repository!(repository, directory)
    url = repository_url(repository)
    tags = remote_tags(url)
    if (existing = tags[release_tag])
      run('git', 'fetch', '--quiet', '--no-tags', url, "refs/tags/#{release_tag}")
      unless git('rev-parse', "#{existing}^{tree}").to_s.strip == tree
        fail!("#{repository} has #{release_tag} with a tree other than #{directory}/ at #{tag}")
      end
      puts "release: #{repository} has #{release_tag} already, with #{directory}/ at #{tag}"
      return [repository, release_tag]
    end
    released = tags.keys.filter_map { |name| name[1..] if name.start_with?('v') && VERSION.match?(name[1..]) }
    latest = released.max { |a, b| compare_versions(a, b) }
    if latest && compare_versions(version, latest) <= 0
      fail!("#{repository}'s latest release is v#{latest}; #{release_tag} would move its main back " \
            '(copy a release made before the release repositories before the next one is published)')
    end

    parent = git('ls-remote', url, 'refs/heads/main').to_s.split.first
    run('git', 'fetch', '--quiet', '--no-tags', url, 'refs/heads/main') if parent
    # A main that holds this tree already is this release's snapshot, from
    # a push that stopped before the tag: only the tag is missing.
    snapshot = if parent && git('rev-parse', "#{parent}^{tree}").to_s.strip == tree
                 parent
               else
                 commit_tree!(tree: tree, parent: parent, from: commit,
                              message: "#{directory} #{version}\n\n#{directory}/ at #{tag} in #{MONOREPO} (#{commit}).\n")
               end
    run('git', 'push', '--atomic', url, "#{snapshot}:refs/heads/main", "#{snapshot}:refs/tags/#{release_tag}")
    [repository, release_tag]
  end

  # `git commit-tree` of `tree` on `parent` (none when nil), with the
  # identity and dates of the commit `from`.
  def commit_tree!(tree:, parent:, from:, message:)
    fields = git('show', '-s', '--format=%an%x00%ae%x00%ad%x00%cn%x00%ce%x00%cd', '--date=raw', from).to_s.chomp.split("\0")
    fail!("cannot read the commit #{from}") unless fields.size == 6
    env = %w[GIT_AUTHOR_NAME GIT_AUTHOR_EMAIL GIT_AUTHOR_DATE GIT_COMMITTER_NAME GIT_COMMITTER_EMAIL GIT_COMMITTER_DATE]
          .zip(fields).to_h
    command = ['git', 'commit-tree', tree, *(parent ? ['-p', parent] : []), '-m', message]
    out, status = Open3.capture2(env, *command, chdir: REPO.to_s)
    fail!("#{command.join(' ')} failed") unless status.success?
    out.strip
  end

  # Pushes main and the tag to origin (push_to_origin!), then the
  # directory's snapshot to its release repository (push_snapshot!), then
  # creates the GitHub release there with `assets`. Each step can run again
  # after a stop: origin and the release repository take what they have
  # already, and a release that exists is left as it is.
  def push_and_release!(tag:, title:, notes_file:, assets:)
    push_to_origin!(tag)
    repository, release_tag = push_snapshot!(tag)
    if released?(repository, release_tag)
      puts "release: #{repository} has the release #{release_tag} already; left as it is"
      return
    end
    run('gh', 'release', 'create', release_tag, '--repo', repository, '--verify-tag', '--title', title,
        '--notes-file', notes_file.to_s, *assets.map(&:to_s))
  end
end
