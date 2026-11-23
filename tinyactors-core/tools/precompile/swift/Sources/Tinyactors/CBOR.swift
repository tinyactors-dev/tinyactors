// A CBOR coder for Codable (a spike): values cross between the chart's
// Swift and core as CBOR (docs/protocol.md, "Values"), and the standard
// library has none. Decoding parses an item into a tree; encoding builds a
// tree and writes it with the shortest heads.

/// One CBOR item.
public indirect enum CBOR: Equatable {
    case unsigned(UInt64)
    case negative(UInt64) // -1 - n
    case bytes([UInt8])
    case text(String)
    case array([CBOR])
    case map([(CBOR, CBOR)])
    case tagged(UInt64, CBOR)
    case bool(Bool)
    case null
    case undefined
    case float(Double)

    public static func == (a: CBOR, b: CBOR) -> Bool { a.encoded() == b.encoded() }

    /// The item's bytes.
    public func encoded() -> [UInt8] {
        var out: [UInt8] = []
        write(into: &out)
        return out
    }

    func write(into out: inout [UInt8]) {
        func head(_ major: UInt8, _ value: UInt64) {
            let m = major << 5
            switch value {
            case ..<24: out.append(m | UInt8(value))
            case ...0xFF: out += [m | 24, UInt8(value)]
            case ...0xFFFF: out += [m | 25, UInt8(value >> 8), UInt8(value & 0xFF)]
            case ...0xFFFF_FFFF: out.append(m | 26); for i in (0..<4).reversed() { out.append(UInt8((value >> (8 * UInt64(i))) & 0xFF)) }
            default: out.append(m | 27); for i in (0..<8).reversed() { out.append(UInt8((value >> (8 * UInt64(i))) & 0xFF)) }
            }
        }
        switch self {
        case .unsigned(let n): head(0, n)
        case .negative(let n): head(1, n)
        case .bytes(let b): head(2, UInt64(b.count)); out += b
        case .text(let s): let u = Array(s.utf8); head(3, UInt64(u.count)); out += u
        case .array(let items): head(4, UInt64(items.count)); for i in items { i.write(into: &out) }
        case .map(let pairs): head(5, UInt64(pairs.count)); for (k, v) in pairs { k.write(into: &out); v.write(into: &out) }
        case .tagged(let tag, let item): head(6, tag); item.write(into: &out)
        case .bool(let b): out.append(b ? 0xF5 : 0xF4)
        case .null: out.append(0xF6)
        case .undefined: out.append(0xF7)
        case .float(let d):
            out.append(0xFB)
            let bits = d.bitPattern
            for i in (0..<8).reversed() { out.append(UInt8((bits >> (8 * UInt64(i))) & 0xFF)) }
        }
    }

    /// The first item of `bytes`, and where it ends.
    public static func parse(_ bytes: ArraySlice<UInt8>) throws -> (CBOR, Int) {
        var at = bytes.startIndex
        func byte() throws -> UInt8 {
            guard at < bytes.endIndex else { throw CodingError("CBOR ends early") }
            defer { at += 1 }
            return bytes[at]
        }
        func argument(_ info: UInt8) throws -> UInt64 {
            switch info {
            case ..<24: return UInt64(info)
            case 24...27:
                var value: UInt64 = 0
                for _ in 0..<(1 << (info - 24)) { value = value << 8 | UInt64(try byte()) }
                return value
            default: throw CodingError("an indefinite or reserved length")
            }
        }
        func item(_ depth: Int) throws -> CBOR {
            guard depth < 64 else { throw CodingError("nested too deep") }
            let first = try byte()
            let major = first >> 5, info = first & 0x1F
            switch major {
            case 0: return .unsigned(try argument(info))
            case 1: return .negative(try argument(info))
            case 2, 3:
                let n = Int(try argument(info))
                guard at + n <= bytes.endIndex else { throw CodingError("CBOR ends early") }
                let slice = Array(bytes[at..<at + n])
                at += n
                return major == 2 ? .bytes(slice) : .text(String(decoding: slice, as: UTF8.self))
            case 4: return .array(try (0..<Int(try argument(info))).map { _ in try item(depth + 1) })
            case 5: return .map(try (0..<Int(try argument(info))).map { _ in (try item(depth + 1), try item(depth + 1)) })
            case 6: return .tagged(try argument(info), try item(depth + 1))
            default:
                switch info {
                case 20: return .bool(false)
                case 21: return .bool(true)
                case 22: return .null
                case 23: return .undefined
                case 25:
                    let half = UInt16(try argument(25))
                    return .float(Double(Float16Bits(half)))
                case 26: return .float(Double(Float(bitPattern: UInt32(try argument(26)))))
                case 27: return .float(Double(bitPattern: try argument(27)))
                default: throw CodingError("an unknown simple value")
                }
            }
        }
        let value = try item(0)
        return (value, at - bytes.startIndex)
    }
}

