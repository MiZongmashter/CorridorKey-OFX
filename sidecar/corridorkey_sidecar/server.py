"""Stdio server for the CorridorKey sidecar protocol."""

import contextlib
import io
import json
import os
from pathlib import Path
import socket
import sys
import tempfile
import threading
import time

from .cache import InferenceCache
from .model_manager import ModelManager
from .protocol import (
    COMMAND_INFER,
    COMMAND_WARMUP,
    ERROR_INTERNAL,
    ProtocolError,
    _diagnostic_fields,
    _with_backend_queue,
    decode_request,
    encode_response,
    handle_line,
    handle_request,
    make_error,
    make_response,
)
from .redaction import redact_value
from .runtime_config import RuntimeConfig


def make_log_line(level, event, **fields):
    record = {
        "level": level,
        "event": event,
    }
    record.update(redact_value(fields))
    return json.dumps(record, allow_nan=False, separators=(",", ":"), sort_keys=True)


def write_log(errstream, level, event, **fields):
    try:
        line = make_log_line(level, event, **fields)
    except Exception:
        line = json.dumps({"event": "log_encode_failed", "level": "error"}, separators=(",", ":"), sort_keys=True)
    try:
        errstream.write(line + "\n")
        errstream.flush()
    except Exception:
        return False
    return True


def _readline(instream):
    if hasattr(instream, "readline"):
        return instream.readline()
    return next(instream)


def _test_faults_allowed():
    return os.environ.get("CORRIDORKEY_TEST_FAULTS_ALLOWED") == "1"


def _request_summary(line):
    try:
        request = json.loads(line)
    except Exception:
        return {"command": None, "request_id": None, "payload": {}}
    if not isinstance(request, dict):
        return {"command": None, "request_id": None, "payload": {}}
    command = request.get("command")
    request_id = request.get("request_id")
    payload = request.get("payload")
    return {
        "command": command if isinstance(command, str) else None,
        "request_id": request_id if isinstance(request_id, str) else None,
        "payload": payload if isinstance(payload, dict) else {},
    }


def _response_job_id(response):
    payload = response.get("payload", {})
    return payload.get("job_id", "") if isinstance(payload, dict) else ""


def _log_response(errstream, response):
    if response["ok"]:
        payload = response["payload"]
        write_log(
            errstream,
            "info",
            "request_handled",
            job_id=payload.get("job_id", ""),
            download_status=payload.get("download_status", ""),
            download_progress=payload.get("download_progress", ""),
            queue_time_ms=payload.get("queue_time_ms", ""),
            effective_quality=payload.get("effective_quality", ""),
            oom=payload.get("oom", ""),
            downgraded_quality=payload.get("downgraded_quality", ""),
            final_failure=payload.get("final_failure", ""),
        )
    elif response["error"] and response["error"].get("code") == "cancelled":
        write_log(
            errstream,
            "info",
            "request_cancelled",
            job_id=_response_job_id(response),
        )
    else:
        payload = response["payload"]
        write_log(
            errstream,
            "error",
            "protocol_error",
            error=response["error"],
            job_id=payload.get("job_id", ""),
            download_status=payload.get("download_status", ""),
            download_progress=payload.get("download_progress", ""),
            queue_time_ms=payload.get("queue_time_ms", ""),
            effective_quality=payload.get("effective_quality", ""),
            oom=payload.get("oom", ""),
            downgraded_quality=payload.get("downgraded_quality", ""),
            final_failure=payload.get("final_failure", ""),
        )


def _write_response(outstream, errstream, response, write_lock=None):
    try:
        if write_lock is None:
            outstream.write(encode_response(response) + "\n")
            outstream.flush()
        else:
            with write_lock:
                outstream.write(encode_response(response) + "\n")
                outstream.flush()
    except Exception:
        try:
            write_log(errstream, "error", "sidecar_io_error")
        except Exception:
            pass
        return False
    _log_response(errstream, response)
    return True


def _download_worker(line, cache, model_manager, outstream, errstream, write_lock):
    try:
        response, _ = handle_line_with_cache(line, cache, model_manager, errstream)
    except Exception:
        response = make_response(
            None,
            False,
            {},
            make_error(ERROR_INTERNAL, "Internal sidecar error"),
        )
    _write_response(outstream, errstream, response, write_lock)


