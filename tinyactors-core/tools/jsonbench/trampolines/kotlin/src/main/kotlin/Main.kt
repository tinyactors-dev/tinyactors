/* The cost per call of a Kotlin function behind the json datamodel's
 * function ABI (ta_json_function), through JNI: Kotlin calls native
 * bench(), whose loop calls a C trampoline with ta_json_function's
 * signature, which calls back into Kotlin. A spike for
 * docs/proposals/json-datamodel.md; nothing here is a binding's API. */
@file:OptIn(ExperimentalSerializationApi::class)

package jsonbench

import java.lang.management.ManagementFactory
import java.nio.ByteBuffer
import kotlinx.serialization.ExperimentalSerializationApi
import kotlinx.serialization.Serializable
import kotlinx.serialization.cbor.Cbor
import kotlinx.serialization.cbor.CborArray
import kotlinx.serialization.decodeFromByteArray
import kotlinx.serialization.encodeToByteArray

@Serializable data class Item(val sku: String, val qty: Int, val cents: Long)

@Serializable data class Add(val sku: String, val qty: Int)

/* The arguments array, decoded positionally: @CborArray makes a class a
 * CBOR array of its properties in order, so a heterogeneous argument list
 * is a typed class with no custom serializer. */
@CborArray @Serializable data class HasItemsArguments(val items: List<Item>)

@CborArray @Serializable data class AddItemArguments(val items: List<Item>, val add: Add)

/* ta_cbor_read refuses indefinite lengths, kotlinx's default for arrays and
 * maps, so the results must be written with definite ones. */
val cbor = Cbor { useDefiniteLengthEncoding = true }

/* A function behind ta_json_function: the arguments array in, one CBOR
 * item out. `arguments` is valid during the call only (the native side
 * reuses it), as the C ABI's are. */
fun interface JsonFunction {
    fun call(arguments: ByteArray): ByteArray
}

private val TRUE = byteArrayOf(0xF5.toByte())

private fun price(sku: String): Long = when (sku) {
    "tea" -> 450
    "mug" -> 1200
    else -> -1
}

val functions: Array<Pair<String, JsonFunction>> = arrayOf(
    "empty" to JsonFunction { TRUE },
    "hasItems" to JsonFunction { a ->
        val arguments = cbor.decodeFromByteArray<HasItemsArguments>(a)
        cbor.encodeToByteArray(arguments.items.isNotEmpty())
    },
    "addItem" to JsonFunction { a ->
        val arguments = cbor.decodeFromByteArray<AddItemArguments>(a)
        val add = arguments.add
        cbor.encodeToByteArray(arguments.items + Item(add.sku, add.qty, price(add.sku)))
    },
)

/* The direct-ByteBuffer variant: native copies the arguments into one
 * direct buffer over its own memory and passes the length; kotlinx decodes
 * from a ByteArray only, so the bytes are copied into a new one here. */
object Bridge {
    @JvmStatic lateinit var staging: ByteBuffer

    @JvmStatic
    fun callBuffer(function: JsonFunction, length: Int): ByteArray {
        val arguments = ByteArray(length)
        staging.get(0, arguments, 0, length)
        return function.call(arguments)
    }
}

object Native {
    /* the functions, as global references: a function's context is its index */
    external fun register(functions: Array<JsonFunction>)
    /* run the trampoline n times for function `index`; mode 0: a reused
     * byte[], mode 1: a direct ByteBuffer; the elapsed nanoseconds */
    external fun bench(index: Int, n: Int, mode: Int): Long
    /* one call's result, in CBOR diagnostic notation */
    external fun sample(index: Int, mode: Int): String
}

fun main(args: Array<String>) {
    val n = args.getOrNull(0)?.toInt() ?: 1_000_000
    val warmup = args.getOrNull(1)?.toInt() ?: 200_000
    val runs = args.getOrNull(2)?.toInt() ?: 5
    val modes = (args.getOrNull(3) ?: "array").split(",")
    System.load(System.getProperty("jsonbench.lib"))
    Native.register(functions.map { it.second }.toTypedArray())
    val threads = ManagementFactory.getThreadMXBean() as com.sun.management.ThreadMXBean
    val thread = Thread.currentThread().threadId()
    val verbose = System.getenv("JSONBENCH_VERBOSE") != null
    for (mode in modes) {
        val m = if (mode == "array") 0 else 1
        val label = if (m == 0) "kotlin" else "kotlin-directbuffer"
        for ((index, entry) in functions.withIndex()) {
            if (verbose) System.err.println("$label ${entry.first}: ${Native.sample(index, m)}")
            Native.bench(index, warmup, m)
            var best = Long.MAX_VALUE
            var allocated = 0L
            repeat(runs) {
                val before = threads.getThreadAllocatedBytes(thread)
                best = minOf(best, Native.bench(index, n, m))
                allocated = threads.getThreadAllocatedBytes(thread) - before
            }
            val ns = best.toDouble() / n
            println("%s\t%s\t%.1f ns/call".format(label, entry.first, ns) +
                if (verbose) "\t(%.0f B allocated/call)".format(allocated.toDouble() / n) else "")
        }
    }
}
