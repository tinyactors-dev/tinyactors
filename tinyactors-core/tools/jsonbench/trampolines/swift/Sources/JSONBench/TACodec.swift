// A minimal Codable Decoder and Encoder over core's ta_cbor_reader and
// ta_cbor_writer: what a Swift binding would likely ship, since the standard
// library has no CBOR coder. Enough for structs, arrays, strings, integers,
// floats, booleans and null; no superDecoder/superEncoder.
//
// Decoding: a decoder is a view of one whole item's bytes. A keyed container
// scans its map once, recording each key and its value's span; a value is
// decoded from its span by a decoder of its own.
// Encoding: streams into the writer; a container opens with
// TA_CBOR_COUNT_LATER and is closed when the value that opened it returns
// (the writer's depth before and after).
import CTinyactors

struct TACodecError: Error { let what: String }

@inline(__always)
private func cborRead(_ r: inout ta_cbor_reader) throws -> ta_cbor_item {
    var item = ta_cbor_item()
    guard ta_cbor_read(&r, &item) == 1 else { throw TACodecError(what: "short item") }
    return item
}

@inline(__always)
private func itemString(_ item: ta_cbor_item) -> String {
    String(decoding: UnsafeRawBufferPointer(start: item.bytes.pointer, count: Int(item.bytes.length)),
           as: UTF8.self)
}

// ---- decoding -------------------------------------------------------------------

struct TADecoder: Decoder {
    let span: UnsafeRawBufferPointer // one whole item
    var codingPath: [CodingKey] { [] }
    var userInfo: [CodingUserInfoKey: Any] { [:] }

    func reader() -> ta_cbor_reader {
        var r = ta_cbor_reader()
        ta_cbor_reader_init(&r, ta_bytes(pointer: span.baseAddress, length: span.count))
        return r
    }

    func container<Key: CodingKey>(keyedBy type: Key.Type) throws -> KeyedDecodingContainer<Key> {
        KeyedDecodingContainer(try TAKeyedDecoding<Key>(self))
    }
    func unkeyedContainer() throws -> UnkeyedDecodingContainer { try TAUnkeyedDecoding(self) }
    func singleValueContainer() throws -> SingleValueDecodingContainer { TASingleDecoding(decoder: self) }

    // scalars, read from the span's one item
    func scalar() throws -> ta_cbor_item { var r = reader(); return try cborRead(&r) }
    func decodeInt<T: FixedWidthInteger>(_: T.Type) throws -> T {
        let item = try scalar()
        switch Int(item.kind) {
        case TA_CBOR_UINT: guard let v = T(exactly: item.value) else { break }; return v
        case TA_CBOR_NEGINT:
            guard item.value < UInt64(Int64.max), let v = T(exactly: -1 - Int64(item.value)) else { break }
            return v
        default: break
        }
        throw TACodecError(what: "not an integer of \(T.self)")
    }
    func decodeDouble() throws -> Double {
        let item = try scalar()
        switch Int(item.kind) {
        case TA_CBOR_FLOAT: return Double(bitPattern: item.value)
        case TA_CBOR_UINT: return Double(item.value)
        case TA_CBOR_NEGINT: return -1 - Double(item.value)
        default: throw TACodecError(what: "not a number")
        }
    }
    func decodeString() throws -> String {
        let item = try scalar()
        guard Int(item.kind) == TA_CBOR_TEXT else { throw TACodecError(what: "not text") }
        return itemString(item)
    }
    func decodeBool() throws -> Bool {
        let item = try scalar()
        guard Int(item.kind) == TA_CBOR_SIMPLE else { throw TACodecError(what: "not a boolean") }
        if item.value == UInt64(TA_CBOR_TRUE) { return true }
        if item.value == UInt64(TA_CBOR_FALSE) { return false }
        throw TACodecError(what: "not a boolean")
    }
    func decodeNil() -> Bool {
        guard let item = try? scalar() else { return false }
        return Int(item.kind) == TA_CBOR_SIMPLE
            && (item.value == UInt64(TA_CBOR_NULL) || item.value == UInt64(TA_CBOR_UNDEFINED))
    }
    func decodeValue<T: Decodable>(_ type: T.Type) throws -> T {
        if T.self == String.self { return try decodeString() as! T }
        if T.self == Int.self { return try decodeInt(Int.self) as! T }
        return try T(from: self)
    }
}

