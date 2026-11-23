// A small host for the spike: one neutral datamodel (json or staticjs), the
// app's functions on it, one I/O processor for the app, and a clock the app
// moves (simulated time, in milliseconds). Not a binding: what one needs,
// and no more.
import CTinyactors

public enum Datamodel: String { case json, staticjs }

/// A send to the app, read before the ack.
public struct Send {
    public let session: UInt64
    public let event: String
    public let target: String
    public let data: Value
}

public final class Host {
    let host: OpaquePointer
    let datamodel: Datamodel
    let json: OpaquePointer?
    let staticjs: OpaquePointer?
    let binding: ta_datamodel_binding
    let functions: Functions
    var counter = Counter()
    public private(set) var now: UInt64 = 0

    /// What the app does with a send to it, a log line, an actor that ended.
    public var onSend: (Send) -> Void = { _ in }
    public var onLog: (UInt64, String, String) -> Void = { _, _, _ in }
    public var onDone: (UInt64, Value) -> Void = { _, _ in }

    final class Counter { var live = 0 }

    public init(datamodel: Datamodel, chart path: String, functions: Functions, ioProcessor: String) throws {
        self.datamodel = datamodel
        self.functions = functions
        var jsonModel: OpaquePointer?
        var staticjsModel: OpaquePointer?
        switch datamodel {
        case .json:
            var config = ta_datamodel_json_config()
            config.size = UInt32(MemoryLayout<ta_datamodel_json_config>.size)
            config.allocator = ta_runtime_malloc()
            guard ta_datamodel_json_new(&config, &jsonModel) == ta_status(TA_OK) else { throw CodingError("json datamodel") }
            binding = ta_datamodel_json_binding(jsonModel)
        case .staticjs:
            var config = ta_datamodel_staticjs_config()
            config.size = UInt32(MemoryLayout<ta_datamodel_staticjs_config>.size)
            config.allocator = ta_runtime_malloc()
            guard ta_datamodel_staticjs_new(&config, &staticjsModel) == ta_status(TA_OK) else { throw CodingError("staticjs datamodel") }
            binding = ta_datamodel_staticjs_binding(staticjsModel)
        }
        json = jsonModel
        staticjs = staticjsModel
        // the same functions, through the same ABI, whichever datamodel
        for entry in functions.entries {
            let status: ta_status = entry.name.withCString { name in
                entry.chart.withCString { chart in
                    var e = ta_json_function_entry()
                    e.size = UInt32(MemoryLayout<ta_json_function_entry>.size)
                    e.name = ta_string_view(pointer: name, length: UInt32(entry.name.utf8.count))
                    e.chart = ta_string_view(pointer: chart, length: UInt32(entry.chart.utf8.count))
                    e.function = functionTrampoline
                    e.context = Unmanaged.passUnretained(entry).toOpaque()
                    return datamodel == .json ? ta_datamodel_json_register(jsonModel, &e) : ta_datamodel_staticjs_register_json(staticjsModel, &e)
                }
            }
            guard status == ta_status(TA_OK) else { throw CodingError("register \(entry.name)") }
        }
        // the config: {"systemID": "market", "ioProcessors": [{"type": ioProcessor}]}
        let config = CBOR.map([(.text("systemID"), .text("app")),
                               (.text("ioProcessors"), .array([.map([(.text("type"), .text(ioProcessor))])]))]).encoded()
        var made: OpaquePointer?
        var bindings = [binding]
        let status: ta_status = config.withUnsafeBytes { bytes in
            bindings.withUnsafeMutableBufferPointer { b in
                var environment = ta_host_environment()
                environment.size = UInt32(MemoryLayout<ta_host_environment>.size)
                environment.datamodel_count = 1
                environment.datamodels = UnsafePointer(b.baseAddress)
                environment.allocator = ta_runtime_malloc()
                return ta_host_create(ta_bytes(pointer: bytes.baseAddress, length: bytes.count), &environment, &made)
            }
        }
        guard status == ta_status(TA_OK), let made else { throw CodingError("host refused its config") }
        host = made
        // the chart, and the charts it invokes, through the runtime's file resolver
        var compiled: OpaquePointer?
        guard ta_runtime_compile_file(path, 0, &compiled) == ta_status(TA_OK) else {
            var diagnostics: UnsafePointer<ta_diagnostic>?
            let count = compiled.map { ta_compile_diagnostics($0, &diagnostics) } ?? 0
            let text = (0..<Int(count)).map { i -> String in
                let d = diagnostics![i]
                return "\(string(d.file)):\(d.line):\(d.column): \(string(d.message))"
            }.joined(separator: "\n")
            throw CodingError("\(path) does not compile\n\(text)")
        }
        let image = ta_compile_image(compiled)
        var segment: UInt32 = 0
        let added = ta_host_add_image(host, ta_bytes(pointer: image.pointer, length: Int(image.length)), &segment)
        ta_compile_free(compiled)
        guard added == ta_status(TA_OK) else { throw CodingError("add_image: \(String(cString: ta_status_name(added)))") }
    }

