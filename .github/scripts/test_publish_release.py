import importlib.util
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


release = load("publish_release")
telegram = load("telegram_release")
updates = load("xenon_updates")
SHA = "a" * 40
REPO = "miuichina/xenondesktop"


def run(platform, number=1):
    return {"id": number, "run_number": number, "head_sha": SHA,
            "head_branch": "dev", "event": "push", "conclusion": "success",
            "head_repository": {"full_name": REPO},
            "path": f".github/workflows/{release.PLATFORMS[platform][0]}"}


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.previous = Path.cwd()
        os.chdir(self.folder.name)
        self.addCleanup(os.chdir, self.previous)
        Path("event.json").write_text(json.dumps({"workflow_run": {"head_sha": SHA}}))
        self.environment = patch.dict(os.environ, GITHUB_REPOSITORY=REPO,
                                      GITHUB_EVENT_PATH="event.json", GITHUB_ENV="env")
        self.environment.start()
        self.addCleanup(self.environment.stop)

    def prepare(self, message, runs):
        def github(path):
            if "/commits/" in path:
                return {"commit": {"message": message}}
            return {"workflow_runs": runs}
        with patch.object(release, "github", side_effect=github):
            release.prepare()

    def test_skipped_macos_is_not_required(self):
        self.prepare("Ship [skip macos]", [run("win", 10), run("linux", 20)])
        plan = json.loads(Path("release-plan.json").read_text())
        self.assertEqual(set(plan["runs"]), {"win", "linux"})
        self.assertEqual(plan["tag"], "windows-build-10")

    def test_waits_for_linux_and_rejects_other_commits(self):
        other = run("linux", 20)
        other["head_sha"] = "b" * 40
        self.prepare("Ship [skip macos]", [run("win", 10), other])
        self.assertIn("SKIP_PUBLISH=true", Path("env").read_text())
        self.assertFalse(Path("release-plan.json").exists())

    def test_failed_enabled_platform_blocks_release(self):
        failed = run("linux", 20)
        failed["conclusion"] = "failure"
        self.prepare("Ship [skip macos]", [run("win", 10), failed])
        self.assertFalse(Path("release-plan.json").exists())

    def test_skip_release_blocks_publication(self):
        self.prepare("[skip release]", [run("win"), run("linux"), run("macos")])
        self.assertIn("SKIP_PUBLISH=true", Path("env").read_text())

    def test_linux_only_release(self):
        self.prepare("[skip win] [skip macos]", [run("linux", 20)])
        self.assertEqual(json.loads(Path("release-plan.json").read_text())["tag"], "linux-build-20")

    def test_pull_request_build_cannot_be_published(self):
        candidate = run("win")
        candidate["event"] = "pull_request"
        self.assertFalse(release.valid_run(candidate, "win", SHA, REPO))

    def test_downloads_only_enabled_assets_and_checksums_all_of_them(self):
        self.prepare("[skip macos]", [run("win", 10), run("linux", 20)])
        metadata = {"format": 1, "commit": SHA, "tag": "windows-build-10",
                    "build_number": 10, "base_version": 7003001,
                    "package": "td-update-win-x64-7003001", "archive": "Xenon-Windows-x64.zip"}
        def download(command, **kwargs):
            output = Path("release")
            if "Xenon-Windows-x64-Release" in command:
                (output / "xenon-update.json").write_text(json.dumps(metadata))
                names = ["Telegram.exe", "Updater.exe", metadata["package"]]
                with zipfile.ZipFile(output / metadata["archive"], "w") as archive:
                    for name in names:
                        (output / name).write_bytes(b"test")
                        archive.writestr(name, b"test")
            else:
                (output / "Xenon-Linux-x64.zip").write_bytes(b"linux")
        with patch.object(release.subprocess, "run", side_effect=download):
            release.download()
        names = {path.name for path in Path("release").iterdir()}
        self.assertTrue({"Telegram.exe", "Updater.exe", "Xenon-Windows-x64.zip", "Xenon-Linux-x64.zip"} <= names)
        sums = Path("release/SHA256SUMS.txt").read_text()
        for name in names - {"SHA256SUMS.txt"}:
            self.assertIn(name, sums)
        self.assertNotIn("macOS", sums)

    def test_buttons_only_include_present_platforms(self):
        assets = {name: {"browser_download_url": "https://example.com/" + name}
                  for name in ("Xenon-Windows-x64.zip", "Xenon-Linux-x64.zip")}
        self.assertEqual([row[0]["text"] for row in telegram.download_buttons(assets)],
                         ["Download windows zip", "Download linux zip"])

    def test_message_escapes_commits_and_fits_telegram_limit(self):
        messages = telegram.build_messages("abcd123", ["abc: <fix> & update"] * 500)
        self.assertGreater(len(messages), 1)
        self.assertTrue(all(len(message.encode("utf-16-le")) // 2 <= 4096 for message in messages))
        self.assertIn("&lt;fix&gt; &amp; update", messages[0])

    def test_telegram_request_contains_real_inline_buttons(self):
        buttons = [[{"text": "Download windows zip", "url": "https://example.com/windows.zip"}]]
        with patch.object(telegram, "urlopen", return_value=io.BytesIO(b'{"ok":true}')) as request:
            self.assertTrue(telegram.send_message("test-token", "test-chat", "Commit list", buttons))
        payload = json.loads(request.call_args.args[0].data)
        self.assertEqual(payload["reply_markup"], {"inline_keyboard": buttons})
        self.assertEqual(payload["text"], "Commit list")


if __name__ == "__main__":
    unittest.main()
