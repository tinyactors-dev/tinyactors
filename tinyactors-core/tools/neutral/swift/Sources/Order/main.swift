// The order, with json or staticjs charts and the same Swift functions: the
// scripted run, then the cost per event and the datamodel's bytes per actor
// at a population.
//
//   Order json|staticjs [actors]
import Foundation
import Tinyactors

struct Request: Codable { var sku: String; var quantity: Int }
struct Item: Codable { var sku: String; var quantity: Int; var cents: Int }
struct Address: Codable { var country: String }

let prices = ["tea": 450, "mug": 1200]
let functions = Functions()
functions.register("knows") { (r: Request) -> Bool in r.quantity >= 1 && prices[r.sku] != nil }
functions.register("priced") { (r: Request) -> Item in Item(sku: r.sku, quantity: r.quantity, cents: prices[r.sku]!) }
functions.register("placeable") { (items: [Item], address: Address) -> Bool in !items.isEmpty && !address.country.isEmpty }
functions.register("plusLine") { (total: Int, line: Item) -> Int in total + line.cents * line.quantity }

let datamodel = Datamodel(rawValue: CommandLine.arguments.dropFirst().first ?? "json") ?? .json
let actors = CommandLine.arguments.count > 2 ? Int(CommandLine.arguments[2])! : 10_000
let chart = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    .appendingPathComponent("../../../charts/\(datamodel.rawValue)/order.scxml").standardized.path

// the scripted run
do {
    let host = try Host(datamodel: datamodel, chart: chart, functions: functions, ioProcessor: "urn:shop:app")
    host.onLog = { _, label, text in print("  log \(label): \(text)") }
    host.onDone = { _, data in print("  done: \(data)") }
    print("order, \(datamodel.rawValue) chart")
    let order = try host.spawn("order")
    var t: UInt64 = 0
    func step(_ event: String, _ data: some Encodable) throws {
        print("> \(event) \(Value(try CBOR.parse(try cbor(data)[...]).0))")
        try host.send(order, event, data)
        t += 1
        host.run(at: t)
    }
    try step("add", Request(sku: "tea", quantity: 2))
    try step("add", Request(sku: "mug", quantity: 1))
    try step("add", Request(sku: "cake", quantity: 1))
    try step("place", Value(.undefined))
    try step("address", Address(country: "DE"))
    try step("place", Value(.undefined))
}

// the cost: `actors` orders, each sent add, add, address (best of 5 rounds)
do {
    let host = try Host(datamodel: datamodel, chart: chart, functions: functions, ioProcessor: "urn:shop:app")
    var t: UInt64 = 0
    host.run(at: t)
    let before = host.datamodelBytes
    let sessions = try (0..<actors).map { _ in try host.spawn("order") }
    t += 1
    host.run(at: t)
    let idle = host.datamodelBytes
    let tea = Request(sku: "tea", quantity: 2), mug = Request(sku: "mug", quantity: 1), de = Address(country: "DE")
    var best = [Double](repeating: .infinity, count: 2)
    for round in 0..<5 {
        for (k, (event, data)) in [("add", tea as Encodable), ("address", de as Encodable)].enumerated() {
            let start = DispatchTime.now().uptimeNanoseconds
            for (i, s) in sessions.enumerated() {
                if event == "add" { try host.send(s, event, round == 0 && i % 2 == 1 ? mug : tea) }
                else { try host.send(s, event, de) }
                if i % 1024 == 1023 { t += 1; host.run(at: t) }
            }
            t += 1
            host.run(at: t)
            _ = data
            best[k] = min(best[k], Double(DispatchTime.now().uptimeNanoseconds - start) / Double(actors))
        }
    }
    let after = host.datamodelBytes
    print(String(format: "%@ %d actors: datamodel bytes per actor idle %d, after the events %d; ns per event: add %.0f (calls knows and priced), address %.0f (no call)",
                 datamodel.rawValue, actors, (idle - before) / actors, (after - before) / actors, best[0], best[1]))
}

// the Swift side of one call, alone: the arguments read, the result written
do {
    let arguments = try cbor([Request(sku: "tea", quantity: 2)])
    let calls = 200_000
    var best = Double.infinity
    for _ in 0..<5 {
        let start = DispatchTime.now().uptimeNanoseconds
        for _ in 0..<calls {
            guard case .array(let a) = try CBOR.parse(arguments[...]).0 else { fatalError() }
            let r = try decode(Request.self, from: a[0])
            _ = try cbor(Item(sku: r.sku, quantity: r.quantity, cents: prices[r.sku]!)) // priced
        }
        best = min(best, Double(DispatchTime.now().uptimeNanoseconds - start) / Double(calls))
    }
    print(String(format: "the Swift side of one priced call (CBOR to Request, Item to CBOR): %.0f ns", best))
}
