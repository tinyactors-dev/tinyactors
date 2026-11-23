// The app's functions, which a chart calls by name: registered once with
// their Swift types, and offered to either neutral datamodel (json or
// staticjs) through the one function ABI both take (ta_json_call: the
// arguments one CBOR array, the result one CBOR item). The library decodes
// the arguments into the closure's parameter types and encodes its result;
// the app never sees bytes.
//
//   functions.register("crossed") { (rule: Rule, quote: Quote) -> Bool in
//       quote.symbol == rule.symbol && quote.price >= rule.above
//   }
import CTinyactors

public final class Functions {
    final class Entry {
        let name: String
        let chart: String
        let arity: Int
        let body: ([CBOR]) throws -> CBOR
        var message: [CChar] = [] // a failure's text, kept until the next call
        init(name: String, chart: String, arity: Int, body: @escaping ([CBOR]) throws -> CBOR) {
            self.name = name
            self.chart = chart
            self.arity = arity
            self.body = body
        }
    }

    var entries: [Entry] = []
    public init() {}

    func add(_ name: String, _ chart: String, _ arity: Int, _ body: @escaping ([CBOR]) throws -> CBOR) {
        entries.append(Entry(name: name, chart: chart, arity: arity, body: body))
    }

    public func register<R: Encodable>(_ name: String, chart: String = "", _ f: @escaping () throws -> R) {
        add(name, chart, 0) { _ in try tree(try f()) }
    }
    public func register<A: Decodable, R: Encodable>(_ name: String, chart: String = "", _ f: @escaping (A) throws -> R) {
        add(name, chart, 1) { a in try tree(try f(try decode(A.self, from: a[0]))) }
    }
    public func register<A: Decodable, B: Decodable, R: Encodable>(_ name: String, chart: String = "",
                                                                   _ f: @escaping (A, B) throws -> R) {
        add(name, chart, 2) { a in try tree(try f(try decode(A.self, from: a[0]), try decode(B.self, from: a[1]))) }
    }
    public func register<A: Decodable, B: Decodable, C: Decodable, R: Encodable>(_ name: String, chart: String = "",
                                                                                 _ f: @escaping (A, B, C) throws -> R) {
        add(name, chart, 3) { a in
            try tree(try f(try decode(A.self, from: a[0]), try decode(B.self, from: a[1]), try decode(C.self, from: a[2])))
        }
    }
}

/// The one C function every registered function is called through.
let functionTrampoline: ta_json_function = { call in
    guard let call else { return ta_status(TA_ERROR_INVALID) }
    let entry = Unmanaged<Functions.Entry>.fromOpaque(call.pointee.context).takeUnretainedValue()
    do {
        let raw = Array(UnsafeRawBufferPointer(start: call.pointee.arguments.pointer, count: Int(call.pointee.arguments.length)))
        guard case .array(let arguments) = try CBOR.parse(raw[...]).0 else { throw CodingError("arguments are an array") }
        guard arguments.count == entry.arity else {
            throw CodingError("\(entry.name) takes \(entry.arity) arguments, not \(arguments.count)")
        }
        let result = try entry.body(arguments).encoded()
        let room = call.pointee.result!
        guard result.count <= room.pointee.capacity else {
            room.pointee.length = result.count
            return ta_status(TA_ERROR_NO_MEMORY)
        }
        result.withUnsafeBytes { UnsafeMutableRawPointer(room.pointee.pointer!).copyMemory(from: $0.baseAddress!, byteCount: result.count) }
        room.pointee.length = result.count
        return ta_status(TA_OK)
    } catch {
        entry.message = Array("\(entry.name): \(error)".utf8CString)
        entry.message.withUnsafeBufferPointer {
            call.pointee.message = ta_string_view(pointer: $0.baseAddress, length: UInt32($0.count - 1))
        }
        return ta_status(TA_ERROR_EXECUTION)
    }
}
