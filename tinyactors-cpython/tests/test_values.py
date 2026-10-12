"""The value table's own tests (docs/values.md).

The suite's vectors go through this binding's codec both ways, so a value
mapped wrong the same way in encode and decode passes them (ROADMAP.md,
M1, "Gates"). These tests hold each direction to the vectors' bytes and
to the Python values of values.txt's `python` lines, and each row of
docs/values.md to a value of its own.
"""

import enum
import pathlib
import unittest
from collections import OrderedDict

import tinyactors
from tinyactors import UNDEFINED, Address, TinyactorsError, decode, encode

VALUES = pathlib.Path(__file__).resolve().parents[2] / "tinyactors-test/suites/protocol/values.txt"


def cases():
    """The cases of values.txt: (kind, name, {key: value}), in order."""
    found, case = [], None
    for line in VALUES.read_text().splitlines():
        if line.startswith("#"):
            continue
        if not line.strip():
            case = None
            continue
        key, _, value = line.partition(" ")
        if case is None:
            case = (key, value, {})
            found.append(case)
        else:
            case[2].setdefault(key, value)
    return found


def hexed(text):
    return bytes.fromhex(text.replace(" ", ""))


def same(a, b):
    """Equal, and NaN equal to NaN, and -0.0 not 0.0: as CBOR writes them."""
    return encode(a) == encode(b)


class VectorsTest(unittest.TestCase):
    """values.txt, through tinyactors.encode and tinyactors.decode alone."""

    def test_roundtrip_cases(self):
        for kind, name, case in cases():
            if kind != "roundtrip" or name.startswith("ECMAScript:"):
                continue
            with self.subTest(name):
                value = decode(hexed(case["cbor"]))
                self.assertEqual(encode(value).hex(), hexed(case.get("back", case["cbor"])).hex())

    def test_malformed_cases_are_refused(self):
        for kind, name, case in cases():
            if kind != "malformed":
                continue
            with self.subTest(name):
                with self.assertRaises(TinyactorsError) as raised:
                    decode(hexed(case["cbor"]))
                self.assertIn(raised.exception.status, (tinyactors.ERROR_INVALID, tinyactors.ERROR_UNSUPPORTED))

    def test_python_lines_of_encode_cases(self):
        seen = 0
        for kind, name, case in cases():
            if kind != "encode" or "python" not in case:
                continue
            seen += 1
            with self.subTest(name):
                value = eval(case["python"])
                self.assertEqual(encode(value).hex(), hexed(case["cbor"]).hex())
                # and back: decode gives the same value (a float stays a float)
                self.assertTrue(same(decode(encode(value)), value))
        self.assertGreaterEqual(seen, 6)

    def test_python_lines_of_invalid_cases_are_refused(self):
        seen = 0
        for kind, name, case in cases():
            if kind != "invalid" or "python" not in case:
                continue
            seen += 1
            with self.subTest(name):
                with self.assertRaises(TinyactorsError) as raised:
                    encode(eval(case["python"]))
                self.assertEqual(raised.exception.status, tinyactors.ERROR_INVALID)
        self.assertGreaterEqual(seen, 5)


