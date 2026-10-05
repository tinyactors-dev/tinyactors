# frozen_string_literal: true

# A small DSL for the ring protocol's records.
#
# Evaluating a catalog (catalog.rb) builds a RingProto::Model: the scalar
# types, enums, flags, rings and messages, with every message's byte layout
# computed. Renderers (render.rb) turn the model into Markdown, JSON or a C
# header, so the catalog is the only place a record is defined.
#
# Layout rules (see ../reference/ring.md, "Records"):
# - every record starts with the common header (RingProto::HEADER);
# - fixed fields follow in declaration order, each at its natural alignment;
# - text and bytes are spans (u32 offset from the record's start, u32 length)
#   into the record's tail, which follows the fixed part;
# - a record's size is rounded up to 8 bytes.
module RingProto
  Scalar = Data.define(:name, :size, :align, :doc)
  Field = Data.define(:name, :type, :doc, :offset)
  EnumType = Data.define(:name, :base, :values, :doc)
  EnumValue = Data.define(:name, :value, :doc)
  Flag = Data.define(:name, :bit, :doc)
  Ring = Data.define(:name, :producer, :consumer, :loss, :when_full, :doc)
  Message = Data.define(:id, :code, :ring, :summary, :doc, :fields, :flags, :replies, :fixed_size, :see)

  SCALARS = {
    u8: Scalar.new(:u8, 1, 1, 'unsigned 8-bit integer'),
    u16: Scalar.new(:u16, 2, 2, 'unsigned 16-bit integer'),
    u32: Scalar.new(:u32, 4, 4, 'unsigned 32-bit integer'),
    u64: Scalar.new(:u64, 8, 8, 'unsigned 64-bit integer'),
    i32: Scalar.new(:i32, 4, 4, 'signed 32-bit integer'),
    bool: Scalar.new(:bool, 1, 1, '0 or 1'),
    time: Scalar.new(:time, 8, 8, 'nanoseconds on the system clock that h2c.time moves'),
    seq: Scalar.new(:seq, 8, 8, "a record's sequence number in its ring"),
    session: Scalar.new(:session, 8, 8, 'a session ID (core ADR 0066); 0 means the system itself'),
    macrostep: Scalar.new(:macrostep, 8, 8, "a macrostep's number within its session"),
    vref: Scalar.new(:vref, 8, 8, "a value reference: a slot in the binding's value table; 0 means none"),
    blob: Scalar.new(:blob, 8, 8, 'a host-owned byte buffer, read or written through a byte source or sink (core ADR 0090)'),
    span: Scalar.new(:span, 8, 4, 'u32 offset from the record start, u32 length, into the tail'),
    text: Scalar.new(:text, 8, 4, 'a span of UTF-8 text in the tail'),
    bytes: Scalar.new(:bytes, 8, 4, 'a span of bytes in the tail')
  }.freeze

  # The common header every record starts with: 32 bytes.
  HEADER = [
    Field.new(:kind, :u16, "the message's code", 0),
    Field.new(:flags, :u16, "the message's flags (bits 0..7) and the common flags (bits 8..15)", 2),
    Field.new(:size, :u32, 'the whole record in bytes, header and tail included, a multiple of 8', 4),
    Field.new(:seq, :seq, 'the sequence number in this ring, from 1, with no gaps on a lossless ring', 8),
    Field.new(:session, :session, 'the session the record is about, or 0', 16),
    Field.new(:at, :time, "the system clock when it was written (h2c: the input's time)", 24)
  ].freeze
  HEADER_SIZE = 32

  # Flags any record may carry (bits 8..15).
  COMMON_FLAGS = [
    Flag.new(:replaying, 8, 'part of a replay (reference/recording-and-replay.md). On c2h and tel: ' \
                            'written by a replaying session. On h2c: an input of a replay; under a ' \
                            'whole-system replay every h2c record carries it, sessions it spawns ' \
                            'replay, mail between them is delivered as envelopes, and effects that ' \
                            'would leave the process are suppressed until h2c.publish'),
    Flag.new(:recorded, 9, 'the session is recorded: its inputs appear as c2h.input')
  ].freeze

  class Model
    attr_reader :title, :types, :enums, :rings, :messages, :notes

    def initialize
      @types = SCALARS.dup
      @enums = {}
      @rings = {}
      @messages = []
      @notes = []
    end

    def message(id) = @messages.find { |m| m.id == id } || raise(KeyError, "no message #{id}")

    def messages_on(ring) = @messages.select { |m| m.ring == ring }

    def size_of(type)
      return @types.fetch(type).size if @types.key?(type)
      return SCALARS.fetch(@enums.fetch(type).base).size if @enums.key?(type)

      raise KeyError, "unknown type #{type}"
    end

    def align_of(type)
      return @types.fetch(type).align if @types.key?(type)
      return SCALARS.fetch(@enums.fetch(type).base).align if @enums.key?(type)

      raise KeyError, "unknown type #{type}"
    end

    def check!
      codes = @messages.group_by(&:code).select { |_, ms| ms.size > 1 }
      raise "duplicate codes: #{codes.keys.map { |c| format('0x%04x', c) }.join(', ')}" unless codes.empty?

      ids = @messages.group_by(&:id).select { |_, ms| ms.size > 1 }
      raise "duplicate ids: #{ids.keys.join(', ')}" unless ids.empty?

      @messages.each do |m|
        raise "#{m.id}: code 0x0000 is the pad record" if m.code.zero?
        raise "#{m.id}: unknown ring #{m.ring}" unless @rings.key?(m.ring)
        raise "#{m.id}: id must start with its ring" unless m.id.start_with?("#{m.ring}.")

        (m.replies + m.see).each { |other| message(other) }
      end
      self
    end
  end

  # Builds a Model from a catalog block.
  class Builder
    def self.build(&block)
      builder = new
      builder.instance_eval(&block)
      builder.model.check!
    end

    attr_reader :model

    def initialize
      @model = Model.new
    end

    def title(text) = model.instance_variable_set(:@title, text)

    def note(text) = model.notes << text

    def enum(name, base, doc: '', &block)
      values = EnumBuilder.new
      values.instance_eval(&block)
      model.enums[name] = EnumType.new(name, base, values.values, doc)
    end

    def ring(name, producer:, consumer:, loss:, when_full:, doc:)
      model.rings[name] = Ring.new(name, producer, consumer, loss, when_full, doc)
    end

    def message(id, code:, summary:, &block)
      ring = id.split('.').first.to_sym
      body = MessageBuilder.new(model)
      body.instance_eval(&block) if block
      model.messages << Message.new(
        id, code, ring, summary, body.doc_text, body.layout, body.flags, body.replies, body.size, body.see_also
      )
    end
  end

  class EnumBuilder
    attr_reader :values

    def initialize
      @values = []
    end

    def value(name, number, doc = '') = @values << EnumValue.new(name, number, doc)
  end

  class MessageBuilder
    attr_reader :flags, :replies, :see_also

    def initialize(model)
      @model = model
      @fields = []
      @flags = []
      @replies = []
      @see_also = []
      @doc = []
    end

    def doc(text) = @doc << text

    def doc_text = @doc.join("\n\n")

    def field(name, type, doc = '') = @fields << [name, type, doc]

    def flag(name, bit, doc = '')
      raise ArgumentError, "message flags use bits 0..7, not #{bit}" unless (0..7).cover?(bit)

      @flags << Flag.new(name, bit, doc)
    end

    # Messages this one may cause on the other ring.
    def replies_with(*ids) = @replies.concat(ids)

    def see(*ids) = @see_also.concat(ids)

    def layout
      offset = HEADER_SIZE
      @fields.map do |name, type, doc|
        align = @model.align_of(type)
        offset = (offset + align - 1) / align * align
        field = Field.new(name, type, doc, offset)
        offset += @model.size_of(type)
        field
      end
    end

    def size
      last = layout.last
      finish = last ? last.offset + @model.size_of(last.type) : HEADER_SIZE
      (finish + 7) / 8 * 8
    end
  end

  def self.load(path = File.join(__dir__, 'catalog.rb'))
    Builder.build { instance_eval(File.read(path, encoding: 'UTF-8'), path) }
  end
end
