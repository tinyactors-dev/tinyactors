# frozen_string_literal: true

module Rpc
  # Every decision the RPC bindings make, taken from the API description and
  # the policy before any C is written: which JSON member each field becomes,
  # with which codec, whether it is required, how union members are laid out,
  # which outputs a method reports after which result, and what it releases.
  # The emitter only formats this plan. Anything the plan cannot map is an
  # error that names the item, never a silent omission.
  class Plan
    # The runtime (src/binding.c) has codecs for these types.
    RUNTIME_TYPES = { 'string' => %w[ta_str], 'bytes' => %w[ta_bytes], 'host_value' => %w[ta_value] }.freeze
    RUNTIME_OVERRIDES = %w[ta_expression ta_actor_ref].freeze
    PRIMITIVES = { 'uint8_t' => 'u8', 'uint16_t' => 'u16', 'uint32_t' => 'u32', 'int32_t' => 'i32', 'int' => 'i32',
                   'uint64_t' => 'u64', 'size_t' => 'size', 'bool' => 'bool' }.freeze
    PRIMITIVE_TYPES = { 'u8' => 'uint8_t', 'u16' => 'uint16_t', 'u32' => 'uint32_t', 'i32' => 'int32_t', 'u64' => 'uint64_t',
                        'size' => 'size_t', 'bool' => 'bool' }.freeze
    VALUE_MODELS = {
      'actor' => 'TAT_VALUE_MODEL_ACTOR',
      'definition' => 'TAT_VALUE_MODEL_DEFINITION',
      'session_image' => 'TAT_VALUE_MODEL_SESSION_IMAGE'
    }.freeze

    # How a value is converted: kind is :primitive, :enum, :struct, :runtime,
    # or :adapter; name is the codec name; ctype the C type it converts.
    Codec = Struct.new(:kind, :name, :ctype) do
      # C expression decoding jv `value` into `*target`; true on success.
      def decode(value, target)
        case kind
        when :primitive then "tat_decode_#{name}(b, #{value}, (#{ctype} *)#{target})"
        when :adapter then "tat_adapter_decode(b, #{value}, #{name.to_json}, #{target}, sizeof(#{ctype}))"
        else "tat_decode_#{name}(b, #{value}, #{target})"
        end
      end

      # C expression encoding `*source` as a jv.
      def encode(source)
        case kind
        when :primitive then "tat_encode_#{name}(b, (const #{ctype} *)#{source})"
        when :adapter then "tat_adapter_encode(b, #{name.to_json}, #{source}, sizeof(#{ctype}))"
        else "tat_encode_#{name}(b, #{source})"
        end
      end
    end

    # One JSON member. form: :value, :pointer (one element), :handle, :view,
    # :list (terminated), :array (counted).
    Field = Struct.new(:key, :access, :form, :codec, :ctype, :required, :handle, :list, :count, :schema, :zero_omittable,
                       keyword_init: true)

    # A list helper: terminated lists end at `terminator`, a C condition on
    # `item`; counted arrays carry their count type.
    List = Struct.new(:key, :element, :codec, :terminator, :count_ctype, keyword_init: true)

    Struct_ = Struct.new(:name, :fields, :arenas, :zero_is_absent, :tagged, keyword_init: true)
    Tagged = Struct.new(:tag, :enum, :kinds, :inferable, :kind_required, keyword_init: true)
    Kind = Struct.new(:constant, :layout, :member, :access, :fields, :named, :keyed, :keys, keyword_init: true)

    # kind: :function (generated), :external (hand-written handler), or
    # :roundtrip (codec round trip of a struct).
    Method_ = Struct.new(:kind, :name, :handler, :function, :implemented, :storage, :call_args, :fields, :keys, :flattened,
                         :outputs, :result, :retire, :schema, keyword_init: true)
    Output = Struct.new(:key, :storage, :setup, :encode, :valid, :releases, :schema, keyword_init: true)

    attr_reader :enums, :structs, :lists, :arrays, :methods, :receivers, :report, :max_buffer_elements

    def initialize(api, policy)
      @api = api
      @policy = policy
      @errors = []
      @enums = []
      @structs = {}
      @lists = {}
      @arrays = {}
      @methods = []
      @receivers = []
      @report = Hash.new { |hash, key| hash[key] = [] }
      @max_buffer_elements = policy.max_buffer_elements
      plan_types
      plan_roundtrips
      plan_functions
      begin
        policy.check_used!
      rescue Error => e
        @errors << e.message
      end
      raise Error, @errors.uniq.join("\n") unless @errors.empty?
    end

    private

    def error(path, message)
      @errors << "#{path}: #{message}"
      nil
    end

    # ------------------------------------------------------------------
    # Types

    def plan_types
      @api.types.each do |name, entry|
        case entry['kind']
        when 'alias' then @report[:types_alias] << name
        when 'handle', 'view' then @report[:types_reference] << name
        when 'enum'
          if (reason = @policy.skip_type(name)) then @report[:types_skipped] << [name, reason]
          else
            @enums << name
            @report[:types_bound] << name
          end
        when 'struct', 'union' then classify_record(name, entry)
        else error(name, "unknown type kind #{entry['kind']}")
        end
      end
      @api.types.each do |name, entry|
        next unless %w[struct union].include?(entry['kind']) && generated?(name)

        struct_plan(name)
      end
    end

    # codec.roundtrip.<type> decodes a value and encodes it again.
    def plan_roundtrips
      @structs.each_key do |name|
        @methods << Method_.new(kind: :roundtrip, name: "codec.roundtrip.#{name}", handler: "tat_roundtrip_#{name}",
                                schema: { 'codec' => name, 'params' => { 'value' => { 'type' => name, 'required' => true } } })
      end
    end

    def classify_record(name, entry)
      if (reason = @policy.skip_type(name))
        @report[:types_skipped] << [name, reason]
      elsif (reason = @policy.override_type(name))
        error(name, 'the runtime has no hand-written codec for this type') unless RUNTIME_OVERRIDES.include?(name)
        @report[:types_overridden] << [name, reason]
      elsif (types = RUNTIME_TYPES[entry['semantic']])
        error(name, "the runtime has no #{entry['semantic']} codec for this type") unless types.include?(name)
        @report[:types_runtime] << name
      elsif entry['semantic'] == 'adapter' then @report[:types_adapter] << name
      elsif entry['semantic'] == 'vtable'
        error(name, 'adapter operations cannot cross the protocol; skip the vtable in bindings.rb')
      elsif entry['kind'] == 'union'
        error(name, 'a bare union cannot be bound; skip it in bindings.rb')
      else
        @report[:types_bound] << name
      end
    end

    def generated?(name)
      entry = @api.type(name)
      %w[struct union].include?(entry['kind']) && !@policy.skip_type(name) && !@policy.override_type(name) &&
        entry['semantic'].nil? && entry['kind'] == 'struct'
    end

    def struct_plan(name)
      return @structs[name] if @structs.key?(name)

      @structs[name] = nil # breaks cycles; lists refer to their element's codec by name only
      entry = @api.type(name)
      tagged = entry['tagged']
      common = entry['fields'].reject { |slot| tagged && [tagged['tag'], tagged['union']].include?(slot['name']) }
      fields = input_fields(name, common, '')
      plan = Struct_.new(name: name, fields: fields, zero_is_absent: entry['zero_is_absent'] == true,
                         arenas: common.select { |slot| slot['shape'] == 'arena' }.map { |slot| slot['name'] })
      plan.tagged = tagged_plan(name, entry, plan) if tagged
      @structs[name] = plan
    end

    def tagged_plan(name, entry, plan)
      tagged = entry['tagged']
      union = entry['fields'].find { |slot| slot['name'] == tagged['union'] }['type']['record']['fields']
      common_keys = plan.fields.map(&:key) + [tagged['tag']]
      error(name, 'a tagged union that is absent when zero cannot carry other members') if plan.zero_is_absent && plan.fields.any?
      kinds = @api.type(tagged['enum'])['constants'].map do |constant|
        member = tagged['members'][constant['name']]
        next Kind.new(constant: constant['name'], layout: :none, keys: common_keys) unless member

        slot = union.find { |each| each['name'] == member }
        path = "#{name}.#{tagged['union']}.#{member}"
        access = "#{tagged['union']}.#{member}"
        kind = if @policy.flatten_member?(name, member)
                 flattened_member(path, slot, access)
               else
                 field = input_field(path, slot, member, access)
                 field.required = !(field.form != :value && optional?(slot)) if field
                 Kind.new(layout: :keyed, keyed: field, keys: [member])
               end
        next unless kind

        kind.constant = constant['name']
        kind.member = member
        kind.access = access
        collisions = kind.keys & common_keys
        error(path, "members #{collisions.join(', ')} collide with #{name}'s own members; do not flatten it") if collisions.any?
        kind.keys = common_keys + kind.keys
        kind
      end.compact
      keyed = kinds.select { |kind| kind.layout == :keyed }
      Tagged.new(tag: tagged['tag'], enum: tagged['enum'], kinds: kinds,
                 inferable: keyed.map { |kind| [kind.keyed.key, kind.constant] },
                 kind_required: !plan.zero_is_absent)
    end

    def flattened_member(path, slot, access)
      if (record = slot['type']['record'])
        return error(path, 'only struct members can be flattened') unless record['kind'] == 'struct'

        fields = input_fields(path, record['fields'], "#{access}.")
        Kind.new(layout: :inline, fields: fields, keys: fields.map(&:key))
      elsif (name = slot['type']['type']) && generated?(name) && !@api.type(name)['tagged']
        nested = struct_plan(name)
        Kind.new(layout: :named, named: name, keys: nested.fields.map(&:key))
      else
        error(path, 'only plain struct members can be flattened')
      end
    end

    # JSON members of a struct: inputs only. Outputs are reported by methods;
    # counts are implied by their lists; arenas are supplied by the binding.
    def input_fields(owner, slots, prefix)
      slots.filter_map do |slot|
        next if slot['direction'] == 'out' || slot['count_of'] || slot['shape'] == 'arena'

        path = "#{owner}.#{slot['name']}"
        next error(path, 'an anonymous record can only be a tagged union') if slot['type'].key?('record')

        field = input_field(path, slot, slot['name'], "#{prefix}#{slot['name']}", siblings: slots, prefix: prefix)
        field&.required = required?(slot, field)
        field
      end
    end

    def optional?(slot) = slot['optional'] == true
    def required?(slot, field) = field.form != :value && field.form != :view && !optional?(slot)

    def input_field(path, slot, key, access, siblings: nil, prefix: '')
      type = slot['type']
      unless @api.pointer?(type)
        codec = value_codec(path, type) or return nil
        return Field.new(key: key, access: access, form: :value, codec: codec, ctype: @api.c_type(type),
                         schema: schema_label(type, codec), zero_omittable: %i[primitive enum].include?(codec.kind))
      end

      target = @api.resolve(type)['pointer']
      case slot['shape']
      when 'single'
        return error(path, "#{slot['direction']} pointers cannot cross the protocol; skip its type or function in bindings.rb") unless slot['direction'] == 'in'

        codec = value_codec(path, target) or return nil
        Field.new(key: key, access: access, form: :pointer, codec: codec, ctype: @api.c_type(target),
                  schema: schema_label(target, codec))
      when 'handle'
        handle = @api.type(target['type'])['handle']
        Field.new(key: key, access: access, form: :handle, handle: handle, schema: "handle:#{handle}")
      when 'view'
        Field.new(key: key, access: access, form: :view, handle: target['type'].delete_prefix('ta_'),
                  schema: "borrowed:#{target['type']}")
      when 'terminated'
        list = list_helper(path, target, terminator: slot['terminator']) or return nil
        Field.new(key: key, access: access, form: :list, list: list, schema: "list<#{list.element}>")
      when 'counted'
        count = siblings&.find { |each| each['name'] == slot['count'] } or return error(path, 'count member not found')
        list = array_helper(path, target, count['type']) or return nil
        Field.new(key: key, access: access, form: :array, list: list, count: "#{prefix}#{slot['count']}",
                  schema: "list<#{list.element}>")
      else
        error(path, "#{slot['shape']} pointers cannot cross the protocol; skip its type or function in bindings.rb")
      end
    end

    # The codec for a value of `type` (not a pointer).
    def value_codec(path, type)
      resolved = @api.resolve(type)
      if (builtin = resolved['builtin'])
        name = PRIMITIVES[builtin] or return error(path, "no JSON codec for #{builtin}")
        return Codec.new(:primitive, name, PRIMITIVE_TYPES.fetch(name))
      end
      name = resolved['type'] or return error(path, "no JSON codec for #{resolved.inspect}")
      entry = @api.type(name)
      return error(path, "#{name} is skipped in bindings.rb, so #{path} cannot be bound") if @policy.skip_type(name)

      case entry['kind']
      when 'enum' then Codec.new(:enum, name, name)
      when 'struct'
        return Codec.new(:runtime, name, name) if @policy.override_type(name) || RUNTIME_TYPES.key?(entry['semantic'])
        return Codec.new(:adapter, name, name) if entry['semantic'] == 'adapter'
        return error(path, "#{name} holds adapter operations") if entry['semantic'] == 'vtable'

        Codec.new(:struct, name, name)
      else error(path, "no JSON codec for #{entry['kind']} #{name}")
      end
    end

    def list_helper(path, target, terminator:)
      element = target['type'] or return error(path, 'lists hold named types')
      codec = value_codec(path, target) or return nil
      type = @api.type(element)['fields'].find { |each| each['name'] == terminator }['type']
      name = @api.resolve(type)['type']
      condition = if @api.pointer?(type) then "item->#{terminator} == NULL"
                  elsif name && %w[string bytes].include?(@api.type(name)['semantic']) then "item->#{terminator}.ptr == NULL"
                  else "item->#{terminator} == 0"
                  end
      @lists[element] ||= List.new(key: element, element: element, codec: codec, terminator: condition)
    end

    def array_helper(path, target, count_type)
      element = target['type'] || target['builtin']
      codec = value_codec(path, target) or return nil
      count_ctype = @api.c_type(@api.resolve(count_type))
      key = "#{element}_#{count_ctype}"
      @arrays[key] ||= List.new(key: key, element: element, codec: codec, count_ctype: count_ctype)
    end

    def schema_label(type, codec)
      codec.kind == :adapter ? "adapter:#{codec.name}" : @api.c_type(type)
    end

    # ------------------------------------------------------------------
    # Functions

    def plan_functions
      names = {}
      @api.functions.each do |function|
        name = function['name']
        next unless function['public'] || function['static_inline']

        if (reason = @policy.skip_function(name))
          @report[:functions_skipped] << [name, reason]
          next
        end
        rpc = method_name(name)
        error(name, "method #{rpc} is also #{names[rpc]}") if names[rpc]
        names[rpc] = name
        if (reason = @policy.override_function(name))
          @report[:functions_overridden] << [name, reason]
          @methods << Method_.new(kind: :external, name: rpc, function: name, handler: "tat_handler_#{name}",
                                  schema: { 'function' => name, 'implemented' => function['implemented'] })
          next
        end
        method = method_plan(rpc, function) or next
        @methods << method
        @report[function['implemented'] ? :functions_bound : :functions_unimplemented] << name
      end
    end

    def method_name(function)
      domain, operation = function.delete_prefix('ta_').split('_', 2)
      operation ? "ta.#{domain}.#{operation}" : "ta.#{domain}"
    end

    def method_plan(rpc, function)
      name = function['name']
      result = result_plan(name, function['returns']) or return nil
      storage = []
      call_args = []
      fields = []
      outputs = []
      retire = []
      flattened = nil
      function['params'].each do |param|
        path = "#{name}(#{param['name']})"
        local = "a->#{param['name']}"
        if flatten?(param)
          struct = @api.pointee(param['type'])['type']
          plan = @structs[struct] or return error(path, "#{struct} is not bound, so #{name} cannot be; skip one of them")
          storage << "#{struct} #{param['name']};"
          call_args << "&#{local}"
          flattened = [param['name'], plan]
          outputs.concat(struct_outputs(path, struct, local, result))
          next
        end
        case param['shape']
        when nil
          field = input_field(path, param, param['name'], param['name']) or return nil
          field.required = true
          fields << field
          storage << "#{@api.c_type(param['type'])} #{param['name']};"
          call_args << local
        when 'handle', 'single'
          if param['direction'] == 'out'
            output = value_output(path, param['name'], "a->o_#{param['name']}", @api.resolve(param['type'])['pointer'],
                                  param, result) or return nil
            outputs << output
            call_args << "&a->o_#{param['name']}"
            next
          end
          field = input_field(path, param, param['name'], param['name']) or return nil
          field.required = !optional?(param)
          fields << field
          storage << "#{declaration(param['type'], param['name'])};"
          call_args << local
          retire << [local, nil] if param['destroys']
          retire << [local, param['consumed_on']] if param['consumed_on']
        when 'handle_out'
          outputs << handle_output(param['name'], "a->o_#{param['name']}", param, result)
          call_args << "&a->o_#{param['name']}"
        when 'arena' then call_args << '&b->arena'
        else return error(path, "#{param['shape']} parameters cannot cross the protocol; skip #{name} in bindings.rb")
        end
      end
      receiver(rpc, function, flattened)
      Method_.new(kind: :function, name: rpc, handler: "tat_method_#{name}", function: name, implemented: function['implemented'],
                  storage: storage + outputs.map(&:storage).compact,
                  call_args: call_args, fields: fields, keys: fields.map(&:key), flattened: flattened, outputs: outputs,
                  result: result, retire: retire, schema: method_schema(function, fields, flattened, outputs, result))
    end

    def flatten?(param)
      param['name'] == @policy.flatten_parameter && param['shape'] == 'single' && param['direction'] == 'in' &&
        !optional?(param) && @api.pointee(param['type'])['type'] && @api.kind(@api.pointee(param['type'])['type']) == 'struct'
    end

    # A C declaration of `name` with `type`, keeping the pointee's const.
    def declaration(type, name)
      resolved = @api.resolve(type)
      return "#{@api.c_type(type)} #{name}" unless resolved.key?('pointer')

      "#{resolved['pointer']['const'] ? 'const ' : ''}#{@api.c_type(resolved['pointer'])} *#{name}"
    end

    def result_plan(name, returns)
      type = returns['type']
      return { kind: :void, label: 'void' } if type == { 'builtin' => 'void' }
      return error(name, 'returned pointers cannot cross the protocol; skip the function in bindings.rb') if @api.pointer?(type)

      codec = value_codec(name, type) or return nil
      { kind: returns['success'] ? :status : :value, codec: codec, ctype: @api.c_type(type), success: returns['success'],
        label: @api.c_type(type) }
    end

    # C condition under which an output is valid, or nil when always.
    def valid(slot, result)
      case slot['valid_on']
      when nil, 'always' then nil
      when 'success' then "r == #{result[:success]}"
      else "r == #{slot['valid_on']}"
      end
    end

    def struct_outputs(path, struct, local, result)
      @api.type(struct)['fields'].filter_map do |slot|
        next unless slot['direction'] == 'out'

        field_path = "#{path}.#{slot['name']}"
        target = "#{local}.#{slot['name']}"
        storage = "a->o_#{slot['name']}"
        case slot['shape']
        when 'single'
          output = value_output(field_path, slot['name'], storage, @api.resolve(slot['type'])['pointer'], slot, result)
          output&.setup&.unshift("#{target} = &#{storage};")
          output
        when 'handle_out'
          output = handle_output(slot['name'], storage, slot, result)
          output.setup.unshift("#{target} = &#{storage};")
          output
        when 'view_out' then view_output(field_path, slot, target, storage, result)
        when 'buffer' then buffer_output(field_path, struct, slot, local, result)
        else error(field_path, "#{slot['shape']} outputs cannot cross the protocol; skip the function in bindings.rb")
        end
      end
    end

    def value_output(path, key, storage, target, slot, result)
      codec = value_codec(path, target) or return nil
      condition = valid(slot, result)
      releases = []
      releases << "#{slot['release']}(&#{storage});" if slot['release']
      releases.concat(owned_members(target, storage))
      releases = releases.map { |line| condition ? "if (#{condition}) #{line}" : line }
      Output.new(key: key, storage: "#{@api.c_type(target)} #{storage.delete_prefix('a->')};", setup: [],
                 encode: guarded(condition, codec.encode("&#{storage}")), releases: releases,
                 schema: @api.c_type(target))
    end

    # Releases for owned host values held by value inside an output.
    def owned_members(type, storage)
      name = @api.resolve(type)['type'] or return []
      entry = @api.type(name)
      return [] unless entry['kind'] == 'struct' && entry['semantic'].nil?

      entry['fields'].flat_map do |field|
        access = "#{storage}.#{field['name']}"
        if (release = field['release_if'])
          ["if (#{storage}.#{release['flag']}) #{release['function']}(&#{access});"]
        elsif field['release']
          ["#{field['release']}(&#{access});"]
        elsif !@api.pointer?(field['type']) && field['type']['type']
          owned_members(field['type'], access)
        else []
        end
      end
    end

    def handle_output(key, storage, slot, result)
      target = @api.resolve(@api.resolve(slot['type'])['pointer'])['pointer']
      handle = @api.type(target['type'])['handle']
      Output.new(key: key, storage: "#{target['type']} *#{storage.delete_prefix('a->')};", setup: [],
                 encode: guarded(valid(slot, result), "tat_handle_register(b, #{handle.to_json}, #{storage})"),
                 releases: [], schema: "handle:#{handle}")
    end

    def view_output(path, slot, target, storage, result)
      view = @api.resolve(@api.resolve(slot['type'])['pointer'])['pointer']
      codec = value_codec(path, view) or return nil
      condition = [valid(slot, result), "#{storage} != NULL"].compact.join(' && ')
      Output.new(key: slot['name'], storage: "const #{view['type']} *#{storage.delete_prefix('a->')};",
                 setup: ["#{target} = &#{storage};"], encode: guarded(condition, codec.encode(storage)),
                 releases: [], schema: view['type'])
    end

    def buffer_output(path, struct, slot, local, result)
      element = @api.resolve(slot['type'])['pointer']
      capacity = "#{local}.#{slot['capacity']}"
      needed = @api.type(struct)['fields'].find { |each| each['name'] == slot['needed'] }
      target = "#{local}.#{slot['name']}"
      ctype = @api.c_type(element)
      setup = [
        "if (#{capacity} > #{@max_buffer_elements}u) {",
        "    tat_fail(b, \"capacity exceeds #{@max_buffer_elements}\");",
        "    tat_at(b, #{slot['capacity'].to_json});",
        '    return false;',
        '}',
        "#{target} = #{capacity} == 0 ? NULL : tat_alloc_array(b, #{capacity}, sizeof(#{ctype}), _Alignof(#{ctype}));",
        "if (#{capacity} != 0 && #{target} == NULL) return false;"
      ]
      condition = [valid(slot, result), "#{target} != NULL"].compact.join(' && ')
      if element['builtin'] == 'char'
        encode = "tat_encode_buffer(#{target}, #{capacity})"
        schema = 'string'
      else
        list = array_helper(path, element, needed['type']['pointer']) or return nil
        count = "(*#{local}.#{slot['needed']} < #{capacity} ? *#{local}.#{slot['needed']} : #{capacity})"
        encode = "tat_encode_array_#{list.key}(b, #{target}, #{count})"
        schema = "#{ctype}[]"
      end
      Output.new(key: slot['name'], setup: setup, encode: guarded(condition, encode), releases: [], schema: schema)
    end

    def guarded(condition, expression)
      condition.nil? || condition.empty? ? expression : "(#{condition} ? #{expression} : jv_null())"
    end


    # Methods whose ta_value inputs belong to a receiving actor or definition.
    def receiver(rpc, function, flattened)
      model = function['value_model'] or return
      kind = model['kind']
      return @report[:value_models] << [function['name'], 'any: decoded as jq'] if kind == 'any'

      constant = VALUE_MODELS[kind] or
        return error(function['name'], "the host cannot find the data model of a #{kind}; " \
                                       'resolve it in VALUE_MODELS and value_codec.c')
      path = model['path']
      path = path.drop(1) if flattened && path.first == flattened.first
      return error(function['name'], "value model path #{model['path'].join('.')} is not a top-level member") unless path.size == 1

      @receivers << [rpc, path.first, constant]
    end

    def method_schema(function, fields, flattened, outputs, result)
      params = fields.to_h { |field| [field.key, { 'type' => field.schema, 'required' => field.required }] }
      if flattened
        struct = flattened.last
        struct.fields.each { |field| params[field.key] = { 'type' => field.schema, 'required' => field.required } }
        if (tagged = struct.tagged)
          params[tagged.tag] = { 'type' => tagged.enum, 'required' => tagged.kind_required && tagged.inferable.empty? }
          tagged.kinds.each do |kind|
            case kind.layout
            when :keyed then params[kind.keyed.key] = { 'type' => kind.keyed.schema, 'required' => false }
            when :inline then kind.fields.each { |field| params[field.key] = { 'type' => field.schema, 'required' => false } }
            when :named
              @structs[kind.named].fields.each { |field| params[field.key] = { 'type' => field.schema, 'required' => false } }
            end
          end
        end
      end
      { 'function' => function['name'], 'implemented' => function['implemented'], 'params' => params,
        'result' => result[:label], 'out' => outputs.to_h { |output| [output.key, output.schema] } }
    end
  end
end
