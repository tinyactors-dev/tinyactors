// What the generator writes a class of, per chart.
package tinyactors

import kotlin.properties.ReadWriteProperty
import kotlin.reflect.KProperty

/** _event: what the session's current event says. */
class Event(
    val name: String = "", val type: String = "", val sendid: String = "", val origin: String = "",
    val origintype: String = "", val invokeid: String = "", val raw: String = "", val data: Value = Value(UNDEFINED),
)

/**
 * A session of a chart: the generated subclass holds its variables as
 * properties and its expressions as the branches of these methods.
 */
abstract class Session {
    var _event = Event(); internal set
    var _sessionid = ""; internal set
    var _name = ""; internal set
    var _ioprocessors: Map<String, String> = emptyMap(); internal set
    internal val given = HashSet<Int>()
    internal var request = 0L

    /** SCXML's In(): whether the state is active. */
    fun In(state: String): Boolean = request != 0L && Native.inState(request, state)

    open fun initData(declaration: Int) {}
    open fun setData(declaration: Int, value: Any?) {}
    open fun condition(expression: Int): Boolean = throw noExpression(expression)
    open fun value(expression: Int): Any? = throw noExpression(expression)
    open fun encode(expression: Int): ByteArray = throw noExpression(expression)
    open fun string(expression: Int): String = throw noExpression(expression)
    open fun assign(location: Int, value: Any?): Unit = throw noExpression(location)
    open fun script(expression: Int): Unit = throw noExpression(expression)
    /** The variables, by name, each as its CBOR. */
    open fun variables(): List<Pair<String, ByteArray>> = emptyList()
    open fun setVariables(variables: Value) {}

    fun noExpression(index: Int) = IllegalStateException("no expression $index of this kind in the generated code")
}

/**
 * A <data> variable: typed by its expression, which runs when the chart
 * binds it (initData), not when the session is made.
 */
class Late<T>(private val initializer: () -> T, private val codec: Codec<T>) : ReadWriteProperty<Any?, T> {
    private var value: Any? = Unset

    fun initialize() { value = initializer() }

    @Suppress("UNCHECKED_CAST")
    override fun getValue(thisRef: Any?, property: KProperty<*>): T =
        if (value === Unset) throw IllegalStateException("${property.name} is not bound yet") else value as T

    override fun setValue(thisRef: Any?, property: KProperty<*>, value: T) { this.value = value }

    @Suppress("UNCHECKED_CAST")
    fun encode(): ByteArray = if (value === Unset) UNDEFINED else codec.encode(value as T)
    fun decode(bytes: ByteArray) { value = codec.decode(bytes) }

    private object Unset
}

inline fun <reified T> late(noinline initializer: () -> T): Late<T> = Late(initializer, codecOf<T>())

/** The generated code of one chart. */
class Chart(val name: String, val fingerprint: Long, val data: List<String>, val make: () -> Session)
