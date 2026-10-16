import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "xenon_updates", Path(__file__).with_name("xenon_updates.py"))
updates = importlib.util.module_from_spec(spec)
spec.loader.exec_module(updates)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root / "source"
        self.source.mkdir()
        self.env = dict(os.environ, GIT_AUTHOR_NAME="Updater test",
                        GIT_AUTHOR_EMAIL="test@example.invalid",
                        GIT_COMMITTER_NAME="Updater test",
                        GIT_COMMITTER_EMAIL="test@example.invalid")
        self.git("init", "-q")
        version = self.source / "Telegram/build/version"
        version.parent.mkdir(parents=True)
        version.write_text("AppVersion 7003001\n")
        self.first = self.commit("First release")
        self.middle = self.commit("Intermediate release")
        self.last = self.commit("Newest release")
        self.metadata = self.root / "xenon-update.json"
        updates.build_metadata(self.source, self.metadata, 42, 123)

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.source), *args],
                              check=True, capture_output=True, env=self.env).stdout.decode().strip()

    def commit(self, title):
        self.git("add", ".")
        self.git("commit", "-q", "--allow-empty", "-m", title)
        return self.git("rev-parse", "HEAD")

    def test_metadata_identifies_binary_source_and_same_version_rebuild(self):
        data = json.loads(self.metadata.read_text())
        self.assertEqual(data["commit"], self.last)
        self.assertEqual(data["commit_time"], int(self.git("show", "-s", "--format=%ct", "HEAD")))
        self.assertEqual(data["build_number"], 42)
        self.assertEqual(data["base_version"], 7003001)
        self.assertEqual(data["package"], "td-update-win-x64-7003001")
        self.assertEqual(data["tag"], "windows-build-123")

    def test_notes_include_every_intermediate_release(self):
        notes = self.root / "notes.md"
        updates.release_notes(self.source, self.metadata, self.first, notes)
        text = notes.read_text(encoding="utf-8")
        self.assertIn("Intermediate release", text)
        self.assertIn("Newest release", text)
        self.assertNotIn("First release", text)
        self.assertIn(self.last, text)

    def test_initial_notes_only_include_current_commit(self):
        notes = self.root / "notes.md"
        updates.release_notes(self.source, self.metadata, "", notes)
        self.assertIn("Newest release", notes.read_text())
        self.assertNotIn("First release", notes.read_text())

    def test_invalid_build_ids_rejected(self):
        for build, run_id in ((0, 123), (-1, 123), (2**32, 123), (42, 0)):
            with self.assertRaises(ValueError):
                updates.build_metadata(self.source, self.metadata, build, run_id)

    def test_diverged_history_rejected(self):
        self.git("checkout", "--orphan", "unrelated")
        other = self.commit("Unrelated history")
        with self.assertRaises(subprocess.CalledProcessError):
            updates.release_notes(self.source, self.metadata, other, self.root / "notes.md")

    def test_missing_signing_key_rejected(self):
        with self.assertRaisesRegex(ValueError, "XENON_UPDATE_SIGNING_KEY"):
            updates.package(self.source, self.root / "package", 42, 123, "")

    def test_signing_key_removed_after_packer_failure(self):
        public = self.source / "Telegram/Resources/xenon-update"
        public.mkdir(parents=True)
        (public / "manifest.min.json").write_text('{"keys":[{"id":"test"}]}')
        output = self.root / "package"
        with patch.object(updates.shutil, "copy2", side_effect=OSError("simulated failure")):
            with self.assertRaises(OSError):
                updates.package(self.source, output, 42, 123, "temporary test key")
        self.assertFalse((output / "signing-key.pem").exists())

    def test_trust_manifest_signature_and_distinct_release_key(self):
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives import serialization
        public = self.root / "public"
        private = self.root / "private"
        updates.init_keys(public, private)
        root_key = serialization.load_pem_public_key((public / "root-public.pem").read_bytes())
        signature = (public / "manifest.sig").read_bytes()
        manifest = (public / "manifest.min.json").read_bytes()
        root_key.verify(signature, manifest)
        with self.assertRaises(InvalidSignature):
            root_key.verify(signature, manifest + b" ")
        release_key = serialization.load_pem_private_key((private / "release-private.pem").read_bytes(), None)
        with self.assertRaises(InvalidSignature):
            release_key.public_key().verify(signature, manifest)
        with self.assertRaises(ValueError):
            updates.init_keys(public, private)


if __name__ == "__main__":
    unittest.main()
