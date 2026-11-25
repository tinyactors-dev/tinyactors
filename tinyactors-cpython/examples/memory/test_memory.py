"""The measurements run, and the core's share is what `mise run layout` in
tinyactors-core gives: a message 106 B (57 B without tracing), a delayed
send 151 B (102 B)."""

import ctypes
import os
import unittest

import main

MADV_PAGEOUT = 21  # Linux's <sys/mman.h>, since 5.4


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
    def test_each_thing_costs_memory_and_the_core_reserves_its_records(self):
        for datamodel in ["python", "ecmascript"]:
            for tracing in [True, False]:
                with self.subTest(datamodel=datamodel, tracing=tracing):
                    results = main.run(count=5000, datamodel=datamodel, tracing=tracing)
                    self.assertEqual(results["message"]["waiting"]["region"], 106 if tracing else 57)
                    self.assertEqual(results["delayed send"]["pending"]["region"], 151 if tracing else 102)
                    self.assertGreater(results["actor"]["started"]["region"], 81)
                    # a hibernated actor keeps its records only: 81 B, up to 89 B as the indexes round up
                    hibernated = results["hibernated"]["hibernated"]
                    self.assertTrue(81 <= hibernated["region"] <= 89, hibernated)
                    self.assertGreater(hibernated["blob"], 0, hibernated)
                    rows = [results["actor"]["started"], results["actor"]["handled"], hibernated,
                            results["message"]["waiting"], results["delayed send"]["pending"]]
                    for row in rows:
                        self.assertGreater(row["resident"], 0, row)

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
