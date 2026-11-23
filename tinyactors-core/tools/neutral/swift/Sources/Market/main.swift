// The market tracker, on a simulated clock: the app's side (the price feed,
// the device's network, the UI) and the charts' side (tools/neutral/charts),
// with json or staticjs:
//
//   Market json|staticjs
import Foundation
import Tinyactors

let datamodel = Datamodel(rawValue: CommandLine.arguments.dropFirst().first ?? "json") ?? .json
let charts = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("../../../charts/\(datamodel.rawValue)").standardized
let host = try Host(datamodel: datamodel, chart: charts.appendingPathComponent("tracker.scxml").path,
                    functions: marketFunctions(), ioProcessor: "urn:market:app")

func say(_ text: String) {
    let t = String(format: "%6.2fs", Double(host.now) / 1000)
    print("\(t)  \(text)")
}

/// What the app's side does at a time; a sequence number keeps their order.
var schedule: [(at: UInt64, order: Int, action: () throws -> Void)] = []
var order = 0
func at(_ time: UInt64, _ action: @escaping () throws -> Void) {
    order += 1
    schedule.append((time, order, action))
}

// ---- the price feed, which a real app runs over a WebSocket ----
var deviceOnline = true, serverUp = true, connected = false
var subscribed: [String] = []
let base = ["AAPL": 189.0, "MSFT": 410.0, "TSLA": 250.0]
func quote(_ symbol: String) -> Quote {
    Quote(symbol: symbol, price: base[symbol]! + 0.5 * Double(host.now / 1000), at: Int(host.now))
}
var connects = 0

let tracker = try host.spawn("tracker")
host.onLog = { _, label, text in say("log  \(label): \(text)") }
host.onSend = { send in
    switch send.event {
    case "connect":
        connects += 1
        say("app  connect, attempt \(connects)")
        at(host.now + 200) {
            if deviceOnline && serverUp {
                connected = true
                try host.send(tracker, "feed.connected")
            } else {
                try host.send(tracker, "feed.failed")
            }
        }
    case "subscribe":
        subscribed = (try? send.data.decode(Subscribe.self).symbols) ?? []
        say("app  subscribe \(subscribed.joined(separator: ", ")): a snapshot follows")
        at(host.now + 50) { try host.send(tracker, "feed.snapshot", Snapshot(quotes: subscribed.map(quote))) }
    case "disconnect":
        connected = false
        say("app  disconnect")
    case "notify":
        let fired = try! send.data.decode(Fired.self)
        say("UI   alert: \(fired.symbol) at \(String(format: "%.2f", fired.price)) crossed \(String(format: "%.2f", fired.above))"
            + (fired.late ? ", while you were offline" : ""))
    default:
        say("app  \(send.event) \(send.data)")
    }
}

// the feed's ticks, once a second while connected
for t in stride(from: UInt64(1000), through: 18000, by: 1000) {
    at(t) {
        guard connected && deviceOnline && serverUp else { return }
        for symbol in subscribed { try host.send(tracker, "feed.tick", quote(symbol)) }
    }
}
// the user, the device and the server
at(1500) {
    say("UI   alert when AAPL above 193, MSFT above 412")
    try host.send(tracker, "alert.set", RuleRequest(symbol: "AAPL", above: 193))
    try host.send(tracker, "alert.set", RuleRequest(symbol: "MSFT", above: 412))
}
at(4500) {
    say("OS   airplane mode on")
    deviceOnline = false
    connected = false
    try host.send(tracker, "network.offline")
}
at(4600) {
    say("app  the socket reports its error (it closed with the network)")
    try host.send(tracker, "feed.lost")
}
at(6000) {
    say("UI   watch TSLA")
    try host.send(tracker, "watch.add", Watch(symbol: "TSLA"))
}
at(7000) { say("UI   report"); try host.send(tracker, "report") }
at(10000) {
    say("OS   airplane mode off")
    deviceOnline = true
    try host.send(tracker, "network.online")
}
at(13500) {
    say("app  the server drops the connection (the device is online)")
    serverUp = false
    connected = false
    try host.send(tracker, "feed.lost")
}
at(15000) { serverUp = true }
at(18000) { say("UI   report"); try host.send(tracker, "report") }

print("market tracker, \(datamodel.rawValue) charts")
var deadline = host.run(at: 0)
while true {
    let action = schedule.min { ($0.at, $0.order) < ($1.at, $1.order) }
    let next = min(action?.at ?? .max, deadline ?? .max)
    if next > 18500 { break }
    deadline = host.run(at: next) // timers due now first
    while let due = schedule.filter({ $0.at <= next }).min(by: { ($0.at, $0.order) < ($1.at, $1.order) }) {
        schedule.removeAll { $0.order == due.order }
        try due.action()
    }
    deadline = host.run(at: next)
}
print("connect attempts: \(connects); datamodel bytes now: \(host.datamodelBytes)")
