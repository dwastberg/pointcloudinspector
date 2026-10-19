# Releasing Point Cloud Inspector

The **Release** workflow starts when a `vX.Y.Z` tag is pushed. It builds and tests
Windows x64 MSI, macOS ARM64 DMG, and Linux x86_64 Flatpak packages from the
tagged commit, then publishes a release with a download table, generated release
notes, and a SHA-256 checksum for each installer. Release checks use Bash on
macOS/Linux and PowerShell on Windows; the release process requires no Python.

## Prepare and release

1. Update `project(pcinspector VERSION X.Y.Z ...)` in `CMakeLists.txt` and commit
   the version change along with the release workflow and supporting scripts.
   Keep the version numeric; release-candidate suffixes are not supported.
2. Push the commit and run **Actions → CI Linux** against it before tagging.
   Require all three jobs to pass: GCC Debug, GCC Release, and the Flatpak SDK
   build/tests. This checks optimized compiler warnings and Flatpak dependency
   builds before starting a release. CI Linux creates an installer artifact
   without publishing a release. Native diagnostics are retained separately as
   `linux-ci-diagnostics` and `linux-ci-release-diagnostics`; SDK/runtime and
   CMake/CTest details are in `linux-flatpak-diagnostics`, including on failure.
   Run the desired Windows, macOS, sanitizer, and GPU checks separately.
   The release workflow itself requires non-GPU tests and capability checks in
   all three package jobs to pass; it does not enforce the earlier CI Linux
   result or run the separate sanitizer or GPU suites.
   For local Flatpak verification on Apple silicon, run
   `./scripts/test-linux-container.py --preset flatpak --arch arm64`.
   See [local Flatpak checks](../scripts/README.md#local-flatpak-checks) for
   reports, caching, and x86-64 verification through Rosetta. ARM64 local results
   do not replace the x86-64 CI check required before tagging.
3. Push the commit and create an annotated tag pointing to it. For version
   `0.1.0`, when the intended release commit is checked out:

   ```sh
   git push origin HEAD
   git tag -a v0.1.0 -m "Point Cloud Inspector 0.1.0"
   git push origin v0.1.0
   ```

4. Follow **Actions → Release**. All three platform jobs must succeed before
   publication. The publish job verifies all three installers and their checksum
   files, then creates and publishes the release using GitHub CLI. The tag must
   already exist and match the CMake version.
5. Check the release page: each of the three installer links and checksum links
   should download successfully, and the generated notes should describe the
   release. Smoke-test installation, application startup, and opening a point
   cloud on each supported platform for the first release.

The repository's built-in `GITHUB_TOKEN` is used; no personal access token or
additional secrets are needed. Build jobs have read access and only the publish
job requests `contents: write`. Repository or organization policy must allow
that permission. All three package workflows remain available as independent
manual actions, which create Actions artifacts without publishing a release.

## Recover a failed release

- If a package build or test fails, no release is published. In the failed run,
  select **Re-run failed jobs** after resolving any transient cause. Re-run
  package jobs replace their own earlier installer and diagnostic artifacts so
  repeated uploads do not collide.
- If uploading fails, an unpublished draft may remain. Delete only that
  unpublished draft from GitHub Releases, keeping the Git tag, then re-run the
  failed jobs. Draft recovery is manual.
- If the release already exists, publication fails without changing it. If
  publication succeeded but the workflow was interrupted, check the release
  page; there is no need to run publication again.
- Keep the successful package jobs and their original Actions artifacts when
  retrying. Rebuilding can produce different installer bytes. Expired artifacts
  cannot be recovered by the publish job.
- For a source or workflow fix, commit it, increment the version, and push a new
  tag. Never move or reuse an existing release tag. Published releases are not
  overwritten.

## Signing and supported packages

The first release process uses the existing signing setup: Windows MSI is
unsigned, and the macOS app is ad-hoc signed without Developer ID signing or
notarization. This status is included in the release notes. Certificate signing,
notarization, prereleases, Intel macOS builds, Linux ARM builds, and store or
package-manager publication are separate future additions.

## Validate changes to release automation

From the repository root:

```sh
bash -n scripts/release.sh
bash scripts/release.sh version 0.1.0
actionlint .github/workflows/release.yml .github/workflows/package-windows.yml \
  .github/workflows/package-macos.yml .github/workflows/package-linux-flatpak.yml
```

Use the current CMake version for the version check. On Windows, check the
PowerShell helper with `./scripts/release-version.ps1 -ExpectedVersion 0.1.0`.
Both helpers optionally check the generated `pcinspector-version.txt` file.
