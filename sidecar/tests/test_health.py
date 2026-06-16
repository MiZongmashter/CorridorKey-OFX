import builtins
import json
import io
import os
import queue
import subprocess
import sys
import tempfile
import threading
import time
import types
import unittest
from unittest import mock
from pathlib import Path

from sidecar.corridorkey_sidecar.server import (
    _call_backend_request,
    handle_line_with_model_manager,
    main,
)
from sidecar.corridorkey_sidecar.protocol import make_response, request_hash_for_payload
from sidecar.tests.test_model_download import file_url, patched_env, write_fixture_package


class SidecarServerTests(unittest.TestCase):
    def test_health_liveness_does_not_probe_model_status(self):
        class BlockingModelManager:
            def status(self):
                raise AssertionError("health must not scan model status")

        response, should_shutdown = handle_line_with_model_manager(
            json.dumps(
                {
                    "request_id": "req-health-liveness",
                    "command": "health",
                    "payload": {},
                }
            ),
            model_manager=BlockingModelManager(),
        )

        self.assertFalse(should_shutdown)
        self.assertEqual(response["request_id"], "req-health-liveness")
        self.assertIs(response["ok"], True)
        self.assertEqual(response["payload"]["backend"], "stub")
        self.assertNotIn("model_status", response["payload"])

    def test_non_backend_commands_do_not_probe_backend_adapter(self):
        requests = [
            {"request_id": "req-nonbackend-health", "command": "health", "payload": {}},
            {"request_id": "req-nonbackend-status", "command": "status", "payload": {}},
            {
                "request_id": "req-nonbackend-model-status",
                "command": "model_status",
                "payload": {},
            },
            {
                "request_id": "req-nonbackend-cancel",
                "command": "cancel",
                "payload": {"job_id": "job-nonbackend"},
            },
            {
                "request_id": "req-nonbackend-cancel-download",
                "command": "cancel_download",
                "payload": {"job_id": "job-nonbackend"},
            },
            {"request_id": "req-nonbackend-shutdown", "command": "shutdown", "payload": {}},
        ]

        with mock.patch(
            "sidecar.corridorkey_sidecar.server._handle_backend_request",
            side_effect=AssertionError("backend adapter should not handle protocol commands"),
        ):
            responses = [
                handle_line_with_model_manager(json.dumps(request), model_manager=None)
                for request in requests
            ]

        self.assertTrue(all(response[0]["ok"] for response in responses))
        self.assertTrue(responses[-1][1])

    def test_server_stdout_is_protocol_only_and_stderr_is_structured(self):
        requests = "\n".join(
            [
                json.dumps(
                    {
                        "request_id": "req-health",
                        "command": "health",
                        "payload": {},
                    }
                ),
                json.dumps(
                    {
                        "request_id": "req-status",
                        "command": "status",
                        "payload": {},
                    }
                ),
                json.dumps(
                    {
                        "request_id": "req-shutdown",
                        "command": "shutdown",
                        "payload": {},
                    }
                ),
            ]
        )

        completed = subprocess.run(
            [sys.executable, "-m", "sidecar.corridorkey_sidecar.server"],
            input=requests + "\n",
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        stdout_lines = completed.stdout.splitlines()
        self.assertEqual(len(stdout_lines), 3)
        responses = [json.loads(line) for line in stdout_lines]
        self.assertEqual(
            [response["request_id"] for response in responses],
            ["req-health", "req-status", "req-shutdown"],
        )
        self.assertTrue(all("level" not in response for response in responses))
        self.assertTrue(all("event" not in response for response in responses))
        self.assertTrue(all(set(response) == {"request_id", "ok", "payload", "error"} for response in responses))
        self.assertNotIn("model_status", responses[0]["payload"])
        self.assertIn("model_status", responses[1]["payload"])

        stderr_lines = completed.stderr.splitlines()
        self.assertGreaterEqual(len(stderr_lines), 2)
        logs = [json.loads(line) for line in stderr_lines]
        self.assertTrue(all("level" in entry for entry in logs))
        self.assertTrue(all("event" in entry for entry in logs))
        self.assertTrue(all("request_id" not in entry for entry in logs))

    def test_backend_stdout_is_redirected_to_structured_stderr(self):
        def noisy_backend(request, model_manager):
            print("Resizing encoder.model.pos_embed from torch checkpoint")
            os.write(1, b"native stdout from backend\n")
            return make_response(
                request["request_id"],
                True,
                {
                    "job_id": request["payload"]["job_id"],
                    "backend": "torch_cpu",
                    "backend_status": "ready",
                    "quality": request["payload"]["quality"],
                },
                None,
            )

        with tempfile.TemporaryDirectory(prefix="corridorkey-render-test-") as tmp:
            payload = {
                "frame_id": "frame-noisy-backend",
                "job_id": "job-noisy-backend",
                "render_window_x1": "0",
                "render_window_y1": "0",
                "render_window_x2": "1",
                "render_window_y2": "1",
                "source_frame_blob_path": str(Path(tmp) / "source.ckfb"),
                "alpha_hint_frame_blob_path": str(Path(tmp) / "alpha_hint.ckfb"),
                "alpha_hint_source": "external",
                "screen_color": "auto",
                "quality": "high_1024",
                "input_color_space": "host_managed",
                "despill_strength": "5",
                "backend": "auto",
                "output_mode": "processed_rgba",
            }
            payload["request_hash"] = request_hash_for_payload(payload)
            requests = "\n".join(
                [
                    json.dumps(
                        {
                            "request_id": "req-noisy-backend",
                            "command": "infer",
                            "payload": payload,
                        }
                    ),
                    json.dumps(
                        {
                            "request_id": "req-shutdown",
                            "command": "shutdown",
                            "payload": {},
                        }
                    ),
                ]
            )
            outstream = io.StringIO()
            errstream = io.StringIO()

            with mock.patch(
                "sidecar.corridorkey_sidecar.backends.torch_backend.handle_backend_request",
                side_effect=noisy_backend,
            ):
                result = main(
                    instream=io.StringIO(requests + "\n"),
                    outstream=outstream,
                    errstream=errstream,
                )

        self.assertEqual(result, 0)
        self.assertNotIn("Resizing encoder", outstream.getvalue())
        responses = [json.loads(line) for line in outstream.getvalue().splitlines()]
        self.assertEqual(
            [response["request_id"] for response in responses],
            ["req-noisy-backend", "req-shutdown"],
        )
        self.assertTrue(all(set(response) == {"request_id", "ok", "payload", "error"} for response in responses))

        logs = [json.loads(line) for line in errstream.getvalue().splitlines()]
        stdout_logs = [entry for entry in logs if entry.get("event") == "backend_stdout"]
        messages = [entry["message"] for entry in stdout_logs]
        self.assertIn("Resizing encoder.model.pos_embed from torch checkpoint", messages)
        self.assertIn("native stdout from backend", messages)

    def test_backend_import_fd_stdout_is_redirected_to_structured_stderr(self):
        original_import = builtins.__import__

        def fake_import(name, globals=None, locals=None, fromlist=(), level=0):
            if name.endswith("torch_backend"):
                os.write(1, b"native stdout during backend import\n")

                def handle_backend_request(request, model_manager):
                    os.write(1, b"native stdout during warmup handler\n")
                    return make_response(
                        request["request_id"],
                        True,
                        {
                            "job_id": request["payload"]["job_id"],
                            "backend": "torch_cpu",
                            "backend_status": "ready",
                        },
                        None,
                    )

                return types.SimpleNamespace(
                    handle_backend_request=handle_backend_request
                )
            return original_import(name, globals, locals, fromlist, level)

        errstream = io.StringIO()
        try:
            builtins.__import__ = fake_import
            response = _call_backend_request(
                {
                    "request_id": "req-import-noise",
                    "command": "warmup",
                    "payload": {
                        "job_id": "job-import-noise",
                        "backend": "auto",
                        "quality": "high_1024",
                    },
                },
                model_manager=None,
                errstream=errstream,
            )
        finally:
            builtins.__import__ = original_import

        self.assertIs(response["ok"], True)
        logs = [json.loads(line) for line in errstream.getvalue().splitlines()]
        self.assertIn(
            "native stdout during backend import",
            [entry.get("message") for entry in logs if entry.get("event") == "backend_stdout"],
        )
        self.assertIn(
            "native stdout during warmup handler",
            [entry.get("message") for entry in logs if entry.get("event") == "backend_stdout"],
        )

    def test_invalid_request_id_does_not_leak_to_stdout_or_stderr(self):
        requests = "\n".join(
            [
                json.dumps(
                    {
                        "request_id": {"projectName": "SecretShow"},
                        "command": "health",
                        "payload": {},
                    }
                ),
                json.dumps(
                    {
                        "request_id": "req-shutdown",
                        "command": "shutdown",
                        "payload": {},
                    }
                ),
            ]
        )

        completed = subprocess.run(
            [sys.executable, "-m", "sidecar.corridorkey_sidecar.server"],
            input=requests + "\n",
            text=True,
            capture_output=True,
            timeout=5,
            check=False,
        )

        self.assertEqual(completed.returncode, 0, completed.stderr)
        combined_output = completed.stdout + completed.stderr
        self.assertNotIn("SecretShow", combined_output)
        responses = [json.loads(line) for line in completed.stdout.splitlines()]
        self.assertIsNone(responses[0]["request_id"])
        self.assertIs(responses[0]["ok"], False)

    def test_server_io_failure_writes_structured_stderr_without_traceback(self):
        class BrokenWriter:
            def write(self, text):
                raise BrokenPipeError("broken /Users/alice/SecretShow/plate.exr")

            def flush(self):
                raise BrokenPipeError("broken /Users/alice/SecretShow/plate.exr")

        errstream = io.StringIO()

        result = main(
            instream=io.StringIO(
                json.dumps(
                    {
                        "request_id": "req-health",
                        "command": "health",
                        "payload": {},
                    }
                )
                + "\n"
            ),
            outstream=BrokenWriter(),
            errstream=errstream,
        )

        self.assertEqual(result, 1)
        stderr_text = errstream.getvalue()
        self.assertNotIn("Traceback", stderr_text)
        self.assertNotIn("SecretShow", stderr_text)
        logs = [json.loads(line) for line in stderr_text.splitlines()]
        self.assertEqual(logs[-1]["event"], "sidecar_io_error")

    def test_stderr_failure_does_not_escape_main(self):
        class BrokenErr:
            def write(self, text):
                raise BrokenPipeError("stderr closed")

            def flush(self):
                raise BrokenPipeError("stderr closed")

        outstream = io.StringIO()

        result = main(
            instream=io.StringIO(
                json.dumps(
                    {
                        "request_id": "req-shutdown",
                        "command": "shutdown",
                        "payload": {},
                    }
                )
                + "\n"
            ),
            outstream=outstream,
            errstream=BrokenErr(),
        )

        self.assertEqual(result, 0)
        response = json.loads(outstream.getvalue().strip())
        self.assertEqual(response["request_id"], "req-shutdown")
        self.assertIs(response["ok"], True)

    def test_stdin_failure_writes_structured_stderr_without_traceback(self):
        class BrokenInput:
            def readline(self):
                raise OSError("stdin failed /Users/alice/SecretShow/plate.exr")

        errstream = io.StringIO()

        result = main(instream=BrokenInput(), outstream=io.StringIO(), errstream=errstream)

        self.assertEqual(result, 1)
        stderr_text = errstream.getvalue()
        self.assertNotIn("Traceback", stderr_text)
        self.assertNotIn("SecretShow", stderr_text)
        logs = [json.loads(line) for line in stderr_text.splitlines()]
        self.assertEqual(logs[-1]["event"], "sidecar_io_error")

    def test_fault_modes_require_explicit_test_gate(self):
        previous_mode = os.environ.get("CORRIDORKEY_TEST_SERVER_MODE")
        previous_allowed = os.environ.get("CORRIDORKEY_TEST_FAULTS_ALLOWED")
        os.environ["CORRIDORKEY_TEST_SERVER_MODE"] = "invalid_json_on_health"
        os.environ.pop("CORRIDORKEY_TEST_FAULTS_ALLOWED", None)
        try:
            outstream = io.StringIO()
            result = main(
                instream=io.StringIO(
                    json.dumps(
                        {
                            "request_id": "req-health",
                            "command": "health",
                            "payload": {},
                        }
                    )
                    + "\n"
                ),
                outstream=outstream,
                errstream=io.StringIO(),
            )
        finally:
            if previous_mode is None:
                os.environ.pop("CORRIDORKEY_TEST_SERVER_MODE", None)
            else:
                os.environ["CORRIDORKEY_TEST_SERVER_MODE"] = previous_mode
            if previous_allowed is None:
                os.environ.pop("CORRIDORKEY_TEST_FAULTS_ALLOWED", None)
            else:
                os.environ["CORRIDORKEY_TEST_FAULTS_ALLOWED"] = previous_allowed

        self.assertEqual(result, 0)
        response = json.loads(outstream.getvalue().strip())
        self.assertIs(response["ok"], True)

    def test_authorized_fault_mode_can_emit_invalid_json(self):
        previous_mode = os.environ.get("CORRIDORKEY_TEST_SERVER_MODE")
        previous_allowed = os.environ.get("CORRIDORKEY_TEST_FAULTS_ALLOWED")
        os.environ["CORRIDORKEY_TEST_SERVER_MODE"] = "invalid_json_on_health"
        os.environ["CORRIDORKEY_TEST_FAULTS_ALLOWED"] = "1"
        try:
            outstream = io.StringIO()
            result = main(
                instream=io.StringIO(
                    json.dumps(
                        {
                            "request_id": "req-health",
                            "command": "health",
                            "payload": {},
                        }
                    )
                    + "\n"
                ),
                outstream=outstream,
                errstream=io.StringIO(),
            )
        finally:
            if previous_mode is None:
                os.environ.pop("CORRIDORKEY_TEST_SERVER_MODE", None)
            else:
                os.environ["CORRIDORKEY_TEST_SERVER_MODE"] = previous_mode
            if previous_allowed is None:
                os.environ.pop("CORRIDORKEY_TEST_FAULTS_ALLOWED", None)
            else:
                os.environ["CORRIDORKEY_TEST_FAULTS_ALLOWED"] = previous_allowed

        self.assertEqual(result, 0)
        self.assertEqual(outstream.getvalue(), "{not-json}\n")

    def test_server_reads_cancel_while_download_is_running(self):
        class QueueInput:
            def __init__(self):
                self.lines = queue.Queue()

            def readline(self):
                return self.lines.get()

            def put(self, request):
                self.lines.put(json.dumps(request) + "\n")

            def close(self):
                self.lines.put("")

        with tempfile.TemporaryDirectory() as tmp:
            package = Path(tmp) / "package"
            write_fixture_package(package, payload=b"0123456789abcdef" * 40000)
            model_root = Path(tmp) / "installed"
            instream = QueueInput()
            outstream = io.StringIO()
            errstream = io.StringIO()

            with patched_env(
                CORRIDORKEY_MODEL_ROOT=str(model_root),
                CORRIDORKEY_TEST_FAULTS_ALLOWED="1",
                CORRIDORKEY_TEST_DOWNLOAD_CHUNK_DELAY_MS="5",
            ):
                thread = threading.Thread(
                    target=main,
                    kwargs={
                        "instream": instream,
                        "outstream": outstream,
                        "errstream": errstream,
                    },
                )
                thread.start()
                instream.put(
                    {
                        "request_id": "req-download-running",
                        "command": "download_model",
                        "payload": {
                            "job_id": "job-download-running",
                            "manifest_url": file_url(package / "model-manifest.json"),
                        },
                    }
                )
                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline:
                    part_files = list((model_root / ".downloads").glob("**/*.part"))
                    if part_files and part_files[0].stat().st_size > 0:
                        break
                    time.sleep(0.01)
                instream.put(
                    {
                        "request_id": "req-status-running",
                        "command": "model_status",
                        "payload": {},
                    }
                )
                instream.put(
                    {
                        "request_id": "req-cancel-running",
                        "command": "cancel",
                        "payload": {"job_id": "job-download-running"},
                    }
                )
                instream.close()
                thread.join(timeout=4)

            self.assertFalse(thread.is_alive())
            responses = [json.loads(line) for line in outstream.getvalue().splitlines()]
            self.assertEqual(
                [response["request_id"] for response in responses],
                ["req-status-running", "req-cancel-running", "req-download-running"],
            )
            self.assertIs(responses[0]["ok"], True)
            self.assertEqual(responses[0]["payload"]["download_status"], "downloading")
            self.assertIs(responses[1]["ok"], True)
            self.assertEqual(responses[1]["payload"]["cancel"], "accepted")
            self.assertIs(responses[2]["ok"], False)
            self.assertEqual(responses[2]["error"]["code"], "cancelled")
            self.assertEqual(responses[2]["payload"]["download_status"], "cancelled")


if __name__ == "__main__":
    unittest.main()
