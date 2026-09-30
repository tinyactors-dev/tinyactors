# frozen_string_literal: true

require 'fileutils'
require 'json'
require 'tmpdir'
require 'toolchain'

module Changelog
  # A component's public API at one revision: the declarations of its
  # public headers (C) or declaration file (TypeScript), found by ast-grep's
  # parsers, and the difference between two revisions.
  #
  # An entity is one top-level declaration: a function, a typedef, a struct
  # or enum with a body, a macro, or an `export`. Two revisions differ in an
  # entity when its declaration differs once comments and white space are
  # left out; a function's body does not count, only its signature.
  module APISurface
    Entity = Struct.new(:kind, :name, :file, :text, :lines, keyword_init: true) do
      def key = "#{kind} #{name}"
    end

    C_RULE = <<~YAML
      id: c-entities
      language: c
      rule:
        any:
          - kind: preproc_def
          - kind: preproc_function_def
          - kind: type_definition
          - kind: function_definition
          - kind: declaration
            has: {kind: function_declarator, stopBy: end}
          - kind: struct_specifier
            has: {field: body, kind: field_declaration_list}
          - kind: enum_specifier
            has: {field: body, kind: enumerator_list}
    YAML

    TS_RULE = <<~YAML
      id: ts-entities
      language: typescript
      rule:
        kind: export_statement
        inside: {kind: program}
    YAML

    Delta = Struct.new(:added, :removed, :changed, keyword_init: true) do
      def empty? = added.empty? && removed.empty? && changed.empty?

      # Names of what a caller must change: removed and changed entities.
      def breaking_names = (removed + changed.map(&:first)).map(&:name).uniq
    end

    module_function

    # key => Entity, for the component's public API at `rev`: from the
    # clang-generated description (include/*.api.json, which `mise run api`
    # keeps current) when the component has one, else from its headers or
    # declaration file.
    def surface(repo, component, rev)
      return json_surface(repo, component, rev) if component.api_json

      paths = api_paths(repo, component, rev)
      return {} if paths.empty?

      Dir.mktmpdir('changelog-api') do |tmp|
        paths.each do |path|
          target = File.join(tmp, path)
          FileUtils.mkdir_p(File.dirname(target))
          File.write(target, repo.show(rev, path))
        end
        entities = paths.group_by { |path| rule_for(path) }.flat_map do |rule, group|
          scan(rule, group.map { |path| File.join(tmp, path) }, tmp)
        end
        entities.reject { |entity| ignored?(component, entity) }.to_h { |entity| [entity.key, entity] }
      end
    end

    # The description lists functions, types (struct, enum, alias, handle,
    # view), constants and flag sets. An entity's lines are every leaf of its
    # entry, docs left out, so a changed field type, enum value, parameter or
    # layout (size, alignment, offset) shows as the lines it changed.
    def json_surface(repo, component, rev)
      text = repo.show(rev, "#{component.dir}/#{component.api_json}")
      return {} unless text

      api = JSON.parse(text)
      entities = []
      api.fetch('functions', []).each { |entry| entities << json_entity('function', entry.fetch('name'), entry, component) }
      api.fetch('types', {}).each { |name, entry| entities << json_entity(entry.fetch('kind', 'type'), name, entry, component) }
      api.fetch('constants', []).each { |entry| entities << json_entity('constant', entry.fetch('name'), entry, component) }
      api.fetch('flag_sets', {}).each { |name, entry| entities << json_entity('flag set', name, entry, component) }
      entities.compact.to_h { |entity| [entity.key, entity] }
    end

    def json_entity(kind, name, entry, component)
      return if component.api_ignore && name.match?(component.api_ignore)

      lines = flatten(entry, named: true)
      Entity.new(kind: kind, name: name, file: component.api_json, text: lines.join(' | '), lines: lines)
    end

    JSON_SKIPPED = %w[doc implemented].freeze

    # "fields[kind].type.builtin = uint32_t": every leaf of a JSON value with
    # its path; list items that have a name are addressed by it.
    def flatten(value, path = '', named: false)
      case value
      when Hash
        value.flat_map do |key, inner|
          next [] if JSON_SKIPPED.include?(key) || (named && key == 'name')

          flatten(inner, path.empty? ? key : "#{path}.#{key}")
        end
      when Array
        value.each_with_index.flat_map do |inner, index|
          label = inner.is_a?(Hash) ? inner['name'] : nil
          flatten(inner, "#{path}[#{label || index}]", named: !label.nil?)
        end
      else
        ["#{path} = #{value}"]
      end
    end

    def delta(old_surface, new_surface)
      added = (new_surface.keys - old_surface.keys).map { |key| new_surface[key] }
      removed = (old_surface.keys - new_surface.keys).map { |key| old_surface[key] }
      changed = (old_surface.keys & new_surface.keys).filter_map do |key|
        [new_surface[key], old_surface[key]] if new_surface[key].text != old_surface[key].text
      end
      order = ->(entity) { [entity.file, entity.kind, entity.name] }
      Delta.new(added: added.sort_by(&order), removed: removed.sort_by(&order),
                changed: changed.sort_by { |(entity, _old)| order.call(entity) })
    end

    # The lines an entity gained and lost, as "+ line" and "- line".
    def line_diff(new_entity, old_entity)
      gone = old_entity.lines - new_entity.lines
      gained = new_entity.lines - old_entity.lines
      gone.map { |line| "- #{line}" } + gained.map { |line| "+ #{line}" }
    end

    def api_paths(repo, component, rev)
      present = repo.files(rev, component.dir)
      component.api_files.flat_map do |pattern|
        full = "#{component.dir}/#{pattern}"
        present.select { |path| File.fnmatch?(full, path, File::FNM_PATHNAME) }
      end.uniq.sort
    end

    def rule_for(path) = path.end_with?('.h') ? C_RULE : TS_RULE

    def scan(rule, files, tmp)
      out = Toolchain::Tool[:ast_grep].run('scan', '--inline-rules', rule, '--json=compact', *files)
      matches = JSON.parse(out).map do |match|
        [match.fetch('file'), match.dig('range', 'byteOffset', 'start'), match.dig('range', 'byteOffset', 'end'), match.fetch('text')]
      end
      matches = drop_nested(matches)
      matches.filter_map do |file, _from, _to, text|
        relative = file.delete_prefix("#{tmp}/")
        entity(relative, text)
      end
    end

    # A struct written inside a typedef is part of the typedef.
    def drop_nested(matches)
      matches.reject do |file, from, to, _text|
        matches.any? do |other_file, other_from, other_to, _other|
          other_file == file && (other_from <= from && to <= other_to) && (other_from != from || other_to != to)
        end
      end
    end

    def entity(file, text)
      lines = strip_comments(text).lines.map(&:strip).reject(&:empty?)
      collapsed = lines.join(' ')
      kind, name = classify(file, collapsed)
      return unless name

      shown = kind == 'function' && collapsed.include?('{') ? signature_lines(lines) : lines
      Entity.new(kind: kind, name: name, file: file, text: shown.join(' '), lines: shown)
    end

    # A function definition's signature: the lines before its body.
    def signature_lines(lines)
      cut = lines.index { |line| line.include?('{') }
      head = lines.first(cut + 1)
      head[-1] = head[-1].sub(/\{.*\z/, '').strip
      head.reject(&:empty?)
    end

    def strip_comments(text)
      text.gsub(%r{/\*.*?\*/}m, '').gsub(%r{(^|\s)//[^\n]*}, '\1')
    end

    def classify(file, text)
      return ['export', export_name(text)] if file.end_with?('.ts')

      case text
      when /\A#\s*define\s+(\w+)/ then ['macro', Regexp.last_match(1)]
      when /\Atypedef\b/ then ['type', typedef_name(text)]
      when /\A(struct|enum|union)\s+(\w+)/ then [Regexp.last_match(1), Regexp.last_match(2)]
      else
        name = text[/\b([a-z_][a-z0-9_]*)\s*\(/, 1]
        ['function', name]
      end
    end

    def typedef_name(text)
      text[/\(\s*\*\s*(\w+)\s*\)/, 1] ||
        text.sub(/;\s*\z/, '').sub(/(\s+[A-Z_][A-Z0-9_]*(\([^)]*\))?)+\z/, '')[/(\w+)\s*\z/, 1]
    end

    def export_name(text)
      words = /\Aexport\s+(?:declare\s+)?(?:abstract\s+)?(?:async\s+)?(?:const\s+enum|const|let|var|function\*?|class|interface|type|enum|namespace)\s+(\w+)/
      text[words, 1] || text[0, 48]
    end

    def ignored?(component, entity)
      component.api_ignore && entity.name.match?(component.api_ignore)
    end
  end
end
