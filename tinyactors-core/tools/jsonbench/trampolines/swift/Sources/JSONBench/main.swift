// json-bench, Swift: the cost per call of a Swift function behind the json
// datamodel's ABI (ta_json_function). One @convention(c) trampoline; the
// call's context is an Unmanaged box of a Swift closure. The loop is C
// (bench_run), so a call's time includes the C -> Swift crossing.
//
//   JSONBench [iterations] [runs]
//   (JSONBENCH_ONLY=<language/name part>, JSONBENCH_VERBOSE=1: arguments and results)
import CTinyactors
import Foundation
import SwiftCBOR

struct Item: Codable { var sku: String; var qty: Int; var cents: Int }
struct Add: Codable { var sku: String; var qty: Int }

/// hasItems(items): arguments [items]
struct HasItemsArguments: Decodable {
    var items: [Item]
    init(from decoder: Decoder) throws {
        var c = try decoder.unkeyedContainer()
        items = try c.decode([Item].self)
    }
}

/// addItem(items, add): arguments [items, add]
struct AddItemArguments: Decodable {
    var items: [Item]
    var add: Add
    init(from decoder: Decoder) throws {
        var c = try decoder.unkeyedContainer()
        items = try c.decode([Item].self)
        add = try c.decode(Add.self)
    }
}

func addItem(_ a: AddItemArguments) -> [Item] {
    var items = a.items
    let cents = items.first(where: { $0.sku == a.add.sku })?.cents ?? 0
    items.append(Item(sku: a.add.sku, qty: a.add.qty, cents: cents))
    return items
}

// ---- the trampoline --------------------------------------------------------------

typealias Body = (UnsafeMutablePointer<ta_json_call>) throws -> Void

final class FunctionBox {
    let body: Body
    init(_ body: @escaping Body) { self.body = body }
}

let failure: StaticString = "the Swift function threw"

/// The one C entry point every Swift function goes through.
let trampoline: ta_json_function = { call in
    let call = call!
    let box = Unmanaged<FunctionBox>.fromOpaque(call.pointee.context).takeUnretainedValue()
    do {
        try box.body(call)
        return taOK
    } catch {
        failure.withUTF8Buffer { u in
            call.pointee.message = ta_string_view(
                pointer: UnsafeRawPointer(u.baseAddress!).assumingMemoryBound(to: CChar.self),
                length: UInt32(u.count))
        }
        return ta_status(TA_ERROR_INVALID)
    }
}

@inline(__always)
func arguments(_ call: UnsafeMutablePointer<ta_json_call>) -> UnsafeRawBufferPointer {
    UnsafeRawBufferPointer(start: call.pointee.arguments.pointer, count: call.pointee.arguments.length)
}

@inline(__always)
func writeBool(_ call: UnsafeMutablePointer<ta_json_call>, _ b: Bool) {
    ta_cbor_write_simple(call.pointee.result, UInt32(b ? TA_CBOR_TRUE : TA_CBOR_FALSE))
}

// ---- the functions -----------------------------------------------------------------

/// SwiftCBOR's coders take Data and give Data: the arguments wrapped without
/// a copy; the result copied into the writer item by item.
func swiftCBORArguments(_ call: UnsafeMutablePointer<ta_json_call>) -> Data {
    Data(bytesNoCopy: UnsafeMutableRawPointer(mutating: call.pointee.arguments.pointer!),
         count: call.pointee.arguments.length, deallocator: .none)
}

func swiftCBORResult(_ call: UnsafeMutablePointer<ta_json_call>, _ data: Data) {
    data.withUnsafeBytes { b in
        bench_copy_item(call.pointee.result, ta_bytes(pointer: b.baseAddress, length: b.count))
    }
}

let swiftCBORDecoder = CodableCBORDecoder()
let swiftCBOREncoder = CodableCBOREncoder()

struct Function { let language: String; let name: String; let arguments: UInt32; let body: Body }

