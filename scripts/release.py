#!/usr/bin/env python3
"""Validate release versions and publish verified packages using GitHub CLI."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


VERSION_PATTERN = r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)"
PACKAGES = (
    ("windows-x64.msi", "Windows x64", "Windows installer (.msi)"),
    ("macOS-arm64.dmg", "macOS Apple silicon", "macOS installer (.dmg)"),
    ("linux-x86_64.flatpak", "Linux x86_64", "Linux Flatpak (.flatpak)"),
)


def validate_version(version):
    if not re.fullmatch(VERSION_PATTERN, version):
        raise ValueError(f"Expected a numeric X.Y.Z version, got {version!r}")
    return version


def read_version(cmake=Path("CMakeLists.txt"), tag=None, expected="", version_file=None):
    text = re.sub(r"#\[[=]*\[.*?\][=]*\]", "", cmake.read_text(), flags=re.S)
    text = re.sub(r"#[^\n]*", "", text)
    project = re.search(r"\bproject\s*\(\s*pcinspector\b([^)]*)\)", text, re.I)
    match = re.search(r"\bVERSION\s+([^\s)]+)", project[1], re.I) if project else None
    if not match:
        raise ValueError("Cannot find pcinspector's project VERSION in CMakeLists.txt")
    version = validate_version(match[1])
    if tag is not None and (not re.fullmatch(f"v{VERSION_PATTERN}", tag)
                            or tag != f"v{version}"):
        raise ValueError(f"Release tag {tag!r} must equal v{version}")
    if expected and validate_version(expected) != version:
        raise ValueError(f"Expected version {expected}, but CMake declares {version}")
    if version_file is not None:
        configured = validate_version(version_file.read_text().strip())
        if configured != version:
            raise ValueError(f"Configured package version {configured} differs from {version}")
    return version


def package_names(version):
    validate_version(version)
    return [f"PointCloudInspector-{version}-{suffix}" for suffix, _, _ in PACKAGES]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_assets(directory, version):
    packages = package_names(version)
    expected = set(packages + [name + ".sha256" for name in packages])
    actual = {path.name for path in directory.iterdir()}
    if actual != expected:
        raise ValueError(f"Release assets differ: missing {sorted(expected - actual)}, "
                         f"unexpected {sorted(actual - expected)}")
    for name in expected:
        path = directory / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"Release asset must be a nonempty regular file: {name}")
    hashes = {name: sha256(directory / name) for name in expected}
    for name in packages:
        # CPack writes a bare digest; sha256sum and shasum also write a filename.
        checksum = (directory / (name + ".sha256")).read_text().strip()
        match = re.fullmatch(r"([0-9a-fA-F]{64})(?:[ \t]+\*?([^\r\n]+))?", checksum)
        if not match or (match[2] is not None and match[2] != name):
            raise ValueError(f"Invalid checksum file for {name}")
        if match[1].lower() != hashes[name]:
            raise ValueError(f"SHA-256 mismatch for {name}")
    return hashes


def download_notes(repository, tag, version):
    base = f"https://github.com/{repository}/releases/download/{tag}"
    rows = ["## Downloads", "", "| Platform | Download | SHA-256 |",
            "|---|---|---|"]
    for name, (_, platform, label) in zip(package_names(version), PACKAGES):
        rows.append(f"| {platform} | [{label}]({base}/{name}) | "
                    f"[Checksum]({base}/{name}.sha256) |")
    linux_package = package_names(version)[2]
    rows.extend([
        "", "## Installation", "",
        "- **Windows:** Run the MSI installer. Administrator access is required.",
        "- **macOS:** Open the DMG and drag Point Cloud Inspector to Applications.",
        "- **Linux:** Install Flatpak and add Flathub for the KDE runtime, then "
        "install the downloaded bundle:",
        "", "```sh",
        "flatpak remote-add --user --if-not-exists flathub "
        "https://flathub.org/repo/flathub.flatpakrepo",
        f"flatpak install --user ./{linux_package}",
        "flatpak run io.github.dwastberg.PointCloudInspector", "```", "",
        "**Signing:** The Windows installer is unsigned. The macOS app is "
        "ad-hoc signed and is not Developer ID signed or notarized; operating "
        "system security prompts may appear.",
        "",
    ])
    return "\n".join(rows) + "\n"


class GitHub:
    def __init__(self, repository):
        if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository):
            raise ValueError("Repository must be OWNER/REPO")
        self.repository = repository

    def run(self, *arguments):
        command = ["gh", *arguments]
        if arguments[0] == "release":
            command.extend(["--repo", self.repository])
        return subprocess.run(command, check=True, capture_output=True, text=True).stdout

    def api(self, endpoint, fields=None):
        arguments = ["api", f"repos/{self.repository}/{endpoint}"]
        if fields is not None:
            arguments.extend(["--method", "POST"])
            for key, value in fields.items():
                arguments.extend(["-f", f"{key}={value}"])
        return json.loads(self.run(*arguments))

    def release(self, tag):
        try:
            return self.api(f"releases/tags/{tag}")
        except subprocess.CalledProcessError as error:
            # An authentication or network failure must not look like an absent release.
            if "(HTTP 404)" not in (error.stderr or ""):
                raise
        # The tag endpoint can omit drafts. Authenticated release listings include
        # them; paginate so an older interrupted draft can still be recovered.
        page = 1
        while True:
            releases = self.api(f"releases?per_page=100&page={page}")
            matches = [item for item in releases if item["tag_name"] == tag]
            if len(matches) > 1:
                raise ValueError("Multiple releases use this tag; refusing to choose one")
            if matches:
                return matches[0]
            if len(releases) < 100:
                return None
            page += 1

    def verify_tag(self, tag, commit):
        if not re.fullmatch(r"[0-9a-f]{40}", commit):
            raise ValueError("Release commit must be a full GitHub commit SHA")
        obj = self.api(f"git/ref/tags/{tag}")["object"]
        seen = set()
        while obj["type"] == "tag":
            if obj["sha"] in seen:
                raise ValueError("Release tag contains a cycle")
            seen.add(obj["sha"])
            obj = self.api(f"git/tags/{obj['sha']}")["object"]
        if obj["type"] != "commit" or obj["sha"] != commit:
            raise ValueError("Remote release tag no longer points to the build commit")


def verify_uploaded(github, tag, release, directory, version, hashes):
    assets = release["assets"]
    if len(assets) != len(hashes) or {asset["name"] for asset in assets} != set(hashes):
        raise ValueError("GitHub Release must contain exactly the six expected assets")
    for asset in assets:
        if (asset["state"] != "uploaded"
                or asset["size"] != (directory / asset["name"]).stat().st_size):
            raise ValueError(f"Incomplete release asset: {asset['name']}")
    # Verify actual download bytes, including checksum files, before publication.
    with tempfile.TemporaryDirectory(prefix="pci-release-verify-") as temporary:
        github.run("release", "download", tag, "--dir", temporary, "--pattern", "*")
        if validate_assets(Path(temporary), version) != hashes:
            raise ValueError("Uploaded release assets differ from the built packages")


def publish(directory, version, tag, commit, github):
    if tag != f"v{validate_version(version)}":
        raise ValueError("Release tag does not match the package version")
    hashes = validate_assets(directory, version)
    github.verify_tag(tag, commit)
    release = github.release(tag)
    url = f"https://github.com/{github.repository}/releases/tag/{tag}"
    if release is not None and not release["draft"]:
        verify_uploaded(github, tag, release, directory, version, hashes)
        print(f"Already published and verified: {url}")
        return url
    if release is not None and any(asset["name"] not in hashes for asset in release["assets"]):
        raise ValueError("Existing draft contains unexpected assets; refusing to replace it")

    generated = github.api("releases/generate-notes", {
        "tag_name": tag, "target_commitish": commit,
    })["body"]
    notes = download_notes(github.repository, tag, version) + generated + "\n"
    with tempfile.TemporaryDirectory(prefix="pci-release-notes-") as temporary:
        notes_file = Path(temporary) / "notes.md"
        notes_file.write_text(notes, encoding="utf-8")
        title = f"Point Cloud Inspector {version}"
        if release is None:
            github.run("release", "create", tag, "--draft", "--verify-tag",
                       "--target", commit, "--title", title, "--notes-file", str(notes_file))
        else:
            github.run("release", "edit", tag, "--title", title,
                       "--notes-file", str(notes_file), "--prerelease=false")

    uploads = []
    for name, (_, _, label) in zip(package_names(version), PACKAGES):
        uploads.extend([f"{directory / name}#{label}",
                        f"{directory / (name + '.sha256')}#{label} — SHA-256"])
    github.run("release", "upload", tag, *uploads, "--clobber")
    release = github.release(tag)
    if release is None or not release["draft"]:
        raise ValueError("Expected an unpublished draft while verifying uploaded assets")
    verify_uploaded(github, tag, release, directory, version, hashes)
    github.verify_tag(tag, commit)
    github.run("release", "edit", tag, "--draft=false", "--verify-tag")
    print(f"Published: {url}")
    return url


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    version_parser = commands.add_parser("version", help="Validate and print the CMake version")
    publish_parser = commands.add_parser("publish", help="Upload verified assets and publish")
    for command in (version_parser, publish_parser):
        command.add_argument("--cmake", type=Path, default=Path("CMakeLists.txt"))
        command.add_argument("--tag", required=command is publish_parser)
        command.add_argument("--expected-version", default="")
        command.add_argument("--version-file", type=Path)
    publish_parser.add_argument("--directory", type=Path, required=True)
    publish_parser.add_argument("--repo", required=True)
    publish_parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    try:
        version = read_version(args.cmake, args.tag, args.expected_version, args.version_file)
        if args.command == "version":
            print(version)
        else:
            url = publish(args.directory, version, args.tag, args.commit, GitHub(args.repo))
            if os.environ.get("GITHUB_STEP_SUMMARY"):
                with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as summary:
                    summary.write(f"[Point Cloud Inspector {version}]({url})\n")
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        detail = error.stderr if isinstance(error, subprocess.CalledProcessError) else str(error)
        print(f"Release failed: {detail}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
