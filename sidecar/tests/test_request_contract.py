import json
import os
from pathlib import Path
import tempfile
import unittest

from sidecar.corridorkey_sidecar.protocol import (
    COMMAND_CANCEL,
    COMMAND_INFER,
    COMMAND_WARMUP,
    ERROR_INVALID_REQUEST,
    ERROR_UNKNOWN_COMMAND,
    ProtocolError,
    decode_request,
    handle_line,
    request_hash_for_payload,
)

OUTPUT_MODES = (
    "processed_rgba",
    "matte",
    "straight_fg",
    "alpha_hint_view",
    "checker_comp",
    "status",
)


class patched_env:
    def __init__(self, **values):
        self.values = values
        self.previous = {}

    def __enter__(self):
        self.previous = {key: os.environ.get(key) for key in self.values}
        for key, value in self.values.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value

    def __exit__(self, exc_type, exc, tb):
        for key, value in self.previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def infer_payload(despill_strength="5", *, output_mode="processed_rgba", job_id="job-request-contract"):
    root = Path(tempfile.gettempdir()) / "corridorkey-render-request-contract"
    payload = {
        "frame_id": "frame-42",
        "job_id": job_id,
        "render_window_x1": "10",
        "render_window_y1": "20",
        "render_window_x2": "74",
        "render_window_y2": "92",
        "source_frame_blob_path": str(root / "source-frame.ckfb"),
        "alpha_hint_frame_blob_path": str(root / "alpha-hint-frame.ckfb"),
        "alpha_hint_source": "external",
        "screen_color": "green",
        "quality": "high_1024",
        "input_color_space": "host_managed",
        "despill_strength": despill_strength,
        "backend": "stub",
        "output_mode": output_mode,
    }
    payload["request_hash"] = request_hash_for_payload(payload)
    return payload


def decode_infer(payload):
    return decode_request(
        json.dumps(
            {
                "request_id": "req-infer-1",
                "command": COMMAND_INFER,
                "payload": payload,
            }
        )
    )


