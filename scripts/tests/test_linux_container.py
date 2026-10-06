"""Regression tests for source fidelity, failure propagation and cleanup."""

import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "test-linux-container.py"
SPEC = importlib.util.spec_from_file_location("linux_container", SCRIPT)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


class ContainerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pci tests with spaces ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def git(self, repo, *arguments):
        return subprocess.check_output(["git", "-c", "user.name=Test", "-c",
                                        "user.email=test@example.invalid", *arguments],
                                       cwd=repo, stderr=subprocess.STDOUT)

    def repo(self, name):
        path = self.root / name
        path.mkdir()
        self.git(path, "init")
        (path / "tracked.txt").write_text("committed")
        (path / ".gitignore").write_text("build/\n")
        self.git(path, "add", ".")
        self.git(path, "commit", "-m", "initial")
        return path

    def test_snapshot_captures_edits_untracked_and_submodules_without_builds(self):
        repo = self.repo("source tree")
        dependency = self.repo("dependency")
        self.git(repo, "-c", "protocol.file.allow=always", "submodule", "add",
                 str(dependency), "third party")
        self.git(repo, "commit", "-am", "add dependency")
        (repo / "tracked.txt").write_text("working tree")
        (repo / "new file.txt").write_text("new")
        (repo / "third party" / "tracked.txt").write_text("modified submodule")
        (repo / "link").symlink_to("tracked.txt")
        (repo / "build").mkdir()
        (repo / "build" / "stale.o").write_text("excluded")
        (repo / "third party" / "build").mkdir()
        (repo / "third party" / "build" / "stale.o").write_text("excluded")
        destination = self.root / "source.tar"
        result = runner.snapshot(repo, destination)
        self.assertEqual(len(result["snapshot_sha256"]), 64)
        with tarfile.open(destination) as archive:
            self.assertEqual(archive.extractfile("tracked.txt").read(), b"working tree")
            self.assertEqual(archive.extractfile("third party/tracked.txt").read(), b"modified submodule")
            self.assertEqual(archive.extractfile("new file.txt").read(), b"new")
            self.assertTrue(archive.getmember("link").issym())
            self.assertFalse(any(".git" in Path(name).parts or "build" in Path(name).parts
                                 for name in archive.getnames()))

    def test_uninitialized_submodule_is_rejected(self):
        repo = self.repo("source")
        dependency = self.repo("dependency")
        self.git(repo, "-c", "protocol.file.allow=always", "submodule", "add", str(dependency), "dep")
        self.git(repo, "submodule", "deinit", "-f", "dep")
        with self.assertRaisesRegex(RuntimeError, "submodules"):
            runner.snapshot(repo, self.root / "source.tar")

    def test_zero_exit_without_complete_test_results_is_not_a_pass(self):
        (self.root / "result.json").write_text('{"stage":"complete","exit_code":0}')
        self.assertEqual(runner.lane_result("ci", 0, self.root, 1)["status"], "failed")
        (self.root / "ctest.xml").write_text('<testsuite><testcase name="skipped"><skipped/></testcase></testsuite>')
        self.assertEqual(runner.lane_result("ci", 0, self.root, 1)["status"], "failed")
        (self.root / "ctest.xml").write_text('<testsuite><testcase name="ok"/></testsuite>')
        self.assertEqual(runner.lane_result("ci", 0, self.root, 1)["status"], "passed")
        self.assertEqual(runner.lane_result("ci", 1, self.root, 1)["status"], "failed")

    def test_junit_reports_failed_names_and_skips(self):
        report = self.root / "ctest.xml"
        report.write_text('<testsuites><testsuite><testcase name="ok"/>'
                          '<testcase name="bad"><failure/></testcase>'
                          '<testcase name="skip"><skipped/></testcase></testsuite></testsuites>')
        self.assertEqual(runner.test_counts(report), {
            "total": 3, "passed": 1, "failed": 1, "skipped": 1, "failures": ["bad"]})

    def test_runs_later_configurations_after_failure_and_cleans_up(self):
        args = argparse.Namespace(arch="amd64", cpus=4, memory="16G", dns="1.1.1.1", long_stress=False)
        summary = {"configurations": [{"preset": p, "status": "not-run"} for p in runner.PRESETS]}
        with patch.object(runner, "run_logged", return_value=1) as run, \
                patch.object(runner, "remove_container") as cleanup:
            runner.execute_lanes(args, "test-image", self.root, self.root, summary)
        self.assertEqual(run.call_count, 4)
        self.assertEqual(cleanup.call_count, 4)
        self.assertTrue(all(item["status"] == "failed" for item in summary["configurations"]))
        tsan = run.call_args_list[-1].args[0]
        self.assertIn("sysctl.vm.mmap_rnd_bits=28", tsan)
        self.assertIn(f"source={self.root},target=/input,readonly", tsan)
        self.assertNotIn("--kernel-arg", run.call_args_list[0].args[0])

    def test_interrupt_cleans_up_and_records_remaining_not_run(self):
        args = argparse.Namespace(arch="amd64", cpus=4, memory="16G", dns="1.1.1.1", long_stress=False)
        summary = {"configurations": [{"preset": p, "status": "not-run"} for p in runner.PRESETS]}
        with patch.object(runner, "run_logged", side_effect=KeyboardInterrupt), \
                patch.object(runner, "remove_container") as cleanup:
            with self.assertRaises(KeyboardInterrupt):
                runner.execute_lanes(args, "image", self.root, self.root, summary)
        cleanup.assert_called_once()
        saved = json.loads((self.root / "summary.json").read_text())
        self.assertEqual(saved["configurations"][0]["status"], "interrupted")
        self.assertEqual(saved["configurations"][1]["status"], "not-run")

    def test_native_arm_omits_rosetta_and_preserves_tsan_settings(self):
        for arch in ("arm64", "amd64"):
            with self.subTest(arch=arch):
                args = argparse.Namespace(arch=arch, cpus=4, memory="16G", dns="1.1.1.1", long_stress=False)
                command = runner.container_args(args, "image", "name", self.root, self.root, "tsan")
                self.assertEqual(command[command.index("--platform") + 1], f"linux/{arch}")
                self.assertEqual("--rosetta" in command, arch == "amd64")
                self.assertIn(f"PCI_CONTAINER_ARCH={arch}", command)
                self.assertIn("sysctl.vm.mmap_rnd_bits=28", command)

    def test_arm_context_changes_only_compiler_name_and_separates_image_cache(self):
        repo = SCRIPT.parent.parent
        amd_context = self.root / "amd64"
        arm_context = self.root / "arm64"
        amd_image = runner.prepare_image_context(repo, amd_context, "amd64")
        arm_image = runner.prepare_image_context(repo, arm_context, "arm64")
        self.assertNotEqual(amd_image, arm_image)
        self.assertEqual(arm_image, runner.prepare_image_context(repo, self.root / "arm64-repeat", "arm64"))
        for name, relative in runner.IMAGE_FILES.items():
            original = (repo / relative).read_bytes()
            self.assertEqual((amd_context / name).read_bytes(), original)
            arm_data = (arm_context / name).read_bytes()
            if name == "validation-environment-linux.yml":
                self.assertIn(b"clangxx_linux-aarch64=22.*", arm_data)
                self.assertEqual(arm_data.replace(b"clangxx_linux-aarch64", b"clangxx_linux-64"), original)
            else:
                self.assertEqual(arm_data, original)

    def test_arm_context_rejects_unexpected_compiler_manifest(self):
        repo = self.root / "repo"
        for relative in runner.IMAGE_FILES.values():
            path = repo / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("unexpected manifest\n")
        with self.assertRaisesRegex(RuntimeError, "exactly one Linux Clang"):
            runner.prepare_image_context(repo, self.root / "context", "arm64")

    def test_shared_runner_stops_at_failed_stage_and_preserves_exit_code(self):
        scripts = self.root / "scripts"
        scripts.mkdir()
        shutil.copy(SCRIPT.parent / "run-linux-ci.sh", scripts)
        commands = self.root / "bin"
        commands.mkdir()
        cmake = commands / "cmake"
        cmake.write_text('#!/bin/sh\nif [ "$1" = "--build" ]; then echo "build failure"; exit 7; fi\n')
        cmake.chmod(0o755)
        import os
        result = subprocess.run(["bash", str(scripts / "run-linux-ci.sh"), "ci"],
                                env={**os.environ, "PATH": str(commands) + ":" + os.environ["PATH"]},
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(result.returncode, 7)
        reports = self.root / "build" / "ci" / "Testing"
        self.assertEqual(json.loads((reports / "result.json").read_text()), {"stage": "build", "exit_code": 7})
        self.assertIn("build failure", (reports / "build.log").read_text())
        self.assertFalse((reports / "test.log").exists())


if __name__ == "__main__":
    unittest.main()