struct TASingleDecoding: SingleValueDecodingContainer {
    let decoder: TADecoder
    var codingPath: [CodingKey] { [] }
    func decodeNil() -> Bool { decoder.decodeNil() }
    func decode(_: Bool.Type) throws -> Bool { try decoder.decodeBool() }
    func decode(_: String.Type) throws -> String { try decoder.decodeString() }
    func decode(_: Double.Type) throws -> Double { try decoder.decodeDouble() }
    func decode(_: Float.Type) throws -> Float { Float(try decoder.decodeDouble()) }
    func decode(_ t: Int.Type) throws -> Int { try decoder.decodeInt(t) }
    func decode(_ t: Int8.Type) throws -> Int8 { try decoder.decodeInt(t) }
    func decode(_ t: Int16.Type) throws -> Int16 { try decoder.decodeInt(t) }
    func decode(_ t: Int32.Type) throws -> Int32 { try decoder.decodeInt(t) }
    func decode(_ t: Int64.Type) throws -> Int64 { try decoder.decodeInt(t) }
    func decode(_ t: UInt.Type) throws -> UInt { try decoder.decodeInt(t) }
    func decode(_ t: UInt8.Type) throws -> UInt8 { try decoder.decodeInt(t) }
    func decode(_ t: UInt16.Type) throws -> UInt16 { try decoder.decodeInt(t) }
    func decode(_ t: UInt32.Type) throws -> UInt32 { try decoder.decodeInt(t) }
    func decode(_ t: UInt64.Type) throws -> UInt64 { try decoder.decodeInt(t) }
    func decode<T: Decodable>(_ t: T.Type) throws -> T { try decoder.decodeValue(t) }
}

struct TAUnkeyedDecoding: UnkeyedDecodingContainer {
    let base: UnsafeRawBufferPointer
    var reader: ta_cbor_reader
    let count: Int?
    var currentIndex = 0
    var codingPath: [CodingKey] { [] }
    var isAtEnd: Bool { currentIndex >= count! }

    init(_ d: TADecoder) throws {
        base = d.span
        reader = d.reader()
        let head = try cborRead(&reader)
        guard Int(head.kind) == TA_CBOR_ARRAY else { throw TACodecError(what: "not an array") }
        count = Int(head.count)
    }
    mutating func next() throws -> TADecoder {
        guard !isAtEnd else { throw TACodecError(what: "past the array's end") }
        let start = reader.at
        let end = bench_skip(&reader)
        guard reader.status == TA_OK else { throw TACodecError(what: "malformed") }
        currentIndex += 1
        return TADecoder(span: UnsafeRawBufferPointer(rebasing: base[start..<end]))
    }
    mutating func decodeNil() throws -> Bool {
        let d = try next()
        if d.decodeNil() { return true }
        currentIndex -= 1; reader = TADecoder(span: base).reader() // rare path: not used here
        throw TACodecError(what: "decodeNil on a non-null in an array is unsupported")
    }
    mutating func decode(_: Bool.Type) throws -> Bool { try next().decodeBool() }
    mutating func decode(_: String.Type) throws -> String { try next().decodeString() }
    mutating func decode(_: Double.Type) throws -> Double { try next().decodeDouble() }
    mutating func decode(_: Float.Type) throws -> Float { Float(try next().decodeDouble()) }
    mutating func decode(_ t: Int.Type) throws -> Int { try next().decodeInt(t) }
    mutating func decode(_ t: Int8.Type) throws -> Int8 { try next().decodeInt(t) }
    mutating func decode(_ t: Int16.Type) throws -> Int16 { try next().decodeInt(t) }
    mutating func decode(_ t: Int32.Type) throws -> Int32 { try next().decodeInt(t) }
    mutating func decode(_ t: Int64.Type) throws -> Int64 { try next().decodeInt(t) }
    mutating func decode(_ t: UInt.Type) throws -> UInt { try next().decodeInt(t) }
    mutating func decode(_ t: UInt8.Type) throws -> UInt8 { try next().decodeInt(t) }
    mutating func decode(_ t: UInt16.Type) throws -> UInt16 { try next().decodeInt(t) }
    mutating func decode(_ t: UInt32.Type) throws -> UInt32 { try next().decodeInt(t) }
    mutating func decode(_ t: UInt64.Type) throws -> UInt64 { try next().decodeInt(t) }
    mutating func decode<T: Decodable>(_ t: T.Type) throws -> T { try next().decodeValue(t) }
    mutating func nestedContainer<N: CodingKey>(keyedBy t: N.Type) throws -> KeyedDecodingContainer<N> {
        try next().container(keyedBy: t)
    }
    mutating func nestedUnkeyedContainer() throws -> UnkeyedDecodingContainer { try next().unkeyedContainer() }
    mutating func superDecoder() throws -> Decoder { throw TACodecError(what: "no superDecoder") }
}

