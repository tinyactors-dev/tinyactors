# frozen_string_literal: true

# The schema generator: a JSON Schema (2020-12) in, C tables out, which the
# hand-written checker (tools/tinyactors/schema_check.c) walks to check a
# message's CBOR tokens against the schema. The tables are data, not code: a
# change to the schema changes data, and a change to what can be checked
# changes this file and the checker together.
#
# It supports a small set of keywords, each a check of one CBOR token
# (docs/proposals/cli-json-rpc.md, section 7.8), and refuses every other with
# its JSON pointer, so a check never silently goes missing:
#
#   type, properties, required, additionalProperties (false only), enum and
#   const (strings, integers, booleans), minimum and maximum (integers within
#   64 bits), minLength, maxLength, minItems, maxItems, items (one schema),
#   pattern (only those of PATTERNS, each a function of the checker), oneOf
#   (its branches told apart by type, by a member's const, or by a member
#   only one branch takes), not: {required: [a, b]}, $ref to #/$defs/...
#   without a cycle, true; and, as documentation only, $schema, $id, $defs,
#   title, description, $comment, examples, default.
#
# Usage, as the build runs it (mise/core.rb):
#   generate.rb SCHEMA OUTPUT_DIRECTORY NAME      the tables of one schema
#   generate.rb --fixtures TESTS OUTPUT_DIRECTORY  the fixtures' tables and
#                                                  documents, for test_schema_check
require 'json'
require 'pathname'

