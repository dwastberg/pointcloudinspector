"""Regression tests for source fidelity, failure propagation and cleanup."""

import argparse
import importlib.util
import json
import os
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
                patch.object(runner, "prepare_native_image", return_value={"image": "native-image"}), \
                patch.object(runner, "prepare_flatpak_image", return_value={"image": "sdk-image", "cache_volume": "cache"}), \
                patch.object(runner, "acquire_flatpak_cache"), \
                patch.object(runner, "remove_container") as cleanup:
            runner.execute_lanes(args, self.root, self.root, summary)
        self.assertIn("ci-release", runner.PRESETS)
        self.assertEqual(run.call_count, len(runner.PRESETS))
        self.assertEqual(cleanup.call_count, len(runner.PRESETS))
        self.assertTrue(all(item["status"] == "failed" for item in summary["configurations"]))
        for preset, call in zip(runner.PRESETS, run.call_args_list):
            self.assertEqual(call.args[1], self.root / preset / "container.log")
            self.assertEqual(call.args[2], 7200 if preset == "flatpak" else 3600 if preset in ("ci", "ci-release") else 5400)
        tsan = run.call_args_list[runner.PRESETS.index("tsan")].args[0]
        self.assertIn("sysctl.vm.mmap_rnd_bits=28", tsan)
        self.assertIn(f"source={self.root},target=/input,readonly", tsan)
        self.assertNotIn("--kernel-arg", run.call_args_list[0].args[0])

    def test_interrupt_cleans_up_and_records_remaining_not_run(self):
        args = argparse.Namespace(arch="amd64", cpus=4, memory="16G", dns="1.1.1.1", long_stress=False)
        summary = {"configurations": [{"preset": p, "status": "not-run"} for p in runner.PRESETS]}
        with patch.object(runner, "run_logged", side_effect=KeyboardInterrupt), \
                patch.object(runner, "prepare_native_image", return_value={"image": "image"}), \
                patch.object(runner, "remove_container") as cleanup:
            with self.assertRaises(KeyboardInterrupt):
                runner.execute_lanes(args, self.root, self.root, summary)
        cleanup.assert_called_once()
        saved = json.loads((self.root / "summary.json").read_text())
        self.assertEqual(saved["configurations"][0]["status"], "interrupted")
        self.assertEqual(saved["configurations"][1]["status"], "not-run")

    def test_flatpak_only_does_not_prepare_native_image(self):
        args = argparse.Namespace(arch="arm64", cpus=4, memory="16G", dns="1.1.1.1", long_stress=True)
        summary = {"configurations": [{"preset": "flatpak", "status": "not-run"}]}
        with patch.object(runner, "prepare_native_image") as native, \
                patch.object(runner, "prepare_flatpak_image", return_value={"image": "sdk", "cache_volume": "cache"}), \
                patch.object(runner, "acquire_flatpak_cache"), \
                patch.object(runner, "run_logged", return_value=1) as run, \
                patch.object(runner, "remove_container"):
            runner.execute_lanes(args, self.root, self.root, summary)
        native.assert_not_called()
        self.assertEqual(run.call_args.args[2], 7200)
        self.assertEqual(summary["configurations"][0]["status"], "failed")

    def test_native_setup_failure_does_not_prevent_flatpak(self):
        args = argparse.Namespace(arch="arm64", cpus=4, memory="16G", dns="1.1.1.1", long_stress=False)
        summary = {"configurations": [{"preset": p, "status": "not-run"} for p in ("ci", "ci-release", "flatpak")]}
        with patch.object(runner, "prepare_native_image", side_effect=RuntimeError("native setup failed")) as native, \
                patch.object(runner, "prepare_flatpak_image", return_value={"image": "sdk", "cache_volume": "cache"}) as sdk, \
                patch.object(runner, "acquire_flatpak_cache"), \
                patch.object(runner, "run_logged", return_value=1) as run, \
                patch.object(runner, "remove_container") as cleanup:
            runner.execute_lanes(args, self.root, self.root, summary)
        native.assert_called_once()
        sdk.assert_called_once()
        run.assert_called_once()
        cleanup.assert_called_once()
        self.assertIn("native setup failed", summary["configurations"][0]["error"])
        self.assertEqual(summary["configurations"][2]["status"], "failed")

    def test_flatpak_host_lock_prevents_duplicate_volume_attachment_and_releases(self):
        with patch.object(runner, "ROOT", self.root), \
                patch.object(runner.subprocess, "run", return_value=argparse.Namespace(returncode=0)) as command:
            first = runner.acquire_flatpak_cache("test-cache")
            try:
                with self.assertRaisesRegex(RuntimeError, "already in use"):
                    runner.acquire_flatpak_cache("test-cache")
                command.assert_called_once()
            finally:
                first.close()
            second = runner.acquire_flatpak_cache("test-cache")
            second.close()
            self.assertEqual(command.call_count, 2)

    def test_flatpak_requires_nonempty_bundle_and_matching_checksum(self):
        (self.root / "result.json").write_text('{"stage":"complete","exit_code":0}')
        (self.root / "ctest.xml").write_text('<testsuite><testcase name="ok"/></testsuite>')
        bundle = self.root / "test.flatpak"
        checksum = self.root / "test.flatpak.sha256"
        for content in (None, b"", b"bundle"):
            if content is not None:
                bundle.write_bytes(content)
            self.assertEqual(runner.lane_result("flatpak", 0, self.root, 1)["status"], "failed")
        checksum.write_text("wrong  test.flatpak\n")
        self.assertEqual(runner.lane_result("flatpak", 0, self.root, 1)["status"], "failed")
        checksum.write_text(f"{runner.file_sha256(bundle)}  test.flatpak\n")
        self.assertEqual(runner.lane_result("flatpak", 0, self.root, 1)["status"], "passed")

    def test_flatpak_architecture_and_sandbox_flags_do_not_affect_native_jobs(self):
        for arch, flatpak_arch in runner.FLATPAK_ARCHES.items():
            with self.subTest(arch=arch):
                args = argparse.Namespace(arch=arch, cpus=4, memory="16G", dns="1.1.1.1",
                                          long_stress=False, flatpak_volume="test-cache")
                command = runner.container_args(args, "sdk", "test", self.root, self.root, "flatpak")
                self.assertIn(f"PCI_FLATPAK_ARCH={flatpak_arch}", command)
                self.assertIn("type=volume,source=test-cache,target=/cache", command)
                self.assertIn("--cap-add", command)
                self.assertIn("--masked-path", command)
                self.assertEqual("--rosetta" in command, arch == "amd64")
                native = runner.container_args(args, "native", "test", self.root, self.root, "ci")
                self.assertNotIn("--cap-add", native)
                self.assertNotIn("--masked-path", native)

    def test_flatpak_cache_is_separated_by_architecture_image_and_checkout(self):
        metadata = [{"variants": [{"platform": {"architecture": arch}, "digest": f"sha256:{arch}"}
                                  for arch in ("arm64", "amd64")]}]
        volumes = []
        with patch.object(runner, "capture", return_value=json.dumps(metadata)), \
                patch.object(runner.subprocess, "run", return_value=argparse.Namespace(returncode=0)):
            for arch in ("arm64", "arm64", "amd64"):
                args = argparse.Namespace(arch=arch, rebuild_image=False)
                detail = runner.prepare_flatpak_image(args, self.root, {})
                volumes.append(detail["cache_volume"])
                self.assertTrue(detail["image"].endswith(f"@sha256:{arch}"))
            metadata[0]["variants"][0]["digest"] = "sha256:updated"
            with patch.object(runner, "capture", return_value=json.dumps(metadata)):
                volumes.append(runner.prepare_flatpak_image(argparse.Namespace(arch="arm64", rebuild_image=False), self.root, {})["cache_volume"])
            with patch.object(runner, "ROOT", self.root), \
                    patch.object(runner, "flatpak_image", return_value="sdk:test"):
                volumes.append(runner.prepare_flatpak_image(argparse.Namespace(arch="arm64", rebuild_image=False), self.root, {})["cache_volume"])
        self.assertEqual(volumes[0], volumes[1])
        self.assertEqual(len(set(volumes)), 4)

    def test_flatpak_manifest_preparation_preserves_packaging_and_test_options(self):
        spec = importlib.util.spec_from_file_location("flatpak_manifest", SCRIPT.parent / "container/prepare-flatpak-manifest.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        manifest = {"runtime-version": "6.10", "build-options": {"env": {"KEEP": "yes"}},
                    "modules": [{"name": "dependency"}, {"name": "app", "test-commands": ["ctest"],
                                 "config-opts": ["-DTEST=ON"], "build-options": {"env": {"CTEST_PARALLEL_LEVEL": "3"}}}]}
        result = module.prepare(manifest, {"DISPLAY": ":99", "DBUS_SESSION_BUS_ADDRESS": "unix:test"})
        self.assertEqual(result["modules"][0], {"name": "dependency"})
        self.assertEqual(result["modules"][-1]["test-commands"], ["ctest"])
        self.assertEqual(result["modules"][-1]["config-opts"], ["-DTEST=ON"])
        self.assertTrue(result["modules"][-1]["run-tests"])
        self.assertEqual(result["build-options"]["env"]["KEEP"], "yes")
        self.assertNotIn("DISPLAY", result["build-options"]["env"])
        options = result["modules"][-1]["build-options"]
        self.assertEqual(options["env"]["DISPLAY"], ":99")
        self.assertEqual(options["env"]["CTEST_PARALLEL_LEVEL"], "3")

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

    def shared_runner(self, preset, failed_stage="", long_stress="OFF"):
        scripts = self.root / "scripts"
        scripts.mkdir(exist_ok=True)
        shutil.copy(SCRIPT.parent / "run-linux-ci.sh", scripts)
        commands = self.root / "bin"
        commands.mkdir(exist_ok=True)
        tools = {
            "cmake": '''case "$1" in
  --version) echo 'cmake version test'; exit 0 ;;
  --preset)
    step=configure
    mkdir -p "build/$2/CMakeFiles/test"
    echo 'CMAKE_BUILD_TYPE:STRING=Release' > "build/$2/CMakeCache.txt"
    echo '[]' > "build/$2/compile_commands.json"
    echo 'set(CMAKE_CXX_COMPILER_ID "GNU")' > "build/$2/CMakeFiles/test/CMakeCXXCompiler.cmake"
    echo 'configure details' > "build/$2/CMakeFiles/CMakeConfigureLog.yaml" ;;
  -DBUILD_DIR=*) step=compiler ;;
  --build)
    step=build
    case "$*" in *format-check*) step=format ;; esac ;;
  *) exit 99 ;;
esac
echo "$*"
if [ "$step" = "$FAIL_STAGE" ]; then echo "$step failure"; exit 7; fi
''',
            "ctest": '''echo "$*"
while [ "$#" -gt 0 ]; do
  if [ "$1" = --output-junit ]; then
    shift
    echo '<testsuite><testcase name="ok"/></testsuite>' > "$1"
  fi
  shift
done
if [ "$FAIL_STAGE" = test ]; then echo 'test failure'; exit 8; fi
''',
            "c++": "echo 'GCC test'\n",
            "micromamba": "echo 'https://example.invalid/exact-package-build.tar.bz2'\n",
        }
        for name, body in tools.items():
            command = commands / name
            command.write_text("#!/bin/sh\nset -eu\n" + body)
            command.chmod(0o755)
        return subprocess.run(["bash", str(scripts / "run-linux-ci.sh"), preset],
                              env={**os.environ, "PATH": str(commands) + ":" + os.environ["PATH"],
                                   "CXX": "c++", "FAIL_STAGE": failed_stage,
                                   "PCINSPECTOR_LONG_STRESS": long_stress},
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def test_shared_runner_preserves_failures_and_diagnostics_for_both_gcc_presets(self):
        for preset in ("ci", "ci-release"):
            for stage, code in (("configure", 7), ("compiler", 7), ("build", 7), ("test", 8)):
                with self.subTest(preset=preset, stage=stage):
                    shutil.rmtree(self.root / "build", ignore_errors=True)
                    result = self.shared_runner(preset, stage)
                    self.assertEqual(result.returncode, code, result.stdout.decode())
                    reports = self.root / "build" / preset / "Testing"
                    self.assertEqual(json.loads((reports / "result.json").read_text()),
                                     {"stage": stage, "exit_code": code})
                    self.assertIn(f"{stage} failure", (reports / f"{stage}.log").read_text())
                    self.assertTrue((reports / "configuration" / "CMakeCache.txt").is_file())
                    self.assertTrue((reports / "configuration" / "compile_commands.json").is_file())
                    self.assertTrue((reports / "configuration" / "CMakeFiles" / "test" /
                                     "CMakeCXXCompiler.cmake").is_file())
                    self.assertIn("exact-package-build", (reports / "packages.txt").read_text())
                    if stage != "test":
                        self.assertFalse((reports / "test.log").exists())

    def test_shared_runner_separates_reports_and_formats_only_debug(self):
        for preset in ("ci", "ci-release"):
            result = self.shared_runner(preset, long_stress="ON")
            self.assertEqual(result.returncode, 0, result.stdout.decode())
            reports = self.root / "build" / preset / "Testing"
            self.assertEqual(json.loads((reports / "result.json").read_text()),
                             {"stage": "complete", "exit_code": 0})
            self.assertEqual((reports / "format.log").exists(), preset == "ci")
            self.assertIn("-DPCINSPECTOR_ENABLE_LONG_STRESS_TESTS=ON",
                          (reports / "configure.log").read_text())
            self.assertIn(f"--preset {preset}", (reports / "test.log").read_text())
            self.assertEqual(runner.lane_result(preset, 0, reports, 1)["status"], "passed")
        self.assertTrue((self.root / "build" / "ci" / "Testing" / "ctest.xml").is_file())
        self.assertTrue((self.root / "build" / "ci-release" / "Testing" / "ctest.xml").is_file())


if __name__ == "__main__":
    unittest.main()