class RequestContractTests(unittest.TestCase):
    def test_request_hash_matches_cross_language_golden_value(self):
        payload = {
            "frame_id": "frame-42",
            "job_id": "job-request-contract",
            "render_window_x1": "10",
            "render_window_y1": "20",
            "render_window_x2": "74",
            "render_window_y2": "92",
            "source_frame_blob_path": "/tmp/corridorkey-render-request-contract/source-frame.ckfb",
            "alpha_hint_frame_blob_path": "/tmp/corridorkey-render-request-contract/alpha-hint-frame.ckfb",
            "alpha_hint_source": "external",
            "screen_color": "green",
            "quality": "high_1024",
            "input_color_space": "host_managed",
            "despill_strength": "5",
            "backend": "stub",
            "output_mode": "processed_rgba",
        }
        self.assertEqual(request_hash_for_payload(payload), "645daac84e998b34")

    def test_infer_contract_validates_and_normalizes_payload(self):
        payload = infer_payload("5.000")
        payload["request_hash"] = infer_payload("5")["request_hash"]
        request = decode_infer(payload)

        self.assertEqual(request["request_id"], "req-infer-1")
        self.assertEqual(request["command"], COMMAND_INFER)
        self.assertEqual(request["payload"]["despill_strength"], "5")
        self.assertEqual(request["payload"]["alpha_hint_source"], "external")
        self.assertEqual(request["payload"]["screen_color"], "green")
        self.assertEqual(request["payload"]["quality"], "high_1024")
        self.assertEqual(request["payload"]["input_color_space"], "host_managed")
        self.assertEqual(request["payload"]["backend"], "stub")
        self.assertEqual(request["payload"]["output_mode"], "processed_rgba")
        self.assertNotIn("source_media_path", request["payload"])
        self.assertNotIn("raw_media_path", request["payload"])
        self.assertNotIn("project_path", request["payload"])

    def test_infer_contract_hash_is_deterministic_and_includes_despill(self):
        payload_a = infer_payload("5")
        payload_b = infer_payload("5.1")
        payload_c = infer_payload("5", job_id="job-request-contract-2")

        self.assertEqual(request_hash_for_payload(payload_a), payload_a["request_hash"])
        self.assertEqual(request_hash_for_payload(payload_a), request_hash_for_payload(payload_a))
        self.assertNotEqual(payload_a["request_hash"], payload_b["request_hash"])
        self.assertNotEqual(payload_a["request_hash"], payload_c["request_hash"])

    def test_all_output_modes_are_valid(self):
        for output_mode in OUTPUT_MODES:
            with self.subTest(output_mode=output_mode):
                request = decode_infer(infer_payload(output_mode=output_mode))
                self.assertEqual(request["payload"]["output_mode"], output_mode)

    def test_infer_contract_rejects_invalid_enums_and_ranges(self):
        invalid_cases = {
            "output_mode": "processed_rgb",
            "screen_color": "purple",
            "quality": "full",
            "input_color_space": "aces",
            "alpha_hint_source": "luma",
            "backend": "rocm",
            "despill_strength": "10.1",
        }

        for key, value in invalid_cases.items():
            with self.subTest(key=key):
                payload = infer_payload()
                payload[key] = value
                payload["request_hash"] = request_hash_for_payload(payload)
                with self.assertRaises(ProtocolError) as context:
                    decode_infer(payload)
                self.assertEqual(context.exception.code, ERROR_INVALID_REQUEST)

    def test_infer_contract_rejects_raw_media_and_project_paths(self):
        temp_root = Path(tempfile.gettempdir())
        invalid_cases = (
            ("source_frame_blob_path", "/Users/example/project/plate.exr"),
            ("source_frame_blob_path", str(temp_root / "corridorkey-render-request-contract" / "bad\0source.ckfb")),
            ("source_frame_blob_path", str(temp_root / "unscoped-frame.ckfb")),
            ("source_frame_blob_path", str(temp_root / "corridorkey-plate.exr")),
            (
                "source_frame_blob_path",
                str(temp_root / "corridorkey-render-request-contract" / "plate.exr"),
            ),
            (
                "source_frame_blob_path",
                str(temp_root / "corridorkey-cache" / "source-frame.ckfb"),
            ),
            (
                "alpha_hint_frame_blob_path",
                str(temp_root / "corridorkey-render-other-contract" / "alpha-hint-frame.ckfb"),
            ),
            ("source_media_path", "/Users/example/show/source.mov"),
            ("raw_media_path", "/Users/example/show/raw.exr"),
            ("project_path", "/Users/example/show/secret_project.drp"),
        )

        for key, value in invalid_cases:
            with self.subTest(key=key):
                payload = infer_payload()
                payload[key] = value
                payload["request_hash"] = request_hash_for_payload(payload)
                with self.assertRaises(ProtocolError) as context:
                    decode_infer(payload)
                self.assertEqual(context.exception.code, ERROR_INVALID_REQUEST)

    def test_infer_contract_rejects_nul_path_without_losing_request_id(self):
        payload = infer_payload()
        payload["source_frame_blob_path"] = (
            str(Path(tempfile.gettempdir()) / "corridorkey-render-request-contract" / "bad")
            + "\0source.ckfb"
        )
        payload["request_hash"] = request_hash_for_payload(payload)

        response, should_shutdown = handle_line(
            json.dumps(
                {
                    "request_id": "req-nul-path",
                    "command": COMMAND_INFER,
                    "payload": payload,
                }
            )
        )

        self.assertFalse(should_shutdown)
        self.assertEqual(response["request_id"], "req-nul-path")
        self.assertIs(response["ok"], False)
        self.assertEqual(response["error"]["code"], ERROR_INVALID_REQUEST)

    def test_infer_contract_accepts_raw_and_resolved_temp_path_aliases(self):
        temp_root = Path(tempfile.gettempdir())
        resolved_temp_root = temp_root.resolve(strict=False)
        roots = [temp_root / "corridorkey-render-request-contract"]
        if resolved_temp_root != temp_root:
            roots.append(resolved_temp_root / "corridorkey-render-request-contract")

        for root in roots:
            with self.subTest(root=str(root)):
                payload = infer_payload()
                payload["source_frame_blob_path"] = str(root / "source-frame.ckfb")
                payload["alpha_hint_frame_blob_path"] = str(root / "alpha-hint-frame.ckfb")
                payload["request_hash"] = request_hash_for_payload(payload)
                request = decode_infer(payload)
                self.assertEqual(request["payload"]["source_frame_blob_path"], str(root / "source-frame.ckfb"))

    def test_infer_contract_rejects_hash_mismatch(self):
        payload = infer_payload()
        payload["quality"] = "draft_512"

        with self.assertRaises(ProtocolError) as context:
            decode_infer(payload)
        self.assertEqual(context.exception.code, ERROR_INVALID_REQUEST)

        payload = infer_payload()
        payload["job_id"] = "job-request-contract-2"
        with self.assertRaises(ProtocolError) as context:
            decode_infer(payload)
        self.assertEqual(context.exception.code, ERROR_INVALID_REQUEST)

    def test_infer_contract_requires_request_hash(self):
        payload = infer_payload()
        del payload["request_hash"]

        with self.assertRaises(ProtocolError) as context:
            decode_infer(payload)
        self.assertEqual(context.exception.code, ERROR_INVALID_REQUEST)

    def test_non_stub_infer_is_recognized_but_not_executed(self):
        payload = infer_payload()
        payload["backend"] = "torch_cpu"
        payload["request_hash"] = request_hash_for_payload(payload)
        response, should_shutdown = handle_line(
            json.dumps(
                {
                    "request_id": "req-infer-1",
                    "command": COMMAND_INFER,
                    "payload": payload,
                }
            )
        )

        self.assertFalse(should_shutdown)
        self.assertEqual(response["request_id"], "req-infer-1")
        self.assertIs(response["ok"], False)
        self.assertEqual(response["payload"]["backend"], "torch_cpu")
        self.assertEqual(response["payload"]["backend_status"], "blocked")
        self.assertEqual(response["payload"]["model_status"], "missing")
        self.assertEqual(response["payload"]["model_source_status"], "ready")
        self.assertEqual(response["payload"]["install_status"], "not_installed")
        self.assertEqual(response["error"]["code"], "blocked_backend")

    def test_non_stub_warmup_reports_backend_runtime_unconfigured(self):
        response, should_shutdown = handle_line(
            json.dumps(
                {
                    "request_id": "req-warmup-nonstub",
                    "command": COMMAND_WARMUP,
                    "payload": {
                        "job_id": "job-warmup-nonstub",
                        "backend": "torch_cpu",
                        "quality": "high_1024",
                    },
                }
            )
        )

        self.assertFalse(should_shutdown)
        self.assertEqual(response["request_id"], "req-warmup-nonstub")
        self.assertIs(response["ok"], False)
        self.assertEqual(response["payload"]["backend"], "torch_cpu")
        self.assertEqual(response["payload"]["backend_status"], "blocked")
        self.assertEqual(response["payload"]["model_status"], "missing")
        self.assertEqual(response["payload"]["model_source_status"], "ready")
        self.assertEqual(response["payload"]["install_status"], "not_installed")
        self.assertEqual(response["error"]["code"], "blocked_backend")

    def test_infer_contract_rejects_unknown_fields(self):
        payload = infer_payload()
        payload["unexpected"] = "ignored"

        with self.assertRaises(ProtocolError) as context:
            decode_infer(payload)
        self.assertEqual(context.exception.code, ERROR_INVALID_REQUEST)

    def test_infer_contract_keeps_known_payload_fields(self):
        payload = infer_payload()
        request = decode_infer(payload)
        self.assertEqual(set(request["payload"]), set(payload))

    def test_warmup_and_cancel_have_job_ids(self):
        with tempfile.TemporaryDirectory() as tmp:
            with patched_env(CORRIDORKEY_MODEL_ROOT=tmp, CORRIDORKEY_MODEL_DIR=None):
                warmup_response, should_shutdown = handle_line(
                    json.dumps(
                        {
                            "request_id": "req-warmup",
                            "command": COMMAND_WARMUP,
                            "payload": {
                                "job_id": "job-warmup",
                                "backend": "stub",
                                "quality": "high_1024",
                            },
                        }
                    )
                )

        self.assertFalse(should_shutdown)
        self.assertIs(warmup_response["ok"], True)
        self.assertEqual(warmup_response["payload"]["job_id"], "job-warmup")
        self.assertEqual(warmup_response["payload"]["warmup"], "ready")
        self.assertEqual(warmup_response["payload"]["model_status"], "missing")
        self.assertEqual(warmup_response["payload"]["model_source_status"], "ready")
        self.assertEqual(warmup_response["payload"]["install_status"], "not_installed")

        cancel_response, should_shutdown = handle_line(
            json.dumps(
                {
                    "request_id": "req-cancel",
                    "command": COMMAND_CANCEL,
                    "payload": {"job_id": "job-warmup"},
                }
            )
        )

        self.assertFalse(should_shutdown)
        self.assertIs(cancel_response["ok"], True)
        self.assertEqual(cancel_response["payload"], {"job_id": "job-warmup", "cancel": "accepted"})

    def test_download_model_command_requires_job_payload(self):
        response, should_shutdown = handle_line(
            json.dumps(
                {
                    "request_id": "req-intent",
                    "command": "download_model",
                    "payload": {"quality": "full_2048"},
                }
            )
        )

        self.assertFalse(should_shutdown)
        self.assertEqual(response["request_id"], "req-intent")
        self.assertIs(response["ok"], False)
        self.assertEqual(response["error"]["code"], ERROR_INVALID_REQUEST)


if __name__ == "__main__":
    unittest.main()
