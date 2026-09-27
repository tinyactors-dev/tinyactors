# frozen_string_literal: true

require 'digest'
require 'fileutils'
require 'open3'
require 'pathname'
require 'tmpdir'

# What every project's release task shares (docs/adr/0001-packaging-and-releases.md):
# the checks before a release, versions and tags, release notes from the
# Conventional Commits, SHA256SUMS, the npm credentials and the GitHub
# release. A project's task says what to build and publish; this module
# knows nothing about any one project.
module Release
  REPO = Pathname.new(__dir__).join('../..').expand_path

  # The commit author's trailer on every commit a release makes.
  TRAILER = 'Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>'

  module_function

  def fail!(message)
    abort "release: #{message}"
  end

  # Runs `command`, streaming its output; fails the release when it fails.
  def run(*command, chdir: REPO, env: {})
    puts "$ #{command.join(' ')}"
    pid = Process.spawn(env, *command, chdir: chdir.to_s)
    _pid, status = Process.wait2(pid)
    fail!("#{command.join(' ')} failed") unless status.success?
  end

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

  # The tree is clean, on main, and in step with origin/main.
  def check_tree!
    git('fetch', '--quiet', '--tags', 'origin') or fail!('git fetch origin failed')
    status = git('status', '--porcelain') or fail!('git status failed')
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

  # Release notes: the Conventional Commits since the previous release that
  # touch `path`, grouped by type. The first release lists the features and
  # fixes since the start, after `summary`.
  def notes(prefix:, path:, title:, summary:)
    previous = released_versions(prefix).last
    range = previous ? ["#{prefix}v#{previous}..HEAD"] : []
    subjects = (git('log', '--format=%s', *range, '--', path) || '').lines.map(&:strip)
    groups = { 'feat' => 'Features', 'fix' => 'Fixes' }
    groups['perf'] = 'Performance' if previous
    sections = groups.filter_map do |type, heading|
      lines = subjects.select { |subject| subject.match?(/\A#{type}(\([^)]*\))?!?:/) }
      next if lines.empty?

      "## #{heading}\n\n#{lines.map { |line| "- #{line}" }.join("\n")}\n"
    end
    intro = previous ? "Changes since #{previous}." : summary
    "# #{title}\n\n#{intro}\n\n#{sections.join("\n")}"
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
  def npm_publish!(tarballs)
    published = []
    with_npm_userconfig do |userconfig|
      tarballs.each do |tarball|
        command = ['fnox', 'exec', '--', 'npm', 'publish', tarball.to_s, '--access', 'public',
                   '--userconfig', userconfig]
        puts "$ npm publish #{File.basename(tarball)} --access public"
        next published << tarball if system(*command, chdir: REPO.to_s)

        left = tarballs - published
        abort <<~STOPPED
          release: npm publish #{File.basename(tarball)} failed; stopping.
            published: #{published.empty? ? 'none' : published.map { |t| File.basename(t) }.join(', ')}
            left, to publish by hand in this order (fnox exec -- npm publish <tgz> --access public):
              #{left.map(&:to_s).join("\n      ")}
          Then push the commit and tag, and create the GitHub release.
        STOPPED
      end
    end
    published
  end

  # Commits `paths` as `subject` and tags the commit, annotated with `notes`.
  def commit_and_tag!(paths:, subject:, tag:, notes_file:)
    run('git', 'add', '--', *paths.map(&:to_s))
    run('git', 'commit', '--quiet', '-m', subject, '-m', TRAILER)
    run('git', 'tag', '--annotate', '--cleanup=verbatim', tag, '--file', notes_file.to_s)
  end

  # Pushes main and the tag, then creates the GitHub release with `assets`.
  def push_and_release!(tag:, title:, notes_file:, assets:)
    run('git', 'push', 'origin', 'main', tag)
    run('gh', 'release', 'create', tag, '--verify-tag', '--title', title, '--notes-file', notes_file.to_s,
        *assets.map(&:to_s))
  end
end