module Schema
  # A keyword the generator does not support, or a schema it cannot check.
  class Unsupported < StandardError; end

  TYPES = %w[object array string integer number boolean null].freeze
  TYPE_BITS = TYPES.each_with_index.to_h { |type, index| [type, 1 << index] }.freeze
  ALL_TYPES = TYPE_BITS.values.sum
  DOCUMENTATION = %w[$schema $id $defs title description $comment examples default].freeze
  OBJECT_KEYWORDS = %w[properties required additionalProperties not].freeze
  ASSERTIONS = %w[type properties required additionalProperties enum const minimum maximum minLength maxLength
                  minItems maxItems items pattern oneOf not $ref].freeze
  # The patterns the checker implements, each by a C function: the command
  # carries no regular-expression engine. Read as ECMA-262 reads them (`$`
  # at the end of the text only).
  PATTERNS = {
    '^[A-Za-z0-9_-]*$' => 'BASE64URL',
    '^[ \t]*([0-9]+(\.[0-9]*)?|\.[0-9]+)(ms|s)[ \t]*$' => 'DURATION',
    '^[0-9a-f]*$' => 'HEX'
  }.freeze
  PATTERN_NUMBERS = { 'BASE64URL' => 1, 'DURATION' => 2, 'HEX' => 3 }.freeze
  INTEGER_LOW = -(2**64)
  INTEGER_HIGH = (2**64) - 1

  Node = Struct.new(:pointer, :types, :members, :closed, :values, :minimum, :maximum, :min_length, :max_length,
                    :min_items, :max_items, :pattern, :items, :choices, :exclusive, keyword_init: true)
  Member = Struct.new(:name, :node, :required)
  # A branch of a oneOf and what selects it: the token's type among `types`,
  # then, when `key` is set, that member present (and equal to `value`, when
  # set).
  Choice = Struct.new(:node, :types, :key, :value)
  Entry = Struct.new(:name, :params, :result)

  # A schema compiled into nodes. Node 0 is the document's root; each $def
  # has a node (one that is only a $ref has its target's), and so has each
  # schema inside them.
  class Compiled
    attr_reader :nodes, :definitions, :methods, :notifications, :file

    def initialize(document, file:)
      @document = document
      @file = file
      @nodes = []
      @definitions = {}
      @compiling = []
      definitions = document.is_a?(Hash) ? document['$defs'] : nil
      refuse('', '$defs', 'is not an object') unless definitions.nil? || definitions.is_a?(Hash)
      @nodes << nil
      @nodes[0] = build(document, '')
      (definitions || {}).each_key { |name| definition(name) }
      @methods = entries('request')
      @notifications = entries('notification')
    end

    def node_of(name) = @definitions.fetch(name)

    private

    def refuse(pointer, keyword, what)
      location = keyword ? "#{pointer}/#{escape(keyword)}" : pointer
      raise Unsupported, "#{@file}##{location}: #{keyword ? "#{keyword} " : ''}#{what}"
    end

    def escape(token) = token.to_s.gsub('~', '~0').gsub('/', '~1')

    # The node of $defs/<name>, compiled once.
    def definition(name)
      return @definitions[name] if @definitions.key?(name)

      pointer = "/$defs/#{escape(name)}"
      raise Unsupported, "#{@file}##{pointer}: a cycle of $refs (#{(@compiling + [name]).join(' -> ')})" if @compiling.include?(name)

      @compiling << name
      schema = @document['$defs'][name]
      if schema.is_a?(Hash) && schema.key?('$ref') && (schema.keys - DOCUMENTATION - ['$ref']).empty?
        index = reference(schema['$ref'], pointer)
      else
        index = @nodes.size
        @nodes << nil
        @nodes[index] = build(schema, pointer)
      end
      @compiling.pop
      @definitions[name] = index
    end

    def reference(target, pointer)
      match = %r{\A#/\$defs/([^/]+)\z}.match(target.to_s)
      refuse(pointer, '$ref', "#{target.inspect} is not a reference to #/$defs/...") unless match
      name = match[1].gsub('~1', '/').gsub('~0', '~')
      refuse(pointer, '$ref', "#{target.inspect} names no definition") unless @document['$defs']&.key?(name)
      definition(name)
    end

    # The node index of a schema met inside another.
    def compile(schema, pointer)
      if schema.is_a?(Hash) && schema.key?('$ref')
        others = schema.keys - DOCUMENTATION - ['$ref']
        refuse(pointer, others.first, 'beside $ref is not supported') unless others.empty?
        return reference(schema['$ref'], pointer)
      end
      index = @nodes.size
      @nodes << nil
      @nodes[index] = build(schema, pointer)
      index
    end

    def build(schema, pointer)
      node = Node.new(pointer: pointer, types: 0, members: [], closed: false, values: [], choices: [])
      return node if schema == true
      refuse(pointer, nil, 'the schema false is not supported') if schema == false
      refuse(pointer, nil, 'is not a schema') unless schema.is_a?(Hash)

      schema.each_key do |keyword|
        next if DOCUMENTATION.include?(keyword) || ASSERTIONS.include?(keyword)

        refuse(pointer, keyword, 'is not supported')
      end
      if schema.key?('$ref')
        others = schema.keys - DOCUMENTATION - ['$ref']
        refuse(pointer, others.first, 'beside $ref is not supported') unless others.empty?
      end
      if pointer != '' && schema.key?('$defs')
        refuse(pointer, '$defs', 'is supported at the root only')
      end
      if schema.key?('oneOf') && (blocked = (schema.keys & OBJECT_KEYWORDS).first)
        refuse(pointer, blocked, 'beside oneOf is not supported')
      end
      types(node, schema['type'], pointer) if schema.key?('type')
      object(node, schema, pointer)
      values(node, schema, pointer)
      bounds(node, schema, pointer)
      if schema.key?('pattern')
        name = PATTERNS[schema['pattern']]
        refuse(pointer, 'pattern', "#{schema['pattern'].inspect} not supported (the checker implements #{PATTERNS.keys.map(&:inspect).join(', ')})") unless name
        node.pattern = name
      end
      if schema.key?('items')
        refuse(pointer, 'items', 'must be one schema') unless schema['items'].is_a?(Hash) || schema['items'] == true
        node.items = compile(schema['items'], "#{pointer}/items")
      end
      one_of(node, schema['oneOf'], pointer) if schema.key?('oneOf')
      node
    end

    def types(node, type, pointer)
      list = type.is_a?(Array) ? type : [type]
      if list.empty? || list.uniq.size != list.size || !list.all? { |name| TYPES.include?(name) }
        refuse(pointer, 'type', "#{type.inspect} is not a type or a list of them (#{TYPES.join(', ')})")
      end
      node.types = list.sum { |name| TYPE_BITS[name] }
    end

    def object(node, schema, pointer)
      if schema.key?('properties')
        properties = schema['properties']
        refuse(pointer, 'properties', 'is not an object') unless properties.is_a?(Hash)
        properties.each do |name, property|
          node.members << Member.new(name, compile(property, "#{pointer}/properties/#{escape(name)}"), false)
        end
      end
      if schema.key?('required')
        required = schema['required']
        unless required.is_a?(Array) && required.all?(String) && required.uniq.size == required.size
          refuse(pointer, 'required', 'is not a list of distinct names')
        end
        required.each do |name|
          member = node.members.find { |candidate| candidate.name == name }
          refuse(pointer, 'required', "names #{name.inspect}, which properties does not") unless member
          member.required = true
        end
      end
      if schema.key?('additionalProperties')
        refuse(pointer, 'additionalProperties', 'is supported as false only') unless schema['additionalProperties'] == false
        node.closed = true
      end
      return unless schema.key?('not')

      excluded = schema['not']
      names = excluded.is_a?(Hash) && excluded.keys == ['required'] ? excluded['required'] : nil
      unless names.is_a?(Array) && names.size == 2 && names.all?(String) && names.uniq.size == 2
        refuse(pointer, 'not', 'is supported as {"required": [a, b]} only')
      end
      names.each do |name|
        next if node.members.any? { |member| member.name == name }

        refuse(pointer, 'not', "names #{name.inspect}, which properties does not")
      end
      node.exclusive = names
    end

    def value_of(value, pointer, keyword)
      case value
      when String, true, false then value
      when Integer
        refuse(pointer, keyword, "#{value} is past 64 bits") unless value.between?(INTEGER_LOW, INTEGER_HIGH)
        value
      else refuse(pointer, keyword, "#{value.inspect} is not a string, an integer or a boolean")
      end
    end

    def values(node, schema, pointer)
      refuse(pointer, 'const', 'beside enum is not supported') if schema.key?('const') && schema.key?('enum')
      if schema.key?('const')
        node.values = [value_of(schema['const'], pointer, 'const')]
      elsif schema.key?('enum')
        list = schema['enum']
        refuse(pointer, 'enum', 'is not a list of values') unless list.is_a?(Array) && !list.empty?
        refuse(pointer, 'enum', 'repeats a value') unless list.uniq.size == list.size
        node.values = list.map { |value| value_of(value, pointer, 'enum') }
      end
    end

    def bounds(node, schema, pointer)
      %w[minimum maximum].each do |keyword|
        next unless schema.key?(keyword)

        value = schema[keyword]
        refuse(pointer, keyword, "#{value.inspect} is not an integer") unless value.is_a?(Integer)
        refuse(pointer, keyword, "#{value} is past 64 bits") unless value.between?(INTEGER_LOW, INTEGER_HIGH)
        node[keyword.to_sym] = value
      end
      { 'minLength' => :min_length, 'maxLength' => :max_length, 'minItems' => :min_items,
        'maxItems' => :max_items }.each do |keyword, field|
        next unless schema.key?(keyword)

        value = schema[keyword]
        refuse(pointer, keyword, "#{value.inspect} is not an integer from 0") unless value.is_a?(Integer) && value.between?(0, INTEGER_HIGH)
        node[field] = value
      end
    end

    # ---- oneOf: what tells its branches apart --------------------------------

    def one_of(node, branches, pointer)
      refuse(pointer, 'oneOf', 'is not a list of schemas') unless branches.is_a?(Array) && !branches.empty?
      indices = branches.each_with_index.map { |branch, index| compile(branch, "#{pointer}/oneOf/#{index}") }
      node.choices = selectors(indices, node.types.zero? ? ALL_TYPES : node.types, pointer)
    end

    # The types a value of `index` can have (all, when it says nothing).
    def types_of(index)
      node = @nodes[index]
      return node.types unless node.types.zero?
      return node.choices.map { |choice| types_of(choice.node) }.reduce(:|) unless node.choices.empty?

      ALL_TYPES
    end

    def overlap?(first, second)
      widen = ->(bits) { bits & TYPE_BITS['number'] != 0 ? bits | TYPE_BITS['integer'] : bits }
      widen.call(first).anybits?(widen.call(second))
    end

    # The members a value of `index` must have.
    def required_of(index)
      node = @nodes[index]
      own = node.members.select(&:required).map(&:name)
      return own if node.choices.empty?

      own | node.choices.map { |choice| required_of(choice.node) }.reduce(:&)
    end

    def const_of(index, key)
      node = @nodes[index]
      member = node.members.find { |candidate| candidate.name == key }
      return nil unless member

      values = @nodes[member.node].values
      values.size == 1 && values.first.is_a?(String) ? values.first : nil
    end

    # Whether a value of `index` may have the member `key`.
    def allows?(index, key)
      node = @nodes[index]
      return node.choices.any? { |choice| allows?(choice.node, key) } unless node.choices.empty?
      return true unless node.closed

      node.members.any? { |member| member.name == key }
    end

    # The branches' selectors; a value reaches them with one of `within`,
    # the types the oneOf's own node allows.
    def selectors(indices, within, pointer)
      types = indices.map { |index| types_of(index) & within }
      overlapping = indices.each_index.select do |i|
        indices.each_index.any? { |j| i != j && overlap?(types[i], types[j]) }
      end
      choices = indices.each_with_index.map { |index, i| Choice.new(index, types[i], nil, nil) }
      return choices if overlapping.empty?

      unless overlapping.all? { |i| types[i] == TYPE_BITS['object'] }
        refuse(pointer, 'oneOf', 'has branches of one type that are not objects told apart by a member')
      end
      group = overlapping.map { |i| indices[i] }
      if (key = constant_key(group))
        overlapping.each { |i| choices[i].key, choices[i].value = key, const_of(indices[i], key) }
        return choices
      end
      overlapping.each do |i|
        key = required_of(indices[i]).find do |name|
          overlapping.all? { |j| j == i || !allows?(indices[j], name) }
        end
        refuse(pointer, 'oneOf', 'has no discriminator: no member with a different const in each branch, and no member only one branch takes') unless key
        choices[i].key = key
      end
      choices
    end

    # A member every branch requires with a const of its own.
    def constant_key(group)
      required_of(group.first).find do |key|
        consts = group.map { |index| required_of(index).include?(key) ? const_of(index, key) : nil }
        consts.none?(&:nil?) && consts.uniq.size == consts.size
      end
    end

    # The methods of $defs/<name>: a oneOf told apart by `method`, each
    # branch's params and $defs/<method>.result.
    def entries(name)
      return [] unless @definitions.key?(name)

      node = @nodes[@definitions[name]]
      node.choices.filter_map do |choice|
        next nil unless choice.key == 'method'

        branch = @nodes[choice.node]
        params = branch.members.find { |member| member.name == 'params' }
        Entry.new(choice.value, params&.node, @definitions["#{choice.value}.result"])
      end
    end
  end

  def self.parse(text, file)
    JSON.parse(text, allow_duplicate_key: false)
  rescue JSON::ParserError => e
    raise Unsupported, "#{file}: not JSON: #{e.message}"
  end

  # ---- CBOR, as the server's JSON reader makes it ----------------------------

  # A JSON value as the CBOR the command's JSON reader writes (RFC 8949,
  # section 6.2): integral numbers within 64 bits as integers, others as
  # floats; text, arrays and maps as they are.
  module CBOR
    module_function

    def encode(value, out = +''.b)
      case value
      when Hash
        head(5, value.size, out)
        value.each do |key, item|
          encode(key, out)
          encode(item, out)
        end
      when Array
        head(4, value.size, out)
        value.each { |item| encode(item, out) }
      when String
        bytes = value.b
        head(3, bytes.bytesize, out)
        out << bytes
      when true then out << "\xf5".b
      when false then out << "\xf4".b
      when nil then out << "\xf6".b
      when Integer then integer(value, out)
      when Float
        if value.finite? && value == value.floor && !(value.zero? && (1.0 / value).negative?) &&
           value.to_i.between?(INTEGER_LOW, INTEGER_HIGH)
          integer(value.to_i, out)
        else
          out << "\xfb".b << [value].pack('G')
        end
      else raise ArgumentError, "not a JSON value: #{value.inspect}"
      end
      out
    end

    def integer(value, out)
      value.negative? ? head(1, -1 - value, out) : head(0, value, out)
    end

    def head(major, argument, out)
      top = major << 5
      if argument < 24 then out << [top | argument].pack('C')
      elsif argument < 2**8 then out << [top | 24, argument].pack('CC')
      elsif argument < 2**16 then out << [top | 25, argument].pack('Cn')
      elsif argument < 2**32 then out << [top | 26, argument].pack('CN')
      else out << [top | 27, argument].pack('CQ>')
      end
    end
  end

  # ---- C ------------------------------------------------------------------------

  # The C a compiled schema becomes: its tables, under `prefix`.
  class Writer
    def initialize(compiled, prefix:, visibility: 'static ')
      @compiled = compiled
      @prefix = prefix
      @visibility = visibility
    end

    def self.text(value)
      escaped = value.b.each_byte.map do |byte|
        case byte
        when 0x22 then '\\"'
        when 0x5c then '\\\\'
        when 0x3f then '\\?'
        when 0x20..0x7e then byte.chr
        else format('\\%03o', byte)
        end
      end
      "\"#{escaped.join}\""
    end

    def self.view(value) = value.nil? ? '{0, 0}' : "{#{text(value)}, #{value.b.bytesize}u}"

    def self.integer(value)
      return '{0, 0, 0}' if value.nil?

      value.negative? ? "{UINT64_C(#{-1 - value}), 1, 0}" : "{UINT64_C(#{value}), 0, 0}"
    end

    def self.constant(name) = name.gsub(/([a-z0-9])([A-Z])/, '\1_\2').gsub(/[^A-Za-z0-9]+/, '_').upcase

    # The tables, and the tool_schema `name` over them (with `text`), of
    # `storage` ("static " for one the file keeps to itself).
    def tables(name, text: nil, storage: '')
      members = []
      values = []
      choices = []
      exclusives = []
      rows = @compiled.nodes.map do |node|
        member_start = members.size
        node.members.each { |member| members << member }
        value_start = values.size
        node.values.each { |value| values << value }
        choice_start = choices.size
        node.choices.each { |choice| choices << choice }
        exclusive = 'TA_NIL'
        if node.exclusive
          exclusive = "#{exclusives.size}u"
          exclusives << node.exclusive
        end
        node_row(node, member_start, value_start, choice_start, exclusive)
      end
      out = +''
      out << "#{@visibility}const tool_schema_node #{@prefix}_nodes[] = {\n#{rows.join(",\n")}\n};\n"
      out << array('tool_schema_member', 'members', members) do |member|
        "    {#{Writer.view(member.name)}, #{member.node}u, #{member.required ? 1 : 0}u}"
      end
      out << array('tool_schema_value', 'values', values) { |value| value_row(value) }
      out << array('tool_schema_choice', 'choices', choices) do |choice|
        "    {#{choice.node}u, #{choice.types}u, #{Writer.view(choice.key)}, #{choice.value.nil? ? 0 : 1}u, 0, " \
          "#{Writer.view(choice.value)}}"
      end
      out << array('tool_schema_exclusive', 'exclusives', exclusives) do |pair|
        "    {#{Writer.view(pair[0])}, #{Writer.view(pair[1])}}"
      end
      entries = @compiled.methods
      out << array('tool_schema_entry', 'methods', entries) do |entry|
        "    {#{Writer.view(entry.name)}, #{entry.params || 'TA_NIL'}, #{entry.result || 'TA_NIL'}}"
      end
      if text
        # bytes, not a string literal: C11 promises literals of 4095 bytes only
        out << "static const unsigned char #{@prefix}_text[] = {\n"
        rows = text.b.bytes.each_slice(16).map { |row| "    #{row.map { |byte| format('0x%02x', byte) }.join(', ')}" }
        out << rows.join(",\n") << "\n};\n"
      end
      out << "#{storage}const tool_schema #{name} = {\n"
      out << "    #{@prefix}_nodes, #{pointer('members', members)}, #{pointer('values', values)}, " \
             "#{pointer('choices', choices)},\n    #{pointer('exclusives', exclusives)}, " \
             "#{pointer('methods', entries)}, #{@compiled.nodes.size}u, #{entries.size}u,\n"
      out << (text ? "    {#{@prefix}_text, sizeof #{@prefix}_text}\n" : "    {0, 0}\n")
      out << "};\n"
      out
    end

    private

    def pointer(table, rows) = rows.empty? ? '0' : "#{@prefix}_#{table}"

    def array(type, table, rows, &)
      return '' if rows.empty?

      "#{@visibility}const #{type} #{@prefix}_#{table}[] = {\n#{rows.map(&).join(",\n")}\n};\n"
    end

    def node_row(node, member_start, value_start, choice_start, exclusive)
      checks = []
      checks << 'TOOL_SCHEMA_MINIMUM' unless node.minimum.nil?
      checks << 'TOOL_SCHEMA_MAXIMUM' unless node.maximum.nil?
      checks << 'TOOL_SCHEMA_MIN_LENGTH' unless node.min_length.nil?
      checks << 'TOOL_SCHEMA_MAX_LENGTH' unless node.max_length.nil?
      checks << 'TOOL_SCHEMA_MIN_ITEMS' unless node.min_items.nil?
      checks << 'TOOL_SCHEMA_MAX_ITEMS' unless node.max_items.nil?
      checks << 'TOOL_SCHEMA_CLOSED' if node.closed
      pattern = node.pattern ? "TOOL_SCHEMA_PATTERN_#{node.pattern}" : 'TOOL_SCHEMA_PATTERN_NONE'
      "    /* #{node.pointer.empty? ? '(root)' : node.pointer.gsub('*/', '* /')} */\n" \
        "    {#{node.types}u, #{checks.empty? ? '0' : checks.join(' | ')}, #{Writer.integer(node.minimum)}, " \
        "#{Writer.integer(node.maximum)},\n     " \
        "UINT64_C(#{node.min_length || 0}), UINT64_C(#{node.max_length || 0}), " \
        "UINT64_C(#{node.min_items || 0}), UINT64_C(#{node.max_items || 0}),\n     " \
        "#{pattern}, #{node.items || 'TA_NIL'}, #{member_start}u, #{node.members.size}u, #{value_start}u, " \
        "#{node.values.size}u, #{choice_start}u, #{node.choices.size}u, #{exclusive}, 0}"
    end

    def value_row(value)
      case value
      when String then "    {TOOL_SCHEMA_VALUE_TEXT, 0, {0, 0, 0}, #{Writer.view(value)}}"
      when true, false then "    {TOOL_SCHEMA_VALUE_BOOLEAN, 0, {#{value ? 1 : 0}u, 0, 0}, {0, 0}}"
      else "    {TOOL_SCHEMA_VALUE_INTEGER, 0, #{Writer.integer(value)}, {0, 0}}"
      end
    end
  end

  NOTICE = "/* Generated by tools/tinyactors/schema/generate.rb from %s: do not edit. */\n"

  # Writes `path` when its bytes change, so a build that regenerates the
  # same text recompiles nothing.
  def self.write(path, text)
    path = Pathname.new(path)
    return if path.file? && path.binread == text

    path.dirname.mkpath
    temporary = path.sub_ext("#{path.extname}.tmp")
    temporary.binwrite(text)
    temporary.rename(path)
  end

  # The tables of the schema at `source` as <name>.h and <name>.c in
  # `directory`, its symbols `tool_<name>` (`tool_schema_run`), its methods
  # and definitions as enums, and its bytes for tool_schema_text.
  def self.generate(source, directory, name)
    source = Pathname.new(source)
    text = source.binread
    compiled = Compiled.new(parse(text, source.basename.to_s), file: source.basename.to_s)
    symbol = "tool_#{name}"
    upper = symbol.upcase
    notice = format(NOTICE, source.basename)
    header = +"#{notice}#ifndef #{upper}_H\n#define #{upper}_H\n\n#include \"schema.h\"\n\n"
    header << "/* The schema's methods, in its order. */\ntypedef enum #{symbol}_method {\n"
    compiled.methods.each_with_index do |entry, index|
      header << "    #{upper}_METHOD_#{Writer.constant(entry.name)} = #{index},\n"
    end
    header << "    #{upper}_METHOD_COUNT = #{compiled.methods.size}\n} #{symbol}_method;\n\n"
    header << "/* Each definition's node, for tool_schema_check_message. */\ntypedef enum #{symbol}_node {\n"
    header << "    #{upper}_NODE_ROOT = 0"
    order = JSON.parse(text)['$defs']&.keys || []
    order.each { |definition| header << ",\n    #{upper}_NODE_#{Writer.constant(definition)} = #{compiled.node_of(definition)}" }
    header << "\n} #{symbol}_node;\n\nextern const tool_schema #{symbol};\n\n#endif\n"
    source_text = +"#{notice}#include \"#{name}.h\"\n\n"
    source_text << Writer.new(compiled, prefix: symbol).tables(symbol, text: text)
    write(Pathname.new(directory).join("#{name}.h"), header)
    write(Pathname.new(directory).join("#{name}.c"), source_text)
  end

  # ---- fixtures ------------------------------------------------------------------

  # A fixture case: tests/<case>/schema.json, accept/*.json and refuse/*.json.
  # Each document file is {"document": <value>, "method": <name>?, "error":
  # {"code", "reason", "path"}} (error: refuse only). With a method, the
  # document is that method's params, checked from "params"; without, the
  # whole document against the schema's root.
  Case = Struct.new(:name, :directory, :schema, :documents)
  Document = Struct.new(:name, :file, :value, :method, :error)

  def self.cases(tests)
    Pathname.new(tests).children.select(&:directory?).sort.filter_map do |directory|
      schema = directory.join('schema.json')
      next nil unless schema.file?

      documents = %w[accept refuse].flat_map do |kind|
        folder = directory.join(kind)
        next [] unless folder.directory?

        folder.glob('*.json').sort.map do |file|
          body = parse(file.read, file.to_s)
          raise Unsupported, "#{file}: not a fixture: #{body.keys}" unless body.is_a?(Hash) && body.key?('document')
          raise Unsupported, "#{file}: a refused document names its error" if kind == 'refuse' && !body['error'].is_a?(Hash)

          Document.new("#{kind}/#{file.basename('.json')}", file, body['document'], body['method'],
                       kind == 'refuse' ? body['error'] : nil)
        end
      end
      Case.new(directory.basename.to_s, directory, schema, documents)
    end
  end

  # The cases' tables, documents and expectations, as schema_fixtures.{h,c}.
  # A case whose schema.json the generator refuses is a case of its own: it
  # names the refusal it expects in expected-refusal.txt, and has no tables.
  def self.generate_fixtures(tests, directory)
    rows = []
    out = +"#{format(NOTICE, 'tools/tinyactors/schema/tests')}#include \"schema_fixtures.h\"\n\n"
    cases(tests).each_with_index do |test_case, number|
      next if test_case.directory.join('expected-refusal.txt').file?

      compiled = Compiled.new(parse(test_case.schema.read, test_case.schema.to_s), file: 'schema.json')
      prefix = "fixture_#{number}"
      out << Writer.new(compiled, prefix: prefix).tables(prefix, storage: 'static ')
      test_case.documents.each_with_index do |document, index|
        bytes = CBOR.encode(document.value)
        out << "static const unsigned char #{prefix}_document_#{index}[] = {#{bytes.bytes.map { |byte| format('0x%02x', byte) }.join(', ')}};\n"
        error = document.error || {}
        method = document.method ? compiled.methods.index { |entry| entry.name == document.method } : nil
        raise Unsupported, "#{document.file}: no method #{document.method.inspect} in the schema" if document.method && method.nil?

        rows << "    {#{Writer.view(test_case.name)}, #{Writer.view(document.name)}, &#{prefix}, " \
                "#{method.nil? ? 'TA_NIL' : "#{method}u"}, {#{prefix}_document_#{index}, sizeof #{prefix}_document_#{index}}, " \
                "#{document.error ? 0 : 1}u, #{error.fetch('code', 0)}, #{Writer.view(error['reason'])}, " \
                "#{Writer.view(error['path'])}}"
      end
    end
    out << "\nconst test_schema_fixture test_schema_fixtures[] = {\n#{rows.join(",\n")}\n};\n"
    out << "const uint32_t test_schema_fixture_count = #{rows.size}u;\n"
    header = <<~C
      #{format(NOTICE, 'tools/tinyactors/schema/tests')}#ifndef TEST_SCHEMA_FIXTURES_H
      #define TEST_SCHEMA_FIXTURES_H

      #include "schema.h"

      /* A document of a fixture case, and what checking it must give. */
      typedef struct test_schema_fixture {
          ta_string_view case_name, document_name;
          const tool_schema *schema;
          uint32_t method; /* its params against this method's; TA_NIL: the root */
          ta_bytes cbor;
          uint32_t accepted;
          int32_t code;
          ta_string_view reason, path;
      } test_schema_fixture;

      extern const test_schema_fixture test_schema_fixtures[];
      extern const uint32_t test_schema_fixture_count;

      #endif
    C
    write(Pathname.new(directory).join('schema_fixtures.h'), header)
    write(Pathname.new(directory).join('schema_fixtures.c'), out)
  end
end

if $PROGRAM_NAME == __FILE__
  begin
    if ARGV.first == '--fixtures'
      abort 'usage: generate.rb --fixtures TESTS OUTPUT_DIRECTORY' unless ARGV.size == 3
      Schema.generate_fixtures(ARGV[1], ARGV[2])
    else
      abort 'usage: generate.rb SCHEMA OUTPUT_DIRECTORY NAME' unless ARGV.size == 3
      Schema.generate(*ARGV)
    end
  rescue Schema::Unsupported => e
    warn "generate.rb: #{e.message}"
    exit 1
  end
end
