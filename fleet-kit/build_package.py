#!/usr/bin/env python3
"""Reproducible builder for a self-contained macOS CU8000 Fleet Kit zip archive.

Assembles all scripts, compiled TV binaries, ASTC assets, and portable gateway files
alongside locally supplied runtime binaries (Samsung's sdb, samsung-tv-root, and any
locally built kernel/VPN binaries — none of which are redistributed in this public repo).

Usage:
    python3 fleet-kit/build_package.py \\
        --runtime-dir ./local-runtime \\
        [--version v1.0.0] \\
        [--out-dir dist]

Expected layout inside --runtime-dir (see docs/BUILDING-FROM-SOURCE.md):
    sdb                              Samsung SDB binary for macOS
    python-lib/samsung_tv_root/      samsung-tv-root Python package
    root-payloads/                   SamsungTvArchiveRoot.dll, SamsungTvRootAgent.dll
    ui/living-room-hub-native        Compiled ARMv7 C Hub binary
    ui/LivingRoomHub.dll             (Optional) .NET NUI fallback DLL + json configs
    vpn/lightway-client              (Optional) Compiled ARMv7 lightway-client binary
    vpn/codex-vpn-ctld               (Optional) Compiled ARMv7 codex-vpn-ctld binary
    vpn/ca.pem                       (Optional) ExpressVPN Lightway public CA cert
    vpn/endpoints.tsv                (Optional) Your Lightway location endpoints
    vpn/locations.json               (Optional) Your Lightway location metadata
    onboot/uepclear5.ko              (Optional) Locally built anchor-gated .ko (0090)
    onboot/ueprestore5.ko            (Optional) Locally built restore .ko (0090)
    onboot/uepclear6.ko              (Optional) Locally built anchor-gated .ko (0080)
    onboot/ueprestore6.ko            (Optional) Locally built restore .ko (0080)
    onboot/libkUEPUser.relaxed.so    (Optional) Locally derived relaxed library copy
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import pathlib
import shutil
import stat
import struct
import subprocess
import sys
import zipfile
from typing import Dict, List

FLEET_DIR = pathlib.Path(__file__).resolve().parent
REPO_ROOT = FLEET_DIR.parent


def sha256_file(path: pathlib.Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def copy_file(src: pathlib.Path, dst: pathlib.Path, mode: int | None = None) -> None:
    if not src.is_file():
        raise FileNotFoundError(f"Required artifact missing: {src}")
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)
    if mode is not None:
        os.chmod(dst, mode)


def copy_if_exists(src: pathlib.Path, dst: pathlib.Path, mode: int | None = None) -> bool:
    if not src.is_file():
        return False
    copy_file(src, dst, mode)
    return True


def copy_tree_clean(src: pathlib.Path, dst: pathlib.Path) -> None:
    if not src.is_dir():
        raise FileNotFoundError(f"Required directory missing: {src}")

    def ignore(_dir: str, names: List[str]) -> List[str]:
        return [
            n
            for n in names
            if n in ("__pycache__", ".pytest_cache", ".DS_Store", ".venv", ".env")
            or n.endswith(".pyc")
        ]

    shutil.copytree(src, dst, ignore=ignore, dirs_exist_ok=True)


def audit_armv7_elf(path: pathlib.Path) -> Dict[str, object]:
    data = path.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise RuntimeError(f"{path} is not an ELF file")
    ei_class, ei_data = data[4], data[5]
    _et, e_machine, _ev, _ee, _eph, _esh, e_flags = struct.unpack("<HHIIIII", data[16:40])
    if ei_class != 1 or ei_data != 1 or e_machine != 40:
        raise RuntimeError(f"{path} is not a 32-bit little-endian ARM ELF")
    abi_ver = (e_flags >> 24) & 0xFF
    is_hard = bool(e_flags & 0x00000400)
    if abi_ver != 5 or is_hard:
        raise RuntimeError(f"{path} failed ARM EABI5 softfp gate (e_flags=0x{e_flags:08x})")
    return {
        "file": path.name,
        "bytes": len(data),
        "e_flags": f"0x{e_flags:08x}",
        "sha256": hashlib.sha256(data).hexdigest(),
    }


def verify_no_private_files(stage_dir: pathlib.Path, deny_terms: List[str]) -> Dict[str, int]:
    """Fail hard if forbidden secret files or caller-supplied denylist strings appear in the kit."""
    deny_bytes = [t.encode("utf-8") for t in deny_terms if len(t) >= 4]
    scanned = 0
    for p in sorted(stage_dir.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(stage_dir).as_posix()
        if ".private" in p.parts or p.name == ".env" or "client.yaml" in p.name:
            raise RuntimeError(f"FORBIDDEN private file found in staged kit: {rel}")
        raw = p.read_bytes()
        scanned += 1
        for needle in deny_bytes:
            if needle in raw:
                raise RuntimeError(f"Denylisted term detected in staged file: {rel}")
    return {"files_scanned": scanned, "deny_terms_checked": len(deny_bytes)}


def verify_shell_scripts(stage_dir: pathlib.Path) -> int:
    count = 0
    for p in sorted(stage_dir.rglob("*")):
        if not p.is_file():
            continue
        if p.suffix == ".sh":
            first_line = p.read_text(encoding="utf-8", errors="ignore").splitlines()[:1]
            interp = "bash" if first_line and "bash" in first_line[0] else "sh"
            res = subprocess.run([interp, "-n", str(p)], capture_output=True, text=True)
            if res.returncode != 0:
                raise RuntimeError(f"Shell syntax error in {p}: {res.stderr}")
            count += 1
        elif p.suffix == ".command":
            res = subprocess.run(["bash", "-n", str(p)], capture_output=True, text=True)
            if res.returncode != 0:
                raise RuntimeError(f"Bash syntax error in {p}: {res.stderr}")
            count += 1
    return count


def verify_python_imports(stage_dir: pathlib.Path) -> str:
    py = "/usr/bin/python3" if pathlib.Path("/usr/bin/python3").is_file() else sys.executable
    code = (
        "import sys; "
        f"sys.path.insert(0, {str(stage_dir / 'bin')!r}); "
        f"sys.path.insert(0, {str(stage_dir / 'runtime' / 'python-lib')!r}); "
        "import fleet_config, tv_client, cu8000_setup; "
        "print(sys.version.split()[0])"
    )
    res = subprocess.run([py, "-c", code], capture_output=True, text=True)
    if res.returncode != 0:
        raise RuntimeError(f"Python import check failed ({py}): {res.stderr}")
    return res.stdout.strip()


def write_zip_preserving_modes(stage_dir: pathlib.Path, zip_path: pathlib.Path) -> None:
    zip_path.parent.mkdir(parents=True, exist_ok=True)
    root_name = stage_dir.name
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for p in sorted(stage_dir.rglob("*")):
            if not p.is_file():
                continue
            arcname = f"{root_name}/{p.relative_to(stage_dir).as_posix()}"
            st = p.stat()
            zi = zipfile.ZipInfo(arcname)
            zi.date_time = dt.datetime.fromtimestamp(st.st_mtime).timetuple()[:6]
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = (stat.S_IFREG | (st.st_mode & 0o777)) << 16
            zf.writestr(zi, p.read_bytes())


def build_package(
    version: str,
    out_dir: pathlib.Path,
    runtime_dir: pathlib.Path,
    deny_terms: List[str] | None = None,
) -> Dict[str, object]:
    pkg_name = f"cu8000-property-kit-{version}"
    stage_dir = out_dir / pkg_name
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    stage_dir.mkdir(parents=True, exist_ok=True)

    # 1. Manager guide, double-clickable launcher, example profile
    copy_file(REPO_ROOT / "docs" / "MANAGER-SETUP-GUIDE.md", stage_dir / "START-HERE.md", 0o644)
    copy_file(FLEET_DIR / "Setup-TV.command", stage_dir / "Setup-TV.command", 0o755)
    copy_file(FLEET_DIR / "property.example.json", stage_dir / "property.example.json", 0o644)
    (stage_dir / "receipts").mkdir(parents=True, exist_ok=True)
    (stage_dir / "receipts" / ".gitkeep").write_text("", encoding="utf-8")

    # 2. Fleet CLI & helpers
    for py_name in ("cu8000_setup.py", "fleet_config.py", "tv_client.py"):
        copy_file(FLEET_DIR / "bin" / py_name, stage_dir / "bin" / py_name, 0o755)

    # 3. Locally supplied runtime (sdb + samsung_tv_root + TV root payloads)
    copy_if_exists(runtime_dir / "sdb", stage_dir / "runtime" / "sdb", 0o755)
    if (runtime_dir / "python-lib" / "samsung_tv_root").is_dir():
        copy_tree_clean(runtime_dir / "python-lib" / "samsung_tv_root", stage_dir / "runtime" / "python-lib" / "samsung_tv_root")
    if (runtime_dir / "root-payloads").is_dir():
        copy_tree_clean(runtime_dir / "root-payloads", stage_dir / "runtime" / "root-payloads")

    # 4. On-boot scripts + optional locally built kernel modules & relaxed lib
    onboot_dst = stage_dir / "payloads" / "onboot"
    copy_file(FLEET_DIR / "tv-payloads" / "onboot" / "hub-init.awk", onboot_dst / "hub-init.awk", 0o755)
    copy_file(FLEET_DIR / "tv-payloads" / "onboot" / "onboot.sh", onboot_dst / "onboot.sh", 0o644)
    for opt in ("uepclear5.ko", "ueprestore5.ko", "uepclear6.ko", "ueprestore6.ko", "libkUEPUser.relaxed.so"):
        copy_if_exists(runtime_dir / "onboot" / opt, onboot_dst / opt, 0o755 if opt.endswith(".so") else 0o644)

    # 5. Bloat strip + rollback scripts
    strip_dst = stage_dir / "payloads" / "strip"
    copy_file(FLEET_DIR / "tv-payloads" / "strip" / "apply-strip.sh", strip_dst / "apply-strip.sh", 0o755)
    copy_file(FLEET_DIR / "tv-payloads" / "strip" / "restore-strip.sh", strip_dst / "restore-strip.sh", 0o755)

    # 6. TV Hub scripts, generic ASTC assets, and locally compiled ARMv7 binaries
    ui_dst = stage_dir / "payloads" / "ui"
    copy_file(REPO_ROOT / "tv-hub" / "start-hub.sh", ui_dst / "start-hub.sh", 0o755)
    copy_file(REPO_ROOT / "tv-hub" / "tune-tv.sh", ui_dst / "tune-tv.sh", 0o755)
    for asset in sorted((REPO_ROOT / "tv-hub" / "assets").iterdir()):
        if asset.is_file() and asset.suffix in (".astc", ".png"):
            copy_file(asset, ui_dst / "assets" / asset.name, 0o644)
    elf_reports: List[Dict[str, object]] = []
    if copy_if_exists(runtime_dir / "ui" / "living-room-hub-native", ui_dst / "living-room-hub-native", 0o755):
        elf_reports.append(audit_armv7_elf(ui_dst / "living-room-hub-native"))
    for fn in ("LivingRoomHub.dll", "LivingRoomHub.deps.json", "LivingRoomHub.runtimeconfig.json"):
        copy_if_exists(runtime_dir / "ui" / fn, ui_dst / fn, 0o644)

    # 7. VPN payloads (scripts from repo + optional locally built binaries/endpoints)
    vpn_dst = stage_dir / "payloads" / "vpn"
    for fn in ("ctl.sh", "wrapper.sh", "teardown.sh", "status-write.sh"):
        copy_file(FLEET_DIR / "tv-payloads" / "vpn" / fn, vpn_dst / fn, 0o755)
    for act in sorted((FLEET_DIR / "tv-payloads" / "vpn" / "actions").glob("vpn-*.sh")):
        copy_file(act, vpn_dst / "actions" / act.name, 0o755)
    if copy_if_exists(runtime_dir / "vpn" / "lightway-client", vpn_dst / "bin" / "lightway-client", 0o755):
        elf_reports.append(audit_armv7_elf(vpn_dst / "bin" / "lightway-client"))
    if copy_if_exists(runtime_dir / "vpn" / "codex-vpn-ctld", vpn_dst / "bin" / "codex-vpn-ctld", 0o755):
        elf_reports.append(audit_armv7_elf(vpn_dst / "bin" / "codex-vpn-ctld"))
    for opt in ("ca.pem", "endpoints.tsv", "locations.json"):
        copy_if_exists(runtime_dir / "vpn" / opt, vpn_dst / opt, 0o644)

    # 8. Portable Unified Media OS Gateway bundle
    copy_tree_clean(REPO_ROOT / "gateway", stage_dir / "gateway")
    os.chmod(stage_dir / "gateway" / "setup-gateway.sh", 0o755)

    # ---- Pre-release safety & integrity gates ----
    leak_report = verify_no_private_files(stage_dir, deny_terms or [])
    shell_count = verify_shell_scripts(stage_dir)
    py_ver = verify_python_imports(stage_dir)

    file_entries: Dict[str, Dict[str, object]] = {}
    sum_lines: List[str] = []
    for p in sorted(stage_dir.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(stage_dir).as_posix()
        digest = sha256_file(p)
        file_entries[rel] = {"bytes": p.stat().st_size, "sha256": digest}
        sum_lines.append(f"{digest}  {rel}")

    manifest = {
        "package": pkg_name,
        "version": version,
        "built_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "gates": {
            "privacy_gate": leak_report,
            "shell_scripts_checked": shell_count,
            "python_import_verified": py_ver,
            "armv7_elf_audits": elf_reports,
        },
        "file_count": len(file_entries),
        "files": file_entries,
    }
    (stage_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (stage_dir / "SHA256SUMS.txt").write_text("\n".join(sum_lines) + "\n", encoding="utf-8")

    zip_path = out_dir / f"{pkg_name}.zip"
    write_zip_preserving_modes(stage_dir, zip_path)
    return {
        "stage_dir": str(stage_dir),
        "zip_path": str(zip_path),
        "zip_bytes": zip_path.stat().st_size,
        "zip_sha256": sha256_file(zip_path),
        "file_count": len(file_entries) + 2,
        "gates": manifest["gates"],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description="Build a shareable macOS CU8000 Fleet Kit zip archive")
    parser.add_argument("--runtime-dir", type=pathlib.Path, default=REPO_ROOT / "local-runtime")
    parser.add_argument("--version", default=dt.datetime.now().strftime("v%Y.%m.%d"))
    parser.add_argument("--out-dir", type=pathlib.Path, default=REPO_ROOT / "dist")
    parser.add_argument("--deny", action="append", default=[], help="Extra sensitive strings that must never appear in the zip")
    args = parser.parse_args()
    summary = build_package(args.version, args.out_dir.resolve(), args.runtime_dir.resolve(), deny_terms=args.deny)
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