let functions: [Function] = [
    Function(language: "swift", name: "empty", arguments: 0) { call in
        writeBool(call, true)
    },
    Function(language: "swift", name: "hasItems", arguments: 1) { call in
        let a = try HasItemsArguments(from: TADecoder(span: arguments(call)))
        writeBool(call, !a.items.isEmpty)
    },
    Function(language: "swift", name: "addItem", arguments: 2) { call in
        let a = try AddItemArguments(from: TADecoder(span: arguments(call)))
        try TAEncoder(w: call.pointee.result).encodeValue(addItem(a))
    },
    Function(language: "swift+SwiftCBOR", name: "hasItems", arguments: 1) { call in
        let a = try swiftCBORDecoder.decode(HasItemsArguments.self, from: swiftCBORArguments(call))
        writeBool(call, !a.items.isEmpty)
    },
    Function(language: "swift+SwiftCBOR", name: "addItem", arguments: 2) { call in
        let a = try swiftCBORDecoder.decode(AddItemArguments.self, from: swiftCBORArguments(call))
        swiftCBORResult(call, try swiftCBOREncoder.encode(addItem(a)))
    },
]

// ---- the driver ---------------------------------------------------------------------

let argv = CommandLine.arguments
let iterations = argv.count > 1 ? UInt64(argv[1])! : 1_000_000
let runs = argv.count > 2 ? Int(argv[2])! : 5
let verbose = ProcessInfo.processInfo.environment["JSONBENCH_VERBOSE"] != nil
/// JSONBENCH_ONLY=swift/hasItems runs the functions whose "language/name" contains it.
let only = ProcessInfo.processInfo.environment["JSONBENCH_ONLY"]

let roomBytes = 64 * 1024 // the datamodel's default max_document_bytes
let room = UnsafeMutableRawPointer.allocate(byteCount: roomBytes, alignment: 16)
var argumentBlocks: [UInt32: ta_bytes] = [:]
for which in UInt32(0)...2 {
    let p = UnsafeMutableRawPointer.allocate(byteCount: 256, alignment: 16)
    var out = ta_buffer(pointer: p, capacity: 256, length: 0)
    precondition(bench_arguments(which, &out) == taOK)
    argumentBlocks[which] = ta_bytes(pointer: p, length: out.length)
}

func diagnostic(_ bytes: ta_bytes) -> String {
    var text = [UInt8](repeating: 0, count: 4096)
    return text.withUnsafeMutableBytes { t -> String in
        var out = ta_buffer(pointer: t.baseAddress, capacity: t.count, length: 0)
        guard ta_cbor_diagnostic(bytes, &out) == taOK else { return "?" }
        return String(decoding: UnsafeRawBufferPointer(rebasing: t[0..<out.length]), as: UTF8.self)
    }
}

for f in functions where only.map({ "\(f.language)/\(f.name)".contains($0) }) ?? true {
    let box = Unmanaged.passRetained(FunctionBox(f.body))
    var spec = bench_spec(function: trampoline, context: box.toOpaque(),
                          arguments: argumentBlocks[f.arguments]!, argument_count: f.arguments, pad: 0,
                          iterations: max(iterations / 10, 1), room: ta_buffer(pointer: room, capacity: roomBytes, length: 0))
    var result = bench_result()
    bench_run(&spec, &result) // warm-up
    guard result.status == taOK else { fatalError("\(f.language) \(f.name): status \(result.status)") }
    if verbose {
        print("# \(f.language) \(f.name)(\(diagnostic(spec.arguments))) = \(diagnostic(result.item))")
    }
    spec.iterations = iterations
    var best = UInt64.max
    for _ in 0..<runs {
        bench_run(&spec, &result)
        precondition(result.status == taOK)
        best = min(best, result.nanoseconds)
    }
    let ns = Double(best) / Double(iterations)
    print("\(f.language)\t\(f.name)\t\(String(format: "%.1f", ns)) ns/call")
    box.release()
}
