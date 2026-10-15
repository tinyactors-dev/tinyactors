"""compile_charts, Compiled and the resolver protocol."""

import unittest

import tinyactors
from support import fixture

GOOD = fixture("test_compile/good.python.scxml")
PARENT = fixture("test_compile/parent.python.scxml")


class Resolver:
    def __init__(self, files):
        self.files = files
        self.asked = []

    def resolve(self, base, source):
        self.asked.append((base, source))
        if source not in self.files:
            raise tinyactors.ResolveError("no file %s" % source, line=3, column=7)
        return self.files[source]


class Compile(unittest.TestCase):
    def test_an_error_raises_compile_error_with_every_diagnostic(self):
        bad = fixture("test_compile/bad.python.scxml")
        with self.assertRaises(tinyactors.CompileError) as raised:
            tinyactors.compile_charts([("bad.scxml", bad)])
        diagnostics = raised.exception.diagnostics
        self.assertTrue(diagnostics)
        self.assertEqual(diagnostics[0].severity, "error")
        self.assertEqual(diagnostics[0].file, "bad.scxml")
        self.assertIsInstance(raised.exception, tinyactors.TinyactorsError)

    def test_image_is_stale_after_close_and_close_refuses_a_held_buffer(self):
        compiled = tinyactors.compile_charts([("c.scxml", GOOD)])
        view = memoryview(compiled.image)
        self.assertTrue(bytes(view))
        with self.assertRaises(BufferError):
            compiled.close()
        view.release()
        compiled.close()
        with self.assertRaises(tinyactors.TinyactorsError) as raised:
            compiled.image
        self.assertEqual(raised.exception.status, "TA_ERROR_STALE")

    def test_a_resolver_reads_src_and_refuses_at_its_line(self):
        child = fixture("test_compile/child.python.scxml")
        resolver = Resolver({"child.scxml": child})
        with tinyactors.compile_charts([("parent.scxml", PARENT)], resolver) as compiled:
            self.assertTrue(len(compiled.image))
        self.assertEqual(resolver.asked, [("parent.scxml", "child.scxml")])
        with self.assertRaises(tinyactors.CompileError) as raised:
            tinyactors.compile_charts([("parent.scxml", PARENT)], Resolver({}))
        diagnostic = [d for d in raised.exception.diagnostics if d.severity == "error"][0]
        self.assertEqual((diagnostic.line, diagnostic.column), (3, 7))
        self.assertIn("no file child.scxml", diagnostic.message)

    def test_a_py_file_is_checked_as_it_is_read(self):
        document = fixture("test_compile/script-src.python.scxml")
        with self.assertRaises(tinyactors.CompileError) as raised:
            tinyactors.compile_charts([("c.scxml", document)], Resolver({"reader.py": fixture("test_compile/reader-refused.py")}))
        diagnostic = [d for d in raised.exception.diagnostics if d.severity == "error"][0]
        self.assertEqual(diagnostic.line, 3)
        self.assertIn("import", diagnostic.message)
        with tinyactors.compile_charts([("c.scxml", document)], Resolver({"reader.py": fixture("test_compile/reader.py")})):
            pass

    def test_another_exception_of_a_resolver_is_raised(self):
        class Broken:
            def resolve(self, base, source):
                raise OSError("disk")

        with self.assertRaises(OSError):
            tinyactors.compile_charts([("parent.scxml", PARENT)], Broken())

    def test_without_a_resolver_src_is_refused(self):
        with self.assertRaises(tinyactors.CompileError):
            tinyactors.compile_charts([("parent.scxml", PARENT)])


if __name__ == "__main__":
    unittest.main()
