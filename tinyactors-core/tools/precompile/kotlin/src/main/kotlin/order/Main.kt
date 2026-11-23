// The order example in Kotlin: order.scxml's code is generated at build
// time by the generateCharts task (OrderSession).
package order

import kotlin.system.exitProcess
import tinyactors.Bridge

fun main(arguments: Array<String>) {
    exitProcess(Bridge.runOrderDemo(arguments[0], listOf(OrderSession.chart)))
}
