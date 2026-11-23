// The application's own types and functions, which the chart's Swift
// expressions use as any Swift code in the module would.
import Tinyactors

struct Item: Codable {
    var sku = ""
    var quantity = 0
    var cents = 0
    static let none = Item()
}

/// What an "add" event asks for.
struct Request: Codable {
    var sku: String
    var quantity: Int
}

struct Address: Codable {
    var country = ""
}

struct Catalog {
    let prices = ["tea": 450, "mug": 1200]

    /// Whether an event's data names an item the shop sells.
    func knows(_ data: Value) -> Bool {
        guard let request = try? data.decode(Request.self), request.quantity >= 1 else { return false }
        return prices[request.sku] != nil
    }

    /// The line an event's data asks for, priced.
    func item(_ data: Value) throws -> Item {
        let request = try data.decode(Request.self)
        return Item(sku: request.sku, quantity: request.quantity, cents: prices[request.sku] ?? 0)
    }
}

let catalog = Catalog()