struct TAKeyedDecoding<Key: CodingKey>: KeyedDecodingContainerProtocol {
    struct Entry { let key: UnsafeRawBufferPointer; let value: UnsafeRawBufferPointer }
    var entries: [Entry] = []
    var codingPath: [CodingKey] { [] }
    var allKeys: [Key] {
        entries.compactMap { Key(stringValue: String(decoding: $0.key, as: UTF8.self)) }
    }

    init(_ d: TADecoder) throws {
        var r = d.reader()
        let head = try cborRead(&r)
        guard Int(head.kind) == TA_CBOR_MAP else { throw TACodecError(what: "not a map") }
        entries.reserveCapacity(Int(head.count))
        for _ in 0..<Int(head.count) {
            let k = try cborRead(&r)
            guard Int(k.kind) == TA_CBOR_TEXT else { throw TACodecError(what: "a key that is not text") }
            let start = r.at
            let end = bench_skip(&r)
            guard r.status == TA_OK else { throw TACodecError(what: "malformed") }
            entries.append(Entry(key: UnsafeRawBufferPointer(start: k.bytes.pointer, count: Int(k.bytes.length)),
                                 value: UnsafeRawBufferPointer(rebasing: d.span[start..<end])))
        }
    }
    func find(_ key: Key) -> TADecoder? {
        var name = key.stringValue
        return name.withUTF8 { want in
            for e in entries where e.key.count == want.count
                && memcmp(e.key.baseAddress!, want.baseAddress!, want.count) == 0 {
                return TADecoder(span: e.value)
            }
            return nil
        }
    }
    func value(_ key: Key) throws -> TADecoder {
        guard let d = find(key) else { throw TACodecError(what: "no key \(key.stringValue)") }
        return d
    }
    func contains(_ key: Key) -> Bool { find(key) != nil }
    func decodeNil(forKey key: Key) throws -> Bool { try value(key).decodeNil() }
    func decode(_: Bool.Type, forKey k: Key) throws -> Bool { try value(k).decodeBool() }
    func decode(_: String.Type, forKey k: Key) throws -> String { try value(k).decodeString() }
    func decode(_: Double.Type, forKey k: Key) throws -> Double { try value(k).decodeDouble() }
    func decode(_: Float.Type, forKey k: Key) throws -> Float { Float(try value(k).decodeDouble()) }
    func decode(_ t: Int.Type, forKey k: Key) throws -> Int { try value(k).decodeInt(t) }
    func decode(_ t: Int8.Type, forKey k: Key) throws -> Int8 { try value(k).decodeInt(t) }
    func decode(_ t: Int16.Type, forKey k: Key) throws -> Int16 { try value(k).decodeInt(t) }
    func decode(_ t: Int32.Type, forKey k: Key) throws -> Int32 { try value(k).decodeInt(t) }
    func decode(_ t: Int64.Type, forKey k: Key) throws -> Int64 { try value(k).decodeInt(t) }
    func decode(_ t: UInt.Type, forKey k: Key) throws -> UInt { try value(k).decodeInt(t) }
    func decode(_ t: UInt8.Type, forKey k: Key) throws -> UInt8 { try value(k).decodeInt(t) }
    func decode(_ t: UInt16.Type, forKey k: Key) throws -> UInt16 { try value(k).decodeInt(t) }
    func decode(_ t: UInt32.Type, forKey k: Key) throws -> UInt32 { try value(k).decodeInt(t) }
    func decode(_ t: UInt64.Type, forKey k: Key) throws -> UInt64 { try value(k).decodeInt(t) }
    func decode<T: Decodable>(_ t: T.Type, forKey k: Key) throws -> T { try value(k).decodeValue(t) }
    func nestedContainer<N: CodingKey>(keyedBy t: N.Type, forKey k: Key) throws -> KeyedDecodingContainer<N> {
        try value(k).container(keyedBy: t)
    }
    func nestedUnkeyedContainer(forKey k: Key) throws -> UnkeyedDecodingContainer { try value(k).unkeyedContainer() }
    func superDecoder() throws -> Decoder { throw TACodecError(what: "no superDecoder") }
    func superDecoder(forKey key: Key) throws -> Decoder { throw TACodecError(what: "no superDecoder") }
}

