#!/usr/bin/env python3
"""Run the Linux CI configurations with Apple container 1.5.0 or newer."""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import tarfile
import time
import uuid
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parent.parent
PRESETS = ("ci", "clang-tidy", "asan-ubsan", "tsan")
MAMBA_PLATFORMS = {"amd64": "linux-64", "arm64": "linux-aarch64"}
IMAGE_FILES = {
    "Containerfile": "scripts/container/Containerfile",
    "entrypoint.sh": "scripts/container/entrypoint.sh",
    "ci-environment.yml": ".github/ci-environment.yml",
    "validation-environment-linux.yml": ".github/validation-environment-linux.yml",
}


def capture(args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, text=True, stderr=subprocess.STDOUT, timeout=60).strip()


def write_json(path, value):
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(path)


def run_logged(args, path, timeout):
    """Stream output without losing the exit code; bound setup/build hangs too."""
    print("+ " + subprocess.list2cmdline([str(arg) for arg in args]), flush=True)
    started = time.monotonic()
    with path.open("wb") as output, path.open("rb") as reader:
        process = subprocess.Popen(args, stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=True)
        try:
            while True:
                chunk = reader.read()
                if chunk:
                    sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                    sys.stdout.flush()
                code = process.poll()
                if code is not None:
                    sys.stdout.write(reader.read().decode("utf-8", errors="replace"))
                    return code
                if time.monotonic() - started > timeout:
                    print(f"Command exceeded {timeout}s; see {path}", flush=True)
                    return 124
                time.sleep(0.2)
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


def source_files(repo, prefix=Path()):
    """Read working-tree contents, recursing into initialized Git submodules."""
    entries = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"], cwd=repo
    ).split(b"\0")
    for raw in sorted(set(entries)):
        if not raw:
            continue
        relative = Path(os.fsdecode(raw))
        if ".git" in relative.parts:
            continue
        path = repo / relative
        if path.is_symlink():
            yield path, prefix / relative
        elif path.is_dir():
            if not (path / ".git").exists():
                raise RuntimeError(f"Submodule {path} is not initialized; run git submodule update --init --recursive")
            yield from source_files(path, prefix / relative)
        elif path.exists():
            yield path, prefix / relative


def snapshot(repo, destination):
    status = capture(["git", "submodule", "status", "--recursive"], cwd=repo)
    if any(line.startswith(("-", "U")) for line in status.splitlines()):
        raise RuntimeError("Initialize/resolve submodules first: git submodule update --init --recursive")
    with tarfile.open(destination, "w", dereference=False) as archive:
        for path, name in source_files(repo):
            archive.add(path, arcname=str(name), recursive=False)
    digest = hashlib.sha256()
    with destination.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return {
        "commit": capture(["git", "rev-parse", "HEAD"], cwd=repo),
        "status": capture(["git", "status", "--short"], cwd=repo),
        "submodules": status,
        "snapshot_sha256": digest.hexdigest(),
    }


def prepare_image_context(repo, context, arch):
    context.mkdir()
    digest = hashlib.sha256(arch.encode())
    for name, relative in sorted(IMAGE_FILES.items()):
        data = (repo / relative).read_bytes()
        if arch == "arm64" and name == "validation-environment-linux.yml":
            data, count = re.subn(rb"(?m)^(\s*- )clangxx_linux-64(?=[=\s])",
                                  rb"\1clangxx_linux-aarch64", data)
            if count != 1:
                raise RuntimeError("Expected exactly one Linux Clang compiler dependency in the CI manifest")
        (context / name).write_bytes(data)
        digest.update(name.encode() + b"\0" + data)
    return f"pcinspector-linux-ci:{arch}-" + digest.hexdigest()[:20]


def test_counts(path):
    if not path.exists():
        return None
    root = ET.parse(path).getroot()
    cases = list(root.iter("testcase"))
    failed = [case.get("name") for case in cases
              if case.find("failure") is not None or case.find("error") is not None]
    skipped = sum(case.find("skipped") is not None for case in cases)
    return {"total": len(cases), "passed": len(cases) - len(failed) - skipped,
            "failed": len(failed), "skipped": skipped, "failures": failed}


