#!/usr/bin/env python3
"""Offline verification suite for samsung-cu8000-kit."""
from __future__ import annotations

import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest
import zipfile

FLEET_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(FLEET_DIR))
sys.path.insert(0, str(FLEET_DIR / "bin"))

import build_package as BP  # noqa: E402
import fleet_config as FC  # noqa: E402


class TestFleetConfig(unittest.TestCase):
    def test_render_hub_conf_generic_no_gateway(self) -> None:
        prof = dict(FC.DEFAULT_PROFILE)
        prof["room_title"] = "suite 04"
        prof["hdmi1_name"] = "Apple TV"
        prof["plex_host"] = ""
        prof["gateway_url"] = ""
        conf = FC.render_hub_conf(prof)
        self.assertIn("room_title=SUITE 04", conf)
        self.assertIn("plex_host=", conf)
        self.assertIn("gateway_url=", conf)
        self.assertIn("media_os_enabled=0", conf)
        self.assertIn("hdmi1_name=Apple TV", conf)

    def test_render_hub_conf_with_gateway_and_plex(self) -> None:
        prof = dict(FC.DEFAULT_PROFILE)
        prof["plex_host"] = "10.0.20.50"
        prof["plex_port"] = 32400
        prof["gateway_url"] = "http://10.0.20.50:8765/"
        prof["media_os_enabled"] = True
        conf = FC.render_hub_conf(prof)
        self.assertIn("plex_host=10.0.20.50", conf)
        self.assertIn("gateway_url=http://10.0.20.50:8765", conf)
        self.assertIn("media_os_enabled=1", conf)

    def test_render_boot_conf_unknown_firmware_fail_closed(self) -> None:
        conf = FC.render_boot_conf(
            FC.DEFAULT_PROFILE,
            uep_ko="",
            uep_ko_sha="",
            uep_log_tag="",
            lib_stock_sha="",
            lib_patched_sha="",
        )
        self.assertIn("UEP_KO=''", conf)
        self.assertIn("LIB_STOCK_SHA=''", conf)
        self.assertIn("STRIP_ENABLED='1'", conf)

    def test_render_vpn_network_env_optional_plex(self) -> None:
        prof = dict(FC.DEFAULT_PROFILE)
        prof["plex_host"] = ""
        env_txt = FC.render_vpn_network_env(prof, dev_host="10.0.50.12")
        self.assertIn("DEV_HOST='10.0.50.12'", env_txt)
        self.assertIn("PLEX_HOST=''", env_txt)
        self.assertIn("BYPASS_RFC1918='1'", env_txt)

    def test_sanitize_strips_shell_metacharacters(self) -> None:
        dirty = "Unit 1; rm -rf / `id` $HOME\nEVIL"
        clean = FC.sanitize_single_line(dirty)
        for bad in (";", "`", "$", "\n"):
            self.assertNotIn(bad, clean)


class TestPackageBuilder(unittest.TestCase):
    def test_build_zip_and_verify_gates(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_dir = pathlib.Path(tmp) / "out"
            rt_dir = pathlib.Path(tmp) / "runtime"
            rt_dir.mkdir(parents=True)
            summary = BP.build_package("vtest", out_dir, rt_dir, deny_terms=["SUPER_SECRET_SENTINEL"])
            zip_path = pathlib.Path(summary["zip_path"])
            self.assertTrue(zip_path.is_file())

            extract_dir = pathlib.Path(tmp) / "unzipped"
            with zipfile.ZipFile(zip_path, "r") as zf:
                zf.extractall(extract_dir)
                info = zf.getinfo("cu8000-property-kit-vtest/Setup-TV.command")
                unix_mode = (info.external_attr >> 16) & 0o777
                self.assertTrue(unix_mode & stat.S_IXUSR)

            kit = extract_dir / "cu8000-property-kit-vtest"
            self.assertTrue((kit / "START-HERE.md").is_file())
            self.assertTrue((kit / "manifest.json").is_file())
            self.assertTrue((kit / "SHA256SUMS.txt").is_file())
            self.assertTrue((kit / "gateway" / "setup-gateway.sh").is_file())

            py = "/usr/bin/python3" if pathlib.Path("/usr/bin/python3").is_file() else sys.executable
            res = subprocess.run([py, str(kit / "bin" / "cu8000_setup.py"), "--help"], capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, res.stderr)
            self.assertIn("CU8000 Property Fleet Setup", res.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
