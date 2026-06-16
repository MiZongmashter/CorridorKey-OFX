import json
from contextlib import contextmanager
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from sidecar.corridorkey_sidecar.cache import InferenceCache, cache_key_for_payload
from sidecar.corridorkey_sidecar.protocol import COMMAND_INFER, request_hash_for_payload
from sidecar.corridorkey_sidecar.server import handle_line_with_cache


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


def infer_payload(
    root,
    *,
    frame_id="frame-10",
    bounds=(0, 0, 2, 1),
    source_values=None,
    alpha_values=None,
    alpha_hint_source="external",
    screen_color="green",
    quality="high_1024",
    input_color_space="host_managed",
    despill_strength="5",
    backend="stub",
    output_mode="processed_rgba",
    job_id="job-cache",
):
    source_path = root / "source.ckfb"
    alpha_path = root / "alpha.ckfb"
    write_blob(
        source_path,
        bounds,
        4,
        source_values
        or (
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
    write_blob(alpha_path, bounds, 1, alpha_values or (0.25, 0.75))
    payload = {
        "frame_id": frame_id,
        "job_id": job_id,
        "render_window_x1": str(bounds[0]),
        "render_window_y1": str(bounds[1]),
        "render_window_x2": str(bounds[2]),
        "render_window_y2": str(bounds[3]),
        "source_frame_blob_path": str(source_path),
        "alpha_hint_frame_blob_path": str(alpha_path),
        "alpha_hint_source": alpha_hint_source,
        "screen_color": screen_color,
        "quality": quality,
        "input_color_space": input_color_space,
        "despill_strength": despill_strength,
        "backend": backend,
        "output_mode": output_mode,
    }
    payload["request_hash"] = request_hash_for_payload(payload)
    return payload


def infer(cache, payload):
    response, should_shutdown = handle_line_with_cache(
        json.dumps(
            {
                "request_id": "req-cache",
                "command": COMMAND_INFER,
                "payload": payload,
            }
        ),
        cache,
    )
    assert not should_shutdown
    return response


class CacheTests(unittest.TestCase):
    def test_cache_hits_repeated_fake_inference_without_rehashing_paths(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-cache-hit-") as tmp:
            root = Path(tmp)
            cache = InferenceCache()
            payload = infer_payload(root)

            first = infer(cache, payload)
            second = infer(cache, payload)

            self.assertIs(first["ok"], True)
            self.assertIs(second["ok"], True)
            self.assertEqual(first["payload"]["cache"], "miss")
            self.assertEqual(second["payload"]["cache"], "hit")
            self.assertEqual(
                Path(first["payload"]["processed_rgba_frame_blob_path"]).read_bytes(),
                Path(second["payload"]["processed_rgba_frame_blob_path"]).read_bytes(),
            )

    def test_disabled_cache_reports_disabled_and_does_not_store(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-cache-disabled-") as tmp:
            root = Path(tmp)
            cache = InferenceCache(enabled=False)
            payload = infer_payload(root)

            first = infer(cache, payload)
            second = infer(cache, payload)

            self.assertEqual(first["payload"]["cache"], "disabled")
            self.assertEqual(second["payload"]["cache"], "disabled")
            cache.enabled = True
            self.assertEqual(infer(cache, payload)["payload"]["cache"], "miss")

    def test_input_blob_changes_report_input_invalidation(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-cache-input-") as tmp:
            root = Path(tmp)
            cache = InferenceCache()
            baseline = infer_payload(root)
            self.assertEqual(infer(cache, baseline)["payload"]["cache"], "miss")

            changed_source = infer_payload(
                root,
                source_values=(0.9, 0.1, 0.2, 1.0, 0.2, 0.8, 0.4, 0.5),
            )
            self.assertEqual(infer(cache, changed_source)["payload"]["cache"], "invalidated_input")

            changed_alpha = infer_payload(root, alpha_values=(0.1, 0.9))
            self.assertEqual(infer(cache, changed_alpha)["payload"]["cache"], "invalidated_input")

    def test_output_affecting_fields_report_parameter_invalidation(self):
        cases = {
            "screen_color": {"screen_color": "blue"},
            "quality": {"quality": "draft_512"},
            "input_color_space": {"input_color_space": "linear"},
            "despill_strength": {"despill_strength": "6"},
            "alpha_hint_source": {"alpha_hint_source": "source_alpha"},
            "output_mode": {"output_mode": "matte"},
        }

        for name, overrides in cases.items():
            with self.subTest(name=name):
                with tempfile.TemporaryDirectory(prefix=f"corridorkey-render-cache-{name}-") as tmp:
                    root = Path(tmp)
                    cache = InferenceCache()
                    baseline = infer_payload(root)
                    self.assertEqual(infer(cache, baseline)["payload"]["cache"], "miss")

                    changed = infer_payload(root, **overrides)
                    self.assertEqual(infer(cache, changed)["payload"]["cache"], "invalidated_params")

    def test_cache_key_fields_include_frame_hashes_model_backend_quality_and_params(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-cache-key-") as tmp:
            root = Path(tmp)
            payload = infer_payload(root)
            key = cache_key_for_payload(payload, model_version="stub-v1")

            self.assertEqual(key.frame, "frame-10")
            self.assertEqual(key.model_version, "stub-v1")
            self.assertEqual(key.backend, "stub")
            self.assertEqual(key.quality, "high_1024")
            self.assertEqual(len(key.source_hash), 16)
            self.assertEqual(len(key.alpha_hint_hash), 16)
            self.assertEqual(len(key.params_hash), 16)

            self.assertNotEqual(
                key,
                cache_key_for_payload(infer_payload(root, frame_id="frame-11"), "stub-v1"),
            )
            self.assertNotEqual(key, cache_key_for_payload(payload, model_version="stub-v2"))
            self.assertNotEqual(
                key,
                cache_key_for_payload(infer_payload(root, quality="full_2048"), "stub-v1"),
            )
            self.assertNotEqual(
                key,
                cache_key_for_payload(infer_payload(root, backend="torch_cpu"), "stub-v1"),
            )
            self.assertNotEqual(
                key,
                cache_key_for_payload(infer_payload(root, screen_color="blue"), "stub-v1"),
            )

    def test_backend_and_model_version_changes_are_parameter_invalidations(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-cache-backend-model-") as tmp:
            root = Path(tmp)
            cache = InferenceCache(model_version="stub-v1")
            self.assertEqual(infer(cache, infer_payload(root))["payload"]["cache"], "miss")

            backend_key = cache_key_for_payload(
                infer_payload(root, backend="torch_cpu"), model_version="stub-v1"
            )
            model_key = cache_key_for_payload(infer_payload(root), model_version="stub-v2")

            self.assertEqual(cache.diagnostic_for_key(backend_key), "invalidated_params")
            self.assertEqual(cache.diagnostic_for_key(model_key), "invalidated_params")

            cache.model_version = "stub-v2"
            self.assertEqual(infer(cache, infer_payload(root))["payload"]["cache"], "invalidated_params")

    def test_cache_hit_materializes_outputs_into_current_job_root(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-cache-root-a-") as tmp_a:
            with tempfile.TemporaryDirectory(prefix="corridorkey-render-cache-root-b-") as tmp_b:
                root_a = Path(tmp_a)
                root_b = Path(tmp_b)
                cache = InferenceCache()

                first = infer(cache, infer_payload(root_a, job_id="job-cache-a"))
                first_bytes = Path(first["payload"]["processed_rgba_frame_blob_path"]).read_bytes()
                second = infer(cache, infer_payload(root_b, job_id="job-cache-b"))

                self.assertEqual(second["payload"]["cache"], "hit")
                self.assertEqual(second["payload"]["job_id"], "job-cache-b")
                self.assertEqual(
                    Path(second["payload"]["processed_rgba_frame_blob_path"]).parent,
                    root_b,
                )
                self.assertEqual(
                    Path(second["payload"]["processed_rgba_frame_blob_path"]).read_bytes(),
                    first_bytes,
                )
                self.assertIn(
                    f":{request_hash_for_payload(infer_payload(root_b, job_id='job-cache-b'))}:",
                    second["payload"]["deterministic_key"],
                )

    def test_downgraded_oom_response_is_not_cached_as_requested_quality(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-cache-oom-") as tmp:
            root = Path(tmp)
            cache = InferenceCache()
            payload = infer_payload(root, quality="full_2048")

            with patched_env(
                CORRIDORKEY_TEST_FAULTS_ALLOWED="1",
                CORRIDORKEY_TEST_SYNTHETIC_OOM_QUALITIES="full_2048",
            ):
                downgraded = infer(cache, payload)
            recovered = infer(cache, payload)

            self.assertEqual(downgraded["payload"]["cache"], "miss")
            self.assertEqual(downgraded["payload"]["effective_quality"], "high_1024")
            self.assertEqual(downgraded["payload"]["downgraded_quality"], "true")
            self.assertEqual(recovered["payload"]["cache"], "miss")
            self.assertEqual(recovered["payload"]["effective_quality"], "full_2048")
            self.assertEqual(recovered["payload"]["downgraded_quality"], "false")

    def test_non_cached_oom_does_not_drive_later_invalidation_diagnostics(self):
        for name, payload_overrides in (
            ("params", {"quality": "high_1024"}),
            ("input", {"source_values": (0.9, 0.1, 0.2, 1.0, 0.2, 0.8, 0.4, 0.5)}),
        ):
            with self.subTest(name=name):
                with tempfile.TemporaryDirectory(prefix=f"corridorkey-render-cache-oom-{name}-") as tmp:
                    root = Path(tmp)
                    cache = InferenceCache()

                    with patched_env(
                        CORRIDORKEY_TEST_FAULTS_ALLOWED="1",
                        CORRIDORKEY_TEST_SYNTHETIC_OOM_QUALITIES="full_2048",
                    ):
                        downgraded = infer(cache, infer_payload(root, quality="full_2048"))
                    changed = infer(cache, infer_payload(root, **payload_overrides))

                    self.assertEqual(downgraded["payload"]["downgraded_quality"], "true")
                    self.assertEqual(changed["payload"]["cache"], "miss")

    def test_render_window_participates_in_cache_key(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-cache-window-") as tmp:
            root = Path(tmp)
            one_pixel = infer_payload(
                root,
                bounds=(0, 0, 1, 1),
                source_values=(0.20, 0.70, 0.10, 1.00),
                alpha_values=(0.25,),
            )
            two_pixels = infer_payload(root)

            self.assertNotEqual(
                cache_key_for_payload(one_pixel),
                cache_key_for_payload(two_pixels),
            )

    def test_cache_evicts_old_entries_to_bound_process_lifetime_growth(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-cache-evict-") as tmp:
            root = Path(tmp)
            cache = InferenceCache(max_entries=1)
            first = infer(cache, infer_payload(root, frame_id="frame-1"))
            second = infer(cache, infer_payload(root, frame_id="frame-2"))
            second_again = infer(cache, infer_payload(root, frame_id="frame-2"))
            first_again = infer(cache, infer_payload(root, frame_id="frame-1"))

            self.assertEqual(first["payload"]["cache"], "miss")
            self.assertEqual(second["payload"]["cache"], "miss")
            self.assertEqual(second_again["payload"]["cache"], "hit")
            self.assertEqual(first_again["payload"]["cache"], "miss")

    def test_missing_cached_blob_recomputes_instead_of_returning_internal_error(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-cache-missing-") as tmp:
            root = Path(tmp)
            cache = InferenceCache()
            first = infer(cache, infer_payload(root))
            Path(first["payload"]["processed_rgba_frame_blob_path"]).unlink()

            second = infer(cache, infer_payload(root))

            self.assertIs(second["ok"], True)
            self.assertEqual(second["payload"]["cache"], "miss")

    def test_straight_fg_cache_hit_materializes_without_processed_blob(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-cache-sfg-a-") as tmp_a:
            with tempfile.TemporaryDirectory(prefix="corridorkey-render-cache-sfg-b-") as tmp_b:
                root_a = Path(tmp_a)
                root_b = Path(tmp_b)
                cache = InferenceCache()

                first = infer(cache, infer_payload(root_a, output_mode="straight_fg"))
                second = infer(cache, infer_payload(root_b, output_mode="straight_fg"))

                self.assertIs(first["ok"], True)
                self.assertNotIn("processed_rgba_frame_blob_path", first["payload"])
                self.assertEqual(second["payload"]["cache"], "hit")
                self.assertNotIn("processed_rgba_frame_blob_path", second["payload"])
                self.assertEqual(
                    Path(second["payload"]["straight_fg_frame_blob_path"]).parent,
                    root_b,
                )
                self.assertEqual(
                    Path(second["payload"]["alpha_frame_blob_path"]).parent,
                    root_b,
                )

    def test_stub_cache_miss_uses_fake_infer_without_backend_adapter(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-render-cache-stub-backend-") as tmp:
            root = Path(tmp)
            cache = InferenceCache()

            with patch(
                "sidecar.corridorkey_sidecar.server._handle_backend_request",
                side_effect=AssertionError("stub infer must not use backend adapter"),
            ):
                response = infer(cache, infer_payload(root))

            self.assertIs(response["ok"], True)
            self.assertEqual(response["payload"]["backend"], "stub")
            self.assertEqual(response["payload"]["model"], "stub")
            self.assertEqual(response["payload"]["cache"], "miss")

    def test_status_reports_cache_enabled_when_wrapper_owns_cache(self):
        response, should_shutdown = handle_line_with_cache(
            json.dumps({"request_id": "req-cache-status", "command": "status", "payload": {}}),
            InferenceCache(),
        )

        self.assertFalse(should_shutdown)
        self.assertIs(response["ok"], True)
        self.assertEqual(response["payload"]["cache"], "enabled")


if __name__ == "__main__":
    unittest.main()
