#!/usr/bin/env python3
"""Keep source mtimes consistent with cached Clang precompiled headers."""

import json
import os
from pathlib import Path
import subprocess
import sys


def git(*args):
    return subprocess.check_output(["git", *args])


def tracked_files():
    return [Path(os.fsdecode(name)) for name in git("ls-files", "--recurse-submodules", "-z").split(b"\0") if name]


def restore(manifest):
    if not manifest.is_file():
        print("No source timestamp manifest in the restored build cache.")
        return

    data = json.loads(manifest.read_text(encoding="utf-8"))
    previous_sha = data["commit"]
    if subprocess.run(["git", "cat-file", "-e", f"{previous_sha}^{{commit}}"],
                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
        subprocess.run(["git", "fetch", "--no-tags", "--depth=1", "origin", previous_sha], check=True)

    changed = [os.fsdecode(name) for name in git("diff", "--no-ext-diff", "--name-only", "-z", previous_sha, "HEAD").split(b"\0") if name]
    restored = 0
    for path in tracked_files():
        name = os.fspath(path)
        if any(name == item or name.startswith(item + "/") for item in changed):
            continue
        mtime = data["files"].get(name)
        if mtime is None or not path.is_file() or path.is_symlink():
            continue
        stat = path.stat()
        os.utime(path, ns=(stat.st_atime_ns, mtime))
        restored += 1
    print(f"Restored timestamps for {restored} unchanged source files.")


def save(manifest):
    files = {}
    for path in tracked_files():
        if path.is_file() and not path.is_symlink():
            files[os.fspath(path)] = path.stat().st_mtime_ns
    manifest.parent.mkdir(parents=True, exist_ok=True)
    temporary = manifest.with_suffix(".tmp")
    temporary.write_text(json.dumps({"commit": git("rev-parse", "HEAD").decode().strip(), "files": files}), encoding="utf-8")
    temporary.replace(manifest)
    print(f"Saved timestamps for {len(files)} source files.")


if __name__ == "__main__":
    command, path = sys.argv[1:]
    {"restore": restore, "save": save}[command](Path(path))
