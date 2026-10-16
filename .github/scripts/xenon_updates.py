#!/usr/bin/env python3
"""Prepare Xenon trust files and release metadata without private keys in Git."""

import argparse
import base64
import datetime
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def run(*args, **kwargs):
    return subprocess.run(args, check=True, capture_output=True, **kwargs).stdout


def init_keys(public_dir, private_dir):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    if public_dir.exists() or private_dir.exists():
        raise ValueError("Use new directories; existing update keys must never be overwritten")
    private_dir.mkdir(parents=True, mode=0o755 if os.name == "nt" else 0o700)
    public_dir.mkdir(parents=True)
    key_id = "xenon-release-1"
    keys = {}
    for name in ("root", "release"):
        private = private_dir / f"{name}-private.pem"
        keys[name] = Ed25519PrivateKey.generate()
        private.write_bytes(keys[name].private_bytes(
            serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption()))
        private.chmod(0o600)
    (public_dir / "root-public.pem").write_bytes(keys["root"].public_key().public_bytes(
        serialization.Encoding.PEM, serialization.PublicFormat.SubjectPublicKeyInfo))
    raw = keys["release"].public_key().public_bytes(
        serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    now = int(time.time())
    manifest = {
        "format": 1,
        "manifest_version": 1,
        "issued": now,
        "expires": now + 10 * 365 * 86400,
        "keys": [{"id": key_id, "alg": "Ed25519",
                  "x": base64.urlsafe_b64encode(raw).decode().rstrip("=")}],
        "channels": {name: [[key_id]] for name in
                     ("stable", "beta", "canary-public", "canary-private")},
        "revoked": [],
    }
    manifest_file = public_dir / "manifest.min.json"
    manifest_file.write_bytes(json.dumps(manifest, separators=(",", ":")).encode())
    signature = keys["root"].sign(manifest_file.read_bytes())
    keys["root"].public_key().verify(signature, manifest_file.read_bytes())
    (public_dir / "manifest.sig").write_bytes(signature)


def build_metadata(source, output, build_number, run_id):
    if not 0 < build_number <= 0xFFFFFFFF or run_id <= 0:
        raise ValueError("Invalid release build identity")
    commit = run("git", "-C", str(source), "rev-parse", "HEAD").decode().strip()
    commit_time = int(run("git", "-C", str(source), "show", "-s", "--format=%ct", "HEAD"))
    version_text = (source / "Telegram/build/version").read_text()
    version = int(re.search(r"^AppVersion\s+(\d+)$", version_text, re.M)[1])
    output.write_text(json.dumps({
        "format": 1,
        "tag": f"windows-build-{run_id}",
        "commit": commit,
        "commit_time": commit_time,
        "base_version": version,
        "build_number": build_number,
        "package": f"td-update-win-x64-{version}",
    }, indent=2) + "\n", encoding="utf-8")


def package(source, output, build_number, run_id, private_key):
    if not private_key:
        raise ValueError("Missing XENON_UPDATE_SIGNING_KEY repository secret")
    output.mkdir(parents=True, exist_ok=True)
    metadata_file = output / "xenon-update.json"
    build_metadata(source, metadata_file, build_number, run_id)
    metadata = json.loads(metadata_file.read_text())
    public = source / "Telegram/Resources/xenon-update"
    key_id = json.loads((public / "manifest.min.json").read_text())["keys"][0]["id"]
    signing = output / "signing-key.pem"
    with signing.open("x", encoding="utf-8", newline="\n") as handle:
        handle.write(private_key)
    signing.chmod(0o600)
    try:
        binary_dir = source / "out/Release"
        for filename in ("Telegram.exe", "Updater.exe"):
            shutil.copy2(binary_dir / filename, output / filename)
        run(str(binary_dir / "Packer.exe"), "-path", "Telegram.exe",
            "-path", "Updater.exe", "-target", "win64",
            "-version", str(metadata["base_version"]), "-channel", "stable",
            "-counter", str(build_number), "-keys-loc", str(public.resolve()),
            "-local-key", str(signing.resolve()), "-local-key-id", key_id,
            cwd=output)
    finally:
        signing.unlink(missing_ok=True)
    if not (output / metadata["package"]).is_file():
        raise ValueError("Packer did not produce the expected signed package")


def release_notes(source, metadata_file, previous, output):
    metadata = json.loads(metadata_file.read_text())
    current = metadata["commit"]
    if previous:
        if not re.fullmatch(r"[0-9a-f]{40}", previous):
            raise ValueError("Invalid previous release commit")
        run("git", "-C", str(source), "merge-base", "--is-ancestor", previous, current)
    history = run("git", "-C", str(source), "log", "--reverse", "--format=- %h %s",
                  *([f"{previous}..{current}"] if previous else ["-1", current])).decode("utf-8")
    if len(history.encode("utf-8")) > 100000:
        history = history.encode("utf-8")[:100000].decode("utf-8", errors="ignore")
        history += f"\n\n[All commits](https://github.com/miuichina/xenondesktop/compare/{previous}...{current})\n"
    date = datetime.datetime.fromtimestamp(metadata["commit_time"], datetime.timezone.utc).isoformat()
    notes = (f"Windows x64 · build {metadata['build_number']}\n\n"
             f"Commit: `{current}`\n\nCommit time: {date}\n\n"
             f"<!-- xenon-update: {json.dumps(metadata, separators=(',', ':'))} -->\n\n"
             f"## Changes\n\n{history}")
    output.write_text(notes, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    commands = parser.add_subparsers(dest="command", required=True)
    keys = commands.add_parser("init-keys")
    keys.add_argument("--public-dir", required=True, type=Path)
    keys.add_argument("--private-dir", required=True, type=Path)
    pack = commands.add_parser("package")
    pack.add_argument("--source", required=True, type=Path)
    pack.add_argument("--output", required=True, type=Path)
    pack.add_argument("--build-number", required=True, type=int)
    pack.add_argument("--run-id", required=True, type=int)
    notes = commands.add_parser("release-notes")
    notes.add_argument("--source", required=True, type=Path)
    notes.add_argument("--metadata", required=True, type=Path)
    notes.add_argument("--previous", default="")
    notes.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.command == "init-keys":
        init_keys(args.public_dir, args.private_dir)
    elif args.command == "package":
        package(args.source.resolve(), args.output.resolve(), args.build_number,
                args.run_id, os.environ.get("XENON_UPDATE_SIGNING_KEY", ""))
    else:
        release_notes(args.source, args.metadata, args.previous, args.output)


if __name__ == "__main__":
    main()
