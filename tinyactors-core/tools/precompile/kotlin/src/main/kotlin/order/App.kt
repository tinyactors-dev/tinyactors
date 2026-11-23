// The application's own types and functions, which the chart's Kotlin
// expressions use as any Kotlin code in the package would.
package order

import kotlinx.serialization.Serializable
import tinyactors.Value

@Serializable
data class Item(val sku: String = "", val quantity: Int = 0, val cents: Int = 0)

@Serializable
data class Address(var country: String = "")

object catalog {
    private val prices = mapOf("tea" to 450, "mug" to 1200)

    /** Whether an event's data names an item the shop sells. */
    fun knows(data: Value): Boolean {
        val item = runCatching { data.decode<Item>() }.getOrNull() ?: return false
        return item.quantity >= 1 && item.sku in prices
    }

    /** The line an event's data asks for, priced. */
    fun item(data: Value): Item = data.decode<Item>().let { it.copy(cents = prices.getValue(it.sku)) }
}
