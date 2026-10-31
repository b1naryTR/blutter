import os
import tempfile
import unittest
from unittest import mock

import blutter
import dartvm_fetch_build
from dartvm_fetch_build import DartLibInfo, ninja_command, auto_build_jobs, cmake_low_memory_args
from extract_dart_info import has_shorebird_markers
from scripts.dartvm_patch_shorebird import patch_app_snapshot


class SubprocessFailureExitCodeTest(unittest.TestCase):
    def test_preserves_regular_exit_code(self):
        self.assertEqual(blutter.subprocess_failure_exit_code(7), 7)

    def test_converts_unix_signal_to_shell_exit_code(self):
        with mock.patch.object(blutter.os, "name", "posix"):
            self.assertEqual(blutter.subprocess_failure_exit_code(-11), 139)

    def test_rejects_unrepresentable_status(self):
        self.assertEqual(blutter.subprocess_failure_exit_code(256), 1)



class NinjaCommandTest(unittest.TestCase):
    def test_unset_keeps_ninja_default_parallelism(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(ninja_command(), ["ninja"])

    def test_blank_keeps_ninja_default_parallelism(self):
        with mock.patch.dict(os.environ, {"BLUTTER_BUILD_JOBS": " "}):
            self.assertEqual(ninja_command(), ["ninja"])

    def test_caps_parallel_jobs(self):
        with mock.patch.dict(os.environ, {"BLUTTER_BUILD_JOBS": "2"}):
            self.assertEqual(ninja_command(), ["ninja", "-j", "2"])

    def test_rejects_invalid_job_count(self):
        for value in ("0", "-1", "two"):
            with self.subTest(value=value), mock.patch.dict(os.environ, {"BLUTTER_BUILD_JOBS": value}):
                with self.assertRaises(ValueError):
                    ninja_command()


    def test_auto_sizes_jobs_from_memory(self):
        with mock.patch.dict(os.environ, {"BLUTTER_BUILD_JOBS": "auto"}, clear=True), \
                mock.patch.object(dartvm_fetch_build, "available_memory_mb", return_value=1024 + 3 * 600), \
                mock.patch.object(dartvm_fetch_build, "cpu_count", return_value=8):
            self.assertEqual(ninja_command(), ["ninja", "-j", "3"])


class AutoBuildJobsTest(unittest.TestCase):
    def run_auto(self, env, available, cpus=8):
        with mock.patch.dict(os.environ, env, clear=True), \
                mock.patch.object(dartvm_fetch_build, "available_memory_mb", return_value=available), \
                mock.patch.object(dartvm_fetch_build, "cpu_count", return_value=cpus):
            return auto_build_jobs()

    def test_never_exceeds_cpu_count(self):
        self.assertEqual(self.run_auto({}, 100_000, cpus=8), 8)

    def test_keeps_at_least_one_job_when_memory_is_short(self):
        self.assertEqual(self.run_auto({}, 200), 1)

    def test_low_memory_mode_fits_more_jobs(self):
        available = 1024 + 2520
        self.assertEqual(self.run_auto({}, available), 4)
        self.assertEqual(self.run_auto({"BLUTTER_BUILD_LOW_MEMORY": "1"}, available), 6)

    def test_honours_reserve_and_job_size_overrides(self):
        env = {"BLUTTER_BUILD_RESERVE_MB": "500", "BLUTTER_BUILD_JOB_MB": "250"}
        self.assertEqual(self.run_auto(env, 1500), 4)

    def test_unknown_memory_uses_all_cpus(self):
        self.assertEqual(self.run_auto({}, None, cpus=6), 6)


class LowMemoryCmakeArgsTest(unittest.TestCase):
    def test_disabled_by_default(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertEqual(cmake_low_memory_args(), [])

    def test_passes_gc_params_to_gcc(self):
        with mock.patch.dict(os.environ, {"BLUTTER_BUILD_LOW_MEMORY": "1"}, clear=True), \
                mock.patch.object(dartvm_fetch_build, "_compiler_is_gcc", return_value=True):
            self.assertEqual(cmake_low_memory_args(),
                             ["-DCMAKE_CXX_FLAGS=" + dartvm_fetch_build.GCC_LOW_MEMORY_FLAGS])

    def test_skips_non_gcc_compilers(self):
        with mock.patch.dict(os.environ, {"BLUTTER_BUILD_LOW_MEMORY": "1"}, clear=True), \
                mock.patch.object(dartvm_fetch_build, "_compiler_is_gcc", return_value=False):
            self.assertEqual(cmake_low_memory_args(), [])

    def test_uses_compiler_from_explicit_env(self):
        with mock.patch.dict(os.environ, {"BLUTTER_BUILD_LOW_MEMORY": "1"}, clear=True), \
                mock.patch.object(dartvm_fetch_build, "_compiler_is_gcc", return_value=False) as is_gcc:
            cmake_low_memory_args({"CXX": "/opt/llvm/bin/clang++"})
            is_gcc.assert_called_once_with("/opt/llvm/bin/clang++")


class DartLibCacheIdentityTest(unittest.TestCase):
    def test_snapshot_hash_changes_cache_identity(self):
        first = DartLibInfo("3.11.4", "android", "arm64", True, "aaaa")
        second = DartLibInfo("3.11.4", "android", "arm64", True, "bbbb")
        self.assertNotEqual(first.lib_name, second.lib_name)

    def test_pointer_mode_changes_cache_identity(self):
        compressed = DartLibInfo("3.11.4", "android", "arm64", True, "aaaa")
        uncompressed = DartLibInfo("3.11.4", "android", "arm64", False, "aaaa")
        self.assertNotEqual(compressed.lib_name, uncompressed.lib_name)

    def test_engine_mode_changes_cache_identity(self):
        flutter = DartLibInfo("3.11.4", "android", "arm64", True, "aaaa")
        shorebird = DartLibInfo("3.11.4", "android", "arm64", True, "aaaa", True)
        self.assertNotEqual(flutter.lib_name, shorebird.lib_name)


class ShorebirdDetectionTest(unittest.TestCase):
    def test_requires_both_updater_exports(self):
        self.assertTrue(has_shorebird_markers(b'shorebird_init\0shorebird_update\0'))
        self.assertFalse(has_shorebird_markers(b'shorebird_init\0'))
        self.assertFalse(has_shorebird_markers(b'ordinary flutter engine'))


class ShorebirdSourcePatchTest(unittest.TestCase):
    def test_patches_new_and_old_class_cluster_signatures_once(self):
        for suffix in (' override', ''):
            with self.subTest(suffix=suffix):
                source = f'''class ClassDeserializationCluster {{
  void ReadAlloc(Deserializer* d){suffix} {{
    predefined_start_index_ = d->next_index();
    intptr_t count = d->ReadUnsigned();
    ClassTable* table = d->isolate_group()->class_table();
    for (intptr_t i = 0; i < count; i++) {{
    }}
  }}
}};
'''
                with tempfile.TemporaryDirectory() as directory:
                    path = os.path.join(directory, 'app_snapshot.cc')
                    with open(path, 'w', encoding='utf-8') as f:
                        f.write(source)

                    patch_app_snapshot(path)
                    patch_app_snapshot(path)
                    with open(path, 'r', encoding='utf-8') as f:
                        patched = f.read()

                self.assertEqual(patched.count('const intptr_t last_cid'), 1)
                self.assertEqual(patched.count('table->AllocateIndex(last_cid)'), 1)
                self.assertEqual(patched.count('#if defined(DART_SHOREBIRD)'), 2)


if __name__ == "__main__":
    unittest.main()