    deinit {
        ta_host_free(host)
        if let json { ta_datamodel_json_free(json) }
        if let staticjs { ta_datamodel_staticjs_free(staticjs) }
    }

    public func spawn(_ chart: String) throws -> UInt64 {
        var ref = ta_host_chart_ref()
        guard chart.withCString({ ta_host_chart(host, ta_string_view(pointer: $0, length: UInt32(chart.utf8.count)), &ref) }) == ta_status(TA_OK)
        else { throw CodingError("no chart \(chart)") }
        var spawn = ta_spawn(segment: ref.segment, chart: ref.chart, name: ta_string_view(), params: 0, traceparent: ta_string_view())
        var session: UInt64 = 0
        let status = ta_host_spawn(host, &spawn, &session)
        guard status == ta_status(TA_OK) else { throw CodingError("spawn: \(String(cString: ta_status_name(status)))") }
        return session
    }

    /// An event for a session, its data any Encodable value.
    public func send<T: Encodable>(_ session: UInt64, _ event: String, _ data: T) throws {
        let bytes = try cbor(data)
        var payload: ta_payload = 0
        let made = bytes.withUnsafeBytes { binding.vtable.pointee.from_wire(binding.datamodel, ta_bytes(pointer: $0.baseAddress, length: $0.count), &payload) }
        guard made == ta_status(TA_OK) else { throw CodingError("from_wire") }
        let status: ta_status = event.withCString { name in
            var input = ta_event_in()
            input.size = UInt32(MemoryLayout<ta_event_in>.size)
            input.session = session
            input.name = ta_string_view(pointer: name, length: UInt32(event.utf8.count))
            input.data = payload
            return ta_host_send(host, &input)
        }
        if status != ta_status(TA_OK) {
            binding.vtable.pointee.release(binding.datamodel, payload)
            throw CodingError("send \(event): \(String(cString: ta_status_name(status)))")
        }
    }

    public func send(_ session: UInt64, _ event: String) throws { try send(session, event, Value(.undefined)) }

    func value(_ payload: ta_payload) -> Value {
        var wire = ta_bytes()
        guard payload != 0, binding.vtable.pointee.to_wire(binding.datamodel, payload, &wire) == ta_status(TA_OK) else { return Value(.undefined) }
        let bytes = Array(UnsafeRawBufferPointer(start: wire.pointer, count: wire.length))
        return (try? Value(bytes: bytes)) ?? Value(.undefined)
    }

    func drain() {
        var effects: UnsafePointer<ta_host_effect>?
        while true {
            let count = ta_host_effects(host, &effects)
            if count == 0 { break }
            for i in 0..<Int(count) {
                let e = effects![i]
                switch Int(e.kind) {
                case TA_EFFECT_SEND where e.io_processor == 1:
                    onSend(Send(session: e.session, event: string(e.event), target: string(e.target), data: value(e.payload)))
                case TA_EFFECT_LOG:
                    onLog(e.session, string(e.auxiliary), string(e.event))
                case TA_EFFECT_DONE:
                    onDone(e.session, value(e.payload))
                case TA_EFFECT_ERROR:
                    onLog(e.session, "error", "the actor failed")
                default: break
                }
            }
            ta_host_effects_ack(host)
        }
    }

    /// Moves the clock to `time` (ms) and runs until idle; the next deadline in ms, if any.
    @discardableResult
    public func run(at time: UInt64) -> UInt64? {
        now = time
        while true {
            drain()
            ta_host_advance_time(host, ta_host_time(monotonic: now * 1_000_000, wall_ns: now * 1_000_000))
            let run = ta_host_run(host, 100_000)
            drain()
            switch Int(run.status) {
            case TA_RUN_IDLE:
                return run.next_deadline == UInt64.max ? nil : (run.next_deadline + 999_999) / 1_000_000
            case TA_RUN_FAULT, TA_RUN_EXHAUSTED:
                fatalError("run: \(String(cString: ta_status_name(ta_status(Int32(bitPattern: run.detail)))))")
            default: continue
            }
        }
    }

    /// Bytes the datamodel holds now.
    public var datamodelBytes: Int {
        datamodel == .json ? Int(ta_datamodel_json_bytes(json)) : Int(ta_datamodel_staticjs_bytes(staticjs))
    }
}

func string(_ view: ta_string_view) -> String {
    guard let p = view.pointer, view.length > 0 else { return "" }
    return String(decoding: UnsafeRawBufferPointer(start: p, count: Int(view.length)), as: UTF8.self)
}
