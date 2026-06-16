import concurrent.futures
from contextlib import contextmanager
import json
import os
from pathlib import Path
import struct
import tempfile
import time
import unittest

from sidecar.corridorkey_sidecar.protocol import (
    COMMAND_INFER,
    _GPU_CLASS_QUEUE_LOCK,
    handle_line,
    request_hash_for_payload,
)
from sidecar.corridorkey_sidecar.server import handle_line_with_model_manager


HEADER = struct.Struct("<4sIiiiiI")


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


def write_blob(path, bounds, channels, values):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as handle:
        handle.write(HEADER.pack(b"CKFB", 1, *bounds, channels))
        handle.write(struct.pack(f"<{len(values)}f", *values))


def infer_payload(root, job_id, *, quality="high_1024", screen_color="green", despill="5"):
    source_path = root / "source.ckfb"
    alpha_path = root / "alpha.ckfb"
    write_blob(
        source_path,
        (0, 0, 2, 1),
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
        ),
    )
    write_blob(alpha_path, (0, 0, 2, 1), 1, (0.25, 0.75))
    payload = {
        "frame_id": f"frame-{job_id.removeprefix('job-')}",
        "job_id": job_id,
        "render_window_x1": "0",
        "render_window_y1": "0",
        "render_window_x2": "2",
        "render_window_y2": "1",
        "source_frame_blob_path": str(source_path),
        "alpha_hint_frame_blob_path": str(alpha_path),
        "alpha_hint_source": "external",
        "screen_color": screen_color,
        "quality": quality,
        "input_color_space": "host_managed",
        "despill_strength": despill,
        "backend": "stub",
        "output_mode": "processed_rgba",
    }
    payload["request_hash"] = request_hash_for_payload(payload)
    return payload


def handle_infer(payload, request_id):
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


