import json
import plistlib
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
UNINSTALLER = REPO_ROOT / "packaging" / "uninstall_corridorkey_ofx.py"


class UninstallerTests(unittest.TestCase):
    def run_uninstaller(self, root, *extra):
        plugin = root / "OFX" / "Plugins" / "CorridorKey.ofx.bundle"
        sidecar = root / "install" / "sidecar"
        user = root / ".corridorkey-ofx"
        cmd = [
            sys.executable,
            str(UNINSTALLER),
            "--json",
            "--plugin-bundle",
            str(plugin),
            "--sidecar-install-dir",
            str(sidecar),
            "--user-data-root",
            str(user),
            *extra,
        ]
        return subprocess.run(cmd, check=False, text=True, capture_output=True)

    def make_install_tree(self, root):
        plugin_file = root / "OFX" / "Plugins" / "CorridorKey.ofx.bundle" / "Contents" / "MacOS" / "CorridorKey.ofx"
        info_plist = root / "OFX" / "Plugins" / "CorridorKey.ofx.bundle" / "Contents" / "Info.plist"
        sidecar_file = root / "install" / "sidecar" / "corridorkey_sidecar" / "server.py"
        model_file = root / ".corridorkey-ofx" / "models" / "corridorkey" / "model.bin"
        log_file = root / ".corridorkey-ofx" / "logs" / "sidecar.log"
        sentinel = root / ".corridorkey-ofx" / ".corridorkey-ofx-user-data"
        for path in (plugin_file, info_plist, sidecar_file, model_file, log_file, sentinel):
            path.parent.mkdir(parents=True, exist_ok=True)
            if path.name == "Info.plist":
                with path.open("wb") as output:
                    plistlib.dump({"CFBundleIdentifier": "com.corridorkey.openfx"}, output)
            else:
                path.write_text(path.name, encoding="utf-8")
        return plugin_file, sidecar_file, model_file, log_file

    def test_removes_plugin_and_sidecar_but_preserves_user_models_and_logs_by_default(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            _plugin_file, _sidecar_file, model_file, log_file = self.make_install_tree(root)

            result = self.run_uninstaller(root, "--yes")

            self.assertEqual(result.returncode, 0, result.stderr)
            payload = json.loads(result.stdout)
            self.assertTrue(payload["ok"])
            self.assertFalse((root / "OFX" / "Plugins" / "CorridorKey.ofx.bundle").exists())
            self.assertFalse((root / "install" / "sidecar").exists())
            self.assertTrue(model_file.exists())
            self.assertTrue(log_file.exists())
            self.assertIn("preserved_user_data_default", {item["action"] for item in payload["actions"]})

    def test_purge_user_data_is_explicit_opt_in(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.make_install_tree(root)

            result = self.run_uninstaller(root, "--yes", "--purge-user-data")

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse((root / ".corridorkey-ofx" / "models").exists())
            self.assertFalse((root / ".corridorkey-ofx" / "logs").exists())

    def test_dry_run_reports_actions_without_removing_files(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            plugin_file, sidecar_file, model_file, log_file = self.make_install_tree(root)

            result = self.run_uninstaller(root, "--dry-run")

            self.assertEqual(result.returncode, 0, result.stderr)
            payload = json.loads(result.stdout)
            self.assertTrue(payload["dry_run"])
            self.assertIn("would_remove", {item["action"] for item in payload["actions"]})
            self.assertTrue(plugin_file.exists())
            self.assertTrue(sidecar_file.exists())
            self.assertTrue(model_file.exists())
            self.assertTrue(log_file.exists())

    def test_refuses_destructive_run_without_yes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.make_install_tree(root)

            result = self.run_uninstaller(root)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("--yes", result.stderr)
            self.assertTrue((root / "OFX" / "Plugins" / "CorridorKey.ofx.bundle").exists())
            self.assertTrue((root / "install" / "sidecar").exists())

    def test_refuses_to_remove_non_corridorkey_override_paths(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            unsafe_plugin = root / "OFX" / "Plugins"
            unsafe_sidecar = root / "install" / "not-sidecar"
            user = root / ".corridorkey-ofx"
            unsafe_plugin.mkdir(parents=True)
            unsafe_sidecar.mkdir(parents=True)
            user.mkdir()

            cmd = [
                sys.executable,
                str(UNINSTALLER),
                "--json",
                "--yes",
                "--plugin-bundle",
                str(unsafe_plugin),
                "--sidecar-install-dir",
                str(unsafe_sidecar),
                "--user-data-root",
                str(user),
            ]
            result = subprocess.run(cmd, check=False, text=True, capture_output=True)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("refusing to remove non-CorridorKey", result.stderr)
            self.assertTrue(unsafe_plugin.exists())
            self.assertTrue(unsafe_sidecar.exists())

    def test_refuses_bundle_with_wrong_identifier(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.make_install_tree(root)
            info_plist = root / "OFX" / "Plugins" / "CorridorKey.ofx.bundle" / "Contents" / "Info.plist"
            with info_plist.open("wb") as output:
                plistlib.dump({"CFBundleIdentifier": "com.example.not-corridorkey"}, output)

            result = self.run_uninstaller(root, "--yes")

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("refusing to remove non-CorridorKey plugin", result.stderr)
            self.assertTrue((root / "OFX" / "Plugins" / "CorridorKey.ofx.bundle").exists())

    def test_refuses_to_purge_non_corridorkey_user_data_root(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.make_install_tree(root)
            unsafe_user = root / "user-data"
            (unsafe_user / "models").mkdir(parents=True)

            result = self.run_uninstaller(
                root,
                "--yes",
                "--purge-user-data",
                "--user-data-root",
                str(unsafe_user),
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("refusing to purge non-CorridorKey", result.stderr)
            self.assertTrue((root / "OFX" / "Plugins" / "CorridorKey.ofx.bundle").exists())
            self.assertTrue((root / "install" / "sidecar").exists())
            self.assertTrue((unsafe_user / "models").exists())


if __name__ == "__main__":
    unittest.main()
