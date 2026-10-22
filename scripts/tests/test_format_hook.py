"""Exercise the pre-commit hook through real commits in isolated repositories."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
UNFORMATTED = b"int  value;\n"
FORMATTED = b"int value;\n"


class FormatHookTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pci hook tests ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.repo = self.root / "checkout with spaces"
        self.repo.mkdir()
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith("GIT_")}
        self.env.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        self.log = self.root / "formatter-invocations.jsonl"
        self.env["PCI_TEST_FORMAT_LOG"] = str(self.log)
        self.env["PCI_TEST_FORMAT_VERSION"] = "22.1.8"

        self.formatter = self.root / "formatter with spaces"
        self.formatter.write_text(textwrap.dedent("""\
            #!/usr/bin/env python3
            import json
            import os
            from pathlib import Path
            import sys

            with Path(os.environ["PCI_TEST_FORMAT_LOG"]).open("a") as log:
                log.write(json.dumps(sys.argv[1:]) + "\\n")
            if sys.argv[1:] == ["--version"]:
                print("clang-format version " + os.environ["PCI_TEST_FORMAT_VERSION"])
                sys.exit(0)
            source = sys.stdin.buffer.read()
            if b"FORMAT_FAIL" in source:
                print("formatting failed for test", file=sys.stderr)
                sys.exit(2)
            sys.stdout.buffer.write(source.replace(b"int  value;", b"int value;"))
            """))
        self.formatter.chmod(0o755)

        self.git("init")
        self.git("config", "user.name", "Hook Test")
        self.git("config", "user.email", "hook-test@example.invalid")
        self.git("config", "core.autocrlf", "false")
        self.git("config", "core.fileMode", "true")
        self.git("config", "commit.gpgSign", "false")
        self.write("README.md", b"baseline\n")
        self.write("src/existing.cpp", FORMATTED)
        self.write("src/untouched.cpp", FORMATTED)
        shutil.copy2(ROOT / ".clang-format", self.repo / ".clang-format")
        hook = self.repo / ".githooks/pre-commit"
        hook.parent.mkdir()
        shutil.copy2(ROOT / ".githooks/pre-commit", hook)
        self.git("add", ".")
        self.git("commit", "--no-verify", "-m", "baseline")
        self.git("config", "core.hooksPath", ".githooks")
        self.git("config", "pcinspector.clangFormat", str(self.formatter))

    def git(self, *arguments, check=True):
        return subprocess.run(["git", *arguments], cwd=self.repo, env=self.env,
                              capture_output=True, check=check)

    def write(self, name, content):
        path = self.repo / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def stage(self, name, content=UNFORMATTED):
        path = self.write(name, content)
        self.git("add", "--", name)
        return path

    def commit(self):
        return self.git("commit", "-m", "test hook", check=False)

    def assert_blocked_without_changes(self, message):
        tree = self.git("write-tree").stdout
        head = self.git("rev-parse", "HEAD").stdout
        contents = {path: path.read_bytes() for path in self.repo.rglob("*")
                    if path.is_file() and ".git" not in path.relative_to(self.repo).parts}
        result = self.commit()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(message, result.stderr)
        self.assertEqual(self.git("write-tree").stdout, tree)
        self.assertEqual(self.git("rev-parse", "HEAD").stdout, head)
        for path, content in contents.items():
            self.assertEqual(path.read_bytes(), content, str(path))

    def test_formats_and_commits_only_staged_project_files(self):
        names = ("main.cpp", "src/existing.cpp", "src/new file.h",
                 "tests/test case.cpp", "tools/tool.cpp")
        for name in names:
            self.stage(name)
        self.stage("src/existing.cpp", UNFORMATTED + b"// staged edit\n")
        # These edits must stay outside the commit, even though one is C++.
        self.write("README.md", b"unstaged documentation\n")
        self.write("src/untouched.cpp", UNFORMATTED)
        self.write("src/untracked.cpp", UNFORMATTED)
        result = self.commit()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"formatted and staged 5 C++ file(s)", result.stdout + result.stderr)
        for name in names:
            expected = FORMATTED + b"// staged edit\n" if name == "src/existing.cpp" else FORMATTED
            self.assertEqual(self.git("show", "HEAD:" + name).stdout, expected)
            self.assertEqual((self.repo / name).read_bytes(), expected)
        self.assertEqual(self.git("show", "HEAD:README.md").stdout, b"baseline\n")
        self.assertEqual(self.git("show", "HEAD:src/untouched.cpp").stdout, FORMATTED)
        self.assertEqual((self.repo / "src/untouched.cpp").read_bytes(), UNFORMATTED)
        self.assertEqual((self.repo / "src/untracked.cpp").read_bytes(), UNFORMATTED)
        self.assertEqual(self.git("diff", "--cached", "--name-only").stdout, b"")

    def test_already_formatted_file_commits_without_rewriting(self):
        path = self.stage("src/new.cpp", FORMATTED)
        timestamp = path.stat().st_mtime_ns
        result = self.commit()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(b"formatted and staged", result.stdout + result.stderr)
        self.assertEqual(path.stat().st_mtime_ns, timestamp)

    def test_partially_staged_file_blocks_all_formatting(self):
        self.stage("src/a.cpp")
        self.stage("src/existing.cpp")
        self.write("src/existing.cpp", UNFORMATTED + b"// unfinished edit\n")
        self.assert_blocked_without_changes(b"unstaged changes")
        self.assertFalse(self.log.exists())

    def test_unstaged_style_change_blocks_all_formatting(self):
        self.stage("src/new.cpp")
        style = self.repo / ".clang-format"
        style.write_bytes(style.read_bytes() + b"# unstaged\n")
        self.assert_blocked_without_changes(b"unstaged changes")
        self.assertFalse(self.log.exists())

    def test_untracked_style_file_blocks_all_formatting(self):
        self.git("rm", ".clang-format")
        self.git("commit", "-m", "remove style")
        self.write(".clang-format", b"BasedOnStyle: LLVM\n")
        self.stage("src/new.cpp")
        self.assert_blocked_without_changes(b"unstaged changes")
        self.assertFalse(self.log.exists())

    def test_fully_staged_style_change_is_allowed(self):
        style = (self.repo / ".clang-format").read_bytes() + b"# staged\n"
        self.stage(".clang-format", style)
        self.stage("src/new.cpp")
        result = self.commit()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.git("show", "HEAD:.clang-format").stdout, style)
        self.assertEqual(self.git("show", "HEAD:src/new.cpp").stdout, FORMATTED)

    def test_missing_formatter_blocks_commit_without_changes(self):
        self.git("config", "pcinspector.clangFormat", str(self.root / "missing"))
        self.stage("src/new.cpp")
        self.assert_blocked_without_changes(b"clang-format 22.1.8 was not found")

    def test_incorrect_versions_block_commit_without_changes(self):
        self.stage("src/new.cpp")
        for version in ("21.1.8", "22.1.7", "22.1.80"):
            with self.subTest(version=version):
                self.env["PCI_TEST_FORMAT_VERSION"] = version
                self.assert_blocked_without_changes(b"clang-format 22.1.8 is required")

    def test_formatting_failure_leaves_earlier_files_unchanged(self):
        self.stage("src/a.cpp")
        self.stage("src/z.cpp", UNFORMATTED + b"// FORMAT_FAIL\n")
        self.assert_blocked_without_changes(b"formatting failed for test")
        invocations = [json.loads(line) for line in self.log.read_text().splitlines()]
        self.assertEqual(len(invocations), 3)  # Version, then both source files.

    def test_renamed_file_with_spaces_is_formatted(self):
        self.git("mv", "src/existing.cpp", "src/renamed file.cpp")
        self.stage("src/renamed file.cpp")
        result = self.commit()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.git("show", "HEAD:src/renamed file.cpp").stdout, FORMATTED)
        self.assertFalse((self.repo / "src/existing.cpp").exists())

    def test_filenames_are_null_delimited_and_literal_pathspecs(self):
        for name in ("src/line\nbreak.cpp", "src/[ab].h"):
            self.write(name, UNFORMATTED)
            self.git("--literal-pathspecs", "add", "--", name)
        self.write("src/a.h", b"unstaged file\n")
        result = self.commit()
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in ("src/line\nbreak.cpp", "src/[ab].h"):
            self.assertEqual(self.git("show", "HEAD:" + name).stdout, FORMATTED)
        self.assertEqual((self.repo / "src/a.h").read_bytes(), b"unstaged file\n")
        self.assertNotEqual(self.git("show", "HEAD:src/a.h", check=False).returncode, 0)

    def test_irrelevant_files_deletions_and_symlinks_need_no_formatter(self):
        self.git("config", "pcinspector.clangFormat", str(self.root / "missing"))
        for name in ("README.md", "third_party/vendor.cpp", "src/notes.txt", "other.h"):
            self.stage(name, UNFORMATTED)
        self.git("rm", "src/existing.cpp")
        link = self.repo / "src/link.h"
        link.symlink_to("../other.h")
        self.git("add", "src/link.h")
        result = self.commit()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.log.exists())
        self.assertEqual(self.git("show", "HEAD:third_party/vendor.cpp").stdout, UNFORMATTED)
        self.assertEqual(self.git("show", "HEAD:src/link.h").stdout, b"../other.h")
        self.assertTrue(link.is_symlink())

    def test_executable_permission_is_preserved(self):
        path = self.stage("tools/executable.cpp")
        path.chmod(0o755)
        self.git("add", "tools/executable.cpp")
        result = self.commit()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(path.stat().st_mode & 0o777, 0o755)
        self.assertTrue(self.git("ls-tree", "HEAD", "tools/executable.cpp").stdout.startswith(b"100755"))

    def test_formatter_can_be_found_on_path(self):
        self.git("config", "--unset", "pcinspector.clangFormat")
        directory = self.root / "bin"
        directory.mkdir()
        shutil.copy2(self.formatter, directory / "clang-format")
        self.env["PATH"] = str(directory) + os.pathsep + self.env["PATH"]
        self.stage("src/new.cpp")
        result = self.commit()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.git("show", "HEAD:src/new.cpp").stdout, FORMATTED)

    def test_real_clang_format_when_available(self):
        formatter = shutil.which("clang-format")
        if not formatter:
            self.skipTest("clang-format is not installed")
        version = subprocess.check_output([formatter, "--version"])
        if b"version 22.1.8\n" not in version:
            self.skipTest("clang-format 22.1.8 is not installed")
        self.git("config", "pcinspector.clangFormat", formatter)
        self.stage("src/new.cpp", b"int main(){return 0;}\n")
        result = self.commit()
        self.assertEqual(result.returncode, 0, result.stderr)
        committed = self.git("show", "HEAD:src/new.cpp").stdout
        self.assertEqual(committed, b"int main()\n{\n    return 0;\n}\n")


if __name__ == "__main__":
    unittest.main()
