// What crosses from another actor or the application: a value the chart
// reads by member (`_event.data.country`) and converts to a Swift type where
// it is assigned or decoded.

@dynamicMemberLookup
public struct Value: Codable, CustomStringConvertible {
    public let item: CBOR

    public init(_ item: CBOR) { self.item = item }
    public init(bytes: [UInt8]) throws { item = try CBOR.parse(bytes[...]).0 }

    /// A map's member; undefined when this is not a map or lacks it.
    public subscript(dynamicMember key: String) -> Value { self[key] }
    public subscript(key: String) -> Value {
        guard case .map(let pairs) = item else { return Value(.undefined) }
        return Value(pairs.first { $0.0 == .text(key) }?.1 ?? .undefined)
    }
    /// An array's item; undefined past its end.
    public subscript(index: Int) -> Value {
        guard case .array(let items) = item, items.indices.contains(index) else { return Value(.undefined) }
        return Value(items[index])
    }

    public var isDefined: Bool { item != .undefined }

    /// The value as a Swift type: `let item: Item = try data.decode()`.
    public func decode<T: Decodable>(_ type: T.Type = T.self) throws -> T { try Tinyactors.decode(T.self, from: item) }

    public init(from decoder: Decoder) throws {
        guard let tree = decoder as? TreeDecoder else { throw CodingError("a Value decodes from CBOR only") }
        item = tree.item
    }
    public func encode(to encoder: Encoder) throws {
        guard let tree = encoder as? TreeEncoder else { throw CodingError("a Value encodes to CBOR only") }
        tree.node.value = item
    }

    /// JSON-like text, for logs.
    public var description: String {
        switch item {
        case .unsigned(let n): return String(n)
        case .negative(let n): return "-\(n &+ 1)"
        case .float(let d): return String(d)
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

/// _event: what the session's current event says.
public struct Event: Codable {
    public var name = "", type = "", sendid = "", origin = "", origintype = "", invokeid = "", raw = ""
    public var data = Value(.undefined)
    public init() {}
}

/// What an expression gave, as the type of the location it is assigned to:
/// the value itself when it is a T, a Value decoded into a T.
public func cast<T>(_ value: Any) throws -> T {
    if let t = value as? T { return t }
    if let v = value as? Value, let type = T.self as? Decodable.Type { return try decodeAny(type, from: v.item) as! T }
    throw CodingError("a \(Swift.type(of: value)) is not a \(T.self)")
}

private func decodeAny(_ type: Decodable.Type, from item: CBOR) throws -> Decodable {
    if type == Value.self { return Value(item) }
    return try type.init(from: TreeDecoder(item: item))
}

/// What a <log> or a *expr attribute shows of a value.
public func text(_ value: Any) -> String {
    if let s = value as? String { return s }
    return String(describing: value)
}
