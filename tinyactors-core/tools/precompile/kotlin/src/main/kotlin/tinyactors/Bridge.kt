// The datamodel's one entry point, in Kotlin: native/bridge.c calls `call`
// for each ta_precompiled_request.
package tinyactors

import java.nio.ByteBuffer

object Native {
    init { System.load(System.getProperty("tinyactors.lib")) }
    @JvmStatic external fun runOrderDemo(chartPath: String): Int
    @JvmStatic external fun inState(request: Long, state: String): Boolean
}

object Bridge {
    private const val ATTACH = 1; private const val CREATE = 2; private const val DESTROY = 3
    private const val INIT_DATA = 4; private const val BIND_EVENT = 5; private const val CONDITION = 6
    private const val VALUE = 7; private const val STRING = 8; private const val ASSIGN = 9
    private const val ASSIGN_VALUE = 10; private const val SCRIPT = 11; private const val SERIALIZE = 12
    private const val DESERIALIZE = 13; private const val VARIABLE = 14

    private var charts: List<Chart> = emptyList()
    private val sessions = ArrayList<Session?>() // a session's handle is its index + 1
    private val free = ArrayDeque<Int>()
    private lateinit var output: ByteBuffer

    @JvmStatic fun room(buffer: ByteBuffer) { output = buffer }

    private fun answer(bytes: ByteArray): Long {
        output.clear()
        output.put(bytes)
        return bytes.size.toLong()
    }

    private fun start(session: Session, chart: Chart, reader: Reader) {
        val (major, count) = reader.head()
        require(major == 4 && count == 4L) { "a malformed start" }
        session._sessionid = Reader(reader.item()).text()
        session._name = Reader(reader.item()).text()
        val processors = Reader(reader.item())
        session._ioprocessors = (0 until processors.head().second).associate { processors.text() to processors.text() }
        val params = Value(reader.item())
        chart.data.forEachIndexed { i, id ->
            if (params[id].isDefined) { session.setData(i, params[id]); session.given.add(i) }
        }
    }

    private fun event(bytes: ByteArray): Event {
        val data = Value(bytes)
        fun field(key: String) = data[key].let { if (it.isDefined) Reader(it.bytes).text() else "" }
        return Event(field("name"), field("type"), field("sendid"), field("origin"), field("origintype"), field("invokeid"),
            field("raw"), data["data"])
    }

    @JvmStatic
    fun call(operation: Int, chart: Int, expression: Int, session: Long, number: Int, input: ByteArray?, request: Long): Long {
        when (operation) {
            ATTACH -> {
                val name = String(input!!, Charsets.UTF_8)
                val index = charts.indexOfFirst { it.name == name }
                check(index >= 0) { "no generated code for the chart \"$name\"" }
                check(charts[index].fingerprint == session) { "the chart \"$name\" changed since its code was generated: build again" }
                return index.toLong()
            }
            CREATE, DESERIALIZE -> {
                val made = charts[chart].make()
                val reader = Reader(input!!)
                start(made, charts[chart], reader)
                if (operation == DESERIALIZE) made.setVariables(Value(reader.item()))
                val slot = free.removeLastOrNull()
                return if (slot != null) { sessions[slot] = made; slot + 1L } else { sessions.add(made); sessions.size.toLong() }
            }
        }
        val s = sessions[(session - 1).toInt()]!!
        s.request = request
        try {
            when (operation) {
                DESTROY -> { sessions[(session - 1).toInt()] = null; free.addLast((session - 1).toInt()) }
                INIT_DATA -> if (number !in s.given) s.initData(number)
                BIND_EVENT -> s._event = event(input!!)
                CONDITION -> return if (s.condition(expression)) 1 else 0
                VALUE -> return answer(s.encode(expression))
                STRING -> return answer(s.string(expression).toByteArray(Charsets.UTF_8))
                ASSIGN -> s.assign(expression, s.value(number))
                ASSIGN_VALUE -> s.assign(expression, Value(input!!))
                SCRIPT -> s.script(expression)
                SERIALIZE -> {
                    val variables = s.variables()
                    return answer(variables.fold(head(5, variables.size.toLong())) { b, (name, v) -> b + textItem(name) + v })
                }
                VARIABLE -> {
                    val name = String(input!!, Charsets.UTF_8)
                    return answer(s.variables().first { it.first == name }.second)
                }
                else -> throw UnsupportedOperationException("operation $operation")
            }
            return 0
        } finally {
            s.request = 0
        }
    }

    /** Runs the scripted order (tools/precompile/demo) with these charts' code. */
    fun runOrderDemo(chartPath: String, registered: List<Chart>): Int {
        charts = registered
        return Native.runOrderDemo(chartPath)
    }
}
