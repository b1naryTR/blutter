import unittest
from unittest import mock

import blutter
from dartvm_fetch_build import DartLibInfo


class SubprocessFailureExitCodeTest(unittest.TestCase):
    def test_preserves_regular_exit_code(self):
        self.assertEqual(blutter.subprocess_failure_exit_code(7), 7)

    def test_converts_unix_signal_to_shell_exit_code(self):
        with mock.patch.object(blutter.os, "name", "posix"):
            self.assertEqual(blutter.subprocess_failure_exit_code(-11), 139)

    def test_rejects_unrepresentable_status(self):
        self.assertEqual(blutter.subprocess_failure_exit_code(256), 1)


class DartLibCacheIdentityTest(unittest.TestCase):
    def test_snapshot_hash_changes_cache_identity(self):
        first = DartLibInfo("3.11.4", "android", "arm64", True, "aaaa")
        second = DartLibInfo("3.11.4", "android", "arm64", True, "bbbb")
        self.assertNotEqual(first.lib_name, second.lib_name)

    def test_pointer_mode_changes_cache_identity(self):
        compressed = DartLibInfo("3.11.4", "android", "arm64", True, "aaaa")
        uncompressed = DartLibInfo("3.11.4", "android", "arm64", False, "aaaa")
        self.assertNotEqual(compressed.lib_name, uncompressed.lib_name)


if __name__ == "__main__":
    unittest.main()