def _start_download_worker(line, cache, model_manager, outstream, errstream, write_lock):
    thread = threading.Thread(
        target=_download_worker,
        args=(line, cache, model_manager, outstream, errstream, write_lock),
        daemon=True,
    )
    thread.start()
    return thread


def _handle_test_fault(line, outstream, errstream):
    if not _test_faults_allowed():
        return None

    summary = _request_summary(line)
    mode = os.environ.get("CORRIDORKEY_TEST_SERVER_MODE", "")
    command = summary["command"]
    if mode == "invalid_json_on_health" and command == "health":
        write_log(errstream, "error", "test_fault", mode=mode)
        outstream.write("{not-json}\n")
        outstream.flush()
        return "continue"
    if mode == "error_on_status" and command == "status":
        write_log(errstream, "error", "test_fault", mode=mode)
        outstream.write(
            encode_response(
                make_response(
                    summary["request_id"],
                    False,
                    {},
                    make_error("test_status_error", "Status failed for test"),
                )
            )
            + "\n"
        )
        outstream.flush()
        return "continue"
    if mode == "path_error_on_infer" and command == "infer":
        write_log(errstream, "error", "test_fault", mode=mode)
        outstream.write(
            encode_response(
                make_response(
                    summary["request_id"],
                    False,
                    {"job_id": summary["payload"].get("job_id", "")},
                    make_error(
                        "test_infer_path_error",
                        "Failed reading /Users/alice/SecretShow/shot010/plate.exr",
                    ),
                )
            )
            + "\n"
        )
        outstream.flush()
        return "continue"
    if mode == "path_error_on_warmup" and command == "warmup":
        write_log(errstream, "error", "test_fault", mode=mode)
        outstream.write(
            encode_response(
                make_response(
                    summary["request_id"],
                    False,
                    {"job_id": summary["payload"].get("job_id", "")},
                    make_error(
                        "test_warmup_path_error",
                        "Failed reading /Users/alice/SecretShow/shot010/plate.exr",
                    ),
                )
            )
            + "\n"
        )
        outstream.flush()
        return "continue"
    if mode == "hang_on_health" and command == "health":
        write_log(errstream, "error", "test_fault", mode=mode)
        time.sleep(3600)
        return 2
    if mode == "check_inherited_fd" and command == "health":
        fd_text = os.environ.get("CORRIDORKEY_TEST_EXPECT_CLOSED_FD", "")
        inherited = False
        if fd_text:
            try:
                socket.fromfd(int(fd_text), socket.AF_INET, socket.SOCK_STREAM).close()
                inherited = True
            except OSError:
                inherited = False
        write_log(errstream, "error", "test_fault", mode=mode)
        outstream.write(
            encode_response(
                make_response(
                    summary["request_id"],
                    not inherited,
                    {"fd_inherited": "true" if inherited else "false"},
                    None
                    if not inherited
                    else make_error("fd_inherited", "Sidecar inherited an unsafe host fd"),
                )
            )
            + "\n"
        )
        outstream.flush()
        return "continue"
    if mode == "report_cwd" and command == "health":
        write_log(errstream, "error", "test_fault", mode=mode)
        outstream.write(
            encode_response(
                make_response(
                    summary["request_id"],
                    True,
                    {"cwd": os.getcwd()},
                    None,
                )
            )
            + "\n"
        )
        outstream.flush()
        return "continue"
    if mode == "exit_once_on_status" and command == "status":
        marker = os.environ.get("CORRIDORKEY_TEST_MARKER_PATH")
        if marker:
            marker_path = Path(marker)
            if not marker_path.exists():
                marker_path.write_text("exited\n", encoding="utf-8")
                write_log(errstream, "error", "test_fault", mode=mode)
                return 33
    if mode == "split_warmup_response" and command == "warmup":
        payload = summary["payload"]
        response = encode_response(
            make_response(
                summary["request_id"],
                True,
                {
                    "job_id": payload.get("job_id", ""),
                    "backend": payload.get("backend", "stub"),
                    "model": "stub",
                    "warmup": "ready",
                    "quality": payload.get("quality", "high_1024"),
                    "timing_warmup_ms": "0",
                },
                None,
            )
        )
        split_at = max(1, len(response) // 2)
        write_log(errstream, "error", "test_fault", mode=mode)
        outstream.write(response[:split_at])
        outstream.flush()
        time.sleep(0.12)
        outstream.write(response[split_at:] + "\n")
        outstream.flush()
        return "continue"
    return None


def main(instream=None, outstream=None, errstream=None):
    instream = sys.stdin if instream is None else instream
    outstream = sys.stdout if outstream is None else outstream
    errstream = sys.stderr if errstream is None else errstream
    cache = InferenceCache()
    if os.environ.get("CORRIDORKEY_MODEL_ROOT") and not os.environ.get("CORRIDORKEY_MODEL_DIR"):
        model_root = Path(os.environ["CORRIDORKEY_MODEL_ROOT"])
    else:
        model_root = RuntimeConfig.from_env().model_dir
    model_manager = ModelManager(model_root)
    async_download = None
    write_lock = threading.Lock()

    write_log(errstream, "info", "sidecar_started")
    if _test_faults_allowed() and os.environ.get("CORRIDORKEY_TEST_SERVER_MODE") == "exit_after_start":
        write_log(errstream, "error", "test_fault", mode="exit_after_start")
        return 44

    while True:
        try:
            line = _readline(instream)
        except StopIteration:
            break
        except Exception:
            write_log(errstream, "error", "sidecar_io_error")
            return 1
        if line == "":
            break

        if async_download is not None and not async_download.is_alive():
            async_download.join()
            async_download = None

        fault_result = _handle_test_fault(line, outstream, errstream)
        if fault_result == "continue":
            continue
        if isinstance(fault_result, int):
            return fault_result

        summary = _request_summary(line)
        if summary["command"] == "download_model" and async_download is None:
            async_download = _start_download_worker(
                line, cache, model_manager, outstream, errstream, write_lock
            )
            continue
        if async_download is not None and summary["command"] not in (
            "cancel",
            "cancel_download",
            "health",
            "model_status",
            "status",
        ):
            response = make_response(
                summary["request_id"],
                False,
                {},
                make_error("busy", "download request is still running"),
            )
            if not _write_response(outstream, errstream, response, write_lock):
                return 1
            continue

        try:
            response, should_shutdown = handle_line_with_cache(line, cache, model_manager, errstream)
        except Exception:
            response = make_response(
                None,
                False,
                {},
                make_error(ERROR_INTERNAL, "Internal sidecar error"),
            )
            should_shutdown = False
            write_log(errstream, "error", "internal_error")

        if not _write_response(outstream, errstream, response, write_lock):
            return 1

        if should_shutdown:
            write_log(errstream, "info", "sidecar_shutdown")
            return 0

    if async_download is not None:
        async_download.join()

    write_log(errstream, "info", "sidecar_stdin_closed")
    return 0


def handle_line_with_cache(line, cache, model_manager=None, errstream=None):
    if cache is None:
        return handle_line(line)

    try:
        request = decode_request(line)
    except ProtocolError:
        return handle_line(line)

    if request["command"] in ("health", "status"):
        response, should_shutdown = handle_line_with_model_manager(
            line, model_manager, errstream
        )
        if response["ok"]:
            response["payload"]["cache"] = "enabled" if cache.enabled else "disabled"
        return response, should_shutdown

    if request["command"] != COMMAND_INFER or request["payload"].get("backend") != "stub":
        return handle_line_with_model_manager(line, model_manager, errstream)

    try:
        diagnostic, cached_payload, key = cache.lookup(request["payload"])
    except OSError:
        return handle_line_with_model_manager(line, model_manager, errstream)
    if cached_payload is not None:
        return make_response(request["request_id"], True, cached_payload, None), False

    response, should_shutdown = handle_line_with_model_manager(
        line, model_manager, errstream
    )
    if not response["ok"]:
        return response, should_shutdown

    response["payload"]["cache"] = diagnostic
    cache.store(key, response["payload"])
    return response, should_shutdown


def handle_line_with_model_manager(line, model_manager, errstream=None):
    try:
        request = decode_request(line)
        if _is_backend_request(request):
            backend_response = _handle_backend_request(request, model_manager, errstream)
            if backend_response is not None:
                return backend_response, False
        try:
            return handle_request(request, model_manager)
        except ProtocolError as exc:
            exc.request_id = request["request_id"]
            raise
    except ProtocolError:
        return handle_line(line)


def _is_backend_request(request):
    return (
        request["command"] in (COMMAND_INFER, COMMAND_WARMUP)
        and request["payload"].get("backend") != "stub"
    )


def _handle_backend_request(request, model_manager, errstream=None):
    def run_backend(queue_time_ms):
        response = _call_backend_request(request, model_manager, errstream)
        if request["command"] == COMMAND_INFER:
            payload = response.get("payload", {})
            if isinstance(payload, dict):
                diagnostics = _diagnostic_fields(
                    request["payload"].get("quality", ""),
                    payload.get("effective_quality") or request["payload"].get("quality", ""),
                    queue_time_ms,
                    False,
                    False,
                    not response.get("ok", False),
                )
                for key, value in diagnostics.items():
                    payload.setdefault(key, value)
        return response

    return _with_backend_queue(run_backend)


def _log_backend_stdout(captured_stdout, errstream):
    if errstream is None:
        return
    for line in captured_stdout.getvalue().splitlines():
        if line.strip():
            write_log(errstream, "warning", "backend_stdout", message=line)


def _stream_fileno(stream):
    try:
        return stream.fileno()
    except (AttributeError, io.UnsupportedOperation, OSError, ValueError):
        return None


def _flush_stream(stream):
    try:
        stream.flush()
    except Exception:
        pass


def _stdout_fd():
    for stream in (sys.stdout, getattr(sys, "__stdout__", None)):
        fd = _stream_fileno(stream)
        if fd is not None:
            return fd
    return None


@contextlib.contextmanager
def _capture_backend_stdout(captured_stdout):
    stdout_fd = _stdout_fd()
    if stdout_fd is None:
        with contextlib.redirect_stdout(captured_stdout):
            yield captured_stdout
        return

    _flush_stream(sys.stdout)
    _flush_stream(getattr(sys, "__stdout__", None))

    saved_stdout_fd = None
    fd_capture = None
    stdout_redirected = False
    try:
        saved_stdout_fd = os.dup(stdout_fd)
        fd_capture = tempfile.TemporaryFile(mode="w+b")
        os.dup2(fd_capture.fileno(), stdout_fd)
        stdout_redirected = True
    except OSError:
        if saved_stdout_fd is not None:
            os.close(saved_stdout_fd)
        if fd_capture is not None:
            fd_capture.close()
        with contextlib.redirect_stdout(captured_stdout):
            yield captured_stdout
        return

    try:
        try:
            with contextlib.redirect_stdout(captured_stdout):
                yield captured_stdout
        finally:
            _flush_stream(sys.stdout)
            _flush_stream(getattr(sys, "__stdout__", None))
            if stdout_redirected:
                os.dup2(saved_stdout_fd, stdout_fd)
            os.close(saved_stdout_fd)
            fd_capture.flush()
            fd_capture.seek(0)
            captured_stdout.write(fd_capture.read().decode("utf-8", "replace"))
    finally:
        fd_capture.close()


def _call_backend_request(request, model_manager, errstream=None):
    captured_stdout = io.StringIO()
    try:
        with _capture_backend_stdout(captured_stdout):
            from .backends.torch_backend import handle_backend_request
    except Exception:
        _log_backend_stdout(captured_stdout, errstream)
        return make_response(
            request.get("request_id"),
            False,
            {
                "job_id": request.get("payload", {}).get("job_id", ""),
                "backend": request.get("payload", {}).get("backend", ""),
                "backend_status": "failed",
                "gpu_backends_enabled": "false",
            },
            make_error(ERROR_INTERNAL, "Backend adapter could not be loaded"),
        )
    _log_backend_stdout(captured_stdout, errstream)
    try:
        captured_stdout = io.StringIO()
        with _capture_backend_stdout(captured_stdout):
            return handle_backend_request(request, model_manager)
    finally:
        _log_backend_stdout(captured_stdout, errstream)


if __name__ == "__main__":
    raise SystemExit(main())
