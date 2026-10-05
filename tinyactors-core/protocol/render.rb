# frozen_string_literal: true

# Renders the catalog as text for people and tools: `Render.markdown(model)` and
# `Render.json(model)`. They are called by `mise run protocol` (tinyactors-core/
# mise/tasks/protocol), which writes generated/MESSAGES.md and
# generated/messages.json next to this file.
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
      out << "Generated from [`catalog.rb`](../catalog.rb) by `mise run protocol` in tinyactors-core. Do not edit.\n\n"
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
  end
end
