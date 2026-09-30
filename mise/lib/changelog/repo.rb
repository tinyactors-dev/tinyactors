# frozen_string_literal: true

require 'open3'
require 'pathname'
require 'release'

module Changelog
  # A git repository, and the releases in it.
  class Repo
    attr_reader :root

    def initialize(root = Release::REPO)
      @root = Pathname.new(root)
    end

    # stdout of `git` in the repository; raises when git fails.
    def git(*args)
      out, err, status = Open3.capture3('git', *args, chdir: @root.to_s)
      raise Error, "git #{args.join(' ')} failed: #{err.strip}" unless status.success?

      out
    end

    def head = git('rev-parse', 'HEAD').strip

    # The content of `path` at `rev`, or nil when it does not exist there.
    def show(rev, path)
      out, _err, status = Open3.capture3('git', 'show', "#{rev}:#{path}", chdir: @root.to_s)
      status.success? ? out : nil
    end

    # The files at `rev` whose path is under `dir`, relative to the repository.
    def files(rev, dir) = git('ls-tree', '-r', '--name-only', rev, '--', "#{dir}/").lines.map(&:strip)

    # The version of the latest release of `component` ("0.1.0"), or nil
    # before the first.
    def last_version(component)
      tags = git('tag', '--list', "#{component.prefix}v*").lines.map { |tag| tag.strip.delete_prefix("#{component.prefix}v") }
      tags.select { |version| Release::VERSION.match?(version) }
          .max { |a, b| Release.compare_versions(a, b) }
    end

    def tag_commit(tag) = git('rev-list', '-n1', tag).strip
  end
end