/// A half float's value (Float16 is not on every platform Swift builds for).
private func Float16Bits(_ h: UInt16) -> Float {
    let sign: Float = h & 0x8000 != 0 ? -1 : 1
    let exponent = Int((h >> 10) & 0x1F), mantissa = Float(h & 0x3FF)
    if exponent == 0 { return sign * mantissa * Float(pow2(-24)) }
    if exponent == 31 { return mantissa == 0 ? sign * .infinity : .nan }
    return sign * (1 + mantissa / 1024) * Float(pow2(exponent - 15))
}

private func pow2(_ e: Int) -> Double { e >= 0 ? Double(1 << e) : 1 / Double(1 << -e) }

public struct CodingError: Error, CustomStringConvertible {
    public let description: String
    public init(_ description: String) { self.description = description }
}

// ---- encoding --------------------------------------------------------------------

/// A value's CBOR, through its Encodable conformance.
public func cbor<T: Encodable>(_ value: T) throws -> [UInt8] {
    try tree(value).encoded()
}

func tree<T: Encodable>(_ value: T) throws -> CBOR {
    if let v = value as? Value { return v.item }
    if let d = value as? Double { return .float(d) }
    let node = Node()
    try value.encode(to: TreeEncoder(node: node))
    return node.build()
}

final class Node {
    var value: CBOR = .undefined
    var keys: [String]? = nil
    var items: [Node]? = nil
    func build() -> CBOR {
        if let keys { return .map(zip(keys, items!).map { (.text($0), $1.build()) }) }
        if let items { return .array(items.map { $0.build() }) }
        return value
    }
}

struct TreeEncoder: Encoder {
    let node: Node
    var codingPath: [CodingKey] = []
    var userInfo: [CodingUserInfoKey: Any] = [:]

    func container<Key: CodingKey>(keyedBy: Key.Type) -> KeyedEncodingContainer<Key> {
        if node.keys == nil { node.keys = []; node.items = [] }
        return KeyedEncodingContainer(Keyed<Key>(node: node))
    }
    func unkeyedContainer() -> UnkeyedEncodingContainer {
        if node.items == nil { node.items = [] }
        return Unkeyed(node: node)
    }
    func singleValueContainer() -> SingleValueEncodingContainer { Single(node: node) }

    struct Keyed<Key: CodingKey>: KeyedEncodingContainerProtocol {
        let node: Node
        var codingPath: [CodingKey] = []
        func child(_ key: Key) -> Node {
            let n = Node()
            node.keys!.append(key.stringValue)
            node.items!.append(n)
            return n
        }
        mutating func encodeNil(forKey key: Key) throws { child(key).value = .null }
        mutating func encode<T: Encodable>(_ value: T, forKey key: Key) throws { child(key).value = try tree(value) }
        mutating func nestedContainer<N: CodingKey>(keyedBy: N.Type, forKey key: Key) -> KeyedEncodingContainer<N> {
            TreeEncoder(node: child(key)).container(keyedBy: N.self)
        }
        mutating func nestedUnkeyedContainer(forKey key: Key) -> UnkeyedEncodingContainer { TreeEncoder(node: child(key)).unkeyedContainer() }
        mutating func superEncoder() -> Encoder { TreeEncoder(node: node) }
        mutating func superEncoder(forKey key: Key) -> Encoder { TreeEncoder(node: child(key)) }
    }

