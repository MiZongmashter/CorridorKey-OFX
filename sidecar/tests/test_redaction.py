import json
import unittest

from sidecar.corridorkey_sidecar.redaction import redact_text, redact_value
from sidecar.corridorkey_sidecar.server import make_log_line


class RedactionTests(unittest.TestCase):
    def test_redacts_unix_media_paths(self):
        raw = "/Users/alice/Projects/SecretShow/shot001/plate_v001.exr"

        redacted = redact_text(f"failed reading {raw}")

        self.assertIn("<redacted-path>", redacted)
        self.assertNotIn("alice", redacted)
        self.assertNotIn("SecretShow", redacted)
        self.assertNotIn("plate_v001", redacted)

    def test_redacts_windows_media_paths(self):
        raw = r"C:\Users\alice\Projects\SecretShow\shot001\plate_v001.mov"

        redacted = redact_text(f"failed reading {raw}")

        self.assertIn("<redacted-path>", redacted)
        self.assertNotIn("alice", redacted)
        self.assertNotIn("SecretShow", redacted)
        self.assertNotIn("plate_v001", redacted)

    def test_redacts_unc_paths_and_file_uri_folders(self):
        samples = [
            r"\\server\share",
            r"\\server\share\SecretShow\shot010\plate.exr",
            r"\\server\share\SecretShow\cache",
            "file:///Users/alice/Projects/SecretShow/cache",
        ]

        for raw in samples:
            with self.subTest(raw=raw):
                redacted = redact_text(f"failed reading {raw}")
                self.assertIn("<redacted-path>", redacted)
                self.assertNotIn("SecretShow", redacted)
                self.assertNotIn("shot010", redacted)
                self.assertNotIn("server", redacted)

    def test_redacts_paths_with_spaces(self):
        samples = [
            "/Users/Alice Smith/Projects/Secret Show/cache/file.json",
            r"C:\Users\Alice Smith\Projects\Secret Show\cache\file.json",
            "Secret Show/cache/file.json",
        ]

        for raw in samples:
            with self.subTest(raw=raw):
                redacted = redact_text(f"failed reading {raw}")
                self.assertIn("<redacted-path>", redacted)
                self.assertNotIn("Alice", redacted)
                self.assertNotIn("Secret", redacted)
                self.assertNotIn("file.json", redacted)

    def test_redacts_sensitive_structured_fields(self):
        value = {
            "project_name": "SecretShow",
            "media_path": "/Volumes/jobs/SecretShow/plate.exr",
            "safe": "backend stub",
            "nested": ["ok", "/tmp/SecretShow/cache/file.npy"],
        }

        redacted = redact_value(value)

        self.assertEqual(redacted["project_name"], "<redacted>")
        self.assertEqual(redacted["media_path"], "<redacted-path>")
        self.assertEqual(redacted["safe"], "backend stub")
        self.assertEqual(redacted["nested"], ["ok", "<redacted-path>"])

    def test_redacts_common_structured_key_variants(self):
        value = {
            "projectName": "SecretShow",
            "show_name": "SecretShow",
            "shot_name": "shot001",
            "project_names": ["SecretShow"],
            "file_path": "/Users/alice/Projects/SecretShow/plate.exr",
            "output_path": "/Users/alice/Projects/SecretShow/render.mov",
            "cache_dir": "/Users/alice/Projects/SecretShow/cache",
        }

        redacted = redact_value(value)

        self.assertEqual(redacted["projectName"], "<redacted>")
        self.assertEqual(redacted["show_name"], "<redacted>")
        self.assertEqual(redacted["shot_name"], "<redacted>")
        self.assertEqual(redacted["project_names"], ["<redacted>"])
        self.assertEqual(redacted["file_path"], "<redacted-path>")
        self.assertEqual(redacted["output_path"], "<redacted-path>")
        self.assertEqual(redacted["cache_dir"], "<redacted-path>")

    def test_redacts_nested_values_under_sensitive_keys(self):
        value = {
            "project": {"id": "SecretShow"},
            "showName": {"id": "SecretShow"},
            "project_names": [{"id": "SecretShow"}],
            "file_path": {"id": "/Users/alice/Projects/SecretShow/plate.exr"},
        }

        redacted = redact_value(value)

        self.assertEqual(redacted["project"], "<redacted>")
        self.assertEqual(redacted["showName"], "<redacted>")
        self.assertEqual(redacted["project_names"], ["<redacted>"])
        self.assertEqual(redacted["file_path"], "<redacted-path>")

    def test_structured_log_line_is_json_and_redacted(self):
        line = make_log_line(
            "error",
            "protocol_error",
            request_id="req-1",
            message="bad path /Users/alice/Projects/SecretShow/shot001.exr",
            project_name="SecretShow",
        )

        parsed = json.loads(line)

        self.assertEqual(parsed["level"], "error")
        self.assertEqual(parsed["event"], "protocol_error")
        self.assertEqual(parsed["request_id"], "req-1")
        self.assertNotIn("alice", line)
        self.assertNotIn("SecretShow", line)
        self.assertNotIn("shot001", line)

    def test_redaction_does_not_rewrite_urls(self):
        samples = [
            "doctor endpoint https://example.com/api/v1/status",
            "reference https://example.com/api/v1/plate.exr",
        ]

        for text in samples:
            with self.subTest(text=text):
                self.assertEqual(redact_text(text), text)

    def test_redacts_bare_media_filename_on_line_with_url(self):
        redacted = redact_text("see https://tracker.local/case plate_v001.exr")

        self.assertIn("https://tracker.local/case", redacted)
        self.assertIn("<redacted-file>", redacted)
        self.assertNotIn("plate_v001", redacted)

    def test_redacts_labeled_free_text_project_names(self):
        samples = [
            "project SecretShow failed",
            "project: SecretShow failed",
            "project=SecretShow failed",
            "project: Alpha Beta failed",
            "show SecretShow failed",
            "show=Alpha Beta retry",
            "show=SecretShow failed",
            "shot shot001 failed",
            "shot: shot001 failed",
        ]

        for text in samples:
            with self.subTest(text=text):
                redacted = redact_text(text)
                self.assertIn("<redacted>", redacted)
                self.assertNotIn("SecretShow", redacted)
                self.assertNotIn("shot001", redacted)

    def test_redacts_bare_project_like_tokens(self):
        redacted = redact_text("SecretShow failed on shot001 and Alpha_Beta")

        self.assertNotIn("SecretShow", redacted)
        self.assertNotIn("shot001", redacted)
        self.assertNotIn("Alpha_Beta", redacted)
        self.assertEqual(redact_text("CorridorKey OpenFX doctor"), "CorridorKey OpenFX doctor")


if __name__ == "__main__":
    unittest.main()
