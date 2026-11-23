// The order example in Swift: order.scxml's code is generated at build time
// by the package's plugin (OrderSession).
import Foundation
import Tinyactors

let chart = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("order.scxml").path
exit(runOrderDemo(chartPath: chart, charts: [OrderSession.chart]))