    struct Unkeyed: UnkeyedEncodingContainer {
        let node: Node
        var codingPath: [CodingKey] = []
        var count: Int { node.items!.count }
        func child() -> Node { let n = Node(); node.items!.append(n); return n }
        mutating func encodeNil() throws { child().value = .null }
        mutating func encode<T: Encodable>(_ value: T) throws { child().value = try tree(value) }
        mutating func nestedContainer<N: CodingKey>(keyedBy: N.Type) -> KeyedEncodingContainer<N> { TreeEncoder(node: child()).container(keyedBy: N.self) }
        mutating func nestedUnkeyedContainer() -> UnkeyedEncodingContainer { TreeEncoder(node: child()).unkeyedContainer() }
        mutating func superEncoder() -> Encoder { TreeEncoder(node: child()) }
    }

    struct Single: SingleValueEncodingContainer {
        let node: Node
        var codingPath: [CodingKey] = []
        mutating func encodeNil() throws { node.value = .null }
        mutating func encode(_ value: Bool) throws { node.value = .bool(value) }
        mutating func encode(_ value: String) throws { node.value = .text(value) }
        mutating func encode(_ value: Double) throws { node.value = .float(value) }
        mutating func encode(_ value: Float) throws { node.value = .float(Double(value)) }
        mutating func encode(_ value: Int) throws { node.value = value < 0 ? .negative(UInt64(-1 - value)) : .unsigned(UInt64(value)) }
        mutating func encode(_ value: Int64) throws { try encode(Int(value)) }
        mutating func encode(_ value: Int32) throws { try encode(Int(value)) }
        mutating func encode(_ value: Int16) throws { try encode(Int(value)) }
        mutating func encode(_ value: Int8) throws { try encode(Int(value)) }
        mutating func encode(_ value: UInt) throws { node.value = .unsigned(UInt64(value)) }
        mutating func encode(_ value: UInt64) throws { node.value = .unsigned(value) }
        mutating func encode(_ value: UInt32) throws { node.value = .unsigned(UInt64(value)) }
        mutating func encode(_ value: UInt16) throws { node.value = .unsigned(UInt64(value)) }
        mutating func encode(_ value: UInt8) throws { node.value = .unsigned(UInt64(value)) }
        mutating func encode<T: Encodable>(_ value: T) throws { node.value = try tree(value) }
    }
}

// ---- decoding --------------------------------------------------------------------

/// A value of type T from CBOR, through its Decodable conformance.
public func decode<T: Decodable>(_ type: T.Type, from item: CBOR) throws -> T {
    if type == Value.self { return Value(item) as! T }
    return try T(from: TreeDecoder(item: item))
}

struct TreeDecoder: Decoder {
    let item: CBOR
    var codingPath: [CodingKey] = []
    var userInfo: [CodingUserInfoKey: Any] = [:]

    func container<Key: CodingKey>(keyedBy: Key.Type) throws -> KeyedDecodingContainer<Key> {
        guard case .map(let pairs) = item else { throw CodingError("\(Value(item)) is not a map") }
        var members: [String: CBOR] = [:]
        for (k, v) in pairs { if case .text(let key) = k { members[key] = v } }
        return KeyedDecodingContainer(Keyed<Key>(members: members))
    }
    func unkeyedContainer() throws -> UnkeyedDecodingContainer {
        guard case .array(let items) = item else { throw CodingError("\(Value(item)) is not an array") }
        return Unkeyed(items: items)
    }
    func singleValueContainer() throws -> SingleValueDecodingContainer { Single(item: item) }

    struct Keyed<Key: CodingKey>: KeyedDecodingContainerProtocol {
        let members: [String: CBOR]
        var codingPath: [CodingKey] = []
        var allKeys: [Key] { members.keys.compactMap { Key(stringValue: $0) } }
        func contains(_ key: Key) -> Bool { members[key.stringValue] != nil }
        func member(_ key: Key) throws -> CBOR {
            guard let v = members[key.stringValue] else { throw CodingError("no member \(key.stringValue)") }
            return v
        }
        func decodeNil(forKey key: Key) throws -> Bool {
            switch members[key.stringValue] { case nil, .null?, .undefined?: return true; default: return false }
        }
        func decode<T: Decodable>(_ type: T.Type, forKey key: Key) throws -> T { try Tinyactors.decode(T.self, from: try member(key)) }
        func nestedContainer<N: CodingKey>(keyedBy: N.Type, forKey key: Key) throws -> KeyedDecodingContainer<N> {
            try TreeDecoder(item: try member(key)).container(keyedBy: N.self)
        }
        func nestedUnkeyedContainer(forKey key: Key) throws -> UnkeyedDecodingContainer { try TreeDecoder(item: try member(key)).unkeyedContainer() }
        func superDecoder() throws -> Decoder { TreeDecoder(item: .map(members.map { (.text($0), $1) })) }
        func superDecoder(forKey key: Key) throws -> Decoder { TreeDecoder(item: try member(key)) }
    }

