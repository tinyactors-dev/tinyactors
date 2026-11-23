// The app's types and the functions its charts call, the same for both
// datamodels: the json charts call more of them, since they have no
// operators.
import Foundation
import Tinyactors

struct Quote: Codable { var symbol: String; var price: Double; var at: Int }
struct Snapshot: Codable { var quotes: [Quote] }
struct Rule: Codable { var symbol: String; var above: Double; var fired: Bool }
struct RuleRequest: Codable { var symbol: String; var above: Double }
struct Watch: Codable { var symbol: String }
struct Subscribe: Codable { var symbols: [String] }
struct Fired: Codable { var symbol: String; var above: Double; var price: Double; var late: Bool }

func seconds(_ ms: Int) -> String { String(format: "%.1fs", Double(ms) / 1000) }

func marketFunctions() -> Functions {
    let f = Functions()
    // both datamodels
    f.register("ruleFrom") { (request: RuleRequest) -> Rule in Rule(symbol: request.symbol, above: request.above, fired: false) }
    f.register("crossed") { (rule: Rule, quote: Quote) -> Bool in quote.symbol == rule.symbol && quote.price >= rule.above }
    f.register("summary") { (quotes: [String: Quote], live: Bool) -> String in
        quotes.keys.sorted().map { symbol in
            let q = quotes[symbol]!
            return "\(symbol) \(String(format: "%.2f", q.price)) at \(seconds(q.at))" + (live ? "" : " (stale)")
        }.joined(separator: ", ")
    }
    // what the json charts need instead of an operator
    f.register("due") { (rule: Rule, quote: Quote) -> Bool in !rule.fired && quote.symbol == rule.symbol && quote.price >= rule.above }
    f.register("ms") { (n: Int) -> String in "\(n)ms" }
    f.register("nextBackoff") { (n: Int) -> Int in min(n * 2, 30_000) }
    return f
}
