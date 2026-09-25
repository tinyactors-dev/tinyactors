# frozen_string_literal: true

module API
  # Sizes, alignments, and field offsets for each pinned target, computed by
  # clang itself (see Clang.evaluate), plus the values and types of the
  # header's integer macros.
  module Layout
    module_function

    # {target => {"pointer" =>, "builtins" =>}} and a layout for every sized
    # type, stored on the type as {"layout" => {target => {...}}}.
    def apply(header, model)
      targets = {}
      Clang::TARGETS.each do |target|
        expressions = probes(model)
        values = Clang.evaluate(header, expressions, target: target)
        targets[target] = {
          'pointer' => measure(values, [:pointer]),
          'builtins' => builtins.to_h { |name| [name, measure(values, [:builtin, name])] }
        }
        model['types'].each do |name, entry|
          next unless sized?(entry)

          layout = measure(values, [:type, name])
          offsets = offset_paths(entry).to_h { |path| [path, values.fetch([:offset, name, path])[:value]] }
          layout['offsets'] = offsets unless offsets.empty?
          (entry['layout'] ||= {})[target] = layout
        end
      end
      targets
    end

    # [{name, type, value}] for every object-like TXML_ macro that is a
    # constant (annotation macros, TXML_API and compound-literal defaults are not).
    def constants(header, macros, known:)
      names = macros.select do |name, body|
        name.start_with?('TXML_') && !%w[TXML_API TXML_API_EXTRACT].include?(name) &&
          !body.include?('TXML_ANNOTATION') && !body.include?('__attribute__') &&
          !body.include?('{') # a compound literal such as TXML_LIMITS_DEFAULT is a value, not a constant
      end.keys.sort
      return [] if names.empty?

      per_target = Clang::TARGETS.map do |target|
        Clang.evaluate(header, names.to_h { |name| [name, name] }, target: target)
      end
      names.map do |name|
        values = per_target.map { |values_of| values_of.fetch(name)[:value] }.uniq
        raise Error, "#{name} differs between targets: #{values.join(', ')}" unless values.size == 1

        spelling = per_target.first.fetch(name)[:type]
        type = CType.parse_type(spelling, known: known)
        { 'name' => name, 'type' => type, 'value' => values.first }
      end
    end

    def builtins = CType::BUILTINS - ['void']

    def sized?(entry)
      %w[struct union enum alias].include?(entry['kind'])
    end

    def probes(model)
      expressions = {
        %i[pointer size] => 'sizeof(void *)', %i[pointer align] => '_Alignof(void *)'
      }
      builtins.each do |name|
        expressions[[:builtin, name, :size]] = "sizeof(#{name})"
        expressions[[:builtin, name, :align]] = "_Alignof(#{name})"
      end
      model['types'].each do |name, entry|
        next unless sized?(entry)

        expressions[[:type, name, :size]] = "sizeof(#{name})"
        expressions[[:type, name, :align]] = "_Alignof(#{name})"
        offset_paths(entry).each do |path|
          expressions[[:offset, name, path]] = "__builtin_offsetof(#{name}, #{path})"
        end
      end
      expressions
    end

    def measure(values, key)
      { 'size' => values.fetch([*key, :size])[:value], 'align' => values.fetch([*key, :align])[:value] }
    end

    # Dotted member paths of a struct or union, including the members of its
    # anonymous structs and unions.
    def offset_paths(entry, prefix = nil)
      (entry['fields'] || []).flat_map do |field|
        path = [prefix, field['name']].compact.join('.')
        record = field.dig('type', 'record')
        [path, *(record ? offset_paths(record, path) : [])]
      end
    end
  end
end
