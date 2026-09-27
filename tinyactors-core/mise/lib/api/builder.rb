# frozen_string_literal: true

module API
  Error = Class.new(StandardError)

  # Builds the API model from the header's AST. Structural rules decide what
  # C types determine on their own (a pointer to a handle type is a handle, a
  # `T **` to a handle type creates one); annotations state everything else.
  # Every problem is collected and reported together; nothing is guessed.
  class Builder
    TYPE_ANNOTATIONS = %w[TA_HANDLE TA_VIEW TA_STRING TA_BYTES TA_HOST_VALUE TA_ADAPTER TA_VTABLE TA_TAGGED
                          TA_ZERO_IS_ABSENT TA_SYMBOLS].freeze
    SEMANTICS = { 'TA_STRING' => 'string', 'TA_BYTES' => 'bytes', 'TA_HOST_VALUE' => 'host_value',
                  'TA_ADAPTER' => 'adapter', 'TA_VTABLE' => 'vtable' }.freeze
    SHAPES = %w[TA_IN TA_OUT TA_INOUT TA_TERMINATED_BY TA_COUNTED_BY TA_BUFFER TA_VIEW_OUT TA_ARENA].freeze
    POINTER_FACTS = %w[TA_OPTIONAL TA_ALWAYS TA_VALID_ON TA_RELEASE TA_LIFETIME TA_DESTROYS TA_CONSUMED_ON_OK].freeze
    MEMBER_FACTS = %w[TA_WHEN TA_FLAGS TA_RELEASE_IF].freeze
    SLOT_ANNOTATIONS = (SHAPES + POINTER_FACTS + MEMBER_FACTS).freeze
    FUNCTION_ANNOTATIONS = %w[TA_API TA_VALUE_MODEL TA_ANY_VALUE_MODEL TA_OPTIONAL TA_LIFETIME].freeze
    ARITY = { 'TA_TAGGED' => 2, 'TA_SYMBOLS' => 1, 'TA_CONSTANTS_OF' => 2, 'TA_FLAG_SET' => 2, 'TA_WHEN' => 1,
              'TA_FLAGS' => 1, 'TA_RELEASE_IF' => 2, 'TA_TERMINATED_BY' => 1, 'TA_COUNTED_BY' => 1, 'TA_BUFFER' => 2,
              'TA_VALID_ON' => 1, 'TA_RELEASE' => 1, 'TA_LIFETIME' => 1, 'TA_VALUE_MODEL' => 2 }.freeze
    ANNOTATIONS = (TYPE_ANNOTATIONS + SLOT_ANNOTATIONS + FUNCTION_ANNOTATIONS +
                   %w[TA_CONSTANTS_OF TA_FLAG_SET TA_SUCCESS]).uniq.freeze
    SCOPES = %w[call system definition arena input run].freeze
    INTEGERS = %w[int int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t size_t].freeze
    VALUE_MODELS = { 'actor' => 'ta_actor_id', 'definition' => 'ta_definition_id', 'session_image' => 'ta_session_image' }.freeze
    OUTPUT_SHAPES = %w[single buffer view_out handle_out raw].freeze

    # `imports`: the types of the libraries this header builds on, by name
    # (their API descriptions' `types`). A function may use them, and
    # lookups see them, but the description lists and checks only the
    # header's own types.
    def initialize(ast, implemented:, imports: {})
      @ast = ast
      @implemented = implemented
      @imports = imports
      @errors = []
      @types = {}
      @flag_sets = {}
      @functions = []
    end

    def build
      @releases = []
      @derived = []
      index
      build_enums
      classify_typedefs
      build_types
      build_functions
      check_tagged_unions
      check_outputs
      check_flags
      check_value_models
      describe_handles
      apply_derived
      raise Error, @errors.uniq.join("\n") unless @errors.empty?

      { 'types' => @types.sort.to_h, 'flag_sets' => @flag_sets.sort.to_h, 'functions' => @functions }
    end

    def known?(name) = @typedefs.key?(name) || @imports.key?(name)

    private

    def error(path, message)
      @errors << "#{path}: #{message}"
      nil
    end

    # ------------------------------------------------------------------
    # Declarations

    def index
      @typedefs = {}
      @records = {}
      @anonymous_enums = []
      @function_nodes = []
      @ast.declarations.each do |node|
        case node['kind']
        when 'TypedefDecl' then @typedefs[node['name']] = node
        when 'RecordDecl'
          @records[node['name']] = node if node['name'] && !node['name'].empty? && node['completeDefinition']
        when 'EnumDecl'
          @anonymous_enums << node if node['name'].to_s.empty? && !owned_enum?(node)
        when 'FunctionDecl' then @function_nodes << node
        else error(node['name'] || node['kind'], "unsupported top-level declaration #{node['kind']}")
        end
      end
      @typedefs.each_key { |name| error(name, 'API names start with ta_') unless name.start_with?('ta_') }
    end

    def owned_enum?(node)
      @owned_enum_ids ||= @ast.declarations.select { |each| each['kind'] == 'TypedefDecl' }
                              .filter_map { |typedef| owned_tag_id(typedef) }
      @owned_enum_ids.include?(node['id'])
    end

    # The tag a typedef declares itself, if any. clang writes the typedef's
    # inner type two ways: through clang 21 as an ElaboratedType carrying
    # `ownedTagDecl`, and since clang 22 as the tag type itself, carrying
    # `decl` and `isTagOwned`.
    def owned_tag_id(typedef)
      inner = typedef.dig('inner', 0) or return nil
      return inner.dig('ownedTagDecl', 'id') if inner.key?('ownedTagDecl')

      inner.dig('decl', 'id') if inner['isTagOwned']
    end

    # Annotations keyed by name; duplicates, unknown names, and wrong
    # argument counts are errors.
    def notes(path, annotations, allowed)
      annotations.each_with_object({}) do |annotation, notes|
        name = annotation.name
        if !ANNOTATIONS.include?(name) && name != 'TA_API'
          error(path, "unknown annotation #{name}")
        elsif !allowed.include?(name)
          error(path, "#{name} does not apply here")
        elsif notes.key?(name)
          error(path, "duplicate #{name}")
        elsif annotation.args.size != ARITY.fetch(name, 0)
          error(path, "#{name} takes #{ARITY.fetch(name, 0)} argument(s), got #{annotation.args.size}")
        else
          notes[name] = annotation.args
        end
      end
    end

    # The record or enum a typedef names, following either clang spelling of
    # the inner type (see owned_tag_id); anything else is an alias. A `decl`
    # that is not a tag -- a TypedefType's, say -- falls through to :alias.
    def typedef_target(node)
      inner = node.dig('inner', 0)
      reference = inner && (inner['ownedTagDecl'] || inner['decl'] || inner.dig('inner', 0, 'decl'))
      return [:alias, nil] unless reference && %w[RecordDecl EnumDecl].include?(reference['kind'])

      decl = @ast.node(reference['id'])
      return [:enum, decl] if decl['kind'] == 'EnumDecl'

      complete = decl['completeDefinition'] ? decl : @records[decl['name']]
      [:record, complete]
    end

    def parse_type(path, spelling)
      CType.parse_type(spelling, known: method(:known?))
    rescue CType::Error => e
      error(path, e.message)
    end

    # Follows aliases to the type they name; a const on the alias stays.
    def resolve(type)
      const = type && type['const']
      seen = 0
      while type && type['type'] && entry_of(type['type'])&.dig('kind') == 'alias'
        type = entry_of(type['type'])['type']
        seen += 1
        raise Error, 'alias cycle' if seen > 32
      end
      const && type && !type['const'] ? type.merge('const' => true) : type
    end

    def named(type) = type && type['type']
    # A type's entry: the header's own, else an imported library's.
    def entry_of(name) = name && (@types[name] || @imports[name])
    def kind_of(name) = entry_of(name)&.dig('kind')
    def semantic_of(name) = entry_of(name)&.dig('semantic')

    def integer?(type)
      type = resolve(type)
      type && !type.key?('pointer') && (INTEGERS.include?(type['builtin']) || kind_of(named(type)) == 'enum')
    end

    def strip_const(type) = type.reject { |key, _| key == 'const' }

    # ------------------------------------------------------------------
    # Enums

    def build_enums
      @typedefs.each do |name, node|
        target, decl = typedef_target(node)
        next unless target == :enum

        typedef_notes = notes(name, @ast.annotations(node), %w[TA_SYMBOLS])
        prefix = typedef_notes.dig('TA_SYMBOLS', 0)
        next error(name, 'enums need TA_SYMBOLS("PREFIX_") for stable symbols') unless prefix

        entry = { 'kind' => 'enum', 'constants' => constants(name, decl, prefix) }
        add_doc(entry, node)
        @types[name] = entry
      end
      @anonymous_enums.each do |decl|
        enum_notes = notes('anonymous enum', @ast.annotations(decl), %w[TA_CONSTANTS_OF TA_FLAG_SET])
        if (type, prefix = enum_notes['TA_CONSTANTS_OF'])
          alias_node = @typedefs[type] or next error('anonymous enum', "TA_CONSTANTS_OF names unknown type #{type}")
          underlying = parse_type(type, alias_node.dig('type', 'qualType'))
          next error(type, 'TA_CONSTANTS_OF needs an integer type') unless INTEGERS.include?(underlying&.dig('builtin'))

          entry = { 'kind' => 'enum', 'underlying' => underlying, 'constants' => constants(type, decl, prefix) }
          add_doc(entry, alias_node)
          @types[type] = entry
        elsif (set, prefix = enum_notes['TA_FLAG_SET'])
          @flag_sets[set] = { 'constants' => constants(set, decl, prefix, success: false) }
        else
          first = decl['inner']&.find { |child| child['kind'] == 'EnumConstantDecl' }&.dig('name')
          error("anonymous enum #{first}", 'needs TA_CONSTANTS_OF(type, prefix) or TA_FLAG_SET(name, prefix)')
        end
      end
    end

    def constants(owner, decl, prefix, success: true)
      value = -1
      successes = 0
      list = (decl['inner'] || []).select { |child| child['kind'] == 'EnumConstantDecl' }.map do |constant|
        name = constant['name']
        explicit = Clang.find_node(constant) { |child| child['kind'] == 'ConstantExpr' }
        value = explicit ? Integer(explicit['value']) : value + 1
        constant_notes = notes(name, @ast.annotations(constant), success ? %w[TA_SUCCESS] : [])
        error(name, "does not start with the TA_SYMBOLS prefix #{prefix}") unless name.start_with?(prefix)
        entry = { 'name' => name, 'value' => value, 'symbol' => name.delete_prefix(prefix).downcase }
        error(name, 'has an empty symbol') if entry['symbol'].empty?
        if constant_notes.key?('TA_SUCCESS')
          entry['success'] = true
          successes += 1
        end
        add_doc(entry, constant)
        entry
      end
      error(owner, 'more than one TA_SUCCESS constant') if successes > 1
      list.group_by { |constant| constant['symbol'] }.each do |symbol, same|
        error(owner, "duplicate symbol #{symbol}") if same.size > 1
      end
      list
    end

    # ------------------------------------------------------------------
    # Types

    # First pass: the kind and semantic of every typedef, so the second pass
    # can classify pointers to them.
    def classify_typedefs
      @record_nodes = {}
      @typedefs.each do |name, node|
        next if @types.key?(name)

        target, decl = typedef_target(node)
        typedef_notes = notes(name, @ast.annotations(node), TYPE_ANNOTATIONS - %w[TA_SYMBOLS])
        @typedef_notes ||= {}
        @typedef_notes[name] = typedef_notes
        entry = {}
        case target
        when :enum then next
        when :record
          if decl.nil?
            kinds = typedef_notes.keys & %w[TA_HANDLE TA_VIEW]
            next error(name, 'opaque types need TA_HANDLE or TA_VIEW') unless kinds.size == 1

            entry['kind'] = kinds.first == 'TA_HANDLE' ? 'handle' : 'view'
            entry['handle'] = name.delete_prefix('ta_') if entry['kind'] == 'handle'
            (typedef_notes.keys - kinds).each { |extra| error(name, "#{extra} does not apply to an opaque type") }
          else
            entry['kind'] = decl['tagUsed'] == 'union' ? 'union' : 'struct'
            semantics = typedef_notes.keys & SEMANTICS.keys
            error(name, "conflicting #{semantics.join(', ')}") if semantics.size > 1
            entry['semantic'] = SEMANTICS[semantics.first] if semantics.any?
            %w[TA_HANDLE TA_VIEW].each { |extra| error(name, "#{extra} applies only to opaque types") if typedef_notes.key?(extra) }
            @record_nodes[name] = decl
          end
        when :alias
          (typedef_notes.keys).each { |extra| error(name, "#{extra} does not apply to an alias") }
          entry['kind'] = 'alias'
        end
        add_doc(entry, node)
        @types[name] = entry
      end
    end

    def build_types
      @typedefs.each do |name, node|
        entry = @types[name] or next
        case entry['kind']
        when 'alias' then entry['type'] = alias_type(name, node)
        when 'struct', 'union' then build_record(name, entry)
        end
      end
      @records.each_key do |tag|
        error(tag, 'records need a typedef of the same name') unless @typedefs.key?(tag)
      end
    end

    def alias_type(name, node)
      spelling = node.dig('type', 'qualType')
      return parse_type(name, spelling) unless spelling.include?('(')

      # A function pointer: names and annotations of its parameters exist
      # only in the source text.
      text = @ast.text(node).sub(/\A\s*typedef\s+/, '')
      function_pointer(name, text, spelling)
    end

    def function_pointer(path, text, spelling)
      declaration = CType.parse_declaration(text, known: method(:known?), annotations: ANNOTATIONS)
      expected = parse_type(path, spelling) or return nil
      unless comparable(declaration.type) == comparable(expected)
        return error(path, "source declaration #{CType.render(declaration.type)} differs from clang's #{spelling}")
      end

      function = declaration.type.dig('pointer', 'function') or return error(path, 'expected a function pointer')
      { 'pointer' => { 'function' => callback(path, function) } }
    rescue CType::Error => e
      error(path, e.message)
    end

    def comparable(type)
      case type
      when Hash
        type.reject { |key, _| %w[name annotations].include?(key) }.transform_values { |value| comparable(value) }
      when Array then type.map { |each| comparable(each) }
      else type
      end
    end

    def callback(path, function)
      params = function['params']
      siblings = params.map { |param| [param['name'], param['type']] }
      {
        'returns' => returns(path, function['returns'], {}),
        'params' => params.map do |param|
          slot("#{path}(#{param['name']})", param['name'], param['type'], param['annotations'], siblings)
        end
      }
    end

    def build_record(name, entry)
      decl = @record_nodes.fetch(name)
      typedef_notes = @typedef_notes.fetch(name)
      internal = %w[string bytes host_value].include?(entry['semantic'])
      entry['fields'] = record_fields(name, decl, internal: internal)
      add_doc(entry, decl) unless entry['doc']
      entry['zero_is_absent'] = true if typedef_notes.key?('TA_ZERO_IS_ABSENT')
      if (tag, union = typedef_notes['TA_TAGGED'])
        entry['tagged'] = { 'tag' => tag, 'union' => union }
      end
      check_semantic(name, entry)
    end

    def record_fields(owner, decl, internal: false)
      children = decl['inner'] || []
      fields = []
      children.each_with_index do |child, position|
        next unless child['kind'] == 'FieldDecl'

        path = "#{owner}.#{child['name']}"
        spelling = child.dig('type', 'qualType')
        annotations = @ast.annotations(child)
        # clang 22 spells an inline tag `union (unnamed at file:line:col)`;
        # earlier releases repeat the keyword, `union (anonymous union at ...)`.
        type = if spelling.match?(/\((?:unnamed|anonymous)(?: (?:struct|union))? at /)
                 nested = children[position - 1]
                 next error(path, 'anonymous record must precede its field') unless nested&.dig('kind') == 'RecordDecl'

                 { 'record' => { 'kind' => nested['tagUsed'], 'fields' => record_fields(path, nested) } }
               elsif spelling.include?('(')
                 function_pointer(path, @ast.text(child), spelling)
               else
                 parse_type(path, spelling)
               end
        next unless type

        fields << [child, path, type, annotations]
      end
      siblings = fields.map { |child, _path, type, _| [child['name'], type] }
      fields.map do |child, path, type, annotations|
        field = if internal
                  error(path, 'fields of string, bytes, and host value types take no annotations') unless annotations.empty?
                  { 'name' => child['name'], 'type' => type }
                else
                  slot(path, child['name'], type, annotations, siblings)
                end
        add_doc(field, child)
        field
      end
    end

    def check_semantic(name, entry)
      fields = entry['fields']
      case entry['semantic']
      when 'string', 'bytes'
        error(name, 'strings and bytes are {ptr, len}') unless fields.map { |field| field['name'] } == %w[ptr len]
      when 'adapter'
        first = fields.first
        error(name, 'adapters start with `void *self`') unless first && first['name'] == 'self' &&
                                                                first['type'] == { 'pointer' => { 'builtin' => 'void' } }
      when 'vtable'
        error(name, 'a vtable holds callbacks') unless fields.any? { |field| field['shape'] == 'callback' }
      when nil
        if fields.any? { |field| %w[vtable callback].include?(field['shape']) }
          error(name, 'a struct with operations needs TA_ADAPTER')
        end
      end
    end

    # ------------------------------------------------------------------
    # Slots: fields, parameters, and callback parameters

    # `siblings` are [name, type] pairs of the enclosing struct or parameter
    # list; count, capacity, and needed annotations name one of them.
    def slot(path, name, type, annotations, siblings)
      slot = { 'name' => name, 'type' => type }
      facts = notes(path, annotations, SLOT_ANNOTATIONS)
      if (constant, = facts.delete('TA_WHEN'))
        slot['when'] = constant
      end
      if (set, = facts.delete('TA_FLAGS'))
        error(path, 'TA_FLAGS needs an integer') unless integer?(type)
        slot['flags'] = set
      end
      if (function, flag = facts.delete('TA_RELEASE_IF'))
        release_if(path, slot, function, flag, siblings)
      end
      resolved = resolve(type)
      if resolved&.key?('pointer')
        pointer(path, slot, resolved, facts, siblings)
      else
        # A host value held by value in an output: its receiver owns it.
        if (function, = facts.delete('TA_RELEASE'))
          error(path, 'TA_RELEASE on a member applies to host values') unless semantic_of(named(resolved)) == 'host_value'
          @releases << [path, function, type]
          slot['release'] = function
        end
        facts.each_key { |extra| error(path, "#{extra} applies only to pointers") }
      end
      slot
    end

    def release_if(path, slot, function, flag, siblings)
      flag_type = siblings.find { |name, _| name == flag }&.last
      error(path, "TA_RELEASE_IF flag #{flag} is not a bool member") unless flag_type == { 'builtin' => 'bool' }
      @releases << [path, function, slot['type']]
      slot['release_if'] = { 'function' => function, 'flag' => flag }
    end

    def pointer(path, slot, pointer, facts, siblings)
      target = resolve(pointer['pointer'])
      shape, direction = implied_shape(path, target, facts)
      if shape
        (facts.keys & SHAPES).each { |extra| error(path, "#{extra} is implied by the type; remove it") }
      else
        shapes = facts.keys & SHAPES
        return error(path, "pointer needs one of #{SHAPES.join(', ')}") if shapes.empty?
        return error(path, "conflicting #{shapes.join(', ')}") if shapes.size > 1

        shape, direction = explicit_shape(path, slot, shapes.first, facts.delete(shapes.first), target, siblings)
        facts.delete(shapes.first)
      end
      slot['shape'] = shape
      slot['direction'] = direction
      pointer_facts(path, slot, facts, target)
    end

    # Shapes the C type determines on its own.
    def implied_shape(path, target, facts)
      if target.key?('function') then %w[callback in]
      elsif target.key?('pointer')
        inner = resolve(target['pointer'])
        if kind_of(named(inner)) == 'handle'
          error(path, 'a handle output is `T **`') if target['const']
          %w[handle_out out]
        elsif inner['builtin'] == 'void' then %w[raw out]
        elsif !facts.key?('TA_VIEW_OUT')
          error(path, 'pointer to pointer needs TA_VIEW_OUT')
          %w[invalid out]
        end
      elsif target['builtin'] == 'void' then %w[raw in]
      elsif kind_of(named(target)) == 'handle' then %w[handle in]
      elsif kind_of(named(target)) == 'view' then %w[view in]
      elsif semantic_of(named(target)) == 'vtable' then %w[vtable in]
      end
    end

    def explicit_shape(path, slot, shape, args, target, siblings)
      const = target['const'] == true
      case shape
      when 'TA_IN'
        error(path, 'TA_IN needs a const pointee') unless const
        %w[single in]
      when 'TA_OUT', 'TA_INOUT'
        error(path, "#{shape} needs a non-const pointee") if const
        ['single', shape == 'TA_OUT' ? 'out' : 'inout']
      when 'TA_TERMINATED_BY'
        error(path, 'lists are const') unless const
        terminator(path, target, args.first)
        slot['terminator'] = args.first
        %w[terminated in]
      when 'TA_COUNTED_BY'
        error(path, 'lists are const') unless const
        count = sibling(path, siblings, args.first)
        error(path, "count #{args.first} is not an integer") if count && !integer?(count)
        @derived << [path, args.first, 'count_of', slot['name']] if count
        slot['count'] = args.first
        %w[counted in]
      when 'TA_BUFFER'
        error(path, 'buffers are written; the pointee is not const') if const
        capacity, needed = args
        capacity_type = sibling(path, siblings, capacity)
        error(path, "capacity #{capacity} is not an integer") if capacity_type && !integer?(capacity_type)
        needed_type = sibling(path, siblings, needed)
        if needed_type && !(needed_type.key?('pointer') && integer?(needed_type['pointer']))
          error(path, "needed #{needed} is not a pointer to an integer")
        end
        @derived << [path, capacity, 'capacity_of', slot['name']] if capacity_type
        @derived << [path, needed, 'needed_of', slot['name']] if needed_type
        slot['capacity'] = capacity
        slot['needed'] = needed
        %w[buffer out]
      when 'TA_VIEW_OUT'
        inner = target['pointer'] && resolve(target['pointer'])
        unless !const && inner && inner['const'] && %w[struct union].include?(kind_of(named(inner)))
          error(path, 'TA_VIEW_OUT is `const T **` for a struct T')
        end
        %w[view_out out]
      when 'TA_ARENA'
        error(path, 'TA_ARENA is `ta_arena *`') unless named(target) == 'ta_arena' && !const
        %w[arena inout]
      end
    end

    def sibling(path, siblings, name)
      type = siblings.find { |each, _| each == name }&.last
      error(path, "no member named #{name}") unless type
      type
    end

    def terminator(path, target, field)
      name = named(target)
      names = @record_nodes[name] ? record_field_names(name) : []
      error(path, "list element #{name || CType.render(strip_const(target))} has no member #{field}") unless names.include?(field)
    end

    def record_field_names(name)
      (@record_nodes[name]['inner'] || []).select { |child| child['kind'] == 'FieldDecl' }.map { |child| child['name'] }
    end

    def pointer_facts(path, slot, facts, target)
      output = slot['direction'] == 'out'
      slot['optional'] = true if facts.delete('TA_OPTIONAL')
      if facts.delete('TA_ALWAYS')
        error(path, 'TA_ALWAYS applies to outputs') unless output
        slot['valid_on'] = 'always'
      end
      if (constant, = facts.delete('TA_VALID_ON'))
        error(path, 'TA_VALID_ON applies to outputs') unless output
        error(path, 'TA_ALWAYS and TA_VALID_ON conflict') if slot['valid_on']
        slot['valid_on'] = constant
      end
      slot['valid_on'] ||= 'success' if output && OUTPUT_SHAPES.include?(slot['shape'])
      if (function, = facts.delete('TA_RELEASE'))
        error(path, 'TA_RELEASE applies to single outputs') unless slot['shape'] == 'single' && output
        @releases << [path, function, target]
        slot['release'] = function
      end
      if (scope, = facts.delete('TA_LIFETIME'))
        error(path, "lifetime #{scope} is not one of #{SCOPES.join(', ')}") unless SCOPES.include?(scope)
        slot['lifetime'] = scope
      end
      if facts.delete('TA_DESTROYS')
        error(path, 'TA_DESTROYS applies to handles') unless slot['shape'] == 'handle'
        slot['destroys'] = true
      end
      if facts.delete('TA_CONSUMED_ON_OK')
        error(path, 'TA_CONSUMED_ON_OK applies to handles') unless slot['shape'] == 'handle'
        slot['consumed_on'] = 'TA_OK'
      end
      facts.each_key { |extra| error(path, "#{extra} does not apply to a #{slot['shape']} pointer") }
    end

    # ------------------------------------------------------------------
    # Functions

    def build_functions
      @function_nodes.each do |node|
        name = node['name']
        function_notes = notes(name, @ast.annotations(node), FUNCTION_ANNOTATIONS)
        static_inline = node['storageClass'] == 'static' && node['inline'] == true
        public = function_notes.key?('TA_API')
        error(name, 'public functions are declared with TA_API') unless public || static_inline
        type = parse_type(name, node.dig('type', 'qualType'))
        next unless type

        params = (node['inner'] || []).select { |child| child['kind'] == 'ParmVarDecl' }
        siblings = params.map { |param| [param['name'], parse_type("#{name}(#{param['name']})", param.dig('type', 'qualType'))] }
        entry = { 'name' => name }
        add_doc(entry, node)
        entry['public'] = public
        entry['static_inline'] = static_inline
        entry['implemented'] = static_inline || @implemented.include?(name)
        entry['returns'] = returns(name, type.dig('function', 'returns'), function_notes)
        entry['params'] = params.zip(siblings).map do |param, (_, param_type)|
          next unless param_type

          slot = slot("#{name}(#{param['name']})", param['name'], param_type, @ast.annotations(param), siblings)
          add_doc(slot, param)
          slot
        end.compact
        if (kind, path = function_notes['TA_VALUE_MODEL'])
          entry['value_model'] = { 'kind' => kind, 'path' => path.split('.') }
        elsif function_notes.key?('TA_ANY_VALUE_MODEL')
          entry['value_model'] = { 'kind' => 'any' }
        end
        @functions << entry
      end
    end

    def returns(path, type, function_notes)
      result = { 'type' => type }
      resolved = resolve(type)
      optional = function_notes.key?('TA_OPTIONAL')
      lifetime = function_notes.dig('TA_LIFETIME', 0)
      if resolved&.key?('pointer')
        target = resolve(resolved['pointer'])
        result['shape'] = target['builtin'] == 'void' ? 'raw' : 'single'
        if result['shape'] == 'single' && !lifetime
          error(path, 'a returned pointer needs TA_LIFETIME')
        end
        result['optional'] = true if optional
        if lifetime
          error(path, "lifetime #{lifetime} is not one of #{SCOPES.join(', ')}") unless SCOPES.include?(lifetime)
          result['lifetime'] = lifetime
        end
      else
        error(path, 'TA_OPTIONAL and TA_LIFETIME apply to returned pointers') if optional || lifetime
      end
      if (success = success_of(type))
        result['success'] = success
      end
      result
    end

    def success_of(type)
      resolved = type && (kind_of(named(type)) == 'enum' ? type : resolve(type))
      entry = resolved && entry_of(named(resolved))
      entry && entry['kind'] == 'enum' ? entry['constants'].find { |constant| constant['success'] }&.dig('name') : nil
    end

    # ------------------------------------------------------------------
    # Cross-checks

    def check_tagged_unions
      @types.each do |name, entry|
        next unless entry['fields']

        tagged_union = entry['tagged'] && "#{name}.#{entry['tagged']['union']}"
        each_record(name, entry['fields']) do |path, fields, _owner|
          fields.each do |field|
            next unless field['when'] && path != tagged_union

            error("#{path}.#{field['name']}", 'TA_WHEN belongs in the union of a TA_TAGGED struct')
          end
        end
        tagged = entry['tagged'] or next
        tag = entry['fields'].find { |field| field['name'] == tagged['tag'] }
        union = entry['fields'].find { |field| field['name'] == tagged['union'] }
        enum = tag && entry_of(named(tag['type']))
        next error(name, "TA_TAGGED tag #{tagged['tag']} is not an enum member") unless enum && enum['kind'] == 'enum'
        next error(name, "TA_TAGGED union #{tagged['union']} is not a union member") unless union&.dig('type', 'record', 'kind') == 'union'

        members = {}
        union['type']['record']['fields'].each do |member|
          constant = member['when']
          next error("#{name}.#{tagged['union']}.#{member['name']}", 'union members need TA_WHEN(constant)') unless constant
          next error("#{name}.#{member['name']}", "#{constant} is not a #{named(tag['type'])} constant") unless enum['constants'].any? { |each| each['name'] == constant }
          next error("#{name}.#{member['name']}", "#{constant} already selects #{members[constant]}") if members[constant]

          members[constant] = member['name']
        end
        tagged['enum'] = named(tag['type'])
        tagged['members'] = enum['constants'].to_h { |constant| [constant['name'], members[constant['name']]] }
      end
      @types.each do |name, entry|
        next unless entry['fields']

        each_record(name, entry['fields']) do |path, fields, owner|
          fields.each do |field|
            next unless field.dig('type', 'record', 'kind') == 'union'
            next if owner && owner.dig('tagged', 'union') == field['name'] && path == name

            error("#{path}.#{field['name']}", 'a union needs a TA_TAGGED(tag, union) owner')
          end
        end
      end
    end

    def each_record(path, fields, owner = @types[path], &block)
      block.call(path, fields, owner)
      fields.each do |field|
        record = field.dig('type', 'record') or next
        each_record("#{path}.#{field['name']}", record['fields'], nil, &block)
      end
    end

    # Outputs: validity needs a result to compare with, borrowed references
    # need a lifetime or a release, and releases name a matching function.
    def check_outputs
      users = struct_users
      each_function do |path, function|
        success = function['returns']['success']
        void = function['returns']['type'] == { 'builtin' => 'void' }
        function['params'].each do |param|
          check_validity("#{path}(#{param['name']})", param, success, void, function)
          check_borrowed("#{path}(#{param['name']})", param)
        end
      end
      @types.each do |name, entry|
        next unless entry['fields']

        entry['fields'].each do |field|
          next unless field['valid_on']

          path = "#{name}.#{field['name']}"
          check_borrowed(path, field)
          (users[name] || []).each do |user_path, function|
            success = function['returns']['success']
            case field['valid_on']
            when 'always' then nil
            when 'success'
              error(path, "#{user_path} returns no success result; mark the output TA_ALWAYS or TA_VALID_ON") unless success
            else
              check_constant(path, field['valid_on'], function)
            end
          end
        end
      end
      @releases.each do |path, function, type|
        release = @functions.find { |each| each['name'] == function }
        next error(path, "release function #{function} is not declared") unless release

        first = release['params'].first
        target = first && resolve(first['type'])
        unless target&.key?('pointer') && named(target['pointer']) == named(strip_const(type))
          error(path, "#{function} does not release a #{named(type) || CType.render(type)}")
        end
      end
    end

    def check_validity(path, slot, success, void, function)
      return unless slot['valid_on']

      if void && slot['valid_on'] == 'success'
        slot['valid_on'] = 'always'
      elsif slot['valid_on'] == 'success'
        error(path, 'the function returns no success result; mark the output TA_ALWAYS or TA_VALID_ON') unless success
      elsif slot['valid_on'] != 'always'
        check_constant(path, slot['valid_on'], function)
      end
    end

    def check_constant(path, constant, function)
      enum = entry_of(named(function['returns']['type']))
      return if enum && enum['kind'] == 'enum' && enum['constants'].any? { |each| each['name'] == constant }

      error(path, "#{constant} is not a result of #{function['name'] || 'the callback'}")
    end

    def check_borrowed(path, slot)
      return unless slot['direction'] == 'out' && !slot['release'] && !slot['lifetime']

      pointee = case slot['shape']
                when 'single', 'buffer' then slot['type'] && resolve(slot['type'])['pointer']
                when 'view_out' then resolve(slot['type'])['pointer']['pointer']
                end
      return unless pointee && references?(pointee)

      error(path, 'this output holds borrowed references; add TA_LIFETIME(scope) or TA_RELEASE(function)')
    end

    def references?(type, seen = {})
      type = resolve(type)
      return false unless type
      return true if type.key?('pointer') || type.key?('function')
      return type['record']['fields'].any? { |field| !owned?(field) && references?(field['type'], seen) } if type.key?('record')

      name = named(type) or return false
      entry = entry_of(name)
      return false if entry.nil? || seen[name]

      seen[name] = true
      return true if %w[string bytes host_value adapter].include?(entry['semantic'])
      return false unless entry['fields']

      entry['fields'].any? { |field| !owned?(field) && references?(field['type'], seen) }
    end

    def owned?(field) = field.key?('release') || field.key?('release_if')

    # Every function and callback, with a path for messages.
    def each_function(&block)
      @functions.each { |function| block.call(function['name'], function) }
      @types.each do |name, entry|
        if entry['kind'] == 'alias' && (function = entry.dig('type', 'pointer', 'function'))
          block.call(name, function)
        end
        next unless entry['fields']

        entry['fields'].each do |field|
          function = resolve(field['type'])&.dig('pointer', 'function') or next
          next if field['type']['type'] # a named callback type is checked once, above

          block.call("#{name}.#{field['name']}", function)
        end
      end
    end

    # Struct name => [[path, function]] for every function or callback that
    # receives the struct through an input pointer or by value.
    def struct_users
      users = Hash.new { |hash, key| hash[key] = [] }
      each_function do |path, function|
        function['params'].each do |param|
          type = resolve(param['type'])
          target = param['shape'] == 'single' && param['direction'] == 'in' ? type['pointer'] : type
          name = named(target) or next
          users[name] << ["#{path}(#{param['name']})", function] if @types[name]&.key?('fields')
        end
      end
      users
    end

    def check_flags
      used = []
      @types.each do |name, entry|
        next unless entry['fields']

        each_record(name, entry['fields']) do |path, fields, _|
          fields.each do |field|
            next unless field['flags']

            used << field['flags']
            error("#{path}.#{field['name']}", "unknown flag set #{field['flags']}") unless @flag_sets.key?(field['flags'])
          end
        end
      end
      (@flag_sets.keys - used).each { |set| error(set, 'flag set is not used by any member') }
    end

    # Functions that take host values must say which data model builds them.
    def check_value_models
      @functions.each do |function|
        takes = function['params'].any? { |param| param['direction'] != 'out' && param['shape'] != 'handle' && values_in?(param) }
        model = function['value_model']
        if takes && !model
          error(function['name'], 'takes ta_value inputs; add TA_VALUE_MODEL(kind, path) or TA_ANY_VALUE_MODEL')
        elsif !takes && model
          error(function['name'], 'takes no ta_value input; remove its value model')
        elsif model && model['kind'] != 'any'
          check_value_model_path(function, model)
        end
      end
    end

    def check_value_model_path(function, model)
      path = model['path']
      expected = VALUE_MODELS[model['kind']]
      return error(function['name'], "value model kind must be one of any, #{VALUE_MODELS.keys.join(', ')}") unless expected

      param = function['params'].find { |each| each['name'] == path.first }
      return error(function['name'], "value model path #{path.join('.')} names no parameter") unless param

      type = param['type']
      path.drop(1).each do |member|
        target = resolve(type)
        target = resolve(target['pointer']) if target&.key?('pointer')
        field = entry_of(named(target))&.dig('fields')&.find { |each| each['name'] == member }
        return error(function['name'], "value model path #{path.join('.')} has no member #{member}") unless field

        type = field['type']
      end
      found = resolve(type)&.key?('pointer') ? named(resolve(resolve(type)['pointer'])) : named(type)
      error(function['name'], "value model path #{path.join('.')} is not a #{expected}") unless found == expected
    end

    def values_in?(slot, seen = {})
      type = resolve(slot['type'])
      return false unless type
      return false if %w[handle view raw callback vtable handle_out view_out buffer arena].include?(slot['shape'])

      type = resolve(type['pointer']) if type.key?('pointer')
      type_values?(type, seen)
    end

    def type_values?(type, seen)
      return type['record']['fields'].any? { |field| field['direction'] != 'out' && values_in?(field, seen) } if type.key?('record')

      name = named(type) or return false
      return true if semantic_of(name) == 'host_value'
      return false if seen[name] || %w[adapter vtable].include?(semantic_of(name))

      seen[name] = true
      (entry_of(name)&.dig('fields') || []).any? { |field| field['direction'] != 'out' && values_in?(field, seen) }
    end

    # Handle types list the functions that create, destroy, and take them.
    def describe_handles
      users = struct_users
      @types.each do |name, entry|
        next unless entry['kind'] == 'handle'

        created = []
        destroyed = []
        consumed = []
        @functions.each do |function|
          function['params'].each do |param|
            created << { 'function' => function['name'], 'path' => [param['name']] } if handle_out?(param, name)
            destroyed << { 'function' => function['name'], 'path' => [param['name']] } if param['destroys'] && handle?(param, name)
            if param['consumed_on'] && handle?(param, name)
              consumed << { 'function' => function['name'], 'path' => [param['name']], 'on' => param['consumed_on'] }
            end
          end
        end
        @types.each do |struct, struct_entry|
          (struct_entry['fields'] || []).each do |field|
            next unless handle_out?(field, name)

            (users[struct] || []).each do |path, function|
              next unless function['name']

              created << { 'function' => function['name'], 'path' => [path[/\((\w+)\)\z/, 1], field['name']] }
            end
          end
        end
        entry['created_by'] = created
        entry['destroyed_by'] = destroyed
        entry['consumed_by'] = consumed unless consumed.empty?
        error(name, 'no function creates this handle') if created.empty?
        error(name, 'no function destroys this handle; mark its destroy parameter TA_DESTROYS') if destroyed.empty?
      end
    end

    def handle?(slot, name) = slot['shape'] == 'handle' && named(resolve(slot['type'])['pointer']) == name
    def handle_out?(slot, name) = slot['shape'] == 'handle_out' && named(resolve(resolve(slot['type'])['pointer'])['pointer']) == name

    def add_doc(entry, node)
      doc = node && @ast.doc(node)
      entry['doc'] = doc if doc
    end

    # count_of, capacity_of, and needed_of on the members that lists and
    # buffers name.
    def apply_derived
      @derived.each do |path, member, key, value|
        target = find_slot(path, member) or next error(path, "cannot find #{member} for #{key}")
        target[key] = value
      end
    end

    def find_slot(path, member)
      if (function, = path.match(/\A(\w+)\((\w+)\)\z/)&.captures)
        return @functions.find { |each| each['name'] == function }&.dig('params')&.find { |each| each['name'] == member }
      end

      parts = path.split('.')
      fields = @types.dig(parts.first, 'fields')
      parts[1...-1].each do |part|
        fields = fields&.find { |each| each['name'] == part }&.dig('type', 'record', 'fields')
      end
      fields&.find { |each| each['name'] == member }
    end
  end
end
