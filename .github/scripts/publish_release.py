"""Publish the successful, non-skipped platform builds of one commit."""

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import zipfile

PLATFORMS = {
    "win": ("win.yml", "Xenon-Windows-x64-Release", "Xenon-Windows-x64.zip"),
    "linux": ("linux.yml", "Xenon-Linux-x64-Release", "Xenon-Linux-x64.zip"),
    "macos": ("macos.yml", "Xenon-macOS-arm64-Release", "Xenon-macOS-arm64.zip"),
}
LINUX_UPDATE_METADATA = "xenon-update-linux-x64.json"


def github(path):
    return json.loads(subprocess.check_output(["gh", "api", path]))


def environment(**values):
    with open(os.environ["GITHUB_ENV"], "a", encoding="utf-8") as stream:
        for key, value in values.items():
            stream.write(f"{key}={value}\n")


def expected_platforms(message):
    if "[skip release]" in message:
        return []
    return [name for name in PLATFORMS if f"[skip {name}]" not in message]


def valid_run(run, platform, sha, repo):
    return (run.get("conclusion") == "success"
            and run.get("head_sha") == sha
            and run.get("head_branch") == "dev"
            and run.get("head_repository", {}).get("full_name") == repo
            and run.get("path") == f".github/workflows/{PLATFORMS[platform][0]}"
            and run.get("event") == "push")


def prepare():
    repo = os.environ["GITHUB_REPOSITORY"]
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    inputs = event.get("inputs") or {}
    supplied = {name: inputs.get(key, "") for name, key in
                (("win", "run_id"), ("linux", "linux_run_id"), ("macos", "macos_run_id"))}
    sha = event.get("workflow_run", {}).get("head_sha")
    if not sha:
        anchor = next((value for value in supplied.values() if value), "")
        if not re.fullmatch(r"[0-9]+", anchor):
            raise ValueError("Supply at least one successful platform run ID")
        sha = github(f"repos/{repo}/actions/runs/{anchor}")["head_sha"]
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise ValueError("Invalid source commit")
    commit = github(f"repos/{repo}/commits/{sha}")
    platforms = expected_platforms(commit["commit"]["message"])
    runs = github(f"repos/{repo}/actions/runs?head_sha={sha}&per_page=100")["workflow_runs"]
    selected = {}
    for name in platforms:
        if supplied[name]:
            if not re.fullmatch(r"[0-9]+", supplied[name]):
                raise ValueError("Invalid platform run ID")
            candidates = [github(f"repos/{repo}/actions/runs/{supplied[name]}")]
        else:
            candidates = runs
        matches = [run for run in candidates if valid_run(run, name, sha, repo)]
        if matches:
            selected[name] = max(matches, key=lambda run: run["id"])
    if not platforms or len(selected) != len(platforms):
        print("Publication skipped: disabled by commit or waiting for all enabled platforms.")
        environment(SKIP_PUBLISH="true")
        return
    anchor = selected.get("win") or next(iter(selected.values()))
    tag = f"{'windows' if 'win' in selected else platforms[0]}-build-{anchor['id']}"
    plan = {"repo": repo, "sha": sha, "tag": tag, "runs": selected,
            "title": commit["commit"]["message"].splitlines()[0]}
    Path("release-plan.json").write_text(json.dumps(plan), encoding="utf-8")
    environment(BUILD_SHA=sha, RELEASE_TAG=tag, RUN_ID=anchor["id"])


def validate_windows(metadata, run, plan):
    return (metadata.get("format") == 1 and metadata.get("commit") == plan["sha"]
            and metadata.get("tag") == plan["tag"]
            and metadata.get("build_number") == run["run_number"]
            and isinstance(metadata.get("base_version"), int)
            and metadata["base_version"] > 0
            and metadata.get("package") == f"td-update-win-x64-{metadata['base_version']}"
            and metadata.get("archive") == "Xenon-Windows-x64.zip")


def prepare_linux_metadata(output, run, plan):
    path = output / LINUX_UPDATE_METADATA
    metadata = json.loads(path.read_text(encoding="utf-8"))
    base = metadata.get("base_version")
    if (metadata.get("format") != 1 or metadata.get("commit") != plan["sha"]
            or metadata.get("tag") != f"linux-build-{run['id']}"
            or metadata.get("build_number") != run["run_number"]
            or type(base) is not int or not 0 < base <= 999999999
            or metadata.get("package") != f"td-update-linux-x64-{base}"):
        raise ValueError("Linux metadata does not match its successful build")
    package = output / metadata["package"]
    if not package.is_file() or not 0 < package.stat().st_size <= 256 * 1024 * 1024:
        raise ValueError("Missing or invalid Linux update package")
    metadata["tag"] = plan["tag"]
    path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return metadata