// ---- encoding -------------------------------------------------------------------

struct TAEncoder: Encoder {
    let w: UnsafeMutablePointer<ta_cbor_writer>
    var codingPath: [CodingKey] { [] }
    var userInfo: [CodingUserInfoKey: Any] { [:] }

    func container<Key: CodingKey>(keyedBy type: Key.Type) -> KeyedEncodingContainer<Key> {
        ta_cbor_write_map(w, TA_CBOR_COUNT_LATER)
        return KeyedEncodingContainer(TAKeyedEncoding<Key>(w: w))
    }
    func unkeyedContainer() -> UnkeyedEncodingContainer {
        ta_cbor_write_array(w, TA_CBOR_COUNT_LATER)
        return TAUnkeyedEncoding(w: w)
    }
    func singleValueContainer() -> SingleValueEncodingContainer { TASingleEncoding(w: w) }

    /// Encode one value whole: close what it opened.
    func encodeValue<T: Encodable>(_ value: T) throws {
        if let s = value as? String { return text(s) }
        if let i = value as? Int { return ta_cbor_write_int(w, Int64(i)) }
        let depth = w.pointee.depth
        try value.encode(to: self)
        while w.pointee.depth > depth { ta_cbor_write_end(w) }
    }
    func text(_ s: String) {
        var s = s
        s.withUTF8 { u in
            ta_cbor_write_text(w, ta_string_view(pointer: UnsafeRawPointer(u.baseAddress)?
                .assumingMemoryBound(to: CChar.self), length: UInt32(u.count)))
        }
    }
    func bool(_ b: Bool) { ta_cbor_write_simple(w, UInt32(b ? TA_CBOR_TRUE : TA_CBOR_FALSE)) }
    func int<T: FixedWidthInteger>(_ v: T) {
        if v < 0 { ta_cbor_write_int(w, Int64(v)) } else { ta_cbor_write_uint(w, UInt64(v)) }
    }
    func double(_ d: Double) { ta_cbor_write_float(w, d) }
    func null() { ta_cbor_write_simple(w, UInt32(TA_CBOR_NULL)) }
}

struct TASingleEncoding: SingleValueEncodingContainer {
    let w: UnsafeMutablePointer<ta_cbor_writer>
    var e: TAEncoder { TAEncoder(w: w) }
    var codingPath: [CodingKey] { [] }
    mutating func encodeNil() throws { e.null() }
    mutating func encode(_ v: Bool) throws { e.bool(v) }
    mutating func encode(_ v: String) throws { e.text(v) }
    mutating func encode(_ v: Double) throws { e.double(v) }
    mutating func encode(_ v: Float) throws { e.double(Double(v)) }
    mutating func encode(_ v: Int) throws { e.int(v) }
    mutating func encode(_ v: Int8) throws { e.int(v) }
    mutating func encode(_ v: Int16) throws { e.int(v) }
    mutating func encode(_ v: Int32) throws { e.int(v) }
    mutating func encode(_ v: Int64) throws { e.int(v) }
    mutating func encode(_ v: UInt) throws { e.int(v) }
    mutating func encode(_ v: UInt8) throws { e.int(v) }
    mutating func encode(_ v: UInt16) throws { e.int(v) }
    mutating func encode(_ v: UInt32) throws { e.int(v) }
    mutating func encode(_ v: UInt64) throws { e.int(v) }
    mutating func encode<T: Encodable>(_ v: T) throws { try e.encodeValue(v) }
}

