"""The module imports, once per interpreter, with no GIL of its own."""

import sys
import sysconfig
import unittest

import tinyactors


class ModuleTest(unittest.TestCase):
    def test_the_error_type(self):
        self.assertTrue(issubclass(tinyactors.TinyactorsError, Exception))

    def test_free_threaded_keeps_the_gil_off(self):
        # Py_mod_gil is Py_MOD_GIL_NOT_USED: importing the module does not
        # turn the GIL on in the free-threaded build.
        if not sysconfig.get_config_var("Py_GIL_DISABLED"):
            self.skipTest("an interpreter with the GIL")
        self.assertFalse(sys._is_gil_enabled())


if __name__ == "__main__":
    unittest.main()