def lane_result(preset, code, directory, elapsed):
    marker = directory / "result.json"
    detail = json.loads(marker.read_text()) if marker.exists() else {"stage": "environment"}
    counts = test_counts(directory / "ctest.xml")
    passed = (code == 0 and detail.get("exit_code") == 0
              and detail["stage"] == "complete" and counts is not None
              and counts["total"] > counts["skipped"] and counts["failed"] == 0)
    return {"preset": preset, "status": "passed" if passed else "failed",
            "stage": detail["stage"], "exit_code": code,
            "runner_exit_code": detail.get("exit_code"),
            "seconds": round(elapsed, 1), "tests": counts, "reports": str(directory)}


def container_args(args, image, name, inputs, reports, preset):
    command = ["container", "run", "--rm", "--name", name,
               "--platform", f"linux/{args.arch}", "--init",
               "--cpus", str(args.cpus), "--memory", args.memory, "--dns", args.dns,
               "--mount", f"source={inputs},target=/input,readonly",
               "--mount", f"source={reports},target=/reports",
               "--env", "PCINSPECTOR_LONG_STRESS=" + ("ON" if args.long_stress else "OFF"),
               "--env", f"PCI_CONTAINER_ARCH={args.arch}"]
    if args.arch == "amd64":
        command += ["--rosetta"]
    if preset == "tsan":
        command += ["--kernel-arg", "sysctl.vm.mmap_rnd_bits=28"]
    return command + [image, preset]


