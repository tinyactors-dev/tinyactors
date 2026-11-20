"""The measurements run, and the core's share is what `mise run layout` in
tinyactors-core gives: a message 106 B (57 B without tracing), a delayed
send 151 B (102 B)."""

import unittest

import main


class Memory(unittest.TestCase):
    def test_each_thing_costs_memory_and_the_core_reserves_its_records(self):
        for datamodel in ["python", "ecmascript"]:
            for tracing in [True, False]:
                with self.subTest(datamodel=datamodel, tracing=tracing):
                    results = main.run(count=5000, datamodel=datamodel, tracing=tracing)
                    self.assertEqual(results["message"]["waiting"]["region"], 106 if tracing else 57)
                    self.assertEqual(results["delayed send"]["pending"]["region"], 151 if tracing else 102)
                    self.assertGreater(results["actor"]["started"]["region"], 81)
                    rows = [results["actor"]["started"], results["actor"]["handled"],
                            results["message"]["waiting"], results["delayed send"]["pending"]]
                    for row in rows:
                        self.assertGreater(row["resident"], 0, row)


if __name__ == "__main__":
    unittest.main()
