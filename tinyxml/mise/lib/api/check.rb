# frozen_string_literal: true

module Api
  # Proves the description against the header: it renders every type,
  # field, constant, and function back into C declarations and static
  # assertions and compiles them with the real header for each target. A
  # wrong type, a missing field, a stale layout, or a wrong value fails to
  # compile. The check reads only the finished description.
  module Check
    module_function

    def run(header, model)
      failures = Clang::TARGETS.filter_map do |target|
        errors = Clang.check(header, source(model, target), target: target)
        "#{target}:\n#{errors}" unless errors.empty?
      end
      raise Error, "the description does not match #{header}:\n#{failures.join("\n")}" unless failures.empty?
    end

    def source(model, target)
      lines = ["#include \"#{File.basename(model['header'])}\"", '#include <stddef.h>', '']
      assert = ->(condition, label) { lines << "_Static_assert(#{condition}, #{label.to_json});" }
      model['types'].each do |name, entry|
        case entry['kind']
        when 'alias' then lines << "typedef #{CType.render(entry['type'], name)};"
        when 'enum'
          lines << "typedef #{CType.render(entry['underlying'], name)};" if entry['underlying']
          entry['constants'].each { |constant| assert.call("#{constant['name']} == #{literal(constant['value'])}", constant['name']) }
        when 'struct', 'union'
          fields(name, "((#{name} *)0)->", entry['fields'], assert)
          members(model, name, entry, lines)
        end
        next unless (layout = entry.dig('layout', target))

        assert.call("sizeof(#{name}) == #{layout['size']}", "sizeof #{name}")
        assert.call("_Alignof(#{name}) == #{layout['align']}", "alignment of #{name}")
        (layout['offsets'] || {}).each do |path, offset|
          assert.call("offsetof(#{name}, #{path}) == #{offset}", "offset of #{name}.#{path}")
        end
      end
      model['flag_sets'].each_value do |set|
        set['constants'].each { |constant| assert.call("#{constant['name']} == #{literal(constant['value'])}", constant['name']) }
      end
      model['constants'].each do |constant|
        type = CType.render(constant['type'])
        assert.call("(#{constant['name']}) == #{literal(constant['value'])} && _Generic((#{constant['name']}), #{type}: 1, default: 0)",
                    constant['name'])
      end
      model['functions'].each do |function|
        params = function['params'].map { |param| CType.render(param['type'], param['name']) }
        params = ['void'] if params.empty?
        declarator = "#{function['name']}(#{params.join(', ')})"
        lines << "#{function['static_inline'] ? 'static inline ' : ''}#{CType.render(function['returns']['type'], declarator)};"
      end
      "#{lines.join("\n")}\n"
    end

    # Each field has exactly its described type: a pointer to it matches only
    # a pointer to that type.
    def fields(owner, access, fields, assert)
      fields.each do |field|
        if (record = field.dig('type', 'record'))
          fields(owner, "#{access}#{field['name']}.", record['fields'], assert)
        else
          pointer = CType.render({ 'pointer' => field['type'] })
          assert.call("_Generic(&#{access}#{field['name']}, #{pointer}: 1, default: 0)",
                      "type of #{owner}.#{access.split('->', 2).last}#{field['name']}")
        end
      end
    end

    # Every member is described, in order: a positional initializer with one
    # value per described field fails under -Wmissing-field-initializers when
    # a field is missing and with "excess elements" when one is invented.
    # Structs nested inside records are reached with a designator.
    def members(model, name, entry, lines)
      probe = ->(designator, record) do
        values = record['fields'].map { |field| zero(model, field['type']) }
        init = "{#{values.join(', ')}}"
        init = "{#{designator} = #{init}}" if designator
        lines << "const #{name} txml_members_#{lines.size} = #{init};"
      end
      probe.call(nil, entry) if entry['kind'] == 'struct'
      walk = lambda do |prefix, record|
        record['fields'].each do |field|
          nested = field.dig('type', 'record') or next
          designator = "#{prefix}.#{field['name']}"
          probe.call(designator, nested) if nested['kind'] == 'struct'
          walk.call(designator, nested)
        end
      end
      walk.call('', entry)
    end

    def zero(model, type)
      type = model.dig('types', type['type'], 'type') while type['type'] && model.dig('types', type['type'], 'kind') == 'alias'
      return '{0}' if type.key?('record') || %w[struct union].include?(model.dig('types', type['type'].to_s, 'kind'))

      # Not a bare 0: clang exempts the universal `{0}` from the missing-field warning.
      type.key?('pointer') ? '(void *)0' : '(0 + 0)'
    end

    def literal(value)
      value.negative? || value <= 2_147_483_647 ? value.to_s : "#{value}ull"
    end
  end
end
