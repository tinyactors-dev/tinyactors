// The datamodel's one entry point (ta_precompiled_function), in Swift.
import CTinyactors

nonisolated(unsafe) var charts: [Chart] = []
nonisolated(unsafe) var sessions: [Session?] = [] // a session's handle is its index + 1
nonisolated(unsafe) var free: [Int] = []

private func bytes(_ b: ta_bytes) -> [UInt8] {
    guard let p = b.pointer, b.length > 0 else { return [] }
    return Array(UnsafeRawBufferPointer(start: p, count: Int(b.length)))
}

private func write(_ out: [UInt8], to buffer: inout ta_buffer) -> ta_status {
    guard out.count <= buffer.capacity else { return ta_status(TA_ERROR_NO_MEMORY) }
    out.withUnsafeBytes { UnsafeMutableRawPointer(buffer.pointer!).copyMemory(from: $0.baseAddress!, byteCount: out.count) }
    buffer.length = out.count
    return ta_status(TA_OK)
}

/// CREATE's input: [sessionid, name, {type: location}, params].
private func start(_ session: Session, _ chart: Chart, _ item: CBOR) throws {
    guard case .array(let fields) = item, fields.count == 4 else { throw CodingError("a malformed start") }
    session._sessionid = try decode(String.self, from: fields[0])
    session._name = try decode(String.self, from: fields[1])
    session._ioprocessors = try decode([String: String].self, from: fields[2])
    let params = Value(fields[3])
    for (i, id) in chart.data.enumerated() where params[id].isDefined {
        try session.setData(i, params[id])
        session.given.insert(i)
    }
}

private func call(_ r: UnsafeMutablePointer<ta_precompiled_request>) throws -> ta_status {
    switch Int(r.pointee.operation) {
    case TA_PRECOMPILED_ATTACH:
        let name = String(decoding: bytes(r.pointee.input), as: UTF8.self)
        guard let index = charts.firstIndex(where: { $0.name == name }) else { throw CodingError("no generated code for the chart \"\(name)\"") }
        guard charts[index].fingerprint == r.pointee.session else {
            throw CodingError("the chart \"\(name)\" changed since its code was generated: build again")
        }
        r.pointee.number = UInt32(index)
        return ta_status(TA_OK)
    case TA_PRECOMPILED_CREATE, TA_PRECOMPILED_DESERIALIZE:
        let chart = charts[Int(r.pointee.chart)]
        let session = chart.make()
        let input = bytes(r.pointee.input)
        let (first, length) = try CBOR.parse(input[...])
        try start(session, chart, first)
        if Int(r.pointee.operation) == TA_PRECOMPILED_DESERIALIZE {
            try session.setVariables(Value(try CBOR.parse(input[length...]).0))
        }
        if let i = free.popLast() { sessions[i] = session; r.pointee.session = UInt64(i + 1) }
        else { sessions.append(session); r.pointee.session = UInt64(sessions.count) }
        return ta_status(TA_OK)
    default: break
    }
    let index = Int(r.pointee.session) - 1
    guard let session = sessions[index] else { return ta_status(TA_ERROR_INVALID) }
    session.request = r
    defer { session.request = nil }
    let x = Int(r.pointee.expression)
    switch Int(r.pointee.operation) {
    case TA_PRECOMPILED_DESTROY:
        sessions[index] = nil
        free.append(index)
    case TA_PRECOMPILED_INIT_DATA:
        if !session.given.contains(Int(r.pointee.number)) { try session.initData(Int(r.pointee.number)) }
    case TA_PRECOMPILED_BIND_EVENT:
        session._event = try decode(Event.self, from: try CBOR.parse(bytes(r.pointee.input)[...]).0)
    case TA_PRECOMPILED_CONDITION:
        r.pointee.number = try session.condition(x) ? 1 : 0
    case TA_PRECOMPILED_VALUE:
        return write(try session.encode(x), to: &r.pointee.output)
    case TA_PRECOMPILED_STRING:
        return write(Array(try session.string(x).utf8), to: &r.pointee.output)
    case TA_PRECOMPILED_ASSIGN:
        try session.assign(x, try session.value(Int(r.pointee.number)))
    case TA_PRECOMPILED_ASSIGN_VALUE:
        try session.assign(x, Value(try CBOR.parse(bytes(r.pointee.input)[...]).0))
    case TA_PRECOMPILED_SCRIPT:
        try session.script(x)
    case TA_PRECOMPILED_SERIALIZE:
        return write(variableMap(try session.variables()).encoded(), to: &r.pointee.output)
    case TA_PRECOMPILED_VARIABLE:
        let name = String(decoding: bytes(r.pointee.input), as: UTF8.self)
        guard let v = try session.variables().first(where: { $0.0 == name }) else { return ta_status(TA_ERROR_INVALID) }
        return write(v.1.encoded(), to: &r.pointee.output)
    default:
        return ta_status(TA_ERROR_UNSUPPORTED)
    }
    return ta_status(TA_OK)
}

private let entry: ta_precompiled_function = { _, request in
    guard let request else { return ta_status(TA_ERROR_INVALID) }
    do {
        return try call(request)
    } catch {
        let message = Array(String(describing: error).utf8.prefix(request.pointee.message.capacity))
        _ = write(message, to: &request.pointee.message)
        return ta_status(TA_ERROR_EXECUTION)
    }
}

/// Runs the scripted order (tools/precompile/demo) with these charts' code.
public func runOrderDemo(chartPath: String, charts registered: [Chart]) -> Int32 {
    charts = registered
    var datamodel: OpaquePointer?
    return "swift".withCString { name in
        var config = ta_datamodel_precompiled_config(size: UInt32(MemoryLayout<ta_datamodel_precompiled_config>.size), max_value_bytes: 0,
                                                     name: name, function: entry, context: nil, allocator: ta_runtime_malloc())
        guard ta_datamodel_precompiled_new(&config, &datamodel) == ta_status(TA_OK) else { return 1 }
        defer { ta_datamodel_precompiled_free(datamodel) }
        return ta_demo_order(chartPath, ta_datamodel_precompiled_binding(datamodel))
    }
}
