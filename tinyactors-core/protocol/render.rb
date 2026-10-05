# frozen_string_literal: true

# Renders the catalog's model into the files of src/generated/:
# RingProto::Render.files(model) answers each file's name and text.
# `mise run protocol` in tinyactors-core writes them, and
# `mise run protocol --check` fails when one differs.
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
      out << "Generated from [`catalog.rb`](../../protocol/catalog.rb) by `mise run protocol` in tinyactors-core. " \
             "Do not edit.\n\n"
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


    # ---------------------------------------------------------------- C
    #
    # Three C files, all private units of core's amalgamation (src/internal.h
    # lists the headers): ring_records.h, the record structs, kinds, rings,
    # flags and enums with asserts on every size and offset; ring_codec.h and
    # ring_codec.c, a writer and a size function per record and one
    # table-driven validating reader. Names follow core's: `ta_<ring>_<name>`
    # for a record, `TA_RECORD_KIND_<RING>_<NAME>` for its code, and
    # `ta_record_...` / `TA_RECORD_...` for everything shared.

    C_TYPES = {
      u8: 'uint8_t', u16: 'uint16_t', u32: 'uint32_t', u64: 'uint64_t', i32: 'int32_t', bool: 'uint8_t',
      time: 'uint64_t', seq: 'uint64_t', session: 'uint64_t', macrostep: 'uint64_t', vref: 'uint64_t',
      blob: 'uint64_t', span: 'ta_span32', text: 'ta_span32', bytes: 'ta_span32'
    }.freeze

    # The fields that are spans into the tail, and what a writer takes for each.
    TAIL_TYPES = { span: 'ta_bytes', text: 'ta_str', bytes: 'ta_bytes' }.freeze

    # A citation of a design document. The catalog cites them from the
    # repository root, which suits MESSAGES.md; C comments describe the code
    # alone, so they leave citations out.
    CITATION = %r{\s*\(docs/proposals/[^)]*\)}

    def c_type(model, type)
      model.enums.key?(type) ? C_TYPES.fetch(model.enums[type].base) : C_TYPES.fetch(type)
    end

    def c_name(id) = id.tr('.', '_')
    def c_record(msg) = "ta_#{c_name(msg.id)}"
    def c_kind(msg) = "TA_RECORD_KIND_#{c_name(msg.id).upcase}"
    def c_ring(ring) = "TA_RECORD_RING_#{ring.to_s.upcase}"
    def c_enum_type(enum) = "ta_record_#{enum.name}"
    def tail_fields(msg) = msg.fields.select { |f| TAIL_TYPES.key?(f.type) }

    # "an `h2c.event`", "a `c2h.send`": h is read as a letter.
    def a_record(msg) = "#{msg.id.start_with?('h') ? 'an' : 'a'} `#{msg.id}`"

    def c_prose(text)
      prose = text.to_s.gsub(CITATION, '').gsub(/\s+/, ' ').strip
      raise "a C comment would cite a proposal: #{prose}" if prose.include?('docs/proposals/')
      raise "a C comment would end early: #{prose}" if prose.include?('*/')

      prose
    end

    # A `/** ... */` comment holding `text`, wrapped to 80 columns.
    def c_doc(text, indent = '')
      words = c_prose(text).split
      return '' if words.empty?

      width = 80 - indent.size - 3
      lines = words.each_with_object([+'']) do |word, acc|
        if acc.last.empty? then acc.last << word
        elsif acc.last.size + 1 + word.size > width then acc << word.dup
        else acc.last << ' ' << word
        end
      end
      return "#{indent}/** #{lines.first} */\n" if lines.size == 1 && indent.size + lines.first.size + 7 <= 80

      "#{indent}/**\n#{lines.map { |line| "#{indent} * #{line}\n" }.join}#{indent} */\n"
    end

    # A file comment from Markdown text.
    def c_file_comment(name, markdown)
      body = markdown.lines.map { |line| line.strip.empty? ? " *\n" : " * #{line.rstrip}\n" }.join
      "/**\n * @file #{name}\n *\n#{body} */\n"
    end

    # One C enum per group of constants. The record structs keep fixed-width
    # fields, because the size of a C enum is up to the compiler; a comment
    # names the enum a field holds.
    def c_enum(name, doc, members)
      out = +c_doc(doc)
      out << "typedef enum {\n"
      members.each do |member, value, member_doc|
        out << c_doc(member_doc, '    ')
        out << "    #{member} = #{value},\n"
      end
      out << "} #{name};\n\n"
    end

    def c_field_comment(model, field)
      return " /* #{c_enum_type(model.enums[field.type])} */" if model.enums.key?(field.type)
      return " /* #{field.type} */" if TAIL_TYPES.key?(field.type)

      ''
    end

    RECORDS_COMMENT = <<~MD
      # The ring protocol's records

      Generated from `protocol/catalog.rb` by `mise run protocol` in
      tinyactors-core. Do not edit: change the catalog and render again.
      `mise run protocol --check`, which `mise run test` runs, fails when
      this file is not what the catalog renders.

      Every record that crosses the h2c, c2h and tel rings is a struct here,
      laid out exactly as it lies in a ring: `ta_<ring>_<name>` for the
      record `<ring>.<name>` (`ta_h2c_event` for `h2c.event`). Beside the
      structs are the record kinds (`ta_record_kind`, the 16-bit code in the
      header), the rings (`ta_record_ring`), the common flags
      (`ta_record_flag`, bits 8..15 of the header's `flags`), each record's
      own flags (`ta_<ring>_<name>_flag`, bits 0..7) and the catalog's enums
      (`ta_record_<enum>`). `src/generated/MESSAGES.md` says what every
      record and field means.

      ## Layout

      - Every record starts with `ta_record_header`, 32 bytes: `kind`,
        `flags`, `size`, `seq`, `session`, `at`.
      - The fixed fields follow in the order the catalog declares them,
        each at its natural alignment. Named `pad` arrays fill the gaps, so
        a struct has no padding of the compiler's, and `{0}` zeroes every
        byte of it.
      - Text and bytes are `ta_span32` spans: an offset from the record's
        first byte and a length, into the tail that follows the fixed part.
      - A record's size is a multiple of 8, and so is its fixed part,
        `sizeof` its struct.
      - Fields are fixed-width integers, because the size of a C enum is up
        to the compiler; a comment names the enum a field holds.
      - `_Static_assert`s pin every struct's size and every field's offset
        to the catalog's layout.

      Code 0x0000 is the pad `ta_ring` writes at the end of its buffer; it
      is no record.

      ## Memory

      Types and constants only: nothing here allocates or runs.

      ## Users

      The producers and consumers of h2c, c2h and tel, through the record
      codec (`ring_codec.h`), which writes and checks these records; the
      rings themselves are `ta_ring`s, which frame records by their `kind`,
      `size` and `seq`.
    MD

    def c_records(model)
      out = +c_file_comment('ring_records.h', RECORDS_COMMENT)
      out << "#pragma once\n\n#include <stddef.h>\n#include <stdint.h>\n\n#include \"../span.h\"\n\n"
      out << "/** The common header's size: every record starts with `ta_record_header`. */\n"
      out << "#define TA_RECORD_HEADER_BYTES #{HEADER_SIZE}u\n"
      out << "/** How many rings there are; `ta_record_ring` numbers them from 1. */\n"
      out << "#define TA_RECORD_RING_COUNT #{model.rings.size}u\n\n"
      rings = model.rings.values.each_with_index.map do |r, i|
        [c_ring(r.name), i + 1, "#{r.producer} to #{r.consumer}, #{r.loss == :none ? 'lossless' : 'lossy'}"]
      end
      out << c_enum('ta_record_ring', 'The ring a record crosses.', rings)
      kinds = [['TA_RECORD_KIND_PAD', '0x0000', 'the pad at the end of a ring buffer; no record']] +
              model.messages.map { |m| [c_kind(m), hex(m.code), "`#{m.id}`: #{m.summary}"] }
      out << c_enum('ta_record_kind', 'The header\'s `kind`: the message code.', kinds)
      out << c_enum('ta_record_flag', 'Common flags, bits 8..15 of the header\'s `flags`.',
                    COMMON_FLAGS.map { |f| ["TA_RECORD_FLAG_#{f.name.upcase}", "1 << #{f.bit}", f.doc] })
      model.messages.each do |m|
        next if m.flags.empty?

        prefix = "TA_#{c_name(m.id).upcase}_FLAG"
        out << c_enum("#{c_record(m)}_flag", "`#{m.id}`'s own flags, bits 0..7 of the header's `flags`.",
                      m.flags.map { |f| ["#{prefix}_#{f.name.upcase}", "1 << #{f.bit}", f.doc] })
      end
      model.enums.each_value do |e|
        out << c_enum(c_enum_type(e), "#{e.doc} Stored as #{C_TYPES.fetch(e.base)}.",
                      e.values.map { |v| ["TA_RECORD_#{e.name.upcase}_#{v.name.upcase}", v.value, v.doc] })
      end
      out << c_header_struct(model)
      model.messages.each { |m| out << c_record_struct(model, m) }
      out
    end

    def c_header_struct(model)
      out = +"/** The common header every record starts with. */\ntypedef struct {\n"
      HEADER.each do |f|
        comment = f.name == :kind ? ' /* ta_record_kind */' : ''
        out << c_doc(f.doc, '    ') << "    #{c_type(model, f.type)} #{f.name};#{comment}\n"
      end
      out << "} ta_record_header;\n"
      out << "_Static_assert(sizeof(ta_record_header) == #{HEADER_SIZE}, \"ta_record_header\");\n"
      HEADER.each do |f|
        out << "_Static_assert(offsetof(ta_record_header, #{f.name}) == #{f.offset}, \"header #{f.name}\");\n"
      end
      out
    end

    def c_record_struct(model, msg)
      name = c_record(msg)
      out = +"\n" << c_doc("`#{msg.id}` (#{hex(msg.code)}, on #{msg.ring}): #{msg.summary}.")
      out << "typedef struct {\n    ta_record_header header;\n"
      offset = HEADER_SIZE
      msg.fields.each_with_index do |f, i|
        out << "    uint8_t pad#{i}[#{f.offset - offset}];\n" if f.offset > offset
        out << c_doc(f.doc, '    ') << "    #{c_type(model, f.type)} #{f.name};#{c_field_comment(model, f)}\n"
        offset = f.offset + model.size_of(f.type)
      end
      out << "    uint8_t pad_end[#{msg.fixed_size - offset}];\n" if msg.fixed_size > offset
      out << "} #{name};\n"
      out << "_Static_assert(sizeof(#{name}) == #{msg.fixed_size}, \"#{msg.id}\");\n"
      msg.fields.each do |f|
        out << "_Static_assert(offsetof(#{name}, #{f.name}) == #{f.offset}, \"#{msg.id} #{f.name}\");\n"
      end
      out
    end

    CODEC_COMMENT = <<~MD
      # The record codec

      Generated from `protocol/catalog.rb` by `mise run protocol` in
      tinyactors-core. Do not edit: change the catalog, or the renderer
      (`protocol/render.rb`), and render again.

      Writes the records of `ring_records.h` into bytes a producer reserved,
      and checks a record a consumer read before anything trusts it. h2c is
      where a host's bytes enter core, so the reader is where hostile or
      corrupt input meets core.

      ## Writing

      For every record `<ring>.<name>`:

      - `ta_<ring>_<name>_write(out, fixed, texts)` fills the record in place
        in `out`, for example the bytes and size of a `ta_ring_reserve`
        reservation. It copies the fixed part from `fixed`, every byte of it,
        its pads included (build it with `{0}`); sets `kind` to the record's
        code and `size` to `out.size`; keeps the `seq` already in `out`,
        which the reservation stamped; lays the texts end to end in the
        tail, in the order the catalog declares them, starting right after
        the fixed part; points each span at its text, an empty one at where
        it would start; and zeroes every byte after the last text up to
        `out.size`. The `kind`, `size`, `seq` and span fields of `fixed` are
        ignored. `fixed` may be `out.bytes` itself, so a producer can build
        the fixed part in the reservation; the texts must not lie in `out`.
        Texts that lie end to end in the caller's memory, as a producer that
        composes them in catalog order lays them, are copied as one run.
      - A record with text or bytes has `ta_<ring>_<name>_texts`: a `ta_str`
        per text field and a `ta_bytes` per bytes field, named after it. Its
        `ta_<ring>_<name>_size(texts, &size)` answers the bytes to reserve:
        the fixed part plus the texts' lengths, rounded up to 8. A record
        without them has no `texts` parameter, and its size is `sizeof` its
        struct.

      ## Reading

      `ta_record_read` checks a record's bytes, for example those of a
      `ta_ring_peek` record, as a record of the ring they were read from:

      - the bytes hold at least the 32-byte header, and the kind is not the
        pad;
      - the kind is one the catalog defines, and a kind of that ring;
      - its `size` is a multiple of 8, at least the kind's fixed part and at
        most the bytes given;
      - every span lies in the tail: it starts at or after the fixed part
        and ends at or before `size`, an empty one included.

      One generated descriptor per kind (its ring, its fixed size and the
      offsets of its spans) and one loop do these checks for every kind;
      `ta_record_describe` answers a kind's descriptor. The reader does not
      check `seq`: `ta_ring_peek` does, gap-free on a lossless ring and only
      growing on a lossy one. Nor does it check flag bits or enum values:
      whoever uses a field checks its value.

      After `TA_OK`, `ta_<ring>_<name>_from_view` answers the typed record,
      and `ta_record_view_text` and `ta_record_view_bytes` a span's bytes.

      ## Memory

      None. Every function borrows the caller's bytes for one call: a
      writer writes only `out.bytes[0, out.size)` and reads `fixed` and the
      texts; the reader reads only `bytes[0, size)` of its parameters. A
      view, a typed record and a text it answers point into the caller's
      bytes and stay valid as long as those do, for a ring record until it
      is released. A description points into constant tables. Nothing
      allocates, so every function may run inside a macrostep.

      ## Complexity

      | Operation | Cost |
      |---|---|
      | `ta_<ring>_<name>_write` | O(record size) |
      | `ta_<ring>_<name>_size` | O(spans of the kind) |
      | `ta_record_read` | O(1) to find the kind, plus O(spans of the kind) |
      | `ta_record_describe`, `ta_<ring>_<name>_from_view`, `ta_record_view_text`, `ta_record_view_bytes` | O(1) |

      ## Failure

      A function that can fail answers a `ta_result`, and when it fails it
      changes nothing: no byte of `out`, no `*out`.

      - A writer answers `TA_ERR_INVALID` when `out.bytes` is NULL or not
        8-byte aligned, `out.size` is not a multiple of 8, `fixed` or
        `texts` is NULL, a text has a length but no bytes, or the record
        would exceed the largest size a `u32` holds; and `TA_ERR_FULL` when
        `out.size` is smaller than the record.
      - A size function answers `TA_ERR_INVALID` for the same texts.
      - `ta_record_read` answers `TA_ERR_INVALID` when its bytes are NULL or
        not 8-byte aligned, or its ring is not a `ta_record_ring`;
        `TA_ERR_UNSUPPORTED` for a kind the catalog does not define; and
        `TA_ERR_CORRUPT` for any other check above that fails.
      - `ta_record_describe` answers `TA_ERR_NOT_FOUND` for a kind the
        catalog does not define, the pad included.

      ## Users

      Core reads h2c records and writes c2h and tel records; a host does
      the reverse; a test drives core by writing h2c records and checking
      the c2h records it reads back. Each side reserves and peeks through
      `ta_ring`.
    MD

    def c_codec_header(model)
      out = +c_file_comment('ring_codec.h', CODEC_COMMENT)
      out << <<~C
        #pragma once

        #include "../../include/tinyactors.h"
        #include "ring_records.h"
        #include "../span.h"

        /** Where a writer puts a record: for example a ring reservation's bytes and size. */
        typedef struct {
            /** The record's first byte, 8-byte aligned. */
            unsigned char *bytes;
            /** The bytes the record takes, a multiple of 8; its `size` becomes this. */
            uint32_t size;
        } ta_record_buffer;

        /** What `ta_record_read` checks. */
        typedef struct {
            /** The record's first byte, 8-byte aligned; borrowed for the call. */
            const unsigned char *bytes;
            /** The bytes readable from `bytes`; the record's `size` may not pass it. */
            uint32_t size;
            /** The ring the record was read from. */
            ta_record_ring ring;
        } ta_record_read_params;

        /** A record `ta_record_read` checked. */
        typedef struct {
            /** The record's first byte; borrowed from the caller. */
            const unsigned char *bytes;
            /** The record's `size`. */
            uint32_t size;
            /** The record's `kind`, a `ta_record_kind`. */
            uint16_t kind;
        } ta_record_view;

        /** What the codec knows of a kind. */
        typedef struct {
            /** The ring the kind crosses. */
            ta_record_ring ring;
            /** Its fixed part in bytes: `sizeof` its struct. */
            uint32_t fixed_size;
            /** How many span fields it has. */
            uint32_t span_count;
            /** The offset of each span field, in the order the catalog declares them; constant. */
            const uint32_t *span_offsets;
        } ta_record_description;

        /**
         * Checks the record at `params->bytes` as a record of `params->ring`
         * (see the file comment) and answers it in `*out`. Borrows the bytes
         * for the call; the view points into them. Never allocates; may run
         * inside a macrostep. O(1) plus one step per span of the kind.
         *
         * Answers `TA_ERR_INVALID` for NULL or unaligned bytes or an unknown
         * ring, `TA_ERR_UNSUPPORTED` for a kind the catalog does not define,
         * and `TA_ERR_CORRUPT` for a record too short for the header, a pad, a
         * kind of another ring, a wrong size or a span outside the tail;
         * `*out` is then not written.
         */
        ta_result ta_record_read(const ta_record_read_params *params, ta_record_view *out);

        /**
         * Answers in `*out` the ring, fixed size and span offsets of `kind`.
         * Never allocates; may run inside a macrostep. O(1).
         *
         * Answers `TA_ERR_NOT_FOUND`, and does not write `*out`, for a kind
         * the catalog does not define, the pad included.
         */
        ta_result ta_record_describe(uint16_t kind, ta_record_description *out);

        /**
         * The text `span` names in the record `view` holds. `view` comes from
         * `ta_record_read`, which checked the record's own spans; a span that
         * does not lie inside the record answers empty text with a NULL
         * pointer. Borrows the record's bytes. Never allocates. O(1).
         */
        static inline ta_str ta_record_view_text(const ta_record_view *view, ta_span32 span) {
            ta_str text = { NULL, 0u };
            if (span.offset <= view->size && span.length <= view->size - span.offset) {
                text.ptr = (const char *)view->bytes + span.offset;
                text.len = span.length;
            }
            return text;
        }

        /**
         * The bytes `span` names in the record `view` holds, as
         * `ta_record_view_text` answers text. Never allocates. O(1).
         */
        static inline ta_bytes ta_record_view_bytes(const ta_record_view *view, ta_span32 span) {
            ta_bytes bytes = { NULL, 0u };
            if (span.offset <= view->size && span.length <= view->size - span.offset) {
                bytes.ptr = (const uint8_t *)view->bytes + span.offset;
                bytes.len = span.length;
            }
            return bytes;
        }
      C
      model.messages.each { |m| out << c_codec_declarations(model, m) }
      out
    end

    def c_codec_declarations(model, msg)
      name = c_record(msg)
      tails = tail_fields(msg)
      out = +"\n"
      out << "/* ---- #{msg.id} ---- */\n\n"
      unless tails.empty?
        out << c_doc("The text and bytes of #{a_record(msg)} record, in the order they lie in its tail; " \
                     'a writer borrows them for one call.')
        out << "typedef struct {\n"
        tails.each { |f| out << c_doc(f.doc, '    ') << "    #{TAIL_TYPES.fetch(f.type)} #{f.name};\n" }
        out << "} #{name}_texts;\n\n"
        out << c_doc("Answers in `*out` the bytes #{a_record(msg)} record with `texts` takes: its fixed part, " \
                     "#{msg.fixed_size} bytes, plus their lengths, rounded up to 8. Never allocates; may run " \
                     'inside a macrostep. Answers `TA_ERR_INVALID`, and does not write `*out`, when `texts` ' \
                     'is NULL, a text has a length but no bytes, or the record would exceed the largest ' \
                     'size a `u32` holds.')
        out << "ta_result #{name}_size(const #{name}_texts *texts, uint32_t *out);\n\n"
      end
      texts = tails.empty? ? '' : " and `texts` in the tail"
      out << c_doc("Writes #{a_record(msg)} record into `out` (see the file comment): the fixed part from `fixed`, " \
                   "kind #{hex(msg.code)}, size `out.size`, the seq already in `out`#{texts}. Borrows " \
                   "`fixed`#{tails.empty? ? '' : ' and `texts`'} for the call. Never allocates; may run inside a " \
                   'macrostep. Answers `TA_ERR_INVALID` for a bad buffer or argument and `TA_ERR_FULL` when ' \
                   '`out.size` is too small, and then writes nothing.')
      params = tails.empty? ? '' : ", const #{name}_texts *texts"
      out << "ta_result #{name}_write(ta_record_buffer out, const #{name} *fixed#{params});\n\n"
      out << c_doc("The `#{msg.id}` record `view` holds, or NULL when it holds another kind. `view` comes from " \
                   '`ta_record_read`; the record is borrowed from its bytes. Never allocates. O(1).')
      out << "static inline const #{name} *#{name}_from_view(const ta_record_view *view) {\n"
      out << "    return view->kind == #{c_kind(msg)} ? (const #{name} *)(const void *)view->bytes : NULL;\n}\n"
      out
    end

    def c_codec_source(model)
      out = +<<~C
        /* The record codec: see ring_codec.h. Generated from protocol/catalog.rb
         * by `mise run protocol` in tinyactors-core. Do not edit. */
        #include "ring_codec.h"
        #include "../memory.h"
        #include "../../include/tinyactors.h"
        #include "../span.h"
        #include "ring_records.h"

        /* One kind as the reader and the writers check it. */
        typedef struct {
            uint16_t kind;
            uint8_t ring;
            uint8_t span_count;
            uint32_t fixed_size;
            /* Its first span offset in ta_record_span_offsets. */
            uint32_t span_first;
        } ta_record_descriptor;

        /* Every kind's span offsets, a run per kind, in catalog order. */
        static const uint32_t ta_record_span_offsets[] = {
      C
      first = 0
      firsts = {}
      model.messages.each do |m|
        firsts[m.id] = first
        tails = tail_fields(m)
        next if tails.empty?

        out << "    /* #{m.id} */\n"
        tails.each { |f| out << "    (uint32_t)offsetof(#{c_record(m)}, #{f.name}),\n" }
        first += tails.size
      end
      out << "    0u, /* none */\n" if first.zero?
      out << "};\n\n/* One descriptor per kind, in catalog order. */\n"
      out << "static const ta_record_descriptor ta_record_descriptors[] = {\n"
      model.messages.each do |m|
        out << "    { #{c_kind(m)}, #{c_ring(m.ring)}, #{tail_fields(m).size}u, (uint32_t)sizeof(#{c_record(m)}), " \
               "#{firsts[m.id]}u },\n"
      end
      out << "};\n\n"
      codes = model.messages.map(&:code)
      low = codes.min
      raise 'the catalog has more kinds than a descriptor slot holds' if model.messages.size > 254

      out << "/* Each kind's descriptor index plus one, by its code less #{hex(low)}; 0 for a\n"
      out << " * code no record has. One load finds a kind, read or written. */\n"
      out << "static const uint8_t ta_record_descriptor_slots[#{hex(codes.max - low + 1)}] = {\n"
      model.messages.each_with_index { |m, i| out << "    [#{c_kind(m)} - #{hex(low)}] = #{i + 1}u,\n" }
      out << "};\n\n"
      out << "/* The index of `kind` in ta_record_descriptors, or their count when no\n"
      out << " * record has that kind. */\n"
      out << "static uint32_t ta_record_descriptor_index(uint16_t kind) {\n"
      out << "    uint32_t at = (uint32_t)kind - #{hex(low)}u; /* wraps past the table below the first code */\n"
      out << "    uint32_t slot = at < TA_ARRAY_COUNT(ta_record_descriptor_slots) ? ta_record_descriptor_slots[at] : 0u;\n"
      out << "    return slot == 0u ? TA_ARRAY_COUNT(ta_record_descriptors) : slot - 1u;\n}\n\n"
      out << CODEC_RUNTIME
      model.messages.each { |m| out << c_codec_definitions(m) }
      out
    end

    # The part of ring_codec.c that is the same for every catalog: the
    # helpers every writer and size function calls, and the reading and
    # describing loops over the descriptors.
    CODEC_RUNTIME = <<~C
      /* What a writer checks a buffer against: its kind, its fixed part and the record's size. */
      typedef struct {
          uint16_t kind;
          uint32_t fixed_size;
          uint32_t needed;
      } ta_record_frame;

      /*
       * A record being written: its bytes, where its next text goes, and the
       * run of texts not yet copied. Texts that lie end to end in the caller's
       * memory, as a producer that composes them in catalog order lays them,
       * are copied as one run.
       */
      typedef struct {
          unsigned char *bytes;
          uint32_t cursor;
          const uint8_t *run;
          uint32_t run_at;
          uint32_t run_len;
      } ta_record_tail;

      #define TA_RECORD_TEXT(text) ((ta_bytes){ (const uint8_t *)(const void *)(text).ptr, (text).len })

      /* Adds a text's length to `*size`; false for a text with a length but no bytes. */
      static inline bool ta_record_measure(uint64_t *size, ta_bytes text) {
          *size += text.len;
          return text.ptr != NULL || text.len == 0u;
      }

      /* A measured record's size, rounded up to 8: TA_ERR_INVALID for a bad text
       * or a record past the largest size a u32 holds. */
      static ta_result ta_record_round(uint64_t size, bool valid, uint32_t *out) {
          size = (size + 7u) & ~(uint64_t)7u;
          if (!valid || size > UINT32_MAX) {
              return TA_ERR_INVALID;
          }
          *out = (uint32_t)size;
          return TA_OK;
      }

      /* Checks the buffer, then fills the fixed part and the header; see ring_codec.h. */
      static ta_result ta_record_begin(ta_record_buffer out, const void *fixed, const ta_record_frame *frame) {
          if (out.bytes == NULL || ((uintptr_t)(void *)out.bytes & 7u) != 0u || (out.size & 7u) != 0u ||
              fixed == NULL) {
              return TA_ERR_INVALID;
          }
          if (out.size < frame->needed) {
              return TA_ERR_FULL;
          }
          uint64_t seq = 0u;
          ta_mem_copy(&seq, out.bytes + offsetof(ta_record_header, seq), sizeof(seq));
          if (fixed != (const void *)out.bytes) {
              ta_mem_copy(out.bytes, fixed, frame->fixed_size);
          }
          uint16_t kind = frame->kind;
          ta_mem_copy(out.bytes + offsetof(ta_record_header, kind), &kind, sizeof(kind));
          ta_mem_copy(out.bytes + offsetof(ta_record_header, size), &out.size, sizeof(out.size));
          ta_mem_copy(out.bytes + offsetof(ta_record_header, seq), &seq, sizeof(seq));
          return TA_OK;
      }

      /* Copies the run of texts not yet copied. */
      static void ta_record_flush(ta_record_tail *tail) {
          if (tail->run_len != 0u) {
              ta_mem_copy(tail->bytes + tail->run_at, tail->run, tail->run_len);
          }
          tail->run_len = 0u;
      }

      /* Lays one text at the cursor and points the span at `span_offset` at it. */
      static inline void ta_record_put(ta_record_tail *tail, uint32_t span_offset, ta_bytes text) {
          ta_span32 span = { tail->cursor, text.len };
          ta_mem_copy(tail->bytes + span_offset, &span, sizeof(span));
          if (text.len == 0u) {
              return;
          }
          if (tail->run_len == 0u || text.ptr != tail->run + tail->run_len) {
              ta_record_flush(tail);
              tail->run = text.ptr;
              tail->run_at = tail->cursor;
          }
          tail->run_len += text.len;
          tail->cursor += text.len;
      }

      /* Copies the last run, then zeroes the bytes after the last text: the
       * texts cover the rest of the tail. */
      static void ta_record_finish(ta_record_buffer out, ta_record_tail *tail) {
          ta_record_flush(tail);
          ta_mem_zero(out.bytes + tail->cursor, out.size - tail->cursor);
      }

      ta_result ta_record_read(const ta_record_read_params *params, ta_record_view *out) {
          if (params == NULL || out == NULL || params->bytes == NULL ||
              ((uintptr_t)(const void *)params->bytes & 7u) != 0u) {
              return TA_ERR_INVALID;
          }
          if ((uint32_t)params->ring == 0u || (uint32_t)params->ring > TA_RECORD_RING_COUNT) {
              return TA_ERR_INVALID;
          }
          if (params->size < TA_RECORD_HEADER_BYTES) {
              return TA_ERR_CORRUPT;
          }
          uint16_t kind = 0u;
          uint32_t size = 0u;
          ta_mem_copy(&kind, params->bytes + offsetof(ta_record_header, kind), sizeof(kind));
          ta_mem_copy(&size, params->bytes + offsetof(ta_record_header, size), sizeof(size));
          if (kind == TA_RECORD_KIND_PAD) {
              return TA_ERR_CORRUPT;
          }
          uint32_t index = ta_record_descriptor_index(kind);
          if (index == TA_ARRAY_COUNT(ta_record_descriptors)) {
              return TA_ERR_UNSUPPORTED;
          }
          const ta_record_descriptor *descriptor = &ta_record_descriptors[index];
          if ((uint32_t)descriptor->ring != (uint32_t)params->ring) {
              return TA_ERR_CORRUPT;
          }
          if ((size & 7u) != 0u || size > params->size || size < descriptor->fixed_size) {
              return TA_ERR_CORRUPT;
          }
          for (uint32_t index_in_kind = 0; index_in_kind < descriptor->span_count; index_in_kind += 1u) {
              ta_span32 span = { 0u, 0u };
              ta_mem_copy(&span, params->bytes + ta_record_span_offsets[descriptor->span_first + index_in_kind],
                          sizeof(span));
              if (span.offset < descriptor->fixed_size || span.offset > size || span.length > size - span.offset) {
                  return TA_ERR_CORRUPT;
              }
          }
          out->bytes = params->bytes;
          out->size = size;
          out->kind = kind;
          return TA_OK;
      }

      ta_result ta_record_describe(uint16_t kind, ta_record_description *out) {
          uint32_t index = ta_record_descriptor_index(kind);
          if (index == TA_ARRAY_COUNT(ta_record_descriptors) || out == NULL) {
              return TA_ERR_NOT_FOUND;
          }
          const ta_record_descriptor *descriptor = &ta_record_descriptors[index];
          out->ring = (ta_record_ring)descriptor->ring;
          out->fixed_size = descriptor->fixed_size;
          out->span_count = descriptor->span_count;
          out->span_offsets = &ta_record_span_offsets[descriptor->span_first];
          return TA_OK;
      }
    C

    # Each writer and size function is straight-line code over its own
    # texts: no array of tails to build, and no kind to look up.
    def c_codec_definitions(msg)
      name = c_record(msg)
      kind = c_kind(msg)
      tails = tail_fields(msg)
      out = +"\n/* ---- #{msg.id} ---- */\n"
      if tails.empty?
        out << "\nta_result #{name}_write(ta_record_buffer out, const #{name} *fixed) {\n"
        out << "    const ta_record_frame frame = { #{kind}, (uint32_t)sizeof(#{name}), (uint32_t)sizeof(#{name}) };\n"
        out << "    ta_result result = ta_record_begin(out, fixed, &frame);\n"
        out << "    if (result == TA_OK) {\n"
        out << "        ta_record_tail tail = { out.bytes, (uint32_t)sizeof(#{name}), NULL, 0u, 0u };\n"
        out << "        ta_record_finish(out, &tail);\n    }\n"
        out << "    return result;\n}\n"
        return out
      end
      out << "\nta_result #{name}_size(const #{name}_texts *texts, uint32_t *out) {\n"
      out << "    if (texts == NULL) {\n        return TA_ERR_INVALID;\n    }\n"
      out << "    uint64_t size = sizeof(#{name});\n    bool valid = true;\n"
      tails.each { |f| out << "    valid &= ta_record_measure(&size, TA_RECORD_TEXT(texts->#{f.name}));\n" }
      out << "    return ta_record_round(size, valid, out);\n}\n"
      out << "\nta_result #{name}_write(ta_record_buffer out, const #{name} *fixed, const #{name}_texts *texts) {\n"
      out << "    uint32_t needed = 0u;\n"
      out << "    ta_result result = #{name}_size(texts, &needed);\n"
      out << "    if (result == TA_OK) {\n"
      out << "        const ta_record_frame frame = { #{kind}, (uint32_t)sizeof(#{name}), needed };\n"
      out << "        result = ta_record_begin(out, fixed, &frame);\n    }\n"
      out << "    if (result != TA_OK) {\n        return result;\n    }\n"
      out << "    ta_record_tail tail = { out.bytes, (uint32_t)sizeof(#{name}), NULL, 0u, 0u };\n"
      tails.each do |f|
        out << "    ta_record_put(&tail, (uint32_t)offsetof(#{name}, #{f.name}), TA_RECORD_TEXT(texts->#{f.name}));\n"
      end
      out << "    ta_record_finish(out, &tail);\n    return TA_OK;\n}\n"
      out
    end

    # Every C name the renderer declares, so two of them never collide.
    def c_identifiers(model)
      names = %w[ta_record_header ta_record_ring ta_record_kind ta_record_flag ta_record_buffer
                 ta_record_read_params ta_record_view ta_record_description ta_record_read ta_record_describe
                 ta_record_view_text ta_record_view_bytes TA_RECORD_HEADER_BYTES TA_RECORD_RING_COUNT
                 TA_RECORD_KIND_PAD ta_record_descriptor ta_record_span_offsets ta_record_descriptors
                 ta_record_descriptor_index ta_record_descriptor_slots ta_record_frame ta_record_tail TA_RECORD_TEXT
                 ta_record_measure ta_record_round ta_record_begin ta_record_flush ta_record_put ta_record_finish]
      names += model.rings.keys.map { |r| c_ring(r) }
      names += COMMON_FLAGS.map { |f| "TA_RECORD_FLAG_#{f.name.upcase}" }
      model.enums.each_value do |e|
        names << c_enum_type(e)
        names += e.values.map { |v| "TA_RECORD_#{e.name.upcase}_#{v.name.upcase}" }
      end
      model.messages.each do |m|
        name = c_record(m)
        names += [name, c_kind(m), "#{name}_write", "#{name}_from_view"]
        names += ["#{name}_texts", "#{name}_size"] unless tail_fields(m).empty?
        next if m.flags.empty?

        names << "#{name}_flag"
        names += m.flags.map { |f| "TA_#{c_name(m.id).upcase}_FLAG_#{f.name.upcase}" }
      end
      names
    end

    def check_c_identifiers!(model)
      dupes = c_identifiers(model).tally.select { |_, count| count > 1 }.keys
      raise "the catalog renders these C names twice: #{dupes.join(', ')}" unless dupes.empty?
    end

    # The generated files, by name, as the catalog renders them.
    def files(model)
      check_c_identifiers!(model)
      {
        'MESSAGES.md' => markdown(model),
        'messages.json' => json(model),
        'ring_records.h' => c_records(model),
        'ring_codec.h' => c_codec_header(model),
        'ring_codec.c' => c_codec_source(model)
      }
    end
  end
end
