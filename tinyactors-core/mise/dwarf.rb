# frozen_string_literal: true

# Reads struct layouts, macros and functions from the debug information of
# an object or archive, through the pinned llvm-dwarfdump. `mise run layout`
# and `mise run atlas` use it on the `debuginfo` build (mise/core.rb).
#
#   dwarf = Dwarf.read(['build/debuginfo/lib/libtinyactors_system.a'])
#   dwarf.struct('ta_actor')   # => Dwarf::Struct (size, members)
#   dwarf.macro('TA_ALIGN_MAX') # => 8
#   dwarf.functions            # => [Dwarf::Function] (calls, inlines)
#
# A type has one layout in every object it appears in; a type whose size or
# members differ between two objects is an error.
#
# A function is known by its name and the file that declares it: a static
# inline helper of a header, defined in many objects, is one function, with
# the calls and inlines of every copy. Its calls are the call sites that
# name their callee (DW_AT_call_origin): a call through a pointer names
# none, nor does a tail call whose target is in a register. A call made
# inside an inlined function is that function's, as the source has it.
require 'toolchain'

module Dwarf
  Error = Class.new(StandardError)

  # A member: its name, type name, byte offset and size in bytes. A bit field
  # has `bits` (its width) and `bit_offset` (from the start of the struct).
  Member = Struct.new(:name, :type, :offset, :size, :bits, :bit_offset, keyword_init: true)

  # A struct or union: its name, size, members in order, and where it is declared.
  # `archive` is the object or archive it was first read from.
  Struct = Struct.new(:name, :kind, :size, :members, :declared, :archive, keyword_init: true) do
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

  # A function: its name, the absolute path of the file that declares it and
  # the line, whether it is external, its size in bytes (the largest of its
  # out-of-line copies; nil when every call inlines it, `inline` then true),
  # the archive it was first read from, and the keys ([name, file]) of the
  # functions it calls and inlines, each a function of `functions`.
  Function = ::Struct.new(:name, :file, :line, :external, :size, :inline, :archive, :calls, :inlines,
                          keyword_init: true) do
    def key = [name, file]
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
      @functions = {}
      info.split(/^(?=\S.*:\s+file format )/).each { |object| read_object(object) }
      @macros = {}
      macros.scan(/macro: (\w+) (.+)$/) { |name, value| @macros[name] ||= value.strip }
    end

    def struct(name)
      @structs.fetch(name) { raise Error, "no struct #{name} in the debug information" }
    end

    def struct?(name) = @structs.key?(name)

    def structs = @structs.values

    # The functions defined in the objects read (out of line, or inline
    # only), their calls and inlines resolved to functions among them: a
    # callee declared elsewhere (another object's external function) is
    # found by its name; one defined in no object read (the C library) is
    # left out.
    def functions
      @resolved ||= begin
        by_name = @functions.values.select(&:defined).group_by(&:name)
        resolve = lambda do |key|
          known = @functions[key]
          next key if known&.defined

          candidates = by_name.fetch(key[0], [])
          candidates = candidates.select(&:external) if candidates.size > 1
          candidates.size == 1 ? candidates[0].key : nil
        end
        @functions.values.select(&:defined).map do |raw|
          Function.new(name: raw.name, file: raw.file, line: raw.line, external: raw.external, size: raw.size,
                       inline: raw.size.nil?, archive: raw.archive,
                       calls: raw.calls.filter_map(&resolve).uniq - [raw.key],
                       inlines: raw.inlines.filter_map(&resolve).uniq - [raw.key])
        end
      end
    end

    # A macro's value as an integer (a C integer literal, its suffix dropped).
    def macro(name)
      value = @macros.fetch(name) { raise Error, "no macro #{name} in the debug information (built with -fdebug-macro?)" }
      Integer(value.sub(/[uUlL]+\z/, '').delete_prefix('(').delete_suffix(')'))
    end

    private

    # One object's entries, by offset; each named struct (or a typedef of an
    # unnamed one) is recorded once.
    def read_object(text)
      archive = text[/\A(.*?)(?:\([^()]*\))?:\s+file format /, 1]
      entries = {}
      parents = {}
      stack = []
      current = nil
      open = nil # an attribute whose value goes on over the next lines (a range list), to a blank line
      text.each_line do |line|
        if open && !line.match?(/^(0x\h+:|\s+DW_AT_|\s*$)/)
          current.attributes[open] << line
        elsif (match = line.match(/^(0x\h+):(\s+)(DW_TAG_\w+|NULL)/))
          depth = match[2].length
          stack.pop while stack.any? && stack.last.depth >= depth
          current = Entry.new(match[1].hex, match[3], depth, {}, [])
          if stack.last
            stack.last.children.push(current)
            parents[current.offset] = stack.last
          end
          stack.push(current)
          entries[current.offset] = current
          open = nil
        elsif current && (match = line.match(/^\s+(DW_AT_\w+)\s+\((.*)\)\s*$/))
          open = nil
          current.attributes[match[1]] = match[2]
        elsif current && (match = line.match(/^\s+(DW_AT_\w+)\s+\((.*)$/))
          current.attributes[match[1]] = +"#{match[2]}\n"
          open = match[1]
        end
      end
      read_functions(entries, parents, archive)
      entries.each_value do |entry|
        case entry.tag
        when 'DW_TAG_structure_type', 'DW_TAG_union_type'
          name = string(entry, 'DW_AT_name')
          record(name, entry, entries, archive) if name && entry.attributes.key?('DW_AT_byte_size')
        when 'DW_TAG_typedef'
          target = entries[reference(entry)]
          next unless target && %w[DW_TAG_structure_type DW_TAG_union_type].include?(target.tag)
          next if string(target, 'DW_AT_name') || !target.attributes.key?('DW_AT_byte_size')

          record(string(entry, 'DW_AT_name'), target, entries, archive)
        end
      end
    end

    # Raw functions of one object, by [name, file]: `defined` once an object
    # holds its code or its inline definition; calls and inlines as keys,
    # resolved in `functions`.
    def read_functions(entries, parents, archive)
      entries.each_value do |entry|
        case entry.tag
        when 'DW_TAG_subprogram'
          next if entry.attributes.key?('DW_AT_declaration')

          size = code_size(entry)
          next unless size || entry.attributes.key?('DW_AT_inline')

          function = function_of(entry, entries, archive) or next
          function.defined = true
          function.size = [function.size, size].compact.max
        when 'DW_TAG_call_site', 'DW_TAG_GNU_call_site'
          target = entries[reference(entry, 'DW_AT_call_origin')] or next
          caller = caller_of(entry, entries, parents, archive) or next
          callee = origin(target, entries) or next
          caller.calls << callee
        when 'DW_TAG_inlined_subroutine'
          target = entries[reference(entry, 'DW_AT_abstract_origin')] or next
          caller = caller_of(entry, entries, parents, archive) or next
          callee = origin(target, entries) or next
          caller.inlines << callee
        end
      end
    end

    RawFunction = ::Struct.new(:name, :file, :line, :external, :size, :archive, :calls, :inlines, :defined) do
      def key = [name, file]
    end

    # The raw function an entry (a subprogram or an inlined subroutine) is
    # an instance of, made on first sight.
    def function_of(entry, entries, archive)
      key = origin(entry, entries) or return nil
      named = named(entry, entries)
      @functions[key] ||= RawFunction.new(key[0], key[1], named.attributes['DW_AT_decl_line']&.to_i,
                                          named.attributes.key?('DW_AT_external'), nil, archive, [], [], false)
    end

    # The function whose source makes the call or inlines: the nearest
    # subprogram or inlined subroutine around the entry.
    def caller_of(entry, entries, parents, archive)
      around = parents[entry.offset]
      around = parents[around.offset] while around && !%w[DW_TAG_subprogram DW_TAG_inlined_subroutine].include?(around.tag)
      around && function_of(around, entries, archive)
    end

    # [name, file] of the function an entry stands for, through its
    # abstract origin or specification; nil for one without a name or file.
    def origin(entry, entries)
      named = named(entry, entries) or return nil
      name = string(named, 'DW_AT_name')
      file = string(named, 'DW_AT_decl_file')
      name && file ? [name, file] : nil
    end

    def named(entry, entries)
      seen = 0
      while entry && !entry.attributes.key?('DW_AT_name') && seen < 8
        entry = entries[reference(entry, 'DW_AT_abstract_origin') || reference(entry, 'DW_AT_specification')]
        seen += 1
      end
      entry
    end

    # Bytes of machine code: high_pc less low_pc, or the sum of its ranges.
    def code_size(entry)
      low = entry.attributes['DW_AT_low_pc']&.[](/0x\h+/)&.hex
      high = entry.attributes['DW_AT_high_pc']&.[](/0x\h+/)&.hex
      return high - low if low && high
      return nil unless (ranges = entry.attributes['DW_AT_ranges'])

      ranges.scan(/\[0x(\h+), 0x(\h+)\)/).sum { |from, to| to.hex - from.hex }
    end

    def record(name, entry, entries, archive)
      layout = Dwarf::Struct.new(archive: archive,
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

    def reference(entry, attribute = 'DW_AT_type')
      entry.attributes[attribute]&.[](/\A0x\h+/)&.hex
    end
  end
end
