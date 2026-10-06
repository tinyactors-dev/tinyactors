# frozen_string_literal: true

# What the tasks that package and release @tinyactors/node share
# (docs/adr/0001-packaging-and-releases.md, decisions 10, 11 and 15): the
# platform packages, where the release addons and the tarballs go, and the
# version. The npm packages are
#
# - @tinyactors/node: the binding's JavaScript, bindings/node/ and
#   bindings/shared/ under node/ and shared/ as in the repository, so the
#   relative imports between them are the same, and optionalDependencies on
#   every platform package at exactly its own version;
# - @tinyactors/node-<platform>-<arch>: one addon, tinyactors.node, built by
#   `mise run node:build --target <target>`, with `os` and `cpu` and no
#   `libc` field, since the Linux addon serves glibc and musl.
#
# Their version is bindings/node/package.json's. Its tags continue the
# release line of the addon that lived in tinyactors-napi/ before:
# `tinyactors-napi/v<version>` (Changelog component `napi`).
require 'json'
require 'pathname'
require_relative 'core'

module NodePackage
  BINDING = Core::ROOT.join('bindings/node')
  SHARED = Core::ROOT.join('bindings/shared')
  PACKAGE_JSON = BINDING.join('package.json')
  LICENSE = Core::ROOT.join('../LICENSE').expand_path
  SMOKE = BINDING.join('test/smoke')
  NPM = Core::BUILD.join('node/npm')
  TARBALLS = NPM.join('tarballs')
  RELEASE = NPM.join('release')
  TAG_PREFIX = 'tinyactors-napi/'
  NAME = '@tinyactors/node'

  # Release target => [npm os, npm cpu].
  PLATFORMS = {
    'aarch64-macos' => %w[darwin arm64],
    'x86_64-macos' => %w[darwin x64],
    'x86_64-linux' => %w[linux x64],
    'aarch64-linux' => %w[linux arm64]
  }.freeze

  # The files of the main package, relative to bindings/: what index.js and
  # index.d.ts reach.
  FILES = %w[node/index.js node/index.d.ts node/transports.js node/ws.js shared/core.mjs shared/ecmascript.mjs].freeze

  module_function

  def targets = PLATFORMS.keys

  def addon(target) = Core::BUILD.join('node', target, 'tinyactors.node')

  def platform_name(target) = "#{NAME}-#{PLATFORMS.fetch(target).join('-')}"

  def names = [NAME, *targets.map { |target| platform_name(target) }]

  def manifest = JSON.parse(PACKAGE_JSON.read)

  def version = manifest.fetch('version')

  # npm pack's file name for `name` at `version`.
  def tarball(name, version) = TARBALLS.join("#{name.delete_prefix('@').tr('/', '-')}-#{version}.tgz")

  # The tarballs in publishing order: every platform package before the main
  # package that depends on them.
  def tarballs(version) = [*targets.map { |target| tarball(platform_name(target), version) }, tarball(NAME, version)]

  # The release target of this machine.
  def host_target = Toolchain::Host.current.target.name

  # Reads `--name <value>` the way mise passes usage flags (usage_<name>),
  # or from ARGV when the script runs on its own.
  def flag(name)
    value = ENV["usage_#{name.tr('-', '_')}"].to_s
    return value unless value.empty?

    index = ARGV.index("--#{name}")
    index ? ARGV[index + 1].to_s : ''
  end

  def switch?(name) = ENV["usage_#{name.tr('-', '_')}"] == 'true' || ARGV.include?("--#{name}")
end
