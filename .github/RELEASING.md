# Releasing Point Cloud Inspector

The **Release** workflow starts when a `vX.Y.Z` tag is pushed. It builds and tests
Windows x64 MSI, macOS ARM64 DMG, and Linux x86_64 Flatpak packages from the
tagged commit, then publishes a release with a download table, generated release
notes, installation instructions, and a SHA-256 checksum for each installer.

## Prepare and release

1. Update `project(pcinspector VERSION X.Y.Z ...)` in `CMakeLists.txt` and commit
   the version change along with the release workflow and supporting scripts.
   Keep the version numeric; release-candidate suffixes are not supported.
2. Run the desired manual CI checks against that commit. The release workflow
   itself requires the non-GPU tests and capability checks in all three package
   jobs to pass; it does not run the separate sanitizer or GPU suites.
3. Push the commit and create an annotated tag pointing to it. For version
   `0.1.0`, when the intended release commit is checked out:

   ```sh
   git push origin HEAD
   git tag -a v0.1.0 -m "Point Cloud Inspector 0.1.0"
   git push origin v0.1.0
   ```

4. Follow **Actions → Release**. All three platform jobs must succeed before
   publication. The publish job checks that the remote tag still points to the
   build commit, verifies all six local files, uploads them to a draft, downloads
   and verifies their contents, and then publishes automatically.
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
- If uploading or verification fails, an unpublished draft may remain. Re-run
  the failed jobs: the publish job reuses the draft and replaces its expected
  assets before verifying them. Unexpected draft assets cause a failure and are
  left untouched; remove unintended assets yourself before retrying.
- If publication succeeded but the workflow was interrupted, re-running the
  publish job only checks the published release against the original artifacts.
  It never replaces published files or release notes.
- Keep the successful package jobs and their original Actions artifacts when
  retrying. Rebuilding can produce different installer bytes. Expired artifacts
  cannot be recovered by the publish job.
- For a source or workflow fix, commit it, increment the version, and push a new
  tag. Never move or reuse an existing release tag. A published release with
  missing or different assets fails verification and is not overwritten.

## Signing and supported packages

The first release process uses the existing signing setup: Windows MSI is
unsigned, and the macOS app is ad-hoc signed without Developer ID signing or
notarization. This status is included in the release notes. Certificate signing,
notarization, prereleases, Intel macOS builds, Linux ARM builds, and store or
package-manager publication are separate future additions.

## Validate changes to release automation

From the repository root:

```sh
python3 -m unittest discover -s scripts/tests -p test_release.py -v
python3 scripts/release.py version --tag v0.1.0
actionlint .github/workflows/release.yml .github/workflows/package-windows.yml \
  .github/workflows/package-macos.yml .github/workflows/package-linux-flatpak.yml
```

Use the current version for the tag check. The regression tests use simulated
GitHub responses and temporary package files; they never create a real release.
They cover version mismatches, missing or corrupted assets, interrupted uploads,
draft recovery, remote tag changes, and safe retries of published releases.
