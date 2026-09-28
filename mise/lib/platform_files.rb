# frozen_string_literal: true

require 'pathname'

# Platform-specific source files, chosen by file name as in Go
# (docs/adr/0001-packaging-and-releases.md, decision 9). A source whose name
# ends in `_<suffix>.c` or `_<suffix>.h`, with `<suffix>` in SUFFIXES, is
# built only for that platform: `src/txml_allocator_wasm.c` only for wasm.
# Every other file is portable and built for every target. A suffix not in
# the list is an ordinary part of a name.
#
# Every task that builds from `src/` selects its files through `select` or
# `sources`, given the target, instead of globbing on its own.
module PlatformFiles
  # The workspace's platform suffixes. Add one (`linux`, `macos`,
  # `aarch64`, or a pair such as `linux_aarch64`) when the first file needs
  # it.
  SUFFIXES = %w[wasm].freeze

  module_function

  # The platform suffix of `path`, or nil for a portable file.
  def platform(path)
    stem = File.basename(path.to_s, '.*')
    SUFFIXES.select { |suffix| stem.end_with?("_#{suffix}") }.max_by(&:length)
  end

  def portable?(path) = platform(path).nil?

  # The suffixes a target builds: of its OS, its architecture and the pair
  # (Toolchain::Target#platform_suffixes), those in SUFFIXES. nil is the
  # portable build, which takes no platform file.
  def suffixes_for(target)
    return [] if target.nil?

    target.platform_suffixes & SUFFIXES
  end

  # The files of `paths` to build for `target`: the portable ones, and the
  # target's platform files.
  def select(paths, target: nil)
    wanted = suffixes_for(target)
    paths.select do |path|
      suffix = platform(path)
      suffix.nil? || wanted.include?(suffix)
    end
  end

  # The C sources in `dir` to build for `target`, sorted.
  def sources(dir, target: nil)
    select(Dir[Pathname.new(dir).join('*.c').to_s].sort, target: target)
  end

  # The headers in `dir` to build for `target`, sorted.
  def headers(dir, target: nil)
    select(Dir[Pathname.new(dir).join('*.h').to_s].sort, target: target)
  end
end
