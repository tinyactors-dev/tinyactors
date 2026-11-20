# frozen_string_literal: true

# Reads struct layouts and macros from the debug information of an object
# or archive, through the pinned llvm-dwarfdump. `mise run layout` uses it
# on the `debuginfo` build (mise/core.rb).
#
#   dwarf = Dwarf.read(['build/debuginfo/lib/libtinyactors_system.a'])
#   dwarf.struct('ta_actor')   # => Dwarf::Struct (size, members)
#   dwarf.macro('TA_ALIGN_MAX') # => 8
#
# A type has one layout in every object it appears in; a type whose size or
# members differ between two objects is an error.
require 'toolchain'

module Dwarf
  Error = Class.new(StandardError)

  # A member: its name, type name, byte offset and size in bytes. A bit field
  # has `bits` (its width) and `bit_offset` (from the start of the struct).
  Member = Struct.new(:name, :type, :offset, :size, :bits, :bit_offset, keyword_init: true)

  # A struct or union: its name, size, members in order, and where it is declared.
  Struct = Struct.new(:name, :kind, :size, :members, :declared, keyword_init: true) do
    # The bytes no member covers: [offset, length] pairs, the tail included.
    def holes
      return [] if kind == :union

      holes = []
      covered = 0
      members.each do |member|
        start = member.offset
        holes << [covered, start - covered] if start > covered
        covered = [covered, member.offset + member.size].max
      end
      holes << [covered, size - covered] if size > covered
      holes
    end

    def padding = holes.sum { |_, length| length }
  end

  # One debugging information entry: its tag, depth in the tree, attributes
  # (name => raw text) and children.
  Entry = ::Struct.new(:offset, :tag, :depth, :attributes, :children)

  # The structs and macros of `paths` (objects or archives).
  def self.read(paths)
    dump = Toolchain::Tool[:llvm_dwarfdump]
    info = paths.map { |path| dump.run('--debug-info', path.to_s) }.join("\n")
    macros = paths.map { |path| dump.run('--debug-macro', path.to_s) }.join("\n")
    Reader.new(info, macros)
  end

  class Reader
    def initialize(info, macros)
      @structs = {}
      info.split(/^(?=\S.*:\s+file format )/).each { |object| read_object(object) }
      @macros = {}
      macros.scan(/macro: (\w+) (.+)$/) { |name, value| @macros[name] ||= value.strip }
    end

    def struct(name)
      @structs.fetch(name) { raise Error, "no struct #{name} in the debug information" }
    end

    def struct?(name) = @structs.key?(name)

    # A macro's value as an integer (a C integer literal, its suffix dropped).
    def macro(name)
      value = @macros.fetch(name) { raise Error, "no macro #{name} in the debug information (built with -fdebug-macro?)" }
      Integer(value.sub(/[uUlL]+\z/, '').delete_prefix('(').delete_suffix(')'))
    end

    private

    # One object's entries, by offset; each named struct (or a typedef of an
    # unnamed one) is recorded once.
    def read_object(text)
      entries = {}
      stack = []
      current = nil
      text.each_line do |line|
        if (match = line.match(/^(0x\h+):(\s+)(DW_TAG_\w+|NULL)/))
          depth = match[2].length
          stack.pop while stack.any? && stack.last.depth >= depth
          current = Entry.new(match[1].hex, match[3], depth, {}, [])
          stack.last&.children&.push(current)
          stack.push(current)
          entries[current.offset] = current
        elsif current && (match = line.match(/^\s+(DW_AT_\w+)\s+\((.*)\)\s*$/))
          current.attributes[match[1]] = match[2]
        end
      end
      entries.each_value do |entry|
        case entry.tag
        when 'DW_TAG_structure_type', 'DW_TAG_union_type'
          name = string(entry, 'DW_AT_name')
          record(name, entry, entries) if name && entry.attributes.key?('DW_AT_byte_size')
        when 'DW_TAG_typedef'
          target = entries[reference(entry)]
          next unless target && %w[DW_TAG_structure_type DW_TAG_union_type].include?(target.tag)
          next if string(target, 'DW_AT_name') || !target.attributes.key?('DW_AT_byte_size')

          record(string(entry, 'DW_AT_name'), target, entries)
        end
      end
    end

    def record(name, entry, entries)
      layout = Dwarf::Struct.new(
        name: name, kind: entry.tag == 'DW_TAG_union_type' ? :union : :struct,
        size: number(entry, 'DW_AT_byte_size'),
        members: entry.children.select { |child| child.tag == 'DW_TAG_member' }.map { |child| member(child, entries) },
        declared: declared(entry)
      )
      known = @structs[name]
      if known.nil?
        @structs[name] = layout
      elsif known.size != layout.size || known.members.map(&:to_h) != layout.members.map(&:to_h)
        raise Error, "#{name} has two layouts: #{known.size} B (#{known.declared}) and #{layout.size} B (#{layout.declared})"
      end
    end

    def member(entry, entries)
      type = entries[reference(entry)]
      size = size_of(type, entries)
      if entry.attributes.key?('DW_AT_bit_size')
        bits = number(entry, 'DW_AT_bit_size')
        bit_offset = number(entry, 'DW_AT_data_bit_offset')
        return Member.new(name: string(entry, 'DW_AT_name') || '(unnamed)', type: type_name(type, entries),
                          offset: bit_offset / 8, size: ((bit_offset % 8) + bits + 7) / 8, bits: bits, bit_offset: bit_offset)
      end
      Member.new(name: string(entry, 'DW_AT_name') || '(unnamed)', type: type_name(type, entries),
                 offset: entry.attributes.key?('DW_AT_data_member_location') ? number(entry, 'DW_AT_data_member_location') : 0,
                 size: size)
    end

    # A type's size in bytes, through typedefs, qualifiers and arrays.
    def size_of(type, entries)
      raise Error, 'a member without a type' unless type
      return number(type, 'DW_AT_byte_size') if type.attributes.key?('DW_AT_byte_size')

      case type.tag
      when 'DW_TAG_typedef', 'DW_TAG_const_type', 'DW_TAG_volatile_type', 'DW_TAG_atomic_type', 'DW_TAG_restrict_type'
        size_of(entries[reference(type)], entries)
      when 'DW_TAG_array_type'
        count = type.children.select { |child| child.tag == 'DW_TAG_subrange_type' }.reduce(1) do |product, range|
          length = if range.attributes.key?('DW_AT_count') then number(range, 'DW_AT_count')
                   elsif range.attributes.key?('DW_AT_upper_bound') then number(range, 'DW_AT_upper_bound') + 1
                   else 0 # a flexible array member
                   end
          product * length
        end
        count * size_of(entries[reference(type)], entries)
      when 'DW_TAG_pointer_type' then 8
      else raise Error, "no size for #{type.tag} at 0x#{type.offset.to_s(16)}"
      end
    end

    def type_name(type, entries)
      return 'void' unless type

      name = string(type, 'DW_AT_name')
      case type.tag
      when 'DW_TAG_pointer_type' then "#{type_name(entries[reference(type)], entries)} *"
      when 'DW_TAG_const_type' then "const #{type_name(entries[reference(type)], entries)}"
      when 'DW_TAG_volatile_type' then "volatile #{type_name(entries[reference(type)], entries)}"
      when 'DW_TAG_array_type'
        counts = type.children.select { |child| child.tag == 'DW_TAG_subrange_type' }.map do |range|
          if range.attributes.key?('DW_AT_count') then number(range, 'DW_AT_count')
          elsif range.attributes.key?('DW_AT_upper_bound') then number(range, 'DW_AT_upper_bound') + 1
          else ''
          end
        end
        "#{type_name(entries[reference(type)], entries)}#{counts.map { |c| "[#{c}]" }.join}"
      when 'DW_TAG_structure_type' then name || '(struct)'
      when 'DW_TAG_union_type' then name || '(union)'
      else name || type.tag.delete_prefix('DW_TAG_')
      end
    end

    def declared(entry)
      file = string(entry, 'DW_AT_decl_file')
      line = entry.attributes['DW_AT_decl_line']
      file ? "#{file.sub(%r{\A.*?/tinyactors-core/}, '')}:#{line}" : '?'
    end

    def string(entry, attribute)
      entry.attributes[attribute]&.[](/"((?:[^"\\]|\\.)*)"/, 1)
    end

    def number(entry, attribute)
      Integer(entry.attributes.fetch(attribute)[/\A(0x\h+|\d+)/, 1])
    end

    def reference(entry)
      entry.attributes['DW_AT_type']&.[](/\A0x\h+/)&.hex
    end
  end
end