class RowsTest(unittest.TestCase):
    """docs/values.md, row by row, in both directions."""

    def test_integers_are_exact_to_64_bits(self):
        for value, cbor in [(0, "00"), (23, "17"), (24, "1818"), (-1, "20"), (2**64 - 1, "1bffffffffffffffff"),
                            (-(2**64), "3bffffffffffffffff"), (2**63, "1b8000000000000000"),
                            (-(2**63) - 1, "3b8000000000000000")]:
            with self.subTest(value):
                self.assertEqual(encode(value).hex(), cbor)
                self.assertEqual(decode(hexed(cbor)), value)
                self.assertIs(type(decode(hexed(cbor))), int)
        for value in (2**64, -(2**64) - 1):
            with self.subTest(value), self.assertRaises(TinyactorsError):
                encode(value)

    def test_bool_before_int(self):
        self.assertEqual(encode(True), b"\xf5")
        self.assertEqual(encode(False), b"\xf4")
        self.assertIs(decode(b"\xf5"), True)
        self.assertIs(decode(b"\x01"), 1)

    def test_int_and_str_subclasses_are_their_base(self):
        class Color(enum.IntEnum):
            RED = 3

        self.assertEqual(encode(Color.RED), encode(3))
        self.assertEqual(encode(OrderedDict(a=1)), encode({"a": 1}))

    def test_floats(self):
        self.assertEqual(encode(1.5).hex(), "f93e00")
        self.assertEqual(encode(float("nan")).hex(), "f97e00")
        self.assertIs(type(decode(hexed("f93c00"))), float)

    def test_text_is_utf8_and_a_lone_surrogate_is_refused(self):
        self.assertEqual(decode(encode("tü✓")), "tü✓")
        for value in ("a\udc00b", {"\ud800": 1}, ["\udfff"]):
            with self.subTest(repr(value)), self.assertRaises(TinyactorsError) as raised:
                encode(value)
            self.assertEqual(raised.exception.status, tinyactors.ERROR_INVALID)

    def test_bytes_and_bytearray_are_bytes(self):
        self.assertEqual(encode(b"\x00\xff").hex(), "4200ff")
        self.assertEqual(encode(bytearray(b"\x00\xff")).hex(), "4200ff")
        self.assertIs(type(decode(hexed("4200ff"))), bytes)
        with self.assertRaises(TinyactorsError):
            encode(memoryview(b"ab"))

    def test_tuples_are_arrays_and_come_back_as_lists(self):
        self.assertEqual(encode((1, 2)), encode([1, 2]))
        self.assertEqual(decode(encode((1, (2,)))), [1, [2]])

    def test_maps_keep_their_order_and_need_text_keys(self):
        self.assertEqual(list(decode(encode({"b": 1, "a": 2}))), ["b", "a"])
        self.assertEqual(encode({"$x": 1}).hex(), "a162247801")
        for value in ({1: 2}, {(1,): 2}, {None: 1}):
            with self.subTest(repr(value)), self.assertRaises(TinyactorsError):
                encode(value)

    def test_null_and_undefined_stay_distinct(self):
        self.assertEqual(encode(None), b"\xf6")
        self.assertEqual(encode(UNDEFINED), b"\xf7")
        self.assertIs(decode(b"\xf6"), None)
        self.assertIs(decode(b"\xf7"), UNDEFINED)
        self.assertEqual(decode(encode([UNDEFINED, {"k": UNDEFINED}])), [UNDEFINED, {"k": UNDEFINED}])

    def test_undefined_is_one_falsy_object_that_cannot_be_made_again(self):
        self.assertFalse(UNDEFINED)
        self.assertEqual(repr(UNDEFINED), "tinyactors.UNDEFINED")
        with self.assertRaises(TypeError):
            type(UNDEFINED)()

    def test_address_is_tag_32_on_its_text(self):
        address = Address("tinyactors://h/#_scxml_1")
        self.assertEqual(encode(address).hex(), "d820" + encode("tinyactors://h/#_scxml_1").hex())
        back = decode(encode(address))
        self.assertIsInstance(back, Address)
        self.assertEqual(back, address)
        self.assertEqual(str(back), "tinyactors://h/#_scxml_1")
        self.assertEqual(repr(back), "Address('tinyactors://h/#_scxml_1')")
        self.assertEqual(hash(back), hash(address))
        self.assertNotEqual(address, "tinyactors://h/#_scxml_1")
        self.assertEqual(len({address, back}), 1)
        self.assertEqual(Address("anything"), Address(text="anything"))  # unchecked: core decides
        with self.assertRaises(TypeError):
            Address(1)

    def test_shared_values_are_copied_and_cycles_refused(self):
        shared = {"k": 1}
        self.assertEqual(decode(encode([shared, shared])), [{"k": 1}, {"k": 1}])
        cycle = []
        cycle.append(cycle)
        with self.assertRaises(TinyactorsError):
            encode(cycle)

    def test_nesting_up_to_64(self):
        def nested(n):
            value = 0
            for _ in range(n):
                value = [value]
            return value

        self.assertEqual(decode(encode(nested(64))), nested(64))
        with self.assertRaises(TinyactorsError):
            encode(nested(65))

    def test_other_objects_are_refused(self):
        import datetime
        import decimal

        for value in ({1}, frozenset(), datetime.date(2026, 1, 1), decimal.Decimal(1), object(), print):
            with self.subTest(repr(value)), self.assertRaises(TinyactorsError) as raised:
                encode(value)
            self.assertEqual(raised.exception.status, tinyactors.ERROR_INVALID)
            self.assertIsNone(raised.exception.code)

    def test_decode_refuses_what_is_not_one_item_of_the_table(self):
        for data in (b"", b"\x01\x02", b"\xff", b"\xa1\x01\x02", b"\xc1\x01", b"\xf0"):
            with self.subTest(data.hex()), self.assertRaises(TinyactorsError):
                decode(data)

    def test_decode_takes_any_bytes_like_object(self):
        self.assertEqual(decode(bytearray(b"\x01")), 1)
        self.assertEqual(decode(memoryview(b"\x01")), 1)


class ErrorTest(unittest.TestCase):
    def test_a_bare_status(self):
        with self.assertRaises(TinyactorsError) as raised:
            decode(b"\xff")
        error = raised.exception
        self.assertEqual(error.status, tinyactors.ERROR_INVALID)
        self.assertEqual((error.code, error.reason, error.path), (None, None, None))
        self.assertEqual(str(error), "ta_cbor_decode: TA_ERROR_INVALID")

    def test_status_constants_are_cores(self):
        self.assertEqual((tinyactors.OK, tinyactors.ERROR_EXECUTION, tinyactors.ERROR_INVALID,
                          tinyactors.ERROR_ABORTED), (0, -1, -3, -9))


if __name__ == "__main__":
    unittest.main()
