// What the generator writes a class of, per chart, and how core reaches it.
import CTinyactors

/// A session of a chart: the generated subclass holds its variables as
/// properties and its expressions as the cases of these methods.
open class Session {
    public internal(set) var _event = Event()
    public internal(set) var _sessionid = ""
    public internal(set) var _name = ""
    public internal(set) var _ioprocessors: [String: String] = [:]
    var given: Set<Int> = []
    var request: UnsafeMutablePointer<ta_precompiled_request>?

    public required init() {}

    /// SCXML's In(): whether the state is active.
    public func In(_ state: String) -> Bool {
        guard let request else { return false }
        var name = state
        return name.withUTF8 { bytes in
            bytes.withMemoryRebound(to: CChar.self) {
                ta_datamodel_precompiled_in(request, ta_string_view(pointer: $0.baseAddress, length: UInt32($0.count))) != 0
            }
        }
    }

    open func initData(_ declaration: Int) throws {}
    open func setData(_ declaration: Int, _ value: Any) throws {}
    open func condition(_ expression: Int) throws -> Bool { throw noExpression(expression) }
    open func value(_ expression: Int) throws -> Any { throw noExpression(expression) }
    open func encode(_ expression: Int) throws -> [UInt8] { throw noExpression(expression) }
    open func string(_ expression: Int) throws -> String { throw noExpression(expression) }
    open func assign(_ location: Int, _ value: Any) throws { throw noExpression(location) }
    open func script(_ expression: Int) throws { throw noExpression(expression) }
    /// The variables, by name, each as its CBOR.
    open func variables() throws -> [(String, CBOR)] { [] }
    open func setVariables(_ variables: Value) throws {}

    public func noExpression(_ index: Int) -> Error { CodingError("no expression \(index) of this kind in the generated code") }
}

/// The generated code of one chart.
public struct Chart {
    public let name: String
    public let fingerprint: UInt64
    public let data: [String]
    public let make: () -> Session
    public init(name: String, fingerprint: UInt64, data: [String], make: @escaping () -> Session) {
        self.name = name
        self.fingerprint = fingerprint
        self.data = data
        self.make = make
    }
}

/// The variables as one CBOR map, for a blob.
public func variableMap(_ variables: [(String, CBOR)]) -> CBOR { .map(variables.map { (.text($0.0), $0.1) }) }
public func variableTree<T: Encodable>(_ value: T) throws -> CBOR { try tree(value) }
