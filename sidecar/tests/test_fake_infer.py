import json
import os
from contextlib import contextmanager
from pathlib import Path
import struct
import tempfile
import time
import unittest
from unittest import mock

from sidecar.corridorkey_sidecar.protocol import (
    COMMAND_INFER,
    handle_line,
    request_hash_for_payload,
)


HEADER = struct.Struct("<4sIiiiiI")


def write_blob(path, bounds, channels, values):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as handle:
        handle.write(HEADER.pack(b"CKFB", 1, *bounds, channels))
        handle.write(struct.pack(f"<{len(values)}f", *values))


@contextmanager
def patched_env(**values):
    previous = {key: os.environ.get(key) for key in values}
    try:
        for key, value in values.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value
        yield
    finally:
        for key, value in previous.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value


def read_blob(path):
    with Path(path).open("rb") as handle:
        magic, version, x1, y1, x2, y2, channels = HEADER.unpack(handle.read(HEADER.size))
        assert magic == b"CKFB"
        assert version == 1
        count = (x2 - x1) * (y2 - y1) * channels
        return {
            "bounds": (x1, y1, x2, y2),
            "channels": channels,
            "values": struct.unpack(f"<{count}f", handle.read(count * 4)),
        }


def infer_payload(
    root,
    *,
    screen_color="green",
    quality="high_1024",
    despill_strength="5",
    bounds=(0, 0, 2, 1),
):
    source_path = root / "source.ckfb"
    alpha_path = root / "alpha.ckfb"
    pixel_count = (bounds[2] - bounds[0]) * (bounds[3] - bounds[1])
    write_blob(
        source_path,
        bounds,
        4,
        (
            0.20,
            0.70,
            0.10,
            1.00,
            0.80,
            0.30,
            0.20,
            0.50,
        )
        * (pixel_count // 2),
    )
    write_blob(alpha_path, bounds, 1, (0.25, 0.75) * (pixel_count // 2))

    payload = {
        "frame_id": "frame-10",
        "job_id": "job-fake-infer",
        "render_window_x1": str(bounds[0]),
        "render_window_y1": str(bounds[1]),
        "render_window_x2": str(bounds[2]),
        "render_window_y2": str(bounds[3]),
        "source_frame_blob_path": str(source_path),
        "alpha_hint_frame_blob_path": str(alpha_path),
        "alpha_hint_source": "external",
        "screen_color": screen_color,
        "quality": quality,
        "input_color_space": "host_managed",
        "despill_strength": despill_strength,
        "backend": "stub",
        "output_mode": "processed_rgba",
    }
    payload["request_hash"] = request_hash_for_payload(payload)
    return payload


def handle_infer_payload(payload, request_id="req-fake-infer"):
    response, should_shutdown = handle_line(
        json.dumps(
            {
                "request_id": request_id,
                "command": COMMAND_INFER,
                "payload": payload,
            }
        )
    )
    assert not should_shutdown
    return response


def infer(root, **overrides):
    return handle_infer_payload(infer_payload(root, **overrides))


def blob_bytes(response, key):
    return Path(response["payload"][key]).read_bytes()


class FakeInferTests(unittest.TestCase):
    def setUp(self):
        self._model_dir = tempfile.TemporaryDirectory(prefix="corridorkey-empty-models-")
        self._model_env = patched_env(
            CORRIDORKEY_MODEL_DIR=str(Path(self._model_dir.name) / "models"),
            CORRIDORKEY_MODEL_ROOT=None,
        )
        self._model_env.__enter__()

    def tearDown(self):
        self._model_env.__exit__(None, None, None)
        self._model_dir.cleanup()

    def test_stub_infer_returns_deterministic_frame_blob_paths(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-fake-infer-test-") as tmp:
            root = Path(tmp)
            response_a = infer(root)
            response_b = infer(root)

            self.assertIs(response_a["ok"], True)
            self.assertIsNone(response_a["error"])
            self.assertEqual(response_a["payload"], response_b["payload"])
            self.assertEqual(response_a["payload"]["backend"], "stub")
            self.assertEqual(response_a["payload"]["model"], "stub")
            self.assertEqual(response_a["payload"]["cache"], "miss")
            self.assertEqual(response_a["payload"]["model_status"], "missing")
            self.assertEqual(response_a["payload"]["model_source_status"], "ready")
            self.assertEqual(response_a["payload"]["install_status"], "not_installed")

            processed = read_blob(response_a["payload"]["processed_rgba_frame_blob_path"])
            straight = read_blob(response_a["payload"]["straight_fg_frame_blob_path"])
            alpha = read_blob(response_a["payload"]["alpha_frame_blob_path"])

            self.assertEqual(processed["bounds"], (0, 0, 2, 1))
            self.assertEqual(processed["channels"], 4)
            self.assertEqual(straight["channels"], 3)
            self.assertEqual(alpha["channels"], 1)
            for index, matte in enumerate(alpha["values"]):
                self.assertGreaterEqual(matte, 0.0)
                self.assertLessEqual(matte, 1.0)
                self.assertAlmostEqual(processed["values"][index * 4 + 3], matte)
                for channel in range(3):
                    self.assertGreaterEqual(straight["values"][index * 3 + channel], 0.0)
                    self.assertLessEqual(straight["values"][index * 3 + channel], 1.0)
                    self.assertAlmostEqual(
                        processed["values"][index * 4 + channel],
                        straight["values"][index * 3 + channel] * matte,
                    )
            self.assertEqual(
                Path(response_a["payload"]["processed_rgba_frame_blob_path"]).read_bytes(),
                Path(response_b["payload"]["processed_rgba_frame_blob_path"]).read_bytes(),
            )

    def test_stub_infer_varies_with_screen_color_quality_and_despill(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-fake-infer-variation-") as tmp:
            root = Path(tmp)
            baseline = infer(root, screen_color="green", quality="high_1024", despill_strength="5")

            changed_screen = infer(
                root, screen_color="blue", quality="high_1024", despill_strength="5"
            )
            changed_quality = infer(
                root, screen_color="green", quality="draft_512", despill_strength="5"
            )
            changed_despill = infer(
                root, screen_color="green", quality="high_1024", despill_strength="6"
            )

            self.assertNotEqual(
                baseline["payload"]["deterministic_key"],
                changed_screen["payload"]["deterministic_key"],
            )
            self.assertNotEqual(
                blob_bytes(baseline, "processed_rgba_frame_blob_path"),
                blob_bytes(changed_screen, "processed_rgba_frame_blob_path"),
            )
            self.assertNotEqual(
                baseline["payload"]["deterministic_key"],
                changed_quality["payload"]["deterministic_key"],
            )
            self.assertNotEqual(
                blob_bytes(baseline, "processed_rgba_frame_blob_path"),
                blob_bytes(changed_quality, "processed_rgba_frame_blob_path"),
            )
            self.assertNotEqual(
                baseline["payload"]["deterministic_key"],
                changed_despill["payload"]["deterministic_key"],
            )
            self.assertNotEqual(
                blob_bytes(baseline, "processed_rgba_frame_blob_path"),
                blob_bytes(changed_despill, "processed_rgba_frame_blob_path"),
            )

    def test_stub_infer_errors_keep_request_id(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-fake-infer-error-") as tmp:
            root = Path(tmp)
            payload = infer_payload(root)
            Path(payload["source_frame_blob_path"]).unlink()

            response = handle_infer_payload(payload, "req-fake-infer-error")

            self.assertIs(response["ok"], False)
            self.assertEqual(response["request_id"], "req-fake-infer-error")
            self.assertEqual(response["error"]["code"], "invalid_request")

    def test_stub_infer_rejects_frame_blob_trailing_data(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-fake-infer-trailing-") as tmp:
            root = Path(tmp)
            payload = infer_payload(root)
            with Path(payload["source_frame_blob_path"]).open("ab") as handle:
                handle.write(b"extra")

            response = handle_infer_payload(payload, "req-fake-infer-trailing")

            self.assertIs(response["ok"], False)
            self.assertEqual(response["request_id"], "req-fake-infer-trailing")
            self.assertEqual(response["error"]["code"], "invalid_request")

    def test_stub_infer_rejects_non_rgba_source_blob(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-fake-infer-source-") as tmp:
            root = Path(tmp)
            payload = infer_payload(root)
            write_blob(root / "source.ckfb", (0, 0, 2, 1), 1, (0.5, 0.75))

            response = handle_infer_payload(payload, "req-fake-infer-source")

            self.assertIs(response["ok"], False)
            self.assertEqual(response["request_id"], "req-fake-infer-source")
            self.assertEqual(response["error"]["code"], "invalid_request")

    def test_stub_infer_rejects_mismatched_blob_bounds_before_data_read(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-fake-infer-huge-") as tmp:
            root = Path(tmp)
            payload = infer_payload(root)
            with Path(payload["source_frame_blob_path"]).open("wb") as handle:
                handle.write(HEADER.pack(b"CKFB", 1, 0, 0, 500000, 500000, 4))

            response = handle_infer_payload(payload, "req-fake-infer-huge")

            self.assertIs(response["ok"], False)
            self.assertEqual(response["request_id"], "req-fake-infer-huge")
            self.assertEqual(response["error"]["code"], "invalid_request")

    def test_stub_infer_large_frame_uses_bounded_memory(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-fake-infer-large-") as tmp:
            root = Path(tmp)
            payload = infer_payload(root, bounds=(0, 0, 1024, 1024))

            start = time.monotonic()
            with mock.patch(
                "sidecar.corridorkey_sidecar.protocol._read_frame_blob",
                side_effect=AssertionError("large fake infer must not materialize full blobs"),
            ):
                response = handle_infer_payload(payload, "req-fake-infer-large")
            elapsed = time.monotonic() - start

            self.assertIs(response["ok"], True)
            self.assertLess(elapsed, 5.0)
            processed = read_blob(response["payload"]["processed_rgba_frame_blob_path"])
            straight = read_blob(response["payload"]["straight_fg_frame_blob_path"])
            alpha = read_blob(response["payload"]["alpha_frame_blob_path"])
            self.assertEqual(processed["bounds"], (0, 0, 1024, 1024))
            self.assertEqual(processed["channels"], 4)
            self.assertEqual(straight["channels"], 3)
            self.assertEqual(alpha["channels"], 1)

    def test_large_straight_fg_does_not_generate_unused_processed_blob(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-fake-infer-large-sfg-") as tmp:
            root = Path(tmp)
            payload = infer_payload(root, bounds=(0, 0, 1024, 1024))
            payload["output_mode"] = "straight_fg"
            payload["request_hash"] = request_hash_for_payload(payload)

            response = handle_infer_payload(payload, "req-fake-infer-large-sfg")

            self.assertIs(response["ok"], True)
            self.assertNotIn("processed_rgba_frame_blob_path", response["payload"])
            self.assertIn("straight_fg_frame_blob_path", response["payload"])
            self.assertIn("alpha_frame_blob_path", response["payload"])
            straight = read_blob(response["payload"]["straight_fg_frame_blob_path"])
            alpha = read_blob(response["payload"]["alpha_frame_blob_path"])
            self.assertEqual(straight["bounds"], (0, 0, 1024, 1024))
            self.assertEqual(straight["channels"], 3)
            self.assertEqual(alpha["channels"], 1)

    def test_stub_infer_rejects_raw_media_paths(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-fake-infer-raw-path-") as tmp:
            payload = infer_payload(Path(tmp))
            payload["raw_media_path"] = "/Users/example/show/raw.exr"

            response = handle_infer_payload(payload, "req-fake-infer-raw-path")

            self.assertIs(response["ok"], False)
            self.assertEqual(response["request_id"], "req-fake-infer-raw-path")
            self.assertEqual(response["error"]["code"], "invalid_request")

    def test_stub_infer_output_key_includes_input_bytes(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-fake-infer-input-") as tmp:
            root = Path(tmp)
            payload = infer_payload(root)
            response_a = handle_infer_payload(payload)
            write_blob(
                root / "source.ckfb",
                (0, 0, 2, 1),
                4,
                (0.9, 0.1, 0.2, 1.0, 0.2, 0.8, 0.4, 0.5),
            )
            response_b = handle_infer_payload(payload)

            self.assertNotEqual(
                response_a["payload"]["processed_rgba_frame_blob_path"],
                response_b["payload"]["processed_rgba_frame_blob_path"],
            )
            self.assertNotEqual(
                response_a["payload"]["deterministic_key"],
                response_b["payload"]["deterministic_key"],
            )

    def test_auto_screen_despill_changes_saturated_output_pixels(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-fake-infer-auto-despill-") as tmp:
            root = Path(tmp)
            baseline = infer(root, screen_color="auto", quality="full_2048", despill_strength="0")
            changed = infer(root, screen_color="auto", quality="full_2048", despill_strength="10")

            self.assertNotEqual(
                blob_bytes(baseline, "processed_rgba_frame_blob_path"),
                blob_bytes(changed, "processed_rgba_frame_blob_path"),
            )

    def test_fake_infer_delay_requires_test_fault_gate(self):
        with patched_env(
            CORRIDORKEY_TEST_FAKE_INFER_DELAY_MS="120",
            CORRIDORKEY_TEST_FAULTS_ALLOWED=None,
        ):
            with tempfile.TemporaryDirectory(prefix="corridorkey-render-fake-infer-delay-") as tmp:
                with mock.patch(
                    "sidecar.corridorkey_sidecar.protocol.time.sleep",
                    side_effect=AssertionError("test delay must require fault gate"),
                ):
                    response = infer(Path(tmp))

        self.assertIs(response["ok"], True)

    def test_authorized_fake_infer_delay_waits(self):
        with patched_env(
            CORRIDORKEY_TEST_FAKE_INFER_DELAY_MS="120",
            CORRIDORKEY_TEST_FAULTS_ALLOWED="1",
        ):
            with tempfile.TemporaryDirectory(prefix="corridorkey-render-fake-infer-delay-") as tmp:
                start = time.monotonic()
                response = infer(Path(tmp))
                elapsed_with_gate = time.monotonic() - start

        self.assertIs(response["ok"], True)
        self.assertGreaterEqual(elapsed_with_gate, 0.1)


if __name__ == "__main__":
    unittest.main()
