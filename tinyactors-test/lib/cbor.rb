# frozen_string_literal: true

# CBOR (RFC 8949) for the suite: diagnostic notation to bytes, in the form
# core writes (tinyactors-core/src/base/cbor.c): shortest integers and
# lengths, definite lengths, map keys in written order (ADR 0009, decision
# 3), floats in the shortest exact width, NaN as f97e00; bytes back to
# diagnostic notation as ta_cbor_diagnostic writes it; and the comparison of
# an expected item with an actual one, byte for byte but where the expected
# side holds a wildcard (tag 39999 on a type's name).
module CBOR
  class Error < StandardError; end

  Map = Struct.new(:pairs) # pairs in written order; a key may repeat
  Tag = Struct.new(:number, :item)
  Simple = Struct.new(:value)
  Bytes = Struct.new(:data)
  UNDEFINED = Simple.new(23).freeze

  # Tag 39999 is unassigned in IANA's CBOR tags registry, and core never
  # writes it: 39999("uint") in an expected item matches any one item of
  # that type (suites/protocol/README.md, "Wildcards").
  WILDCARD = 39_999
  WILDCARD_TYPES = {
    'uint' => ->(item) { item.major.zero? },
    'int' => ->(item) { item.major <= 1 },
    'text' => ->(item) { item.major == 3 },
    'bytes' => ->(item) { item.major == 2 },
    'float' => ->(item) { item.major == 7 && item.info >= 25 },
    'any' => ->(_item) { true }
  }.freeze

  # A recursive-descent reader of the notation ta_cbor_diagnostic writes, and
  # what the vector files add: tags, h'..', simple(n), undefined, NaN and the
  # infinities.
  class Parser
    def initialize(text)
      @text = text
      @at = 0
    end

    def parse
      item = value
      space
      raise Error, "unexpected #{@text[@at..]} at #{@at}" unless @at == @text.length

      item
    end

    private

    def space = (@at += 1 while @text[@at]&.match?(/\s/))

    def expect(char)
      space
      raise Error, "expected #{char} at #{@at}" unless @text[@at] == char

      @at += 1
    end

    def value
      space
      case @text[@at]
      when '[' then list
      when '{' then map
      when '"' then text
      when 'h' then bytes
      else word
      end
    end

    def list
      expect('[')
      items = []
      space
      if @text[@at] == ']'
        @at += 1
        return items
      end
      loop do
        items << value
        space
        break @at += 1 if @text[@at] == ']'

        expect(',')
      end
      items
    end

    def map
      expect('{')
      pairs = []
      space
      if @text[@at] == '}'
        @at += 1
        return Map.new(pairs)
      end
      loop do
        key = value
        expect(':')
        pairs << [key, value]
        space
        break @at += 1 if @text[@at] == '}'

        expect(',')
      end
      Map.new(pairs)
    end

    def text
      @at += 1
      out = +''
      loop do
        char = @text[@at] or raise Error, 'unterminated text'
        @at += 1
        break if char == '"'
        next out << char unless char == '\\'

        escape = @text[@at]
        @at += 1
        out << case escape
               when '"', '\\', '/' then escape
               when 'b' then "\b"
               when 'f' then "\f"
               when 'n' then "\n"
               when 'r' then "\r"
               when 't' then "\t"
               when 'u' then unicode
               else raise Error, "bad escape \\#{escape}"
               end
      end
      out.encode('UTF-8')
    end

    def unicode
      code = @text[@at, 4].to_i(16)
      @at += 4
      if code.between?(0xD800, 0xDBFF) && @text[@at, 2] == '\\u'
        low = @text[@at + 2, 4].to_i(16)
        @at += 6
        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00)
      end
      [code].pack('U')
    end

    def bytes
      raise Error, "expected h' at #{@at}" unless @text[@at, 2] == "h'"

      close = @text.index("'", @at + 2) or raise Error, 'unterminated bytes'
      hex = @text[(@at + 2)...close].delete(' ')
      @at = close + 1
      Bytes.new([hex].pack('H*'))
    end

    def word
      start = @at
      @at += 1 while @text[@at]&.match?(/[A-Za-z0-9+\-.]/)
      token = @text[start...@at]
      space
      if @text[@at] == '('
        @at += 1
        if token == 'simple'
          number = word
          expect(')')
          return Simple.new(number)
        end
        raise Error, "bad tag #{token}" unless token.match?(/\A\d+\z/)

        item = value
        expect(')')
        return Tag.new(Integer(token), item)
      end
      case token
      when 'true' then true
      when 'false' then false
      when 'null' then nil
      when 'undefined' then UNDEFINED
      when 'NaN' then Float::NAN
      when 'Infinity' then Float::INFINITY
      when '-Infinity' then -Float::INFINITY
      when /\A-?\d+\z/ then Integer(token)
      when /\A-?\d+(\.\d+)?(e[-+]?\d+)?\z/i then Float(token)
      else raise Error, "unknown word #{token.inspect} at #{start}"
      end
    end
  end

  # Half-precision bits for every double a half holds exactly.
  HALVES = (0...0x10000).each_with_object({}) do |bits, table|
    exponent = (bits >> 10) & 0x1f
    fraction = bits & 0x3ff
    next if exponent == 0x1f && fraction != 0 # NaNs: written as f97e00

    magnitude = if exponent.zero? then fraction * 2.0**-24
                elsif exponent == 0x1f then Float::INFINITY
                else (1024 + fraction) * 2.0**(exponent - 25)
                end
    value = bits >> 15 == 1 ? -magnitude : magnitude
    table[[value].pack('G')] = bits
  end.freeze

  # An item as decoded: its head and its children, and the bytes it was
  # read from, so that two items compare byte for byte.
  Item = Struct.new(:major, :info, :argument, :children, :bytes)

  module_function

  # ---- writing --------------------------------------------------------------

  def head(major, argument)
    if argument < 24 then [(major << 5) | argument].pack('C')
    elsif argument < 0x100 then [(major << 5) | 24, argument].pack('CC')
    elsif argument < 0x10000 then [(major << 5) | 25, argument].pack('Cn')
    elsif argument < 0x100000000 then [(major << 5) | 26, argument].pack('CN')
    elsif argument < 0x10000000000000000 then [(major << 5) | 27, argument].pack('CQ>')
    else raise Error, "#{argument} does not fit in 64 bits"
    end
  end

  def float(value)
    return "\xf9\x7e\x00".b if value.nan?

    half = HALVES[[value].pack('G')]
    return "\xf9".b + [half].pack('n') if half

    single = [value].pack('g')
    return "\xfa".b + single if single.unpack1('g') == value && [single.unpack1('g')].pack('G') == [value].pack('G')

    "\xfb".b + [value].pack('G')
  end

  # Bytes of a Ruby value: Integer, Float, String (text), Bytes, Array, Hash
  # or Map (keys in their order), Tag, Simple, true, false, nil, or an Item
  # (its bytes as read).
  def encode(item)
    case item
    when Integer then item.negative? ? head(1, -1 - item) : head(0, item)
    when Float then float(item)
    when String then head(3, item.bytesize) + item.b
    when Bytes then head(2, item.data.bytesize) + item.data.b
    when Array then head(4, item.size) + item.map { |each| encode(each) }.join.b
    when Hash then head(5, item.size) + item.map { |k, v| encode(k) + encode(v) }.join.b
    when Map then head(5, item.pairs.size) + item.pairs.map { |k, v| encode(k) + encode(v) }.join.b
    when Tag then head(6, item.number) + encode(item.item)
    when Item then item.bytes
    when Simple
      raise Error, "simple(#{item.value}) is reserved" if item.value.between?(24, 31)

      item.value < 24 ? head(7, item.value) : [0xf8, item.value].pack('CC')
    when true then "\xf5".b
    when false then "\xf4".b
    when nil then "\xf6".b
    else raise Error, "cannot encode #{item.inspect}"
    end
  end

  def from_diagnostic(text) = encode(Parser.new(text).parse)

  # ---- reading --------------------------------------------------------------

  # The item `bytes` holds; Error when they are not exactly one well-formed
  # item of definite length.
  def decode(bytes)
    bytes = bytes.b
    item, at = read(bytes, 0, 0)
    raise Error, "#{bytes.bytesize - at} bytes after the item" unless at == bytes.bytesize

    item
  end

  def read(bytes, at, depth)
    raise Error, 'nested too deeply' if depth > 64
    raise Error, 'truncated' if at >= bytes.bytesize

    start = at
    initial = bytes.getbyte(at)
    at += 1
    major = initial >> 5
    info = initial & 0x1f
    raise Error, 'indefinite length' if info == 31
    raise Error, "reserved additional information #{info}" if info.between?(28, 30)

    argument = info
    if info >= 24
      width = 1 << (info - 24)
      raise Error, 'truncated' if at + width > bytes.bytesize

      argument = bytes.byteslice(at, width).unpack1({ 1 => 'C', 2 => 'n', 4 => 'N', 8 => 'Q>' }[width])
      at += width
    end
    children = []
    case major
    when 2, 3
      raise Error, 'truncated' if at + argument > bytes.bytesize

      at += argument
      raise Error, 'text is not UTF-8' if major == 3 && !bytes.byteslice(at - argument, argument).force_encoding('UTF-8').valid_encoding?
    when 4, 5, 6
      (major == 4 ? argument : major == 5 ? 2 * argument : 1).times do
        child, at = read(bytes, at, depth + 1)
        children << child
      end
    when 7
      raise Error, "simple(#{argument}) is reserved" if info == 24 && argument < 32
    end
    [Item.new(major, info, argument, children, bytes.byteslice(start, at - start)), at]
  end

  def payload(item) = item.bytes.byteslice(item.bytes.bytesize - item.argument, item.argument)

  def float_of(item)
    case item.info
    when 25 then half_value(item.argument)
    when 26 then [item.argument].pack('N').unpack1('g')
    else [item.argument].pack('Q>').unpack1('G')
    end
  end

  def half_value(bits)
    exponent = (bits >> 10) & 0x1f
    fraction = bits & 0x3ff
    magnitude = if exponent.zero? then fraction * 2.0**-24
                elsif exponent == 0x1f then fraction.zero? ? Float::INFINITY : Float::NAN
                else (1024 + fraction) * 2.0**(exponent - 25)
                end
    bits >> 15 == 1 ? -magnitude : magnitude
  end

  # A decoded item as a Ruby value: maps become Hashes (in their order),
  # text Strings, byte strings Bytes, tags Tags, other simple values Simples.
  def value(item)
    case item.major
    when 0 then item.argument
    when 1 then -1 - item.argument
    when 2 then Bytes.new(payload(item))
    when 3 then payload(item).force_encoding('UTF-8')
    when 4 then item.children.map { |child| value(child) }
    when 5 then item.children.each_slice(2).to_h { |key, each| [value(key), value(each)] }
    when 6 then Tag.new(item.argument, value(item.children.first))
    else
      if item.info >= 25 then float_of(item)
      else
        { 20 => false, 21 => true, 22 => nil }.fetch(item.argument) { Simple.new(item.argument) }
      end
    end
  end

  def load(bytes) = value(decode(bytes))

  # ---- diagnostic notation, as ta_cbor_diagnostic writes it -----------------

  def diagnostic(item)
    item = decode(item) if item.is_a?(String)
    case item.major
    when 0 then item.argument.to_s
    when 1 then (-1 - item.argument).to_s
    when 2 then "h'#{payload(item).unpack1('H*')}'"
    when 3 then quoted(payload(item))
    when 4 then "[#{item.children.map { |child| diagnostic(child) }.join(',')}]"
    when 5 then "{#{item.children.each_slice(2).map { |key, each| "#{diagnostic(key)}:#{diagnostic(each)}" }.join(',')}}"
    when 6 then "#{item.argument}(#{diagnostic(item.children.first)})"
    else
      if item.info >= 25 then float_text(float_of(item))
      else
        { 20 => 'false', 21 => 'true', 22 => 'null', 23 => 'undefined' }.fetch(item.argument) { "simple(#{item.argument})" }
      end
    end
  end

  ESCAPES = { '"' => '\\"', '\\' => '\\\\', "\b" => '\\b', "\f" => '\\f', "\n" => '\\n', "\r" => '\\r', "\t" => '\\t' }.freeze

  def quoted(bytes)
    text = bytes.b.each_char.map do |char|
      ESCAPES.fetch(char) { char.ord < 0x20 ? format('\\u%04x', char.ord) : char }
    end
    "\"#{text.join.force_encoding('UTF-8')}\""
  end

  # The shortest decimal that reads back as the double, with a point or an
  # exponent: as output_float_text in core (an exponent past 10^21 or below
  # 10^-5).
  def float_text(value)
    return 'NaN' if value.nan?
    return value.negative? ? '-Infinity' : 'Infinity' if value.infinite?
    return value.to_s if value.zero?

    sign = value.negative? ? '-' : ''
    mantissa, exponent = value.abs.to_s.split('e')
    whole, fraction = mantissa.split('.')
    digits = (whole + fraction.to_s).sub(/\A0+/, '')
    point = whole.to_i.zero? ? -fraction.to_s[/\A0*/].size : whole.size
    point += exponent.to_i
    digits = digits.sub(/0+\z/, '')
    digits = '0' if digits.empty?
    sign + if point > 21 || point < -5
             "#{digits[0]}.#{digits.size > 1 ? digits[1..] : '0'}e#{point - 1 >= 0 ? '+' : '-'}#{(point - 1).abs}"
           elsif point >= digits.size then "#{digits}#{'0' * (point - digits.size)}.0"
           elsif point.positive? then "#{digits[0, point]}.#{digits[point..]}"
           else "0.#{'0' * -point}#{digits}"
           end
  end

  # ---- comparing -------------------------------------------------------------

  def wildcard?(item) = item.major == 6 && item.argument == WILDCARD && item.children.first&.major == 3

  # nil when `actual` matches `expected` (Items), else where they first
  # differ: a path such as `.effects[0]{4}`.
  def mismatch(expected, actual, path = '')
    if wildcard?(expected)
      type = payload(expected.children.first)
      test = WILDCARD_TYPES[type] or return "#{path}: unknown wildcard type #{type.inspect}"
      return test.call(actual) ? nil : path.empty? ? '(the item)' : path
    end
    return nil if expected.bytes == actual.bytes
    return path.empty? ? '(the item)' : path unless expected.children.any? && expected.major == actual.major &&
                                                    expected.info == actual.info && expected.argument == actual.argument

    expected.children.each_with_index do |child, index|
      place = case expected.major
              when 4 then "#{path}[#{index}]"
              when 5 then index.even? ? "#{path}{key #{index / 2}}" : "#{path}{#{diagnostic(expected.children[index - 1])}}"
              else "#{path}(#{expected.argument})"
              end
      found = mismatch(child, actual.children[index], place)
      return found if found
    end
    nil
  end
end
