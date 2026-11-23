// A value from another actor or the app, read by member and decoded where
// the app wants a Swift type: `let quote: Quote = try value.decode()`.

@dynamicMemberLookup
public struct Value: Codable, CustomStringConvertible {
    public let item: CBOR
    public init(_ item: CBOR) { self.item = item }
    public init(bytes: [UInt8]) throws { item = try CBOR.parse(bytes[...]).0 }

    public subscript(dynamicMember key: String) -> Value { self[key] }
    public subscript(key: String) -> Value {
        guard case .map(let pairs) = item else { return Value(.undefined) }
        return Value(pairs.first { $0.0 == .text(key) }?.1 ?? .undefined)
    }
    public var isDefined: Bool { item != .undefined }
    public func decode<T: Decodable>(_ type: T.Type = T.self) throws -> T { try Tinyactors.decode(T.self, from: item) }

    public init(from decoder: Decoder) throws {
        guard let tree = decoder as? TreeDecoder else { throw CodingError("a Value decodes from CBOR only") }
        item = tree.item
    }
    public func encode(to encoder: Encoder) throws {
        guard let tree = encoder as? TreeEncoder else { throw CodingError("a Value encodes to CBOR only") }
        tree.node.value = item
    }

    /// JSON-like text.
    public var description: String {
        switch item {
        case .unsigned(let n): return String(n)
        case .negative(let n): return "-\(n &+ 1)"
        case .float(let d): return d == d.rounded() && abs(d) < 1e15 ? String(Int(d)) : String(d)
        case .text(let s): return "\"\(s)\""
        case .bytes(let b): return "h'\(b.map { String($0, radix: 16) }.joined())'"
        case .bool(let b): return String(b)
        case .null: return "null"
        case .undefined: return "undefined"
        case .array(let items): return "[" + items.map { Value($0).description }.joined(separator: ", ") + "]"
        case .map(let pairs): return "{" + pairs.map { "\(Value($0.0)): \(Value($0.1))" }.joined(separator: ", ") + "}"
        case .tagged(let tag, let item): return "\(tag)(\(Value(item)))"
        }
    }
}
