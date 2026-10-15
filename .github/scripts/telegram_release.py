"""Announce a published Windows release without blocking its publication."""

import html
import json
import os
import subprocess
from urllib.error import HTTPError, URLError
from urllib.parse import quote
from urllib.request import Request, urlopen


def git(*args):
    return subprocess.check_output(
        ["git", *args], text=True, encoding="utf-8",
    ).strip()


def github(path):
    return json.loads(subprocess.check_output(
        ["gh", "api", "--paginate", "--slurp", path],
        text=True, encoding="utf-8",
    ))


def previous_release_commit(releases, current, tag):
    for release in sorted(
        releases, key=lambda item: item.get("published_at") or "", reverse=True,
    ):
        if (release["tag_name"] == tag or release["draft"] or release["prerelease"]
                or not release["tag_name"].startswith("windows-build-")
                or not any(asset["name"] == "Telegram.exe" for asset in release["assets"])):
            continue
        commit = git("rev-parse", "--verify", f"refs/tags/{release['tag_name']}^{{commit}}")
        ancestor = subprocess.run(["git", "merge-base", "--is-ancestor", commit, current])
        if ancestor.returncode == 0:
            return commit
        if ancestor.returncode != 1:
            raise RuntimeError("Cannot check previous release ancestry")
    return ""


def release_commits(previous, current):
    revision = [f"{previous}..{current}"] if previous else ["-1", current]
    return git("log", "--topo-order", "--abbrev=7", "--format=%h: %s", *revision, "--").splitlines()


def build_messages(short_sha, commits, asset):
    url = asset["browser_download_url"]
    if not url.startswith("https://"):
        raise ValueError("EXE download URL must use HTTPS")
    lines = [f"<b>Xenon desktop ({html.escape(short_sha)})</b>"]
    for commit in commits:
        lines.extend(html.escape(commit[i:i + 500]) for i in range(0, len(commit), 500))
    lines.extend([
        "",
        f'<a href="{html.escape(url, quote=True)}">Download exe</a>'
        f" ({asset['size'] / (1024 * 1024):.1f} МБ)",
    ])
    messages = []
    message = ""
    for line in lines:
        if len(line.encode("utf-16-le")) // 2 > 4096:
            raise ValueError("Telegram message line is too long")
        candidate = f"{message}\n{line}" if message else line
        if len(candidate.encode("utf-16-le")) // 2 > 4096:
            messages.append(message)
            message = line
        else:
            message = candidate
    if message:
        messages.append(message)
    return messages


def send_message(token, chat_id, message):
    request = Request(
        f"https://api.telegram.org/bot{token}/sendMessage",
        data=json.dumps({
            "chat_id": chat_id,
            "text": message,
            "parse_mode": "HTML",
            "link_preview_options": {"is_disabled": True},
        }).encode("utf-8"),
        headers={"Content-Type": "application/json"},
    )
    try:
        with urlopen(request, timeout=30) as response:
            result = json.load(response)
    except HTTPError as error:
        print(f"Telegram announcement skipped: HTTP {error.code}.")
        return False
    except (URLError, TimeoutError, OSError, ValueError):
        print("Telegram announcement stopped: delivery could not be confirmed.")
        return False
    if result.get("ok") is not True:
        print("Telegram announcement skipped: API rejected the message.")
        return False
    return True


def main():
    token = os.environ.get("TG_TOKEN", "").strip()
    chat_id = os.environ.get("TG_CHAT_ID", "").strip()
    if not token or not chat_id:
        print("Telegram announcement skipped: secrets are not configured.")
        return
    repo = os.environ["GITHUB_REPOSITORY"]
    tag = os.environ["RELEASE_TAG"]
    current = git("rev-parse", "--verify", f"{os.environ['BUILD_SHA']}^{{commit}}")
    release = github(f"repos/{repo}/releases/tags/{quote(tag, safe='')}")[0]
    if release["draft"] or release["prerelease"]:
        raise ValueError("Release is not published")
    asset = next(asset for asset in release["assets"] if asset["name"] == "Telegram.exe")
    pages = github(f"repos/{repo}/releases?per_page=100")
    previous = previous_release_commit([item for page in pages for item in page], current, tag)
    messages = build_messages(git("rev-parse", "--short=7", current), release_commits(previous, current), asset)
    for message in messages:
        if not send_message(token, chat_id, message):
            return
    print("Telegram announcement sent.")


if __name__ == "__main__":
    try:
        main()
    except (KeyError, ValueError, RuntimeError, StopIteration, subprocess.SubprocessError):
        print("Telegram announcement skipped: release information could not be prepared.")
