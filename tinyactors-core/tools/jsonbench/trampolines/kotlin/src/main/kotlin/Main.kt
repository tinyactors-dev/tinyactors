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

@CborArray @Serializable data class NewItemArguments(val add: Add)

/* ta_cbor_read refuses indefinite lengths, kotlinx's default for arrays and
 * maps, so the results must be written with definite ones. */
val cbor = Cbor { useDefiniteLengthEncoding = true }

/* A function behind ta_json_function: the arguments array in, one CBOR
 * item out. `arguments` is valid during the call only (the native side
 * reuses it), as the C ABI's are. */
fun interface JSONFunction {
    fun call(arguments: ByteArray): ByteArray
}

private val TRUE = byteArrayOf(0xF5.toByte())

private fun price(sku: String): Long = when (sku) {
    "tea" -> 450
    "mug" -> 1200
    else -> -1
}

val functions: Array<Pair<String, JSONFunction>> = arrayOf(
    "empty" to JSONFunction { TRUE },
    "hasItems" to JSONFunction { a ->
        val arguments = cbor.decodeFromByteArray<HasItemsArguments>(a)
        cbor.encodeToByteArray(arguments.items.isNotEmpty())
    },
    "addItem" to JSONFunction { a ->
        val arguments = cbor.decodeFromByteArray<AddItemArguments>(a)
        val add = arguments.add
        cbor.encodeToByteArray(arguments.items + Item(add.sku, add.qty, price(add.sku)))
    },
    "newItem" to JSONFunction { a ->
        val add = cbor.decodeFromByteArray<NewItemArguments>(a).add
        cbor.encodeToByteArray(Item(add.sku, add.qty, price(add.sku)))
    },
)

/* The direct-ByteBuffer variant: native copies the arguments into one
 * direct buffer over its own memory and passes the length; kotlinx decodes
 * from a ByteArray only, so the bytes are copied into a new one here. */
object Bridge {
    @JvmStatic lateinit var staging: ByteBuffer

    @JvmStatic
    fun callBuffer(function: JSONFunction, length: Int): ByteArray {
        val arguments = ByteArray(length)
        staging.get(0, arguments, 0, length)
        return function.call(arguments)
    }

    /* The room the driver lends a "bytes-direct" call, as a direct
     * ByteBuffer native makes over it. kotlinx-serialization-cbor encodes to
     * a ByteArray only (Cbor has encodeToByteArray and decodeFromByteArray,
     * no stream, sink or ByteBuffer), so the result is still made as a
     * ByteArray and put into the room here: the copy moves from JNI's
     * GetByteArrayRegion to ByteBuffer.put, and no array crosses back. */
    @JvmStatic lateinit var room: ByteBuffer

    /* the result's length, or minus the length it needs */
    @JvmStatic
    fun callInto(function: JSONFunction, arguments: ByteArray): Int {
        val result = function.call(arguments)
        if (result.size > room.capacity()) return -result.size
        room.put(0, result)
        return result.size
    }
}

object Native {
    /* the functions, as global references: a function's context is its index */
    external fun register(functions: Array<JSONFunction>)
    /* run the trampoline n times for function `index` in a mode of MODES
     * (its index); the elapsed nanoseconds */
    external fun bench(index: Int, n: Int, mode: Int): Long
    /* one call's result, in CBOR diagnostic notation */
    external fun sample(index: Int, mode: Int): String
    /* one call's result bytes */
    external fun result(index: Int, mode: Int): ByteArray
}

/* the native side's modes, by their index there:
 * array:        a reused byte[] in, the result's items re-emitted into a ta_cbor_writer
 * buffer:       a direct ByteBuffer in, the result re-emitted
 * bytes:        a reused byte[] in, the result's bytes copied into the driver's ta_buffer
 * bytes-direct: a reused byte[] in, Kotlin puts the result into a direct ByteBuffer over it
 * bytes+check:  "bytes", then a check pass over the result in C */
val MODES = listOf("array", "buffer", "bytes", "bytes-direct", "bytes+check")

fun main(args: Array<String>) {
    val n = args.getOrNull(0)?.toInt() ?: 1_000_000
    val warmup = args.getOrNull(1)?.toInt() ?: 200_000
    val runs = args.getOrNull(2)?.toInt() ?: 5
    val modes = (args.getOrNull(3) ?: MODES.joinToString(",")).split(",")
    for (mode in modes) require(mode in MODES) { "unknown mode $mode: one of $MODES" }
    System.load(System.getProperty("jsonbench.lib"))
    Native.register(functions.map { it.second }.toTypedArray())
    val threads = ManagementFactory.getThreadMXBean() as com.sun.management.ThreadMXBean
    val thread = Thread.currentThread().threadId()
    val verbose = System.getenv("JSONBENCH_VERBOSE") != null
    /* every mode gives the same bytes per function, before any timing */
    for ((index, entry) in functions.withIndex()) {
        val expected = Native.result(index, 0)
        for ((m, mode) in MODES.withIndex()) {
            val got = Native.result(index, m)
            check(got.contentEquals(expected)) {
                "${entry.first}: mode $mode gave ${got.toHex()}, array gave ${expected.toHex()}"
            }
        }
    }
    for (mode in modes) {
        val m = MODES.indexOf(mode)
        val label = "kotlin"
        for ((index, entry) in functions.withIndex()) {
            if (verbose) System.err.println("$label ${entry.first} $mode: ${Native.sample(index, m)}")
            Native.bench(index, warmup, m)
            var best = Long.MAX_VALUE
            var allocated = 0L
            repeat(runs) {
                val before = threads.getThreadAllocatedBytes(thread)
                best = minOf(best, Native.bench(index, n, m))
                allocated = threads.getThreadAllocatedBytes(thread) - before
            }
            val ns = best.toDouble() / n
            println("%s\t%s\t%s\t%.1f ns/call".format(label, entry.first, mode, ns) +
                if (verbose) "\t(%.0f B allocated/call)".format(allocated.toDouble() / n) else "")
        }
    }
}

private fun ByteArray.toHex(): String = joinToString("") { "%02x".format(it) }
