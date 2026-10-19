"""Apply CI's test session to the app without invalidating dependency caches."""

import json
import os
from pathlib import Path
import sys


def prepare(manifest, environment):
    application = manifest["modules"][-1]
    options = application.setdefault("build-options", {})
    options["test-args"] = ["--socket=x11", "--share=network", *options.get("test-args", [])]
    options.setdefault("env", {}).update({
        "DISPLAY": environment["DISPLAY"],
        "DBUS_SESSION_BUS_ADDRESS": environment["DBUS_SESSION_BUS_ADDRESS"],
    })
    application["run-tests"] = True
    return manifest


if __name__ == "__main__":
    original, destination = map(Path, sys.argv[1:])
    destination.write_text(json.dumps(prepare(json.loads(original.read_text()), os.environ), indent=2) + "\n")
