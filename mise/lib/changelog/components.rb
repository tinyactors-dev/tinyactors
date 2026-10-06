# frozen_string_literal: true

module Changelog
  # What a changelog needs to know about a project that is released on its
  # own: where it lives, which Conventional Commit scopes name it, which
  # files are its public API, and where its ADRs are.
  #
  # The public API is read from `api_json`, the clang-generated description
  # (or descriptions, one per library the component ships)
  # the project keeps in step with its header (`mise run api`), when it has
  # one, and otherwise from `api_files` (globs under `dir`) with ast-grep,
  # whose C parser does not survive every macro the headers use.
  #
  # `api_ignore` names what changes at every release and so says nothing
  # about the API (the version macros).
  #
  # `tag_prefix` is the prefix of its release tags when that is not its
  # directory's: a component that moved keeps its release line.
  Component = Struct.new(:name, :dir, :scopes, :api_json, :api_files, :api_ignore, :tag_prefix, keyword_init: true) do
    # The tag prefix of the component's releases (Release.released_versions).
    def prefix = tag_prefix || path_prefix

    # What the paths of its files begin with.
    def path_prefix = "#{dir}/"

    def tag(version) = "#{prefix}v#{version}"

    def changelog_path = "#{dir}/CHANGELOG.md"

    def adr_dir = "#{dir}/docs/adr"

    def title = "Changelog: #{dir}"
  end

  VERSION_MACROS = /_VERSION(_MAJOR|_MINOR|_PATCH)?\z/

  COMPONENTS = [
    Component.new(name: 'core', dir: 'tinyactors-core', scopes: %w[core],
                  api_json: %w[include/tinyactors.api.json include/tinyactors_runtime.api.json],
                  api_ignore: VERSION_MACROS),
    # The Node.js binding, @tinyactors/node. Its addon lived in
    # tinyactors-napi/ until the standalone runtime replaced it, and its
    # releases keep that line of tags.
    Component.new(name: 'napi', dir: 'tinyactors-core/bindings/node', scopes: %w[napi node],
                  api_files: %w[index.d.ts], api_ignore: nil, tag_prefix: 'tinyactors-napi/'),
    Component.new(name: 'otlp', dir: 'tinyactors-otlp', scopes: %w[otlp],
                  api_files: %w[include/tinyactors_otlp.h], api_ignore: VERSION_MACROS),
    Component.new(name: 'scxml', dir: 'tinyactors-scxml', scopes: %w[scxml],
                  api_json: 'include/ta_scxml.api.json', api_ignore: VERSION_MACROS),
    Component.new(name: 'tinyxml', dir: 'tinyxml', scopes: %w[tinyxml txml],
                  api_json: 'include/txml.api.json', api_ignore: VERSION_MACROS)
  ].freeze

  module_function

  def component(name)
    COMPONENTS.find do |component|
      component.name == name || component.dir == name.to_s.chomp('/') || component.prefix == name.to_s
    end or
      raise Error, "unknown component #{name.inspect}; one of #{COMPONENTS.map(&:name).join(', ')}"
  end

  # Every scope that names a component: a commit that names one is
  # attributed by its scopes, and only a commit that names none by the files
  # it touches (Ledger).
  def component_scopes = COMPONENTS.flat_map(&:scopes)
end
