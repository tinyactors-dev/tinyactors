"""The README says what the language cannot bound (bindings.md, section 8:
"Say what the language cannot bound"; AGENTS.md, "Rules the suite cannot
see"), and its example runs."""

import contextlib
import io
import pathlib
import re
import unittest

README = pathlib.Path(__file__).resolve().parents[1] / "README.md"


class ReadmeTest(unittest.TestCase):
    def setUp(self):
        self.text = " ".join(README.read_text().split())

    def test_it_says_what_python_cannot_bound(self):
        self.assertIn("Chart code is trusted", self.text)
        self.assertIn("Python values lie outside the host's allocator", self.text)
        self.assertIn("Values nest at most 64 deep", self.text)
        self.assertIn("not a value that shares subtrees", self.text)

    def test_its_example_runs(self):
        example = re.search(r"```python\n(.*?)```", README.read_text(), re.S).group(1)
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            exec(compile(example, "README.md", "exec"), {})
        self.assertEqual(printed.getvalue().splitlines(), ["{'variables': {'count': 2}}", "1 {'total': 2}"])


if __name__ == "__main__":
    unittest.main()
