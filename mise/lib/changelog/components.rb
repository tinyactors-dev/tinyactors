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
  Component = Struct.new(:name, :dir, :scopes, :api_json, :api_files, :api_ignore, keyword_init: true) do
    # What the paths of its files and its release tags begin with
    # (`tinyactors-napi/` and `tinyactors-napi/v0.2.0`).
    def prefix = "#{dir}/"

    def tag(version) = "#{prefix}v#{version}"

    def changelog_path = "#{dir}/CHANGELOG.md"

    def adr_dir = "#{dir}/docs/adr"

    def title = "Changelog: #{dir}"
  end

  VERSION_MACROS = /_VERSION(_MAJOR|_MINOR|_PATCH)?\z/

  COMPONENTS = [
    Component.new(name: 'core', dir: 'tinyactors-core', scopes: %w[core],
                  api_json: %w[include/tinyactors.api.json include/tinyactors_runtime.api.json
                             include/tinyactors_datamodel_ecmascript.api.json],
                  api_ignore: VERSION_MACROS),
    # The Node.js binding, @tinyactors/node: the Node-API addon of ADR 0009
    # step 8, back in tinyactors-napi/ (from tinyactors-core/bindings/node,
    # deleted in its step 2), continuing the package's tags and versions.
    Component.new(name: 'napi', dir: 'tinyactors-napi', scopes: %w[napi node],
                  api_files: %w[index.d.ts], api_ignore: nil),
    # The Python extension, `tinyactors`. Its API is defined in C and has no
    # declaration file yet, so no API delta is read for it until one is
    # named here in `api_files`.
    Component.new(name: 'cpython', dir: 'tinyactors-cpython', scopes: %w[cpython python],
                  api_files: [], api_ignore: nil),
    Component.new(name: 'staticjs', dir: 'staticjs', scopes: %w[staticjs],
                  api_json: 'include/staticjs.api.json', api_ignore: VERSION_MACROS),
    # staticjs for JavaScript, @tinyactors/staticjs (docs/adr/0013): its
    # API is its declaration file.
    Component.new(name: 'staticjs-js', dir: 'staticjs-js', scopes: %w[staticjs-js],
                  api_files: %w[index.d.ts], api_ignore: nil),
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
