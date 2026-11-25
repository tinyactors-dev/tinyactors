"""The measurements run, and each core column is what the core's own definitions give: the
reservation is ta-layout's region for the same image and capacities (tinyactors-core's
tools/layout/layout.c, which `mise run build` builds beside the extension as build/ta-layout),
the record sizes are ta-layout's own check, and the use is those records times the pools'
counts. The process column is the machine's and the interpreter's, never asserted."""

import ctypes
import os
import pathlib
import subprocess
import tempfile
import unittest

import main

COUNT = 5000
LAYOUT = pathlib.Path(__file__).resolve().parents[2] / "build" / "ta-layout"
MADV_PAGEOUT = 21  # Linux's <sys/mman.h>, since 5.4


def layout(*arguments):
    """What ta-layout prints for `arguments`, as a dict of name -> bytes."""
    result = subprocess.run([str(LAYOUT), *map(str, arguments)], capture_output=True, text=True)
    if result.returncode != 0:
        raise AssertionError(f"ta-layout {' '.join(map(str, arguments))}: {result.stderr}")
    return {name: int(value) for name, value in (line.split() for line in result.stdout.strip().splitlines())}


def reservation(image, what, tracing):
    """ta-layout's reservation per thing of a row: its region at COUNT and at none, the same
    capacities the example's hosts are given."""
    def region(n):
        system = main.sizes(what, COUNT, n, tracing)
        values = {name: str(value).lower() if isinstance(value, bool) else value for name, value in system.items()}
        return layout(image, *(f"{name}={value}" for name, value in values.items()))["region"]
    return round((region(COUNT) - region(0)) / COUNT)


def page_out_mapped_files():
    """Ask the kernel to reclaim this process's pages of mapped files now, as it does when the
    machine runs short of memory; the bytes it took, 0 where it cannot."""
    libc = ctypes.CDLL(None, use_errno=True)
    libc.madvise.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
    paged = 0
    with open("/proc/self/maps") as maps:
        for line in maps:
            fields = line.split()
            if len(fields) < 6 or not fields[5].startswith("/"):
                continue
            start, end = (int(address, 16) for address in fields[0].split("-"))
            if libc.madvise(start, end - start, MADV_PAGEOUT) == 0:
                paged += end - start
    return paged


class Memory(unittest.TestCase):
    def test_each_core_column_is_what_the_cores_definitions_give(self):
        records = layout()
        for datamodel in ["python", "ecmascript"]:
            for tracing in [True, False]:
                with self.subTest(datamodel=datamodel, tracing=tracing), tempfile.TemporaryDirectory() as directory:
                    image = pathlib.Path(directory) / "counter.tai"
                    image.write_bytes(main.compiled(main.CHART.format(datamodel=datamodel))[0])
                    self.check(str(image), records, datamodel, tracing)

    def check(self, image, records, datamodel, tracing):
        results = main.run(count=COUNT, datamodel=datamodel, tracing=tracing)
        # the records: what core checks against its types' debug information
        units = results["units"]
        self.assertEqual(units, {"actor": records["actor"], "event": records["event" if tracing else "untraced-event"],
                                 "timer": records["timer"]})
        rows = {"started": (results["actor"]["started"], "actor"), "handled": (results["actor"]["handled"], "actor"),
                "hibernated": (results["hibernated"]["hibernated"], "hibernated"),
                "waiting": (results["message"]["waiting"], "message"),
                "pending": (results["delayed send"]["pending"], "delayed send")}
        for key, (row, what) in rows.items():
            self.assertNotIn("failed", row, key)
            # the reservation: ta-layout's region for the same image and capacities
            self.assertEqual(row["reserved"], reservation(image, what, tracing), key)
            # every pool the row fills was sized for it: none grew
            self.assertEqual(row["grown"], [], key)
            if datamodel == "python":
                self.assertIsNone(row["quickjs"], key)
            else:
                self.assertGreater(row["quickjs"], 0, key)
            self.assertIsInstance(row["process"], int, key)  # the machine's: never asserted
        started, handled, hibernated, waiting, pending = (rows[key][0] for key in rows)
        # core used: the pools' records in use, and the slab
        self.assertEqual(waiting["pools"], {"actors": 0, "events": COUNT, "timers": 0})
        self.assertEqual(waiting["used"], units["event"])
        self.assertEqual(waiting["reserved"], units["event"])
        # quickjs keeps the event an actor handled last, as its _event, in the core's pool until
        # the next one (the "python" datamodel detaches it): one more event on quickjs for the
        # delayed sends' actor, one per actor after an event
        kept = 1 if datamodel == "ecmascript" else 0
        self.assertEqual(pending["pools"], {"actors": 0, "events": COUNT + kept, "timers": COUNT})
        self.assertEqual(pending["used"], units["timer"] + units["event"])
        self.assertEqual(pending["reserved"], units["timer"] + units["event"])
        self.assertEqual(started["pools"]["actors"], COUNT)
        self.assertEqual(handled["pools"], {"actors": COUNT, "events": kept * COUNT, "timers": 0})
        self.assertGreater(started["used"], units["actor"])
        self.assertEqual(hibernated["pools"], {"actors": COUNT, "events": 0, "timers": 0})
        self.assertEqual(hibernated["used"], units["actor"])  # its instance went back to the slab
        self.assertGreater(hibernated["blob"], 0)
        # a host sized up front allocates nothing per thing beside its region
        for row in (started, waiting, pending):
            self.assertEqual(row["host_other"], 0, row)

    @unittest.skipUnless(os.path.exists("/proc/self/maps"), "Linux only: memory is the peak elsewhere")
    def test_memory_leaves_out_pages_of_mapped_files_the_kernel_reclaims(self):
        # Under memory pressure the kernel drops the code of the interpreter and the extension
        # between two samples; counted, that made a row negative.
        before = main.resident()
        if page_out_mapped_files() == 0:
            self.skipTest("the kernel refuses MADV_PAGEOUT")
        self.assertGreaterEqual(main.resident(), before)


if __name__ == "__main__":
    unittest.main()