    struct Unkeyed: UnkeyedDecodingContainer {
        let items: [CBOR]
        var codingPath: [CodingKey] = []
        var currentIndex = 0
        var count: Int? { items.count }
        var isAtEnd: Bool { currentIndex >= items.count }
        mutating func next() throws -> CBOR {
            guard !isAtEnd else { throw CodingError("past the array's end") }
            defer { currentIndex += 1 }
            return items[currentIndex]
        }
        mutating func decodeNil() throws -> Bool {
            if !isAtEnd, items[currentIndex] == .null || items[currentIndex] == .undefined { currentIndex += 1; return true }
            return false
        }
        mutating func decode<T: Decodable>(_ type: T.Type) throws -> T { try Tinyactors.decode(T.self, from: try next()) }
        mutating func nestedContainer<N: CodingKey>(keyedBy: N.Type) throws -> KeyedDecodingContainer<N> { try TreeDecoder(item: try next()).container(keyedBy: N.self) }
        mutating func nestedUnkeyedContainer() throws -> UnkeyedDecodingContainer { try TreeDecoder(item: try next()).unkeyedContainer() }
        mutating func superDecoder() throws -> Decoder { TreeDecoder(item: try next()) }
    }

    struct Single: SingleValueDecodingContainer {
        let item: CBOR
        var codingPath: [CodingKey] = []
        func decodeNil() -> Bool { item == .null || item == .undefined }
        func decode(_ type: Bool.Type) throws -> Bool {
            guard case .bool(let b) = item else { throw CodingError("\(Value(item)) is not a Bool") }
            return b
        }
        func decode(_ type: String.Type) throws -> String {
            guard case .text(let s) = item else { throw CodingError("\(Value(item)) is not a String") }
            return s
        }
        func decode(_ type: Double.Type) throws -> Double {
            switch item {
            case .float(let d): return d
            case .unsigned(let n): return Double(n)
            case .negative(let n): return -1 - Double(n)
            default: throw CodingError("\(Value(item)) is not a number")
            }
        }
        func decode(_ type: Float.Type) throws -> Float { Float(try decode(Double.self)) }
        func integer() throws -> Int {
            switch item {
            case .unsigned(let n) where n <= UInt64(Int.max): return Int(n)
            case .negative(let n) where n < UInt64(Int.max): return -1 - Int(n)
            case .float(let d) where d == d.rounded() && abs(d) < 9e15: return Int(d)
            default: throw CodingError("\(Value(item)) is not an integer")
            }
        }
        func decode(_ type: Int.Type) throws -> Int { try integer() }
        func decode(_ type: Int64.Type) throws -> Int64 { Int64(try integer()) }
        func decode(_ type: Int32.Type) throws -> Int32 { Int32(try integer()) }
        func decode(_ type: Int16.Type) throws -> Int16 { Int16(try integer()) }
        func decode(_ type: Int8.Type) throws -> Int8 { Int8(try integer()) }
        func decode(_ type: UInt.Type) throws -> UInt { UInt(try integer()) }
        func decode(_ type: UInt64.Type) throws -> UInt64 { UInt64(try integer()) }
        func decode(_ type: UInt32.Type) throws -> UInt32 { UInt32(try integer()) }
        func decode(_ type: UInt16.Type) throws -> UInt16 { UInt16(try integer()) }
        func decode(_ type: UInt8.Type) throws -> UInt8 { UInt8(try integer()) }
        func decode<T: Decodable>(_ type: T.Type) throws -> T { try Tinyactors.decode(T.self, from: item) }
    }
}
