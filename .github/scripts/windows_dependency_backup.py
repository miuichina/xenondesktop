"""Keep a second copy of Windows dependencies outside Actions Cache."""

from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ARCHIVE = "windows-dependencies.tar.gz"
DIRECTORIES = ("Libraries", "ThirdParty")


def api(path, paginate=False):
    args = ["gh", "api"]
    if paginate:
        args.extend(("--paginate", "--slurp"))
    return json.loads(subprocess.check_output([*args, path]))


def find():
    repo = os.environ["GITHUB_REPOSITORY"]
    repo_id = int(os.environ["GITHUB_REPOSITORY_ID"])
    prefix = os.environ["DEPENDENCY_PREFIX"] + "-"
    compatible_name = re.compile(re.escape(prefix) + r"[0-9a-f]{64}")
    key = os.environ["DEPENDENCY_KEY"]
    now = datetime.now(timezone.utc)
    pages = api(f"repos/{repo}/actions/artifacts?per_page=100", paginate=True)
    candidates = []
    for page in pages:
        for artifact in page["artifacts"]:
            run = artifact.get("workflow_run") or {}
            if (not artifact["expired"] and compatible_name.fullmatch(artifact["name"])
                    and run.get("head_branch") == "dev"
                    and run.get("head_repository_id") == repo_id
                    and str(run["id"]) != os.environ["GITHUB_RUN_ID"]):
                candidates.append(artifact)

    candidates.sort(
        key=lambda artifact: (artifact["name"] == key, artifact["created_at"],
                              artifact["expires_at"]),
        reverse=True,
    )
    for artifact in candidates:
        run_id = artifact["workflow_run"]["id"]
        run = api(f"repos/{repo}/actions/runs/{run_id}")
        if (run["path"] != ".github/workflows/win.yml"
                or run["event"] != "push"
                or run["status"] != "completed"):
            continue
        name = artifact["name"]
        expires = datetime.fromisoformat(artifact["expires_at"].replace("Z", "+00:00"))
        fresh = name == key and expires - now > timedelta(days=14)
        with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as output:
            print(f"run-id={run_id}", f"name={name}",
                  f"fresh={str(fresh).lower()}", sep="\n", file=output)
        print(f"Found Windows dependency backup in run {run_id}: {name}")
        return
    print("No compatible Windows dependency backup")


def pack():
    root = Path(os.environ["BUILD_ROOT"])
    archive = Path(os.environ["RUNNER_TEMP"]) / ARCHIVE
    subprocess.run(["tar.exe", "-czf", str(archive), "-C", str(root),
                    *DIRECTORIES], check=True)


def restore():
    root = Path(os.environ["BUILD_ROOT"]).resolve(strict=True)
    run_id = os.environ["BACKUP_RUN_ID"]
    name = os.environ["BACKUP_NAME"]
    for directory in DIRECTORIES:
        target = root / directory
        if target.exists() and any(target.iterdir()):
            raise ValueError(f"Expected an empty dependency directory: {target}")

    with tempfile.TemporaryDirectory(prefix="dependency-backup-", dir=root) as temp:
        staging = Path(temp)
        subprocess.run(["gh", "run", "download", run_id, "--repo",
                        os.environ["GITHUB_REPOSITORY"], "--name", name,
                        "--dir", temp], check=True)
        subprocess.run(["tar.exe", "-xzf", str(staging / ARCHIVE),
                        "-C", temp], check=True)
        if not all((staging / directory).is_dir() for directory in DIRECTORIES):
            raise ValueError("Windows dependency backup is missing a directory")
        moved = []
        try:
            for directory in DIRECTORIES:
                target = root / directory
                if target.exists():
                    target.rmdir()
                (staging / directory).rename(target)
                moved.append(directory)
        except OSError:
            for directory in reversed(moved):
                (root / directory).rename(staging / directory)
            for directory in DIRECTORIES:
                (root / directory).mkdir(exist_ok=True)
            raise
    print(f"Restored Windows dependencies from artifact {name} in run {run_id}")


if __name__ == "__main__":
    {"find": find, "pack": pack, "restore": restore}[sys.argv[1]]()