class QueueAndOomTests(unittest.TestCase):
    def test_synthetic_oom_downgrades_once_and_reports_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-oom-downgrade-") as tmp:
            with patched_env(
                CORRIDORKEY_TEST_FAULTS_ALLOWED="1",
                CORRIDORKEY_TEST_SYNTHETIC_OOM_QUALITIES="full_2048",
            ):
                response = handle_infer(
                    infer_payload(Path(tmp), "job-oom-downgrade", quality="full_2048"),
                    "req-oom-downgrade",
                )

        self.assertIs(response["ok"], True)
        self.assertIsNone(response["error"])
        self.assertEqual(response["payload"]["requested_quality"], "full_2048")
        self.assertEqual(response["payload"]["effective_quality"], "high_1024")
        self.assertEqual(response["payload"]["oom"], "true")
        self.assertEqual(response["payload"]["downgraded_quality"], "true")
        self.assertEqual(response["payload"]["final_failure"], "false")
        self.assertGreaterEqual(int(response["payload"]["queue_time_ms"]), 0)

    def test_synthetic_oom_returns_clear_failure_when_draft_cannot_downgrade(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-oom-failure-") as tmp:
            with patched_env(
                CORRIDORKEY_TEST_FAULTS_ALLOWED="1",
                CORRIDORKEY_TEST_SYNTHETIC_OOM_QUALITIES="draft_512",
            ):
                response = handle_infer(
                    infer_payload(Path(tmp), "job-oom-failure", quality="draft_512"),
                    "req-oom-failure",
                )

        self.assertIs(response["ok"], False)
        self.assertEqual(response["request_id"], "req-oom-failure")
        self.assertEqual(response["error"]["code"], "gpu_oom")
        self.assertIn("out of memory", response["error"]["message"].lower())
        self.assertEqual(response["payload"]["job_id"], "job-oom-failure")
        self.assertEqual(response["payload"]["requested_quality"], "draft_512")
        self.assertEqual(response["payload"]["effective_quality"], "draft_512")
        self.assertEqual(response["payload"]["oom"], "true")
        self.assertEqual(response["payload"]["downgraded_quality"], "false")
        self.assertEqual(response["payload"]["final_failure"], "true")
        self.assertGreaterEqual(int(response["payload"]["queue_time_ms"]), 0)

    def test_non_oom_infer_failures_keep_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-invalid-diagnostics-") as tmp:
            payload = infer_payload(Path(tmp), "job-invalid-diagnostics", quality="high_1024")
            Path(payload["source_frame_blob_path"]).unlink()

            response = handle_infer(payload, "req-invalid-diagnostics")

        self.assertIs(response["ok"], False)
        self.assertEqual(response["error"]["code"], "invalid_request")
        self.assertEqual(response["payload"]["job_id"], "job-invalid-diagnostics")
        self.assertEqual(response["payload"]["requested_quality"], "high_1024")
        self.assertEqual(response["payload"]["effective_quality"], "high_1024")
        self.assertEqual(response["payload"]["oom"], "false")
        self.assertEqual(response["payload"]["downgraded_quality"], "false")
        self.assertEqual(response["payload"]["final_failure"], "true")
        self.assertGreaterEqual(int(response["payload"]["queue_time_ms"]), 0)

    def test_downgraded_retry_non_oom_failure_preserves_oom_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-retry-failure-") as tmp:
            payload = infer_payload(Path(tmp), "job-retry-failure", quality="full_2048")
            Path(payload["source_frame_blob_path"]).unlink()
            with patched_env(
                CORRIDORKEY_TEST_FAULTS_ALLOWED="1",
                CORRIDORKEY_TEST_SYNTHETIC_OOM_QUALITIES="full_2048",
            ):
                response = handle_infer(payload, "req-retry-failure")

        self.assertIs(response["ok"], False)
        self.assertEqual(response["error"]["code"], "invalid_request")
        self.assertEqual(response["payload"]["job_id"], "job-retry-failure")
        self.assertEqual(response["payload"]["requested_quality"], "full_2048")
        self.assertEqual(response["payload"]["effective_quality"], "high_1024")
        self.assertEqual(response["payload"]["oom"], "true")
        self.assertEqual(response["payload"]["downgraded_quality"], "true")
        self.assertEqual(response["payload"]["final_failure"], "true")
        self.assertGreaterEqual(int(response["payload"]["queue_time_ms"]), 0)

    def test_queued_non_oom_failure_reports_queue_time(self):
        temp_dirs = [
            tempfile.TemporaryDirectory(prefix=f"corridorkey-render-queued-error-{index}-")
            for index in range(2)
        ]
        try:
            slow_payload = infer_payload(Path(temp_dirs[0].name), "job-queued-error-slow")
            failing_payload = infer_payload(Path(temp_dirs[1].name), "job-queued-error-fail")
            Path(failing_payload["source_frame_blob_path"]).unlink()
            with patched_env(
                CORRIDORKEY_TEST_FAULTS_ALLOWED="1",
                CORRIDORKEY_TEST_SYNTHETIC_OOM_QUALITIES=None,
            ):
                with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                    with _GPU_CLASS_QUEUE_LOCK:
                        failing_future = pool.submit(
                            handle_infer, failing_payload, "req-queued-error-fail"
                        )
                        time.sleep(0.12)
                    slow_future = pool.submit(handle_infer, slow_payload, "req-queued-error-slow")
                    failing_response = failing_future.result()
                    slow_response = slow_future.result()
        finally:
            for temp_dir in temp_dirs:
                temp_dir.cleanup()

        self.assertIs(slow_response["ok"], True)
        self.assertIs(failing_response["ok"], False)
        self.assertEqual(failing_response["error"]["code"], "invalid_request")
        self.assertGreaterEqual(int(failing_response["payload"]["queue_time_ms"]), 80)

    def test_validation_failure_reports_session12_diagnostics_when_quality_is_known(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-validation-diagnostics-") as tmp:
            payload = infer_payload(Path(tmp), "job-validation-diagnostics")
            payload["request_hash"] = "wrong"

            response = handle_infer(payload, "req-validation-diagnostics")

        self.assertIs(response["ok"], False)
        self.assertEqual(response["error"]["code"], "invalid_request")
        self.assertEqual(response["payload"]["job_id"], "job-validation-diagnostics")
        self.assertEqual(response["payload"]["requested_quality"], "high_1024")
        self.assertEqual(response["payload"]["effective_quality"], "high_1024")
        self.assertEqual(response["payload"]["queue_time_ms"], "0")
        self.assertEqual(response["payload"]["oom"], "false")
        self.assertEqual(response["payload"]["downgraded_quality"], "false")
        self.assertEqual(response["payload"]["final_failure"], "true")

    def test_non_stub_infer_uses_gpu_class_queue_for_diagnostics(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-backend-queued-") as tmp:
            payload = infer_payload(Path(tmp), "job-backend-queued")
            payload["backend"] = "torch_cpu"
            payload["request_hash"] = request_hash_for_payload(payload)

            with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
                with _GPU_CLASS_QUEUE_LOCK:
                    future = pool.submit(
                        handle_line_with_model_manager,
                        json.dumps(
                            {
                                "request_id": "req-backend-queued",
                                "command": COMMAND_INFER,
                                "payload": payload,
                            }
                        ),
                        None,
                    )
                    time.sleep(0.12)
            response, should_shutdown = future.result()

        self.assertFalse(should_shutdown)
        self.assertIs(response["ok"], False)
        self.assertEqual(response["payload"]["job_id"], "job-backend-queued")
        self.assertEqual(response["payload"]["requested_quality"], "high_1024")
        self.assertEqual(response["payload"]["effective_quality"], "high_1024")
        self.assertEqual(response["payload"]["oom"], "false")
        self.assertEqual(response["payload"]["downgraded_quality"], "false")
        self.assertEqual(response["payload"]["final_failure"], "true")
        self.assertGreaterEqual(int(response["payload"]["queue_time_ms"]), 80)

    def test_gpu_class_queue_serializes_stub_jobs_without_shared_state_contamination(self):
        temp_dirs = [
            tempfile.TemporaryDirectory(prefix=f"corridorkey-render-queue-{index}-")
            for index in range(4)
        ]
        qualities = ["draft_512", "high_1024", "full_2048", "high_1024"]
        screens = ["green", "blue", "auto", "green"]
        try:
            payloads = [
                infer_payload(
                    Path(temp_dirs[index].name),
                    f"job-queue-{index}",
                    quality=qualities[index],
                    screen_color=screens[index],
                    despill=str(3 + index),
                )
                for index in range(len(temp_dirs))
            ]
            with patched_env(
                CORRIDORKEY_TEST_FAULTS_ALLOWED="1",
                CORRIDORKEY_TEST_FAKE_INFER_DELAY_MS="80",
                CORRIDORKEY_TEST_SYNTHETIC_OOM_QUALITIES=None,
            ):
                start = time.monotonic()
                with concurrent.futures.ThreadPoolExecutor(max_workers=len(payloads)) as pool:
                    responses = list(
                        pool.map(
                            lambda item: handle_infer(item[1], f"req-queue-{item[0]}"),
                            enumerate(payloads),
                        )
                    )
                elapsed = time.monotonic() - start
        finally:
            for temp_dir in temp_dirs:
                temp_dir.cleanup()

        self.assertGreaterEqual(elapsed, 0.24)
        queue_times = []
        for index, response in enumerate(responses):
            self.assertIs(response["ok"], True)
            self.assertEqual(response["payload"]["job_id"], f"job-queue-{index}")
            self.assertEqual(response["payload"]["requested_quality"], qualities[index])
            self.assertEqual(response["payload"]["effective_quality"], qualities[index])
            self.assertEqual(response["payload"]["downgraded_quality"], "false")
            self.assertEqual(response["payload"]["final_failure"], "false")
            self.assertIn(f":{qualities[index]}:", response["payload"]["deterministic_key"])
            queue_times.append(int(response["payload"]["queue_time_ms"]))
        self.assertGreaterEqual(max(queue_times), 100)


if __name__ == "__main__":
    unittest.main()
