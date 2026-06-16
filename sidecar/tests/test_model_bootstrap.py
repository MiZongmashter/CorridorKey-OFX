import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
BOOTSTRAP = REPO_ROOT / "scripts" / "download_corridorkey_models.py"

spec = importlib.util.spec_from_file_location("download_corridorkey_models", BOOTSTRAP)
download_corridorkey_models = importlib.util.module_from_spec(spec)
spec.loader.exec_module(download_corridorkey_models)


class ModelBootstrapTests(unittest.TestCase):
    def test_install_mlx_model_writes_green_mlx_manifest(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-model-bootstrap-") as tmp:
            root = Path(tmp)
            source = root / "green-mlx-model.safetensors"
            source.write_bytes(b"mlx checkpoint bytes")
            model_root = root / "models"

            with contextlib.redirect_stdout(io.StringIO()):
                download_corridorkey_models.install_mlx_model(
                    model_root, source, color="green"
                )

            package = model_root / "corridorkey-backend-green-mlx" / "1.0-mlx"
            manifest = json.loads(
                (package / "model-manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["backend_compatibility"], ["mlx"])
            self.assertEqual(manifest["screen_color"], "green")
            self.assertEqual(manifest["local_path"], "green-mlx-model.safetensors")
            self.assertTrue((package / "green-mlx-model.safetensors").is_file())
            self.assertEqual(
                manifest["expected_files"][0]["sha256"],
                download_corridorkey_models.sha256_file(
                    package / "green-mlx-model.safetensors"
                ),
            )

    def test_install_mlx_model_writes_blue_mlx_manifest(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-model-bootstrap-") as tmp:
            root = Path(tmp)
            source = root / "blue-model.safetensors"
            source.write_bytes(b"blue mlx checkpoint bytes")
            model_root = root / "models"

            with mock.patch.object(
                download_corridorkey_models,
                "convert_mlx_checkpoint",
                side_effect=lambda src, dst: shutil.copy2(src, dst),
            ):
                download_corridorkey_models.main(
                    [
                        "--model-dir",
                        str(model_root),
                        "--backend",
                        "mlx",
                        "--color",
                        "blue",
                        "--mlx-blue-checkpoint",
                        str(source),
                    ]
                )

            package = model_root / "corridorkey-backend-blue-mlx" / "1.0-mlx"
            manifest = json.loads(
                (package / "model-manifest.json").read_text(encoding="utf-8")
            )
            self.assertEqual(manifest["backend_compatibility"], ["mlx"])
            self.assertEqual(manifest["screen_color"], "blue")
            self.assertEqual(manifest["local_path"], "blue-mlx-model.safetensors")
            self.assertTrue((package / "blue-mlx-model.safetensors").is_file())
            self.assertEqual(
                manifest["expected_files"][0]["sha256"],
                download_corridorkey_models.sha256_file(
                    package / "blue-mlx-model.safetensors"
                ),
            )

    def test_convert_mlx_checkpoint_validates_converted_output_loads(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-model-bootstrap-") as tmp:
            root = Path(tmp)
            source = root / "source.safetensors"
            destination = root / "converted.safetensors"
            source.write_bytes(b"source")
            calls = []

            def fake_save_file(_weights, path):
                Path(path).write_bytes(b"converted")

            class FakeEngine:
                def __init__(self, path, img_size, tile_size, overlap):
                    calls.append((Path(path), img_size, tile_size, overlap))

            with _fake_mlx_conversion_modules(FakeEngine, fake_save_file):
                download_corridorkey_models.convert_mlx_checkpoint(
                    source, destination
                )

            self.assertTrue(destination.is_file())
            self.assertEqual(calls, [(destination, 64, None, 0)])

    def test_convert_mlx_checkpoint_removes_invalid_converted_output(self):
        with tempfile.TemporaryDirectory(prefix="corridorkey-model-bootstrap-") as tmp:
            root = Path(tmp)
            source = root / "source.safetensors"
            destination = root / "converted.safetensors"
            source.write_bytes(b"source")

            def fake_save_file(_weights, path):
                Path(path).write_bytes(b"converted")

            class FailingEngine:
                def __init__(self, _path, img_size, tile_size, overlap):
                    raise RuntimeError("load failed")

            with _fake_mlx_conversion_modules(FailingEngine, fake_save_file):
                with self.assertRaises(SystemExit) as raised:
                    download_corridorkey_models.convert_mlx_checkpoint(
                        source, destination
                    )

            self.assertIn("failed load validation", str(raised.exception))
            self.assertFalse(destination.exists())

def _fake_mlx_conversion_modules(engine_cls, save_file):
    mlx_module = types.ModuleType("corridorkey_mlx")
    mlx_module.CorridorKeyMLXEngine = engine_cls
    convert_package = types.ModuleType("corridorkey_mlx.convert")
    converter_module = types.ModuleType("corridorkey_mlx.convert.converter")
    converter_module.convert_state_dict = lambda state: (state, [])
    safetensors_module = types.ModuleType("safetensors")
    safetensors_numpy_module = types.ModuleType("safetensors.numpy")
    safetensors_numpy_module.load_file = lambda _path: {"weight": b"value"}
    safetensors_numpy_module.save_file = save_file
    return mock.patch.dict(
        sys.modules,
        {
            "corridorkey_mlx": mlx_module,
            "corridorkey_mlx.convert": convert_package,
            "corridorkey_mlx.convert.converter": converter_module,
            "safetensors": safetensors_module,
            "safetensors.numpy": safetensors_numpy_module,
        },
    )


if __name__ == "__main__":
    unittest.main()
