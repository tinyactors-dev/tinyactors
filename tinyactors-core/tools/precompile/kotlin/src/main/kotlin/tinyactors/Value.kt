// What crosses from another actor or the application: one CBOR item, read
// by member (`_event.data["country"]`) and decoded where the chart assigns
// it to a Kotlin type.
package tinyactors

import kotlinx.serialization.ExperimentalSerializationApi
import kotlinx.serialization.KSerializer
import kotlinx.serialization.cbor.Cbor
import kotlinx.serialization.serializer

@OptIn(ExperimentalSerializationApi::class)
val cborFormat = Cbor {
    useDefiniteLengthEncoding = true // core's reader refuses indefinite lengths
    ignoreUnknownKeys = true
    encodeDefaults = true
}

val UNDEFINED = byteArrayOf(0xF7.toByte())

class Value(val bytes: ByteArray) {
    val isDefined: Boolean get() = !(bytes.size == 1 && bytes[0] == UNDEFINED[0])

    /** A map's member; undefined when this is not a map or lacks it. */
    operator fun get(key: String): Value {
        val r = Reader(bytes)
        val (major, count) = r.head()
        if (major != 5) return Value(UNDEFINED)
        repeat(count.toInt()) {
            val k = r.item()
            val v = r.item()
            if (Reader(k).text() == key) return Value(v)
        }
        return Value(UNDEFINED)
    }

    /** An array's item; undefined past its end. */
    operator fun get(index: Int): Value {
        val r = Reader(bytes)
        val (major, count) = r.head()
        if (major != 4 || index < 0 || index >= count) return Value(UNDEFINED)
        repeat(index) { r.item() }
        return Value(r.item())
    }

    inline fun <reified T> decode(): T = if (T::class == Value::class) this as T else cborFormat.decodeFromByteArray(serializer<T>(), bytes)

    override fun toString(): String = Reader(bytes).text(true)
}

/** Reads CBOR heads and whole items (what Value and the bridge need). */
class Reader(private val bytes: ByteArray, var at: Int = 0) {
    private fun byte(): Int = bytes[at++].toInt() and 0xFF

    fun head(): Pair<Int, Long> {
        val first = byte()
        val info = first and 0x1F
        val value = when {
            info < 24 -> info.toLong()
            info <= 27 -> (0 until (1 shl (info - 24))).fold(0L) { v, _ -> (v shl 8) or byte().toLong() }
            else -> throw IllegalArgumentException("an indefinite length")
        }
        return Pair(first ushr 5, value)
    }

    /** The next whole item's bytes. */
    fun item(): ByteArray {
        val start = at
        skip()
        return bytes.copyOfRange(start, at)
    }

    private fun skip() {
        val (major, value) = head()
        when (major) {
            2, 3 -> at += value.toInt()
            4 -> repeat(value.toInt()) { skip() }
            5 -> repeat(value.toInt() * 2) { skip() }
            6 -> skip()
        }
    }

    /** A text item as a string; with `any`, any item as JSON-like text. */
    fun text(any: Boolean = false): String {
        val start = at
        val (major, value) = head()
        return when (major) {
            0 -> value.toString()
            1 -> (-1 - value).toString()
            3 -> String(bytes, at, value.toInt(), Charsets.UTF_8).also { at += value.toInt() }.let { if (any) "\"$it\"" else it }
            4 -> (0 until value).joinToString(", ", "[", "]") { text(true) }
            5 -> (0 until value).joinToString(", ", "{", "}") { text(true) + ": " + text(true) }
            7 -> when (bytes[start].toInt() and 0xFF) { 0xF4 -> "false"; 0xF5 -> "true"; 0xF6 -> "null"; 0xF7 -> "undefined"
                else -> { at = start; skip(); "a number" } }
            else -> { at = start; skip(); "an item" }
        }
    }
}

/** Writes the heads of a map of variables. */
fun head(major: Int, value: Long): ByteArray = when {
    value < 24 -> byteArrayOf(((major shl 5) or value.toInt()).toByte())
    value <= 0xFF -> byteArrayOf(((major shl 5) or 24).toByte(), value.toByte())
    value <= 0xFFFF -> byteArrayOf(((major shl 5) or 25).toByte(), (value shr 8).toByte(), value.toByte())
    else -> byteArrayOf(((major shl 5) or 26).toByte(), (value shr 24).toByte(), (value shr 16).toByte(), (value shr 8).toByte(), value.toByte())
}

fun textItem(s: String): ByteArray = s.toByteArray(Charsets.UTF_8).let { head(3, it.size.toLong()) + it }

/** A value's CBOR, by its static type's serializer. */
inline fun <reified T> cbor(value: T): ByteArray = codecOf<T>().encode(value)

/** What an expression gave, as the type of the location it is assigned to. */
inline fun <reified T> cast(value: Any?): T = when {
    value is T -> value
    value is Value -> codecOf<T>().decode(value.bytes)
    else -> throw IllegalArgumentException("a ${value?.let { it::class.simpleName }} is not a ${T::class.simpleName}")
}

/** What a <log> or a *expr attribute shows of a value. */
fun text(value: Any?): String = value as? String ?: value.toString()

class Codec<T>(val encode: (T) -> ByteArray, val decode: (ByteArray) -> T)

@Suppress("UNCHECKED_CAST")
inline fun <reified T> codecOf(): Codec<T> =
    if (T::class == Value::class) Codec({ (it as Value).bytes }, { Value(it) as T })
    else serializer<T>().let { s: KSerializer<T> -> Codec({ cborFormat.encodeToByteArray(s, it) }, { cborFormat.decodeFromByteArray(s, it) }) }