struct TAUnkeyedEncoding: UnkeyedEncodingContainer {
    let w: UnsafeMutablePointer<ta_cbor_writer>
    var count = 0
    var e: TAEncoder { TAEncoder(w: w) }
    var codingPath: [CodingKey] { [] }
    mutating func encodeNil() throws { count += 1; e.null() }
    mutating func encode(_ v: Bool) throws { count += 1; e.bool(v) }
    mutating func encode(_ v: String) throws { count += 1; e.text(v) }
    mutating func encode(_ v: Double) throws { count += 1; e.double(v) }
    mutating func encode(_ v: Float) throws { count += 1; e.double(Double(v)) }
    mutating func encode(_ v: Int) throws { count += 1; e.int(v) }
    mutating func encode(_ v: Int8) throws { count += 1; e.int(v) }
    mutating func encode(_ v: Int16) throws { count += 1; e.int(v) }
    mutating func encode(_ v: Int32) throws { count += 1; e.int(v) }
    mutating func encode(_ v: Int64) throws { count += 1; e.int(v) }
    mutating func encode(_ v: UInt) throws { count += 1; e.int(v) }
    mutating func encode(_ v: UInt8) throws { count += 1; e.int(v) }
    mutating func encode(_ v: UInt16) throws { count += 1; e.int(v) }
    mutating func encode(_ v: UInt32) throws { count += 1; e.int(v) }
    mutating func encode(_ v: UInt64) throws { count += 1; e.int(v) }
    mutating func encode<T: Encodable>(_ v: T) throws { count += 1; try e.encodeValue(v) }
    mutating func nestedContainer<N: CodingKey>(keyedBy t: N.Type) -> KeyedEncodingContainer<N> {
        count += 1; return e.container(keyedBy: t) // closed with the outer value
    }
    mutating func nestedUnkeyedContainer() -> UnkeyedEncodingContainer { count += 1; return e.unkeyedContainer() }
    mutating func superEncoder() -> Encoder { e }
}

struct TAKeyedEncoding<Key: CodingKey>: KeyedEncodingContainerProtocol {
    let w: UnsafeMutablePointer<ta_cbor_writer>
    var e: TAEncoder { TAEncoder(w: w) }
    var codingPath: [CodingKey] { [] }
    func key(_ k: Key) { e.text(k.stringValue) }
    mutating func encodeNil(forKey k: Key) throws { key(k); e.null() }
    mutating func encode(_ v: Bool, forKey k: Key) throws { key(k); e.bool(v) }
    mutating func encode(_ v: String, forKey k: Key) throws { key(k); e.text(v) }
    mutating func encode(_ v: Double, forKey k: Key) throws { key(k); e.double(v) }
    mutating func encode(_ v: Float, forKey k: Key) throws { key(k); e.double(Double(v)) }
    mutating func encode(_ v: Int, forKey k: Key) throws { key(k); e.int(v) }
    mutating func encode(_ v: Int8, forKey k: Key) throws { key(k); e.int(v) }
    mutating func encode(_ v: Int16, forKey k: Key) throws { key(k); e.int(v) }
    mutating func encode(_ v: Int32, forKey k: Key) throws { key(k); e.int(v) }
    mutating func encode(_ v: Int64, forKey k: Key) throws { key(k); e.int(v) }
    mutating func encode(_ v: UInt, forKey k: Key) throws { key(k); e.int(v) }
    mutating func encode(_ v: UInt8, forKey k: Key) throws { key(k); e.int(v) }
    mutating func encode(_ v: UInt16, forKey k: Key) throws { key(k); e.int(v) }
    mutating func encode(_ v: UInt32, forKey k: Key) throws { key(k); e.int(v) }
    mutating func encode(_ v: UInt64, forKey k: Key) throws { key(k); e.int(v) }
    mutating func encode<T: Encodable>(_ v: T, forKey k: Key) throws { key(k); try e.encodeValue(v) }
    mutating func nestedContainer<N: CodingKey>(keyedBy t: N.Type, forKey k: Key) -> KeyedEncodingContainer<N> {
        key(k); return e.container(keyedBy: t)
    }
    mutating func nestedUnkeyedContainer(forKey k: Key) -> UnkeyedEncodingContainer { key(k); return e.unkeyedContainer() }
    mutating func superEncoder() -> Encoder { e }
    mutating func superEncoder(forKey k: Key) -> Encoder { key(k); return e }
}