def remove_container(name):
    # Never stop/delete unrelated containers, the shared builder, or the service.
    result = subprocess.run(["container", "delete", "--force", name], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
    if result.returncode and "notFound" not in result.stdout:
        raise RuntimeError(f"Could not remove {name}: {result.stdout.strip()}")


def interrupt_handler(_signal, _frame):
    raise KeyboardInterrupt


def execute_lanes(args, image, inputs, output, summary):
    for index, item in enumerate(summary["configurations"]):
        preset = item["preset"]
        reports = output / preset
        reports.mkdir()
        # The guest builder has a different UID from the macOS user. Set this
        # on the host: virtiofs does not allow guest root to chmod host files.
        reports.chmod(0o777)
        name = "pci-ci-" + uuid.uuid4().hex[:12]
        started = time.monotonic()
        try:
            # Match workflow job limits, while retaining each CTest timeout.
            timeout = 3600 if preset == "ci" else 5400
            code = run_logged(container_args(args, image, name, inputs, reports, preset),
                              reports / "container.log", timeout)
            item = lane_result(preset, code, reports, time.monotonic() - started)
        except KeyboardInterrupt:
            item.update(status="interrupted", stage="container", seconds=round(time.monotonic() - started, 1))
            raise
        except (OSError, ValueError, ET.ParseError, subprocess.SubprocessError) as error:
            item.update(status="failed", stage="environment", error=str(error))
        finally:
            summary["configurations"][index] = item
            try:
                remove_container(name)
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                item.update(status="failed", cleanup_error=str(error))
            write_json(output / "summary.json", summary)


def print_summary(summary, output):
    print(f"\nPlatform: {summary['platform']} — {summary['translation']}")
    print("\nConfiguration  Status       Stage          Tests (passed/total)  Seconds")
    for item in summary["configurations"]:
        counts = item.get("tests")
        tests = f"{counts['passed']}/{counts['total']}" if counts else "not run"
        print(f"{item['preset']:<14} {item['status']:<12} {item.get('stage', '-'):<14} "
              f"{tests:<21} {item.get('seconds', '-')}")
    if summary.get("error"):
        print("Setup error: " + summary["error"])
    print(f"Reports: {output}\nSummary: {output / 'summary.json'}", flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=MAMBA_PLATFORMS, default="amd64",
                        help="amd64 uses Rosetta; arm64 runs natively (default: amd64)")
    parser.add_argument("--preset", choices=PRESETS, action="append", help="Run only this preset (repeatable)")
    parser.add_argument("--long-stress", action="store_true", help="Include CI's optional long stress tests")
    parser.add_argument("--cpus", type=int, default=4, help="CPUs per container (default: 4)")
    parser.add_argument("--memory", default="16G", help="Memory per container (default: 16G)")
    parser.add_argument("--dns", default="1.1.1.1", help="Guest/builder DNS resolver (default: 1.1.1.1)")
    parser.add_argument("--rebuild-image", action="store_true", help="Refresh the base image and reinstall dependencies")
    args = parser.parse_args(argv)
    if args.cpus < 1:
        parser.error("--cpus must be positive")
    if not re.fullmatch(r"[1-9][0-9]*[KMGTP]?", args.memory, flags=re.IGNORECASE):
        parser.error("--memory must be a positive integer with an optional K/M/G/T/P suffix")
    if "," in str(ROOT):
        parser.error("Apple container mount syntax requires a repository path without commas")
    presets = list(dict.fromkeys(args.preset or PRESETS))
    run_id = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:6]
    output = ROOT / "build" / "container-ci" / run_id
    output.mkdir(parents=True)
    summary = {"platform": f"linux/{args.arch}",
               "translation": "Rosetta (ARM Linux kernel)" if args.arch == "amd64" else "native (no translation)",
               "resources": {"cpus": args.cpus, "memory": args.memory, "dns": args.dns},
               "long_stress": args.long_stress,
               "configurations": [{"preset": p, "status": "not-run"} for p in presets]}
    result = 1
    try:
        if platform.system() != "Darwin" or platform.machine() != "arm64":
            raise RuntimeError("This launcher requires Apple silicon macOS and Apple container 1.5.0 or newer")
        for executable in ("container", "git"):
            if not shutil.which(executable):
                raise RuntimeError(f"Required command is missing: {executable}")
        version = capture(["container", "--version"])
        found = re.search(r"version (\d+)\.(\d+)\.(\d+)", version)
        if not found or tuple(map(int, found.groups())) < (1, 5, 0):
            raise RuntimeError("Apple container 1.5.0 or newer is required: " + version)
        summary["container_cli"] = version
        status = subprocess.run(["container", "system", "status"], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        running = status.returncode == 0 and re.search(r"(?m)^status\s+running\s*$", status.stdout)
        if not running:
            if run_logged(["container", "system", "start", "--enable-kernel-install"],
                          output / "service-start.log", 300):
                raise RuntimeError("Could not start Apple container; see service-start.log")
        summary["container_service"] = capture(["container", "system", "version"])
        service_version = re.search(r"container-apiserver\s+(\d+\.\d+\.\d+)", summary["container_service"])
        if not service_version or service_version[1] != ".".join(found.groups()):
            raise RuntimeError("CLI and service versions differ; restart the service using the installed Apple container")

        inputs = output / "input"
        inputs.mkdir()
        summary["source"] = snapshot(ROOT, inputs / "source.tar")
        context = output / "image-context"
        image = prepare_image_context(ROOT, context, args.arch)
        summary["image"] = image
        summary["dependency_manifests"] = [str(context / name) for name in IMAGE_FILES if name.endswith(".yml")]
        existing = subprocess.run(["container", "image", "inspect", image],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        summary["image_reused"] = existing.returncode == 0 and not args.rebuild_image
        if not summary["image_reused"]:
            command = ["container", "build", "--platform", f"linux/{args.arch}", "--cpus", "4",
                       "--memory", "8G", "--dns", args.dns, "--progress", "plain",
                       "--build-arg", f"MAMBA_PLATFORM={MAMBA_PLATFORMS[args.arch]}",
                       "--file", str(context / "Containerfile"), "--tag", image]
            if args.rebuild_image:
                command += ["--no-cache", "--pull"]
            if run_logged(command + [str(context)], output / "image-build.log", 3600):
                raise RuntimeError("Dependency image build failed; see image-build.log")
        (output / "image.json").write_text(capture(["container", "image", "inspect", image]) + "\n")
        execute_lanes(args, image, inputs, output, summary)
        result = 0 if all(item["status"] == "passed" for item in summary["configurations"]) else 1
    except KeyboardInterrupt:
        summary["error"] = "Interrupted; remaining configurations were not run"
        result = 130
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        summary["error"] = str(error)
    finally:
        summary["exit_code"] = result
        write_json(output / "summary.json", summary)
        print_summary(summary, output)
    return result


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, interrupt_handler)
    sys.exit(main())