def download():
    plan = json.loads(Path("release-plan.json").read_text())
    output = Path("release")
    output.mkdir()
    for platform, run in plan["runs"].items():
        subprocess.run(["gh", "run", "download", str(run["id"]), "--repo", plan["repo"],
                        "--name", PLATFORMS[platform][1], "--dir", str(output)], check=True)
        archive = output / PLATFORMS[platform][2]
        if not archive.is_file() or not archive.stat().st_size:
            raise ValueError(f"Missing {platform} archive")
    linux = (prepare_linux_metadata(output, plan["runs"]["linux"], plan)
             if "linux" in plan["runs"] else None)
    if "win" in plan["runs"]:
        metadata = json.loads((output / "xenon-update.json").read_text())
        if not validate_windows(metadata, plan["runs"]["win"], plan):
            raise ValueError("Windows metadata does not match its successful build")
        for name in ("Telegram.exe", "Updater.exe", metadata["package"]):
            if not (output / name).is_file() or not (output / name).stat().st_size:
                raise ValueError(f"Missing Windows asset: {name}")
        with zipfile.ZipFile(output / PLATFORMS["win"][2]) as archive:
            if set(archive.namelist()) != {"Telegram.exe", "Updater.exe", metadata["package"]}:
                raise ValueError("Unexpected Windows ZIP contents")
    elif linux:
        metadata = linux.copy()
    else:
        timestamp = subprocess.check_output(["git", "show", "-s", "--format=%ct", plan["sha"]])
        metadata = {"commit": plan["sha"], "commit_time": int(timestamp),
                    "build_number": next(iter(plan["runs"].values()))["run_number"]}
    metadata["platforms"] = list(plan["runs"])
    Path("release-metadata.json").write_text(json.dumps(metadata), encoding="utf-8")
    with (output / "SHA256SUMS.txt").open("w", encoding="utf-8", newline="\n") as stream:
        for path in sorted(output.iterdir()):
            if path.name != "SHA256SUMS.txt":
                digest = hashlib.sha256()
                with path.open("rb") as source:
                    for block in iter(lambda: source.read(1024 * 1024), b""):
                        digest.update(block)
                stream.write(f"{digest.hexdigest()}  {path.name}\n")


def publish():
    plan = json.loads(Path("release-plan.json").read_text())
    releases = github(f"repos/{plan['repo']}/releases?per_page=100")
    if any(release["tag_name"] == plan["tag"] for release in releases):
        print("This build already has a release.")
        return
    previous = ""
    for release in sorted(releases, key=lambda release: release.get("published_at") or "", reverse=True):
        if release["draft"] or release["prerelease"]:
            continue
        ref = f"refs/tags/{release['tag_name']}^{{commit}}"
        resolved = subprocess.run(["git", "rev-parse", "--verify", ref], capture_output=True, text=True)
        if resolved.returncode:
            continue
        candidate = resolved.stdout.strip()
        if candidate != plan["sha"] and subprocess.run(
                ["git", "merge-base", "--is-ancestor", plan["sha"], candidate]).returncode == 0:
            print("A newer commit has already been released.")
            return
        if subprocess.run(["git", "merge-base", "--is-ancestor", candidate, plan["sha"]]).returncode == 0:
            previous = candidate
            break
    subprocess.run([sys.executable, ".github/scripts/xenon_updates.py", "release-notes",
                    "--source", ".", "--metadata", "release-metadata.json", "--previous", previous,
                    "--output", "notes.md"], check=True)
    with Path("notes.md").open("a", encoding="utf-8") as stream:
        for platform, run in plan["runs"].items():
            stream.write(f"\n\n[{platform} build and logs](https://github.com/{plan['repo']}/actions/runs/{run['id']})")
    assets = [str(path) for path in sorted(Path("release").iterdir()) if path.is_file()]
    subprocess.run(["gh", "release", "create", plan["tag"], *assets, "--repo", plan["repo"],
                    "--target", plan["sha"], "--title", plan["title"], "--notes-file", "notes.md",
                    "--latest"], check=True)
    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as stream:
        stream.write(f"published=true\nbuild_sha={plan['sha']}\ntag={plan['tag']}\n")


if __name__ == "__main__":
    {"prepare": prepare, "download": download, "publish": publish}[sys.argv[1]]()
