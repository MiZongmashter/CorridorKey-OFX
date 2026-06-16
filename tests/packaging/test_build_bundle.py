import importlib.util
import json
import shutil
import tempfile
import unittest
from unittest import mock
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_BUNDLE = REPO_ROOT / "packaging" / "build_bundle.py"

spec = importlib.util.spec_from_file_location("build_bundle", BUILD_BUNDLE)
build_bundle = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_bundle)


class BuildBundleTests(unittest.TestCase):
    def write_sidecar_package(self, root):
        sidecar = root / "sidecar"
        for relative in build_bundle.SIDECAR_PACKAGE_FILES:
            path = sidecar / "corridorkey_sidecar" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(relative, encoding="utf-8")
        return sidecar

    def write_manifest(self, root, model_id, version):
        manifest_dir = root / "source-id" / "source-version"
        manifest_dir.mkdir(parents=True, exist_ok=True)
        (manifest_dir / "model-manifest.json").write_text(
            json.dumps({"model_id": model_id, "version": version}), encoding="utf-8"
        )

    def write_runtime_model(self, root):
        manifest_dir = root / "corridorkey-backend-green" / "1.0"
        manifest_dir.mkdir(parents=True, exist_ok=True)
        (manifest_dir / "weights.fixture").write_text("weights", encoding="utf-8")
        (manifest_dir / "model-manifest.json").write_text(
            json.dumps(
                {
                    "model_id": "corridorkey-backend-green",
                    "version": "1.0",
                    "screen_color": "green",
                    "backend_compatibility": ["torch_cpu"],
                    "expected_files": [
                        {
                            "path": "weights.fixture",
                            "sha256": build_bundle._sha256(
                                manifest_dir / "weights.fixture"
                            ),
                        }
                    ],
                    "local_path": "weights.fixture",
                }
            ),
            encoding="utf-8",
        )

    def write_non_runtime_corridorkey_payload(self, runtime_root):
        (runtime_root / "gvm_core" / "weights" / "unet").mkdir(parents=True)
        (
            runtime_root
            / "gvm_core"
            / "weights"
            / "unet"
            / "diffusion_pytorch_model.safetensors"
        ).write_text("unused pytorch weights", encoding="utf-8")
        for dev_dir in (".github", "tests", "docs"):
            dev_path = runtime_root / dev_dir
            dev_path.mkdir()
            (dev_path / "fixture.txt").write_text(dev_dir, encoding="utf-8")

    def assert_non_runtime_corridorkey_payload_pruned(self, packaged_source):
        self.assertFalse((packaged_source / "gvm_core" / "weights").exists())
        for dev_dir in (".github", "tests", "docs"):
            self.assertFalse((packaged_source / dev_dir).exists())

    def write_runtime_python_tree(self, root):
        base = root / "python-base"
        base_python = base / "bin" / "python3.13"
        base_python.parent.mkdir(parents=True, exist_ok=True)
        base_python.write_text("#!/bin/sh\n", encoding="utf-8")
        base_python.chmod(0o755)
        base_lib = base / "lib"
        stdlib = base_lib / "python3.13"
        stdlib.mkdir(parents=True)
        (stdlib / "_sysconfigdata__darwin_darwin.py").write_text(
            'build_time_path = "/private/var/folders/source/Python-3.13.13"\n'
            f'install_path = "{base}"\n',
            encoding="utf-8",
        )
        (stdlib / "config-3.13-darwin").mkdir()
        (stdlib / "config-3.13-darwin" / "Makefile").write_text(
            "abs_builddir=/private/var/folders/source/Python-3.13.13\n",
            encoding="utf-8",
        )
        (stdlib / "tkinter").mkdir()
        (base_lib / "tk9.0").mkdir()
        (base_lib / "libtcl9.0.dylib").write_text(
            "/private/var/folders/source/tcl9.0\n",
            encoding="utf-8",
        )
        (base / "bin" / "pip").write_text(
            "#!/private/var/folders/source/python\n",
            encoding="utf-8",
        )
        base_site_packages = stdlib / "site-packages"
        (base_site_packages / "pip-26.1.1.dist-info").mkdir(parents=True)
        (base_site_packages / "pip-26.1.1.dist-info" / "direct_url.json").write_text(
            '{"url":"file:///private/var/folders/source/pip.whl"}\n',
            encoding="utf-8",
        )

        venv = root / ".venv"
        runtime_python = venv / "bin" / "python"
        runtime_python.parent.mkdir(parents=True, exist_ok=True)
        runtime_python.write_text("#!/bin/sh\n", encoding="utf-8")
        runtime_python.chmod(0o755)
        (venv / "pyvenv.cfg").write_text(
            f"home = {base / 'bin'}\nversion_info = 3.13.13\n",
            encoding="utf-8",
        )
        site_packages = venv / "lib" / "python3.13" / "site-packages"
        (site_packages / "numpy").mkdir(parents=True)
        (site_packages / "numpy" / "__init__.py").write_text("", encoding="utf-8")
        (site_packages / "numpy" / "__config__.py").write_text(
            'CONFIG = {"python_path": r"/private/var/folders/build-env/bin/python"}\n'
            f'LOCAL = r"{root / ".venv" / "bin" / "python"}"\n',
            encoding="utf-8",
        )
        (site_packages / "contourpy" / "util").mkdir(parents=True)
        (site_packages / "contourpy" / "util" / "_build_config.py").write_text(
            'build_dir = r"/Users/runner/work/contourpy/contourpy/.mesonpy-build"\n',
            encoding="utf-8",
        )
        local_direct_url = site_packages / "local_tmp-1.0.dist-info" / "direct_url.json"
        local_direct_url.parent.mkdir()
        local_direct_url.write_text('{"url":"file:///tmp/build/local-tmp.whl"}\n', encoding="utf-8")
        remote_direct_url = site_packages / "remote_git-1.0.dist-info" / "direct_url.json"
        remote_direct_url.parent.mkdir()
        remote_direct_url.write_text(
            '{"url":"https://example.test/project.git","vcs_info":{"vcs":"git"}}\n',
            encoding="utf-8",
        )
        (site_packages / "_editable_impl_corridorkey.pth").write_text(
            str(root / "CorridorKey-main") + "\n",
            encoding="utf-8",
        )
        editable_finder = site_packages / "__editable___corridorkey_1_0_0_finder.py"
        editable_finder.write_text(
            "MAPPING = {'CorridorKeyModule': "
            + repr(str(root / "CorridorKey-main" / "CorridorKeyModule"))
            + "}\n",
            encoding="utf-8",
        )
        (site_packages / "__editable__.corridorkey.pth").write_text(
            "import __editable___corridorkey_1_0_0_finder; "
            "__editable___corridorkey_1_0_0_finder.install()\n",
            encoding="utf-8",
        )
        (site_packages / "safe-relative.pth").write_text(
            "relative_vendor\n",
            encoding="utf-8",
        )
        return runtime_python

    def test_offline_archive_is_copied_and_marked_ready_in_manifest(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output_root = root / "dist"
            plugin = root / "CorridorKey.ofx"
            plugin.write_text("plugin", encoding="utf-8")
            sidecar = self.write_sidecar_package(root)
            archive = root / "corridorkey-offline-models-test.tar"
            archive.write_text("offline models", encoding="utf-8")
            model_root = root / "models"
            self.write_manifest(model_root, "corridorkey-backend-green", "1.0")

            build_bundle.build_bundle(output_root, plugin, sidecar, model_root, archive)

            packaged_archive = output_root / "offline-models" / archive.name
            self.assertTrue(packaged_archive.is_file())
            manifest_text = (
                output_root / build_bundle.DISTRIBUTION_MANIFEST_NAME
            ).read_text(encoding="utf-8")
            self.assertNotIn(str(archive), manifest_text)
            manifest = json.loads(manifest_text)
            archive_entry = next(
                item
                for item in manifest["files"]
                if item["path"] == f"offline-models/{archive.name}"
            )
            self.assertEqual(archive_entry["size"], archive.stat().st_size)
            self.assertEqual(
                manifest["build_configuration"]["offline_model_package_status"], "ready"
            )
            self.assertEqual(
                manifest["offline_model_package"]["path"],
                f"offline-models/{archive.name}",
            )
            self.assertEqual(
                manifest["offline_model_package"]["sha256"],
                build_bundle._sha256(archive),
            )
            self.assertEqual(
                manifest["artifact_tree_sha256"],
                build_bundle._tree_sha256(output_root / build_bundle.BUNDLE_DIRECTORY),
            )
            self.assertEqual(
                manifest["distribution_tree_sha256"],
                build_bundle._tree_sha256(
                    output_root, {build_bundle.DISTRIBUTION_MANIFEST_NAME}
                ),
            )
            self.assertEqual(
                manifest["artifact_tree_file_count"],
                sum(
                    1
                    for path in (output_root / build_bundle.BUNDLE_DIRECTORY).rglob("*")
                    if path.is_file()
                ),
            )
            self.assertEqual(
                manifest["distribution_tree_file_count"], len(manifest["files"])
            )
            self.assertEqual(
                manifest["distribution_file_count_including_manifest"],
                len(manifest["files"]) + 1,
            )
            self.assertEqual(
                manifest["plugin_binary_path"],
                "CorridorKey.ofx.bundle/Contents/MacOS/CorridorKey.ofx",
            )
            self.assertEqual(
                manifest["plugin_binary_sha256"], build_bundle._sha256(plugin)
            )
            self.assertEqual(manifest["plugin_binary_size"], plugin.stat().st_size)
            self.assertIn("excludes CorridorKey-distribution-manifest.json", manifest["tree_sha256_algorithm"])
            self.assertEqual(
                manifest["build_configuration"]["model_source_status"], "ready"
            )

    def test_explicit_missing_offline_archive_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            plugin = root / "CorridorKey.ofx"
            plugin.write_text("plugin", encoding="utf-8")
            sidecar = self.write_sidecar_package(root)

            with self.assertRaisesRegex(
                FileNotFoundError, "offline model archive not found"
            ):
                build_bundle.build_bundle(
                    root / "dist",
                    plugin,
                    sidecar,
                    root / "models",
                    root / "missing-offline-models.tar",
                )

    def test_runtime_assets_are_packaged_under_sidecar_default_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output_root = root / "dist"
            plugin = root / "CorridorKey.ofx"
            plugin.write_text("plugin", encoding="utf-8")
            sidecar = self.write_sidecar_package(root)
            model_root = root / "models"
            self.write_runtime_model(model_root)
            corridorkey_runtime = root / "CorridorKey-main"
            (corridorkey_runtime / "CorridorKeyModule").mkdir(parents=True)
            (corridorkey_runtime / "CorridorKeyModule" / "__init__.py").write_text(
                "", encoding="utf-8"
            )
            adapter = root / "corridorkey_ofx_adapter.py"
            adapter.write_text("def infer_cpu(request): return {}\n", encoding="utf-8")
            runtime_python = self.write_runtime_python_tree(root)

            build_bundle.build_bundle(
                output_root,
                plugin,
                sidecar,
                model_root,
                corridorkey_runtime_root=corridorkey_runtime,
                corridorkey_adapter=adapter,
                runtime_python=runtime_python,
                include_runtime_assets=True,
            )

            resources = (
                output_root
                / build_bundle.BUNDLE_DIRECTORY
                / build_bundle.CONTENTS_DIRECTORY
                / "Resources"
            )
            self.assertTrue(
                (
                    resources
                    / "external"
                    / "CorridorKey"
                    / "corridorkey_ofx_adapter.py"
                ).is_file()
            )
            self.assertTrue(
                (
                    resources
                    / "external"
                    / "CorridorKey"
                    / "CorridorKey-main"
                    / "CorridorKeyModule"
                    / "__init__.py"
                ).is_file()
            )
            self.assertTrue(
                (
                    resources
                    / ".local"
                    / "models"
                    / "corridorkey"
                    / "corridorkey-backend-green"
                    / "1.0"
                    / "weights.fixture"
                ).is_file()
            )
            self.assertTrue((resources / "python" / "bin" / "python3").is_file())
            self.assertTrue(
                (
                    resources
                    / "python-runtime"
                    / "base"
                    / "bin"
                    / "python3.13"
                ).is_file()
            )
            self.assertTrue(
                (
                    resources
                    / "python-runtime"
                    / "venv"
                    / "lib"
                    / "python3.13"
                    / "site-packages"
                    / "numpy"
                    / "__init__.py"
                ).is_file()
            )
            manifest = json.loads(
                (output_root / build_bundle.DISTRIBUTION_MANIFEST_NAME).read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                manifest["build_configuration"]["runtime_readiness_status"],
                "packaged_runtime_assets_present",
            )
            self.assertEqual(
                manifest["build_configuration"]["packaged_runtime_status"],
                "standalone_ready",
            )
            self.assertEqual(
                manifest["build_configuration"]["packaged_runtime_python_status"],
                "standalone_ready",
            )
            package_notes = "\n".join(manifest["package_notes"])
            self.assertNotIn("Python/runtime dependency isolation", package_notes)
            self.assertNotIn("standalone packaged Python runtime isolation", package_notes)

    def test_runtime_assets_prune_non_runtime_corridorkey_payloads(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output_root = root / "dist"
            plugin = root / "CorridorKey.ofx"
            plugin.write_text("plugin", encoding="utf-8")
            sidecar = self.write_sidecar_package(root)
            model_root = root / "models"
            self.write_runtime_model(model_root)
            corridorkey_runtime = root / "CorridorKey-main"
            runtime_module = corridorkey_runtime / "CorridorKeyModule"
            runtime_module.mkdir(parents=True)
            (runtime_module / "__init__.py").write_text("", encoding="utf-8")
            self.write_non_runtime_corridorkey_payload(corridorkey_runtime)
            adapter = root / "corridorkey_ofx_adapter.py"
            adapter.write_text("def infer_cpu(request): return {}\n", encoding="utf-8")
            runtime_python = self.write_runtime_python_tree(root)

            build_bundle.build_bundle(
                output_root,
                plugin,
                sidecar,
                model_root,
                corridorkey_runtime_root=corridorkey_runtime,
                corridorkey_adapter=adapter,
                runtime_python=runtime_python,
                include_runtime_assets=True,
            )

            packaged_source = (
                output_root
                / build_bundle.BUNDLE_DIRECTORY
                / build_bundle.CONTENTS_DIRECTORY
                / "Resources"
                / "external"
                / "CorridorKey"
                / "CorridorKey-main"
            )
            self.assertTrue(
                (packaged_source / "CorridorKeyModule" / "__init__.py").is_file()
            )
            self.assert_non_runtime_corridorkey_payload_pruned(packaged_source)

    def test_packaged_runtime_resources_can_be_reused_for_release_candidate(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output_root = root / "dist"
            plugin = root / "CorridorKey.ofx"
            plugin.write_text("plugin", encoding="utf-8")
            sidecar = self.write_sidecar_package(root)
            model_root = root / "models"
            self.write_runtime_model(model_root)
            archive = root / "corridorkey-offline-models-test.tar"
            archive.write_text("offline models", encoding="utf-8")
            packaged_resources = root / "installed" / "Contents" / "Resources"
            adapter = packaged_resources / "external" / "CorridorKey" / "corridorkey_ofx_adapter.py"
            runtime_source = (
                packaged_resources
                / "external"
                / "CorridorKey"
                / "CorridorKey-main"
                / "CorridorKeyModule"
            )
            runtime_source.mkdir(parents=True)
            runtime_source.joinpath("__init__.py").write_text("", encoding="utf-8")
            self.write_non_runtime_corridorkey_payload(runtime_source.parent)
            adapter.parent.mkdir(parents=True, exist_ok=True)
            adapter.write_text("def infer_cpu(request): return {}\n", encoding="utf-8")
            packaged_model = (
                packaged_resources
                / ".local"
                / "models"
                / "corridorkey"
                / "corridorkey-backend-green"
                / "1.0"
            )
            shutil.copytree(model_root / "corridorkey-backend-green" / "1.0", packaged_model)
            launcher = packaged_resources / "python" / "bin" / "python3"
            launcher.parent.mkdir(parents=True)
            launcher.write_text("#!/bin/sh\n", encoding="utf-8")
            launcher.chmod(0o755)
            interpreter = packaged_resources / "python-runtime" / "base" / "bin" / "python3.13"
            interpreter.parent.mkdir(parents=True)
            interpreter.write_text("#!/bin/sh\n", encoding="utf-8")
            interpreter.chmod(0o755)
            site_packages = (
                packaged_resources
                / "python-runtime"
                / "venv"
                / "lib"
                / "python3.13"
                / "site-packages"
            )
            site_packages.mkdir(parents=True)
            (site_packages / "numpy").mkdir()

            build_bundle.build_bundle(
                output_root,
                plugin,
                sidecar,
                model_root,
                archive,
                packaged_runtime_resources=packaged_resources,
            )

            resources = (
                output_root
                / build_bundle.BUNDLE_DIRECTORY
                / build_bundle.CONTENTS_DIRECTORY
                / "Resources"
            )
            self.assertTrue(
                (
                    resources
                    / "external"
                    / "CorridorKey"
                    / "corridorkey_ofx_adapter.py"
                ).is_file()
            )
            self.assertTrue(
                (
                    resources
                    / ".local"
                    / "models"
                    / "corridorkey"
                    / "corridorkey-backend-green"
                    / "1.0"
                    / "weights.fixture"
                ).is_file()
            )
            packaged_source = (
                resources / "external" / "CorridorKey" / "CorridorKey-main"
            )
            self.assert_non_runtime_corridorkey_payload_pruned(packaged_source)
            self.assertTrue((resources / "python" / "bin" / "python3").is_file())
            self.assertTrue(
                (
                    resources
                    / "python-runtime"
                    / "base"
                    / "bin"
                    / "python3.13"
                ).is_file()
            )
            manifest = json.loads(
                (output_root / build_bundle.DISTRIBUTION_MANIFEST_NAME).read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                manifest["build_configuration"]["runtime_readiness_status"],
                "packaged_runtime_assets_present",
            )
            self.assertEqual(
                manifest["build_configuration"]["packaged_runtime_status"],
                "standalone_ready",
            )
            self.assertEqual(
                manifest["build_configuration"]["packaged_runtime_repo_status"], "ready"
            )
            self.assertEqual(
                manifest["build_configuration"]["packaged_runtime_model_status"], "ready"
            )
            self.assertEqual(
                manifest["build_configuration"]["packaged_runtime_python_status"],
                "standalone_ready",
            )
            manifest_text = (
                output_root / build_bundle.DISTRIBUTION_MANIFEST_NAME
            ).read_text(encoding="utf-8")
            self.assertNotIn(str(packaged_resources), manifest_text)
            package_notes = "\n".join(manifest["package_notes"])
            self.assertNotIn("full model/runtime readiness", package_notes)

    def test_runtime_python_launcher_and_manifest_do_not_capture_source_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output_root = root / "dist"
            plugin = root / "CorridorKey.ofx"
            plugin.write_text("plugin", encoding="utf-8")
            sidecar = self.write_sidecar_package(root)
            model_root = root / "models"
            self.write_runtime_model(model_root)
            corridorkey_runtime = root / "CorridorKey-main"
            (corridorkey_runtime / "CorridorKeyModule").mkdir(parents=True)
            adapter = root / "corridorkey_ofx_adapter.py"
            adapter.write_text("def infer_cpu(request): return {}\n", encoding="utf-8")
            runtime_python = self.write_runtime_python_tree(root)

            build_bundle.build_bundle(
                output_root,
                plugin,
                sidecar,
                model_root,
                corridorkey_runtime_root=corridorkey_runtime,
                corridorkey_adapter=adapter,
                runtime_python=runtime_python,
                include_runtime_assets=True,
            )

            resources = (
                output_root
                / build_bundle.BUNDLE_DIRECTORY
                / build_bundle.CONTENTS_DIRECTORY
                / "Resources"
            )
            launcher = resources / "python" / "bin" / "python3"
            launcher_text = launcher.read_text(encoding="utf-8")
            manifest_text = (
                output_root / build_bundle.DISTRIBUTION_MANIFEST_NAME
            ).read_text(encoding="utf-8")

            for leaked in (str(runtime_python), str(runtime_python.parents[1]), str(root)):
                self.assertNotIn(leaked, launcher_text)
                self.assertNotIn(leaked, manifest_text)
            self.assertIn("unset CORRIDORKEY_REPO", launcher_text)
            self.assertIn("unset CORRIDORKEY_MODEL_DIR", launcher_text)
            self.assertIn("CORRIDORKEY_TEST_*", launcher_text)
            self.assertIn("CORRIDORKEY_*", launcher_text)
            self.assertIn('cd "$RESOURCES_DIR"', launcher_text)
            self.assertIn("PYTHONDONTWRITEBYTECODE=1", launcher_text)

            manifest = json.loads(manifest_text)
            self.assertEqual(
                manifest["runtime_package"]["python"]["path"],
                "python/bin/python3",
            )
            self.assertEqual(
                manifest["runtime_package"]["python"]["status"],
                "standalone_ready",
            )
            self.assertIn(
                "python-runtime/base/bin/python3.13",
                manifest["runtime_package"]["python"]["interpreter"],
            )

    def test_runtime_site_packages_are_sanitized_before_packaging(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output_root = root / "dist"
            plugin = root / "CorridorKey.ofx"
            plugin.write_text("plugin", encoding="utf-8")
            sidecar = self.write_sidecar_package(root)
            model_root = root / "models"
            self.write_runtime_model(model_root)
            corridorkey_runtime = root / "CorridorKey-main"
            (corridorkey_runtime / "CorridorKeyModule").mkdir(parents=True)
            adapter = root / "corridorkey_ofx_adapter.py"
            adapter.write_text("def infer_cpu(request): return {}\n", encoding="utf-8")
            runtime_python = self.write_runtime_python_tree(root)

            build_bundle.build_bundle(
                output_root,
                plugin,
                sidecar,
                model_root,
                corridorkey_runtime_root=corridorkey_runtime,
                corridorkey_adapter=adapter,
                runtime_python=runtime_python,
                include_runtime_assets=True,
            )

            resources = (
                output_root
                / build_bundle.BUNDLE_DIRECTORY
                / build_bundle.CONTENTS_DIRECTORY
                / "Resources"
            )
            site_packages = (
                resources
                / "python-runtime"
                / "venv"
                / "lib"
                / "python3.13"
                / "site-packages"
            )
            packaged_pth_text = "\n".join(
                path.read_text(encoding="utf-8")
                for path in sorted(site_packages.glob("*.pth"))
            )
            self.assertNotIn(str(root / "CorridorKey-main"), packaged_pth_text)
            self.assertNotIn("__editable___corridorkey_1_0_0_finder", packaged_pth_text)
            self.assertFalse(
                (site_packages / "__editable___corridorkey_1_0_0_finder.py").exists()
            )
            self.assertEqual(
                (site_packages / "safe-relative.pth").read_text(encoding="utf-8"),
                "relative_vendor\n",
            )
            for relative in (
                "numpy/__config__.py",
                "contourpy/util/_build_config.py",
            ):
                metadata_text = (site_packages / relative).read_text(encoding="utf-8")
                self.assertNotIn("/private/var", metadata_text)
                self.assertNotIn("/Users/runner", metadata_text)
                self.assertNotIn(str(root), metadata_text)
                self.assertIn("<packaged-runtime-source>", metadata_text)

            manifest = json.loads(
                (output_root / build_bundle.DISTRIBUTION_MANIFEST_NAME).read_text(
                    encoding="utf-8"
                )
            )
            self.assertEqual(
                manifest["runtime_package"]["python"]["sanitized_runtime_metadata_files"],
                [
                    "contourpy/util/_build_config.py",
                    "numpy/__config__.py",
                ],
            )
            self.assertFalse(
                (site_packages / "local_tmp-1.0.dist-info" / "direct_url.json").exists()
            )
            self.assertTrue(
                (site_packages / "remote_git-1.0.dist-info" / "direct_url.json").exists()
            )

    def test_runtime_python_base_prunes_build_metadata_before_packaging(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            output_root = root / "dist"
            plugin = root / "CorridorKey.ofx"
            plugin.write_text("plugin", encoding="utf-8")
            sidecar = self.write_sidecar_package(root)
            model_root = root / "models"
            self.write_runtime_model(model_root)
            corridorkey_runtime = root / "CorridorKey-main"
            (corridorkey_runtime / "CorridorKeyModule").mkdir(parents=True)
            adapter = root / "corridorkey_ofx_adapter.py"
            adapter.write_text("def infer_cpu(request): return {}\n", encoding="utf-8")
            runtime_python = self.write_runtime_python_tree(root)

            build_bundle.build_bundle(
                output_root,
                plugin,
                sidecar,
                model_root,
                corridorkey_runtime_root=corridorkey_runtime,
                corridorkey_adapter=adapter,
                runtime_python=runtime_python,
                include_runtime_assets=True,
            )

            resources = (
                output_root
                / build_bundle.BUNDLE_DIRECTORY
                / build_bundle.CONTENTS_DIRECTORY
                / "Resources"
            )
            base = resources / "python-runtime" / "base"
            stdlib = base / "lib" / "python3.13"
            self.assertFalse((stdlib / "config-3.13-darwin").exists())
            self.assertFalse((stdlib / "tkinter").exists())
            self.assertFalse((base / "lib" / "tk9.0").exists())
            self.assertFalse((base / "lib" / "libtcl9.0.dylib").exists())
            self.assertFalse((base / "bin" / "pip").exists())
            self.assertFalse(
                (
                    stdlib
                    / "site-packages"
                    / "pip-26.1.1.dist-info"
                    / "direct_url.json"
                ).exists()
            )
            sysconfig_text = (
                stdlib / "_sysconfigdata__darwin_darwin.py"
            ).read_text(encoding="utf-8")
            self.assertNotIn("/private/var", sysconfig_text)
            self.assertNotIn(str(runtime_python.parents[1]), sysconfig_text)

    def test_macos_python_dylib_install_name_is_made_relocatable(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            dylib = root / "lib" / "libpython3.13.dylib"
            dylib.parent.mkdir(parents=True)
            dylib.write_text("mach-o fixture", encoding="utf-8")

            with mock.patch.object(build_bundle.platform, "system", return_value="Darwin"), \
                mock.patch.object(build_bundle.shutil, "which", return_value="/usr/bin/install_name_tool"), \
                mock.patch.object(build_bundle.subprocess, "run") as run:
                rewritten = build_bundle._rewrite_macos_python_install_names(root)

            self.assertEqual(rewritten, ["lib/libpython3.13.dylib"])
            run.assert_called_once_with(
                [
                    "/usr/bin/install_name_tool",
                    "-id",
                    "@rpath/libpython3.13.dylib",
                    str(dylib),
                ],
                check=True,
            )

    def test_rejects_unsafe_model_manifest_path_segments(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            model_root = root / "models"
            self.write_manifest(model_root, "../escape", "1.0")

            with self.assertRaises(ValueError):
                build_bundle._copy_model_manifests(
                    model_root, root / "out", root / "bundle" / "Resources"
                )

            self.assertFalse((root / "escape").exists())

    def test_rejects_duplicate_packaged_model_manifest_targets(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            model_root = root / "models"
            for source_id in ("a", "b"):
                manifest_dir = model_root / source_id / "source-version"
                manifest_dir.mkdir(parents=True, exist_ok=True)
                (manifest_dir / "model-manifest.json").write_text(
                    json.dumps({"model_id": "same-model", "version": "1.0"}),
                    encoding="utf-8",
                )

            with self.assertRaises(ValueError):
                build_bundle._copy_model_manifests(
                    model_root, root / "out", root / "bundle" / "Resources"
                )


if __name__ == "__main__":
    unittest.main()
