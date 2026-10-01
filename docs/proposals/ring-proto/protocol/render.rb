# frozen_string_literal: true

# Renders the catalog: `ruby render.rb markdown|json|c [OUT]`.
# `ruby render.rb all` writes generated/MESSAGES.md, generated/messages.json
# and generated/ring_proto.h next to this file.
require 'json'
require_relative 'dsl'

module RingProto
  module Render
    module_function

    def hex(code) = format('0x%04X', code)

    def type_name(model, type)
      model.enums.key?(type) ? "#{type} (#{model.enums[type].base})" : type.to_s
    end

    def anchor(id) = id.delete('.')  # GitHub drops the dot

    def markdown(model)
      out = +"# #{model.title}\n\n"
      out << "Generated from [`catalog.rb`](../catalog.rb) by `ruby render.rb all`. Do not edit.\n\n"
      model.notes.each { |n| out << "- #{n}\n" }
      out << "\n## Rings\n\n| Ring | Producer → consumer | Loss | When full |\n|---|---|---|---|\n"
      model.rings.each_value do |r|
        out << "| `#{r.name}` | #{r.producer} → #{r.consumer} | #{r.loss} | #{r.when_full} |\n"
      end
      out << "\n## Index\n\n| Code | Message | Summary | Fixed bytes |\n|---|---|---|---|\n"
      model.messages.each do |m|
        out << "| `#{hex(m.code)}` | [`#{m.id}`](##{anchor(m.id)}) | #{m.summary} | #{m.fixed_size} |\n"
      end
      out << "\n## Common header (32 bytes)\n\n| Offset | Field | Type | Meaning |\n|---|---|---|---|\n"
      HEADER.each { |f| out << "| #{f.offset} | `#{f.name}` | #{f.type} | #{f.doc} |\n" }
      out << "\nCommon flags: "
      out << COMMON_FLAGS.map { |f| "bit #{f.bit} `#{f.name}` (#{f.doc})" }.join('; ') << ".\n"
      model.messages.each { |m| out << message_markdown(model, m) }
      out << "\n## Enums\n"
      model.enums.each_value do |e|
        out << "\n### `#{e.name}` (#{e.base})\n\n#{e.doc}\n\n| Value | Name | Meaning |\n|---|---|---|\n"
        e.values.each { |v| out << "| #{v.value} | `#{v.name}` | #{v.doc} |\n" }
      end
      out << "\n## Scalar types\n\n| Type | Bytes | Meaning |\n|---|---|---|\n"
      model.types.each_value { |t| out << "| `#{t.name}` | #{t.size} | #{t.doc} |\n" }
      out
    end

    def message_markdown(model, msg)
      out = +"\n## #{msg.id}\n\n`#{hex(msg.code)}` on `#{msg.ring}`, #{msg.fixed_size} fixed bytes. #{msg.summary}.\n\n"
      out << "#{msg.doc}\n\n" unless msg.doc.empty?
      unless msg.fields.empty?
        out << "| Offset | Field | Type | Meaning |\n|---|---|---|---|\n"
        msg.fields.each { |f| out << "| #{f.offset} | `#{f.name}` | #{type_name(model, f.type)} | #{f.doc} |\n" }
        out << "\n"
      end
      msg.flags.each { |f| out << "- flag bit #{f.bit} `#{f.name}`: #{f.doc}\n" }
      out << "- may be answered by: #{msg.replies.map { |r| "[`#{r}`](##{anchor(r)})" }.join(', ')}\n" if msg.replies.any?
      out << "- see: #{msg.see.map { |r| "[`#{r}`](##{anchor(r)})" }.join(', ')}\n" if msg.see.any?
      out
    end

    def json(model)
      JSON.pretty_generate(
        title: model.title,
        notes: model.notes,
        header: HEADER.map(&:to_h),
        common_flags: COMMON_FLAGS.map(&:to_h),
        rings: model.rings.values.map(&:to_h),
        enums: model.enums.values.map { |e| e.to_h.merge(values: e.values.map(&:to_h)) },
        types: model.types.values.map(&:to_h),
        messages: model.messages.map do |m|
          m.to_h.merge(fields: m.fields.map(&:to_h), flags: m.flags.map(&:to_h))
        end
      ) << "\n"
    end

    C_TYPES = {
      u8: 'uint8_t', u16: 'uint16_t', u32: 'uint32_t', u64: 'uint64_t', i32: 'int32_t', bool: 'uint8_t',
      time: 'uint64_t', seq: 'uint64_t', session: 'uint64_t', macrostep: 'uint64_t', vref: 'uint64_t',
      blob: 'uint64_t', span: 'trp_span', text: 'trp_span', bytes: 'trp_span'
    }.freeze

    def c_type(model, type)
      model.enums.key?(type) ? C_TYPES.fetch(model.enums[type].base) : C_TYPES.fetch(type)
    end

    def c_name(id) = id.tr('.', '_')

    def c_header(model)
      out = +"/* Generated from catalog.rb by render.rb. Do not edit. A sketch of the\n"
      out << " * layout; the real header would be part of tinyactors.h. */\n"
      out << "#pragma once\n#include <stdint.h>\n\n"
      out << "typedef struct { uint32_t offset; uint32_t length; } trp_span;\n\n"
      out << "typedef struct {\n"
      HEADER.each { |f| out << "    #{c_type(model, f.type)} #{f.name};\n" }
      out << "} trp_header;\n_Static_assert(sizeof(trp_header) == #{HEADER_SIZE}, \"header\");\n\n"
      COMMON_FLAGS.each { |f| out << "#define TRP_FLAG_#{f.name.upcase} (1u << #{f.bit})\n" }
      out << "\n"
      model.enums.each_value do |e|
        e.values.each { |v| out << "#define TRP_#{e.name.upcase}_#{v.name.upcase} #{v.value}u\n" }
      end
      out << "\n"
      model.messages.each do |m|
        out << "#define TRP_#{c_name(m.id).upcase} 0x#{m.code.to_s(16).rjust(4, '0')}u\n"
        m.flags.each { |f| out << "#define TRP_#{c_name(m.id).upcase}_#{f.name.upcase} (1u << #{f.bit})\n" }
      end
      model.messages.each do |m|
        out << "\n/* #{m.id}: #{m.summary}. */\ntypedef struct {\n    trp_header header;\n"
        offset = HEADER_SIZE
        m.fields.each_with_index do |f, i|
          out << "    uint8_t pad#{i}[#{f.offset - offset}];\n" if f.offset > offset
          out << "    #{c_type(model, f.type)} #{f.name};\n"
          offset = f.offset + model.size_of(f.type)
        end
        out << "    uint8_t pad_end[#{m.fixed_size - offset}];\n" if m.fixed_size > offset
        out << "} trp_#{c_name(m.id)};\n"
        out << "_Static_assert(sizeof(trp_#{c_name(m.id)}) == #{m.fixed_size}, \"#{m.id}\");\n"
      end
      out
    end
  end
end

if $PROGRAM_NAME == __FILE__
  model = RingProto.load
  format, path = ARGV
  outputs = {
    'markdown' => ['MESSAGES.md', ->(m) { RingProto::Render.markdown(m) }],
    'json' => ['messages.json', ->(m) { RingProto::Render.json(m) }],
    'c' => ['ring_proto.h', ->(m) { RingProto::Render.c_header(m) }]
  }
  if format == 'all'
    dir = File.join(__dir__, 'generated')
    Dir.mkdir(dir) unless Dir.exist?(dir)
    outputs.each_value { |file, render| File.write(File.join(dir, file), render.call(model)) }
  elsif outputs.key?(format)
    text = outputs[format][1].call(model)
    path ? File.write(path, text) : $stdout.write(text)
  else
    warn 'usage: ruby render.rb markdown|json|c [OUT] | all'
    exit 2
  end
end
