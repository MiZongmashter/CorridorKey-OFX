import json
import tempfile
import unittest
from pathlib import Path

from sidecar.corridorkey_sidecar.logging_config import collect_diagnostic_status
from sidecar.corridorkey_sidecar.support_bundle import (
    SESSION21_SUPPORT_BUNDLE_EVIDENCE,
    create_support_bundle,
    evaluate_support_bundle_sufficiency,
)


class SupportBundleTests(unittest.TestCase):
    def test_bundle_contains_required_diagnostics_and_redacts_sensitive_text(self):
        raw_path = "/Users/alice/Projects/SecretShow/shot010/plate_v001.exr"
        doctor_output = f"Doctor failed while reading {raw_path}"
        logs = [
            json.dumps(
                {
                    "level": "error",
                    "event": "infer_failed",
                    "project_name": "SecretShow",
                    "message": f"could not open {raw_path}",
                    "error_code": "model_missing",
                }
            )
        ]
        diagnostics = collect_diagnostic_status(
            host_name="Resolve",
            host_version="21.0",
            backend="stub",
            model="not_loaded",
            pixel_format="RGBA float",
            frame_size="1920x1080",
            timings={"infer_ms": "12"},
            warning_codes=["rough_fallback"],
            error_codes=["model_missing"],
        )

        with tempfile.TemporaryDirectory() as tmp:
            bundle_dir = create_support_bundle(
                Path(tmp),
                logs=logs,
                doctor_output=doctor_output,
                manifest_status={
                    "model_status": "missing",
                    "project_path": raw_path,
                },
                backend_status={
                    "backend_status": "blocked",
                    "last_error": f"adapter saw {raw_path}",
                },
                recent_errors=[
                    {
                        "error_code": "model_missing",
                        "message": f"failed on {raw_path}",
                    }
                ],
                diagnostics=diagnostics,
            )

            self.assertTrue((bundle_dir / "manifest.json").is_file())
            self.assertTrue((bundle_dir / "diagnostics.json").is_file())
            self.assertTrue((bundle_dir / "doctor.txt").is_file())
            self.assertTrue((bundle_dir / "logs" / "redacted.log").is_file())
            self.assertTrue((bundle_dir / "recent_errors.json").is_file())
            self.assertTrue((bundle_dir / "redaction_proof.json").is_file())

            manifest = json.loads((bundle_dir / "manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["session21_evidence_artifact"], SESSION21_SUPPORT_BUNDLE_EVIDENCE)

            diagnostics_json = json.loads(
                (bundle_dir / "diagnostics.json").read_text(encoding="utf-8")
            )
            for key in (
                "plugin_version",
                "sidecar_version",
                "host",
                "os",
                "cpu_summary",
                "gpu_summary",
                "backend",
                "model",
                "pixel_format",
                "frame_size",
                "timings",
                "warning_codes",
                "error_codes",
            ):
                self.assertIn(key, diagnostics_json)

            bundle_text = "\n".join(
                path.read_text(encoding="utf-8")
                for path in bundle_dir.rglob("*")
                if path.is_file()
            )
            self.assertIn("<redacted-path>", bundle_text)
            self.assertNotIn(raw_path, bundle_text)
            self.assertNotIn("SecretShow", bundle_text)
            self.assertNotIn("plate_v001", bundle_text)
            self.assertNotIn("alice", bundle_text)

            sufficiency = evaluate_support_bundle_sufficiency(bundle_dir)
            self.assertTrue(sufficiency["sufficient"], sufficiency)
            self.assertEqual(
                sufficiency["session21_evidence_artifact"],
                SESSION21_SUPPORT_BUNDLE_EVIDENCE,
            )

    def test_sufficiency_fails_without_recent_error_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle_dir = create_support_bundle(
                Path(tmp),
                logs=[],
                doctor_output="Doctor completed",
                manifest_status={"model_status": "missing"},
                backend_status={"backend_status": "blocked"},
                recent_errors=[],
                diagnostics=collect_diagnostic_status(),
            )

            (bundle_dir / "recent_errors.json").unlink()
            sufficiency = evaluate_support_bundle_sufficiency(bundle_dir)

            self.assertFalse(sufficiency["sufficient"])
            self.assertFalse(sufficiency["criteria"]["recent_error_summary"])

    def test_sufficiency_fails_without_nested_required_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle_dir = create_support_bundle(
                Path(tmp),
                logs=[],
                doctor_output="Doctor completed",
                manifest_status={"model_status": "missing"},
                backend_status={"backend_status": "blocked"},
                recent_errors=[],
                diagnostics={**collect_diagnostic_status(), "host": {}, "os": {}},
            )

            sufficiency = evaluate_support_bundle_sufficiency(bundle_dir)

            self.assertFalse(sufficiency["sufficient"])
            self.assertFalse(sufficiency["criteria"]["required_fields"])

    def test_bundle_rejects_frame_content_and_file_uri_leaks(self):
        raw_uri = "file:///Users/alice/Projects/SecretShow/shot010/plate_v001.exr"

        with tempfile.TemporaryDirectory() as tmp:
            bundle_dir = create_support_bundle(
                Path(tmp),
                logs=[f"failed to read {raw_uri}"],
                doctor_output=f"doctor saw {raw_uri}",
                manifest_status={"model_status": "missing"},
                backend_status={"backend_status": "blocked"},
                recent_errors=[{"error_code": "bad_path", "message": raw_uri}],
                diagnostics={
                    **collect_diagnostic_status(),
                    "frame_contents": "raw rgba bytes",
                    "frameContents": "raw rgba bytes",
                    "sampled_pixels": [0.1, 0.2, 0.3, 1.0],
                    "sampledPixels": [0.1, 0.2, 0.3, 1.0],
                    "thumbnail": "base64-image",
                    "thumbnailBase64": "base64-image",
                    "safe_extra": "ok",
                },
            )

            diagnostics = json.loads(
                (bundle_dir / "diagnostics.json").read_text(encoding="utf-8")
            )
            self.assertNotIn("frame_contents", diagnostics)
            self.assertNotIn("frameContents", diagnostics)
            self.assertNotIn("sampled_pixels", diagnostics)
            self.assertNotIn("sampledPixels", diagnostics)
            self.assertNotIn("thumbnail", diagnostics)
            self.assertNotIn("thumbnailBase64", diagnostics)
            self.assertEqual(diagnostics["safe_extra"], "ok")

            bundle_text = "\n".join(
                path.read_text(encoding="utf-8")
                for path in bundle_dir.rglob("*")
                if path.is_file()
            )
            self.assertNotIn(raw_uri, bundle_text)
            self.assertNotIn("SecretShow", bundle_text)
            self.assertNotIn("plate_v001", bundle_text)

            sufficiency = evaluate_support_bundle_sufficiency(bundle_dir)
            self.assertTrue(sufficiency["sufficient"], sufficiency)

    def test_sufficiency_fails_when_bundle_contains_prohibited_text(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle_dir = create_support_bundle(
                Path(tmp),
                logs=[],
                doctor_output="Doctor completed",
                manifest_status={"model_status": "missing"},
                backend_status={"backend_status": "blocked"},
                recent_errors=[],
                diagnostics=collect_diagnostic_status(),
            )
            (bundle_dir / "logs" / "redacted.log").write_text(
                "leaked file:///Users/alice/Projects/SecretShow/shot010/plate.exr\n",
                encoding="utf-8",
            )

            sufficiency = evaluate_support_bundle_sufficiency(bundle_dir)

            self.assertFalse(sufficiency["sufficient"])
            self.assertFalse(sufficiency["criteria"]["prohibited_content_absent"])

    def test_sufficiency_fails_when_plaintext_paths_leak(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle_dir = create_support_bundle(
                Path(tmp),
                logs=[],
                doctor_output="Doctor completed",
                manifest_status={"model_status": "missing"},
                backend_status={"backend_status": "blocked"},
                recent_errors=[],
                diagnostics=collect_diagnostic_status(),
            )
            self.assertTrue(evaluate_support_bundle_sufficiency(bundle_dir)["sufficient"])

            (bundle_dir / "logs" / "redacted.log").write_text(
                "cache /Users/alice/Projects/SecretShow/cache\n"
                "media SecretShow/shot010/plate_v001.exr\n",
                encoding="utf-8",
            )

            sufficiency = evaluate_support_bundle_sufficiency(bundle_dir)

            self.assertFalse(sufficiency["sufficient"])
            self.assertFalse(sufficiency["criteria"]["prohibited_content_absent"])

    def test_bundle_redacts_labeled_free_text_project_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle_dir = create_support_bundle(
                Path(tmp),
                logs=[json.dumps({"message": "project: Alpha Beta failed"})],
                doctor_output="show SecretShow failed",
                manifest_status={"model_status": "missing"},
                backend_status={"backend_status": "blocked"},
                recent_errors=[{"error_code": "failed", "message": "shot shot001 failed"}],
                diagnostics=collect_diagnostic_status(),
            )

            bundle_text = "\n".join(
                path.read_text(encoding="utf-8")
                for path in bundle_dir.rglob("*")
                if path.is_file()
            )

            self.assertNotIn("SecretShow", bundle_text)
            self.assertNotIn("Alpha", bundle_text)
            self.assertNotIn("Beta", bundle_text)
            self.assertNotIn("shot001", bundle_text)
            self.assertTrue(evaluate_support_bundle_sufficiency(bundle_dir)["sufficient"])

    def test_bundle_redacts_bare_project_like_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle_dir = create_support_bundle(
                Path(tmp),
                logs=[json.dumps({"message": "SecretShow failed and Alpha_Beta retry"})],
                doctor_output="shot001 failed",
                manifest_status={"model_status": "missing"},
                backend_status={"backend_status": "blocked"},
                recent_errors=[{"error_code": "failed", "message": "SecretShow retry"}],
                diagnostics=collect_diagnostic_status(),
            )

            bundle_text = "\n".join(
                path.read_text(encoding="utf-8")
                for path in bundle_dir.rglob("*")
                if path.is_file()
            )

            self.assertNotIn("SecretShow", bundle_text)
            self.assertNotIn("Alpha_Beta", bundle_text)
            self.assertNotIn("shot001", bundle_text)
            self.assertTrue(evaluate_support_bundle_sufficiency(bundle_dir)["sufficient"])

    def test_sufficiency_fails_when_labeled_project_name_leaks(self):
        with tempfile.TemporaryDirectory() as tmp:
            bundle_dir = create_support_bundle(
                Path(tmp),
                logs=[],
                doctor_output="Doctor completed",
                manifest_status={"model_status": "missing"},
                backend_status={"backend_status": "blocked"},
                recent_errors=[],
                diagnostics=collect_diagnostic_status(),
            )
            (bundle_dir / "logs" / "redacted.log").write_text(
                "Alpha_Beta failed\n",
                encoding="utf-8",
            )

            sufficiency = evaluate_support_bundle_sufficiency(bundle_dir)

            self.assertFalse(sufficiency["sufficient"])
            self.assertFalse(sufficiency["criteria"]["prohibited_content_absent"])


if __name__ == "__main__":
    unittest.main()
