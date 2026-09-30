# frozen_string_literal: true

module Changelog
  # What a changelog needs to know about a project that is released on its
  # own: where it lives, which Conventional Commit scopes name it, which
  # files are its public API, and where its ADRs are.
  #
  # The public API is read from `api_json`, the clang-generated description
  # the project keeps in step with its header (`mise run api`), when it has
  # one, and otherwise from `api_files` (globs under `dir`) with ast-grep,
  # whose C parser does not survive every macro the headers use.
  #
  # `api_ignore` names what changes at every release and so says nothing
  # about the API (the version macros).
  Component = Struct.new(:name, :dir, :scopes, :api_json, :api_files, :api_ignore, keyword_init: true) do
    # The tag prefix of the component's releases (Release.released_versions).
    def prefix = "#{dir}/"

    def tag(version) = "#{prefix}v#{version}"

    def changelog_path = "#{dir}/CHANGELOG.md"

    def adr_dir = "#{dir}/docs/adr"

    def title = "Changelog: #{dir}"
  end

  VERSION_MACROS = /_VERSION(_MAJOR|_MINOR|_PATCH)?\z/

  COMPONENTS = [
    Component.new(name: 'core', dir: 'tinyactors-core', scopes: %w[core],
                  api_json: 'include/tinyactors.api.json', api_ignore: VERSION_MACROS),
    Component.new(name: 'host', dir: 'tinyactors-host', scopes: %w[host kernel],
                  api_files: %w[include/tinyactors_host.h include/tinyactors_host/*.h], api_ignore: VERSION_MACROS),
    Component.new(name: 'napi', dir: 'tinyactors-napi', scopes: %w[napi],
                  api_files: %w[index.d.ts], api_ignore: nil),
    Component.new(name: 'scxml', dir: 'tinyactors-scxml', scopes: %w[scxml],
                  api_json: 'include/ta_scxml.api.json', api_ignore: VERSION_MACROS),
    Component.new(name: 'tinyxml', dir: 'tinyxml', scopes: %w[tinyxml txml],
                  api_json: 'include/txml.api.json', api_ignore: VERSION_MACROS)
  ].freeze

  module_function

  def component(name)
    COMPONENTS.find { |component| component.name == name || component.dir == name.to_s.chomp('/') } or
      raise Error, "unknown component #{name.inspect}; one of #{COMPONENTS.map(&:name).join(', ')}"
  end

  # Every scope that names a component: a commit that names one is
  # attributed by its scopes, and only a commit that names none by the files
  # it touches (Ledger).
  def component_scopes = COMPONENTS.flat_map(&:scopes)
end
