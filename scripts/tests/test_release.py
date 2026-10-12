"""Release regression tests; GitHub is simulated and no release is published."""

import hashlib
import importlib.util
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "release.py"
SPEC = importlib.util.spec_from_file_location("pci_release", SCRIPT)
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)

VERSION = "0.1.0"
TAG = "v0.1.0"
COMMIT = "a" * 40


class FakeGitHub(release.GitHub):
    """Simulate CLI side effects and serve uploaded files back as downloads."""

    def __init__(self):
        super().__init__("dwastberg/pointcloudinspector")
        self.draft = None
        self.files = {}
        self.commands = []
        self.body = ""
        self.commit = COMMIT
        self.annotated = True
        self.fail_upload = False
        self.corrupt_download = False

    def api(self, endpoint, fields=None):
        if endpoint.startswith("git/ref/tags/"):
            return {"object": {"type": "tag" if self.annotated else "commit",
                               "sha": "b" * 40 if self.annotated else self.commit}}
        if endpoint.startswith("git/tags/"):
            return {"object": {"type": "commit", "sha": self.commit}}
        if endpoint.startswith("releases/tags/"):
            # Simulate GitHub omitting drafts from the tag lookup.
            if self.draft is not False:
                raise subprocess.CalledProcessError(1, ["gh", "api"],
                                                    stderr="gh: Not Found (HTTP 404)")
            return self.release_data()
        if endpoint.startswith("releases?per_page="):
            return [] if self.draft is None else [self.release_data()]
        if endpoint == "releases/generate-notes":
            return {"body": "## What's Changed\n\nGenerated release notes."}
        raise AssertionError(f"Unexpected endpoint: {endpoint}")

    def release_data(self):
        return {"tag_name": TAG, "draft": self.draft, "assets": [
            {"name": name, "state": "uploaded", "size": len(content)}
            for name, content in self.files.items()
        ]}

    def run(self, *arguments):
        self.commands.append(arguments)
        action = arguments[1]
        if action == "create":
            if "--draft" not in arguments or "--verify-tag" not in arguments:
                raise AssertionError("New release must be a draft for an existing tag")
            self.draft = True
        if action in ("create", "edit") and "--notes-file" in arguments:
            self.body = Path(arguments[arguments.index("--notes-file") + 1]).read_text()
        if action == "edit" and "--draft=false" in arguments:
            self.draft = False
        if action == "upload":
            if self.draft is not True:
                raise AssertionError("Published release was modified")
            for index, argument in enumerate(arguments[3:-1]):
                path = Path(argument.split("#", 1)[0])
                self.files[path.name] = path.read_bytes()
                if self.fail_upload and index == 1:
                    raise subprocess.CalledProcessError(1, ["gh", "release", "upload"],
                                                        stderr="Upload interrupted")
        if action == "download":
            directory = Path(arguments[arguments.index("--dir") + 1])
            for name, content in self.files.items():
                (directory / name).write_bytes(content)
            if self.corrupt_download:
                (directory / release.package_names(VERSION)[0]).write_bytes(b"corrupted")
        return ""

    def mutations(self):
        return [command for command in self.commands if command[1] != "download"]


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        # Publication messages refer to the simulated release, so keep them out
        # of test output that might otherwise look like a real deployment.
        self.print_patch = patch("builtins.print")
        self.print_patch.start()
        self.addCleanup(self.print_patch.stop)
        self.temporary = tempfile.TemporaryDirectory(prefix="pci release tests ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cmake = self.root / "CMakeLists.txt"
        self.cmake.write_text("project(pcinspector\n VERSION 0.1.0\n LANGUAGES CXX)\n")
        self.directory = self.root / "packages"
        self.directory.mkdir()
        self.names = release.package_names(VERSION)
        for index, name in enumerate(self.names):
            path = self.directory / name
            path.write_bytes(f"Installer payload for {name}".encode())
            digest = hashlib.sha256(path.read_bytes()).hexdigest()
            # Exercise CPack, shasum, and sha256sum binary-mode checksum formats.
            checksum = [digest, f"{digest}  {name}", f"{digest} *{name}"][index]
            (self.directory / (name + ".sha256")).write_text(checksum + "\n")
        self.github = FakeGitHub()

    def publish(self):
        return release.publish(self.directory, VERSION, TAG, COMMIT, self.github)

    def test_project_tag_and_configured_version_match(self):
        version_file = self.root / "pcinspector-version.txt"
        version_file.write_bytes(b"0.1.0\r\n")
        self.assertEqual(release.read_version(self.cmake, TAG, VERSION, version_file), VERSION)

    def test_malformed_or_mismatched_tag_is_rejected(self):
        for tag in ("v0.2.0", "0.1.0", "v0.1", "v0.1.0-rc.1", "v00.1.0", "v0.1.0\n"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release.read_version(self.cmake, tag)

    def test_reusable_workflow_version_mismatch_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "Expected version"):
            release.read_version(self.cmake, expected="0.2.0")

    def test_configured_version_mismatch_is_rejected(self):
        version_file = self.root / "pcinspector-version.txt"
        version_file.write_text("0.2.0\n")
        with self.assertRaisesRegex(ValueError, "Configured package version"):
            release.read_version(self.cmake, version_file=version_file)

    def test_missing_or_malformed_project_version_is_rejected(self):
        for text in ("project(other VERSION 0.1.0)", "project(pcinspector)",
                     "project(pcinspector VERSION 0.1.0-rc.1)",
                     "# project(pcinspector VERSION 0.1.0)\n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.cmake.write_text(text)
                release.read_version(self.cmake)

    def test_checksum_formats_are_verified(self):
        self.assertEqual(len(release.validate_assets(self.directory, VERSION)), 6)

    def test_missing_installer_or_checksum_prevents_any_github_write(self):
        for name in (self.names[0], self.names[0] + ".sha256"):
            with self.subTest(name=name):
                path = self.directory / name
                content = path.read_bytes()
                path.unlink()
                with self.assertRaisesRegex(ValueError, "missing"):
                    self.publish()
                self.assertEqual(self.github.mutations(), [])
                path.write_bytes(content)

    def test_unexpected_asset_is_rejected(self):
        (self.directory / "test-diagnostics.txt").write_text("Logs")
        with self.assertRaisesRegex(ValueError, "unexpected"):
            self.publish()
        self.assertEqual(self.github.mutations(), [])

    def test_corrupt_installer_prevents_any_github_write(self):
        (self.directory / self.names[0]).write_bytes(b"corrupted")
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            self.publish()
        self.assertEqual(self.github.mutations(), [])

    def test_invalid_checksum_or_wrong_filename_is_rejected(self):
        checksum_file = self.directory / (self.names[0] + ".sha256")
        for checksum in ("not a hash", "f" * 64 + "  another.msi", "f" * 64 + "\nextra"):
            with self.subTest(checksum=checksum), self.assertRaisesRegex(ValueError, "Invalid checksum"):
                checksum_file.write_text(checksum)
                release.validate_assets(self.directory, VERSION)

    def test_empty_file_and_symlink_are_rejected(self):
        path = self.directory / self.names[0]
        original = path.read_bytes()
        path.write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "nonempty regular file"):
            release.validate_assets(self.directory, VERSION)
        path.unlink()
        target = self.root / "external.msi"
        target.write_bytes(original)
        path.symlink_to(target)
        with self.assertRaisesRegex(ValueError, "nonempty regular file"):
            release.validate_assets(self.directory, VERSION)

    def test_new_release_is_verified_before_publication(self):
        url = self.publish()
        self.assertEqual(url, "https://github.com/dwastberg/pointcloudinspector/releases/tag/v0.1.0")
        self.assertFalse(self.github.draft)
        self.assertEqual(len(self.github.files), 6)
        self.assertEqual([command[1] for command in self.github.commands],
                         ["create", "upload", "download", "edit"])
        for name in self.names:
            self.assertIn(f"/releases/download/{TAG}/{name})", self.github.body)
            self.assertIn(f"/releases/download/{TAG}/{name}.sha256)", self.github.body)
        self.assertIn("Generated release notes.", self.github.body)
        self.assertIn("notarized", self.github.body)

    def test_failed_upload_leaves_draft_and_retry_resumes_it(self):
        self.github.fail_upload = True
        with self.assertRaises(subprocess.CalledProcessError):
            self.publish()
        self.assertTrue(self.github.draft)
        self.assertEqual(len(self.github.files), 2)
        self.github.fail_upload = False
        self.publish()
        self.assertFalse(self.github.draft)
        self.assertEqual(sum(command[1] == "create" for command in self.github.commands), 1)
        self.assertEqual(len(self.github.files), 6)

    def test_corrupt_download_leaves_release_unpublished(self):
        self.github.corrupt_download = True
        with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
            self.publish()
        self.assertTrue(self.github.draft)
        self.assertFalse(any("--draft=false" in command for command in self.github.commands))

    def test_published_release_rerun_only_verifies(self):
        self.publish()
        self.github.commands.clear()
        self.publish()
        self.assertEqual(self.github.mutations(), [])
        self.assertEqual([command[1] for command in self.github.commands], ["download"])

    def test_published_release_with_missing_asset_is_not_modified(self):
        self.publish()
        self.github.commands.clear()
        del self.github.files[self.names[0]]
        with self.assertRaisesRegex(ValueError, "six expected assets"):
            self.publish()
        self.assertEqual(self.github.mutations(), [])

    def test_published_release_with_different_valid_packages_is_not_modified(self):
        self.publish()
        self.github.commands.clear()
        name = self.names[0]
        self.github.files[name] = b"x" * len(self.github.files[name])
        self.github.files[name + ".sha256"] = (
            hashlib.sha256(self.github.files[name]).hexdigest() + "\n").encode()
        with self.assertRaisesRegex(ValueError, "differ from the built packages"):
            self.publish()
        self.assertEqual(self.github.mutations(), [])

    def test_unexpected_draft_asset_is_not_modified(self):
        self.github.draft = True
        self.github.files["unrelated.txt"] = b"keep this"
        with self.assertRaisesRegex(ValueError, "unexpected assets"):
            self.publish()
        self.assertEqual(self.github.mutations(), [])

    def test_moved_remote_tag_prevents_any_github_write(self):
        self.github.commit = "c" * 40
        with self.assertRaisesRegex(ValueError, "build commit"):
            self.publish()
        self.assertEqual(self.github.mutations(), [])

    def test_lightweight_release_tag_is_supported(self):
        self.github.annotated = False
        self.publish()
        self.assertFalse(self.github.draft)

    def test_network_and_permission_errors_are_not_missing_releases(self):
        github = release.GitHub("dwastberg/pointcloudinspector")
        for message in ("gh: Forbidden (HTTP 403)", "connection timed out"):
            with self.subTest(message=message), patch.object(
                    github, "run", side_effect=subprocess.CalledProcessError(
                        1, ["gh", "api"], stderr=message)):
                with self.assertRaises(subprocess.CalledProcessError):
                    github.release(TAG)

    def test_older_draft_is_found_after_paginating(self):
        github = release.GitHub("dwastberg/pointcloudinspector")
        not_found = subprocess.CalledProcessError(1, ["gh", "api"],
                                                 stderr="gh: Not Found (HTTP 404)")
        first_page = [{"tag_name": f"v1.0.{index}", "draft": False} for index in range(100)]
        draft = {"tag_name": TAG, "draft": True, "assets": []}
        with patch.object(github, "api", side_effect=[not_found, first_page, [draft]]) as api:
            self.assertEqual(github.release(TAG), draft)
            self.assertEqual(api.call_args_list[-1].args, ("releases?per_page=100&page=2",))

    def test_draft_listing_failure_is_not_treated_as_absent_release(self):
        github = release.GitHub("dwastberg/pointcloudinspector")
        not_found = subprocess.CalledProcessError(1, ["gh", "api"],
                                                 stderr="gh: Not Found (HTTP 404)")
        forbidden = subprocess.CalledProcessError(1, ["gh", "api"],
                                                 stderr="gh: Forbidden (HTTP 403)")
        with patch.object(github, "api", side_effect=[not_found, forbidden]):
            with self.assertRaises(subprocess.CalledProcessError):
                github.release(TAG)


if __name__ == "__main__":
    unittest.main()
