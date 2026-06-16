import json
from pathlib import Path
import struct
import tempfile
import unittest

from sidecar.corridorkey_sidecar.protocol import (
    COMMAND_CANCEL,
    COMMAND_INFER,
    ERROR_INVALID_REQUEST,
    handle_line,
    request_hash_for_payload,
)


HEADER = struct.Struct("<4sIiiiiI")


def write_blob(path, bounds, channels, values):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as handle:
        handle.write(HEADER.pack(b"CKFB", 1, *bounds, channels))
        handle.write(struct.pack(f"<{len(values)}f", *values))


def infer_payload(root, job_id):
    source_path = root / "source.ckfb"
    alpha_path = root / "alpha.ckfb"
    write_blob(source_path, (0, 0, 1, 1), 4, (0.2, 0.6, 0.1, 1.0))
    write_blob(alpha_path, (0, 0, 1, 1), 1, (0.5,))

    payload = {
        "frame_id": "frame-1",
        "job_id": job_id,
        "render_window_x1": "0",
        "render_window_y1": "0",
        "render_window_x2": "1",
        "render_window_y2": "1",
        "source_frame_blob_path": str(source_path),
        "alpha_hint_frame_blob_path": str(alpha_path),
        "alpha_hint_source": "external",
        "screen_color": "green",
        "quality": "high_1024",
        "input_color_space": "host_managed",
        "despill_strength": "5",
        "backend": "stub",
        "output_mode": "processed_rgba",
    }
    payload["request_hash"] = request_hash_for_payload(payload)
    return payload


class CancelTests(unittest.TestCase):
    def test_cancel_command_is_job_scoped_and_idempotent(self):
        for request_id in ("req-cancel-a", "req-cancel-b"):
            response, should_shutdown = handle_line(
                json.dumps(
                    {
                        "request_id": request_id,
                        "command": COMMAND_CANCEL,
                        "payload": {"job_id": "job-cancel-idempotent"},
                    }
                )
            )

            self.assertFalse(should_shutdown)
            self.assertIs(response["ok"], True)
            self.assertEqual(response["payload"]["job_id"], "job-cancel-idempotent")
            self.assertEqual(response["payload"]["cancel"], "accepted")

    def test_cancel_command_does_not_poison_later_jobs(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-cancel-test-") as tmp:
            root = Path(tmp)
            job_id = "job-cancel-then-infer"
            handle_line(
                json.dumps(
                    {
                        "request_id": "req-cancel-before-infer",
                        "command": COMMAND_CANCEL,
                        "payload": {"job_id": job_id},
                    }
                )
            )

            response, should_shutdown = handle_line(
                json.dumps(
                    {
                        "request_id": "req-cancelled-infer",
                        "command": COMMAND_INFER,
                        "payload": infer_payload(root, job_id),
                    }
                )
            )

            self.assertFalse(should_shutdown)
            self.assertIs(response["ok"], True)
            self.assertEqual(response["request_id"], "req-cancelled-infer")
            self.assertEqual(response["payload"]["job_id"], job_id)
            self.assertIsNone(response["error"])

    def test_cancel_rejects_unsafe_job_ids(self):
        response, should_shutdown = handle_line(
            json.dumps(
                {
                    "request_id": "req-cancel-invalid",
                    "command": COMMAND_CANCEL,
                    "payload": {"job_id": "SecretShow"},
                }
            )
        )

        self.assertFalse(should_shutdown)
        self.assertIs(response["ok"], False)
        self.assertEqual(response["error"]["code"], ERROR_INVALID_REQUEST)


if __name__ == "__main__":
    unittest.main()
