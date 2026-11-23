#!/usr/bin/env python3
"""Preserve the mtimes of unchanged inputs when restoring a Ninja build."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def git(*args):
    return subprocess.check_output(["git", *args])


def tracked_files():
    return [Path(os.fsdecode(name)) for name in git("ls-files", "--recurse-submodules", "-z").split(b"\0") if name]


def dependency_files(manifest):
    if not (manifest.parent / ".ninja_deps").is_file():
        return []
    libraries = Path("../Libraries").resolve()
    output = subprocess.check_output(["ninja", "-C", os.fspath(manifest.parent), "-t", "deps"])
    paths = set()
    for line in os.fsdecode(output).splitlines():
        if not line.startswith("    "):
            continue
        path = Path(line.strip())
        if not path.is_absolute():
            path = manifest.parent / path
        path = path.resolve()
        if path.is_relative_to(libraries) and path.is_file():
            paths.add(Path(os.path.relpath(path)))
    return sorted(paths)


def restore(manifest):
    if not manifest.is_file():
        print("No source timestamp manifest in the restored build cache.")
        return

    data = json.loads(manifest.read_text(encoding="utf-8"))
    hashes = data.get("hashes")
    changed = []
    if hashes is None:
        previous_sha = data["commit"]
        if subprocess.run(["git", "cat-file", "-e", f"{previous_sha}^{{commit}}"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode:
            subprocess.run(["git", "fetch", "--no-tags", "--depth=1", "origin", previous_sha], check=True)
        changed = [os.fsdecode(name) for name in git("diff", "--no-ext-diff", "--name-only", "-z", previous_sha, "HEAD").split(b"\0") if name]
    restored = 0
    dependencies = [Path(name) for name in data.get("dependencies", [])]
    libraries = Path("../Libraries").resolve()
    dependencies = [path for path in dependencies if path.resolve().is_relative_to(libraries)]
    for path in tracked_files() + dependencies:
        name = os.fspath(path)
        mtime = data["files"].get(name)
        if mtime is None or not path.is_file() or path.is_symlink():
            continue
        if hashes is not None:
            digest = hashes.get(name)
            if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                continue
        elif any(name == item or name.startswith(item + "/") for item in changed):
            continue
        stat = path.stat()
        os.utime(path, ns=(stat.st_atime_ns, mtime))
        restored += 1
    print(f"Restored timestamps for {restored} unchanged source files.")


def save(manifest, source_only=False):
    files = {}
    hashes = {}
    dependencies = [] if source_only else dependency_files(manifest)
    for path in tracked_files() + dependencies:
        if path.is_file() and not path.is_symlink():
            files[os.fspath(path)] = path.stat().st_mtime_ns
            hashes[os.fspath(path)] = hashlib.sha256(path.read_bytes()).hexdigest()
    manifest.parent.mkdir(parents=True, exist_ok=True)
    temporary = manifest.with_suffix(".tmp")
    temporary.write_text(json.dumps({"commit": git("rev-parse", "HEAD").decode().strip(), "files": files, "hashes": hashes, "dependencies": [os.fspath(path) for path in dependencies]}), encoding="utf-8")
    temporary.replace(manifest)
    print(f"Saved timestamps for {len(files)} source files.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("restore", "save"))
    parser.add_argument("path", type=Path)
    parser.add_argument("--source-only", action="store_true")
    args = parser.parse_args()
    if args.command == "save":
        save(args.path, source_only=args.source_only)
    else:
        restore(args.path)
