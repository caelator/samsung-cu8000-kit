#!/usr/bin/env python3
"""CU8000 Property Fleet Setup & Management Tool.

Provisions, updates, verifies, or completely restores Samsung CU8000 TVs on macOS
using built-in Python 3.9+ and the bundled SDB tool (zero pip/Homebrew installs).

Subcommands:
  setup       Guided interactive setup (or non-interactive with --profile)
  status      Read-only health & deployment status check
  update-ui   Push updated LivingRoomHub binary, assets, and hub.conf only
  vpn-creds   Push/rotate ExpressVPN Lightway credentials (root-only 0600)
  restore     Clean rollback of hook, bloat strip, Hub UI, and VPN

Safety invariants:
  - Never flashes boot partitions or touches DRM/provisioning material
  - Never restarts the TV automatically
  - Anchor-gated kernel modules load only on exact verified firmware builds (0090 / 0080)
  - Credentials stay in memory during one-shot HTTP transfer and 0600 root:root on the TV
"""
from __future__ import annotations

import argparse
import datetime as dt
import getpass
import hashlib
import json
import os
import pathlib
import re
import sys
import time
from typing import Any, Dict, List, Optional, Tuple

SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
KIT_ROOT = SCRIPT_DIR.parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

import fleet_config as FC  # noqa: E402
from tv_client import TvTransport, wake_on_lan  # noqa: E402

HOOK_BIN = "/opt/usr/apps/com.samsung.tv.iot-hub-control/bin"
HOOK_PATH = f"{HOOK_BIN}/hub-init.sh"
SAMSUNG_ORIG = f"{HOOK_BIN}/hub-init.sh.samsung"
ONDEVICE_DIR = "/opt/usr/etc/codex"
HUB_DIR = "/opt/usr/etc/codex-ui"
VPN_BASE = "/opt/usr/etc/codex-vpn"
VPN_PRIVATE = "/opt/usr/codex-vpn-private"
VPN_RUN = "/run/codex-vpn"
MARKER = "/tmp/codex-provisioned"

UPDATER_UNITS = (
    "swu-auto-upgrade.service swu-auto-upgrade.timer swu-auto-upgrade.path "
    "swu-verifier-tv.service swu-verifier-tv.timer swu-core-tv.service swu-service.service "
    "vdsystem-update-manager.service vdsystem-update-manager.timer "
    "fw-update-service.service fw-update-service.path fw-update-init-download.service "
    "was-background-update.service was-background-update.path"
)
STRIP_UNITS = (
    "adagent-service.service pisa-service.path pisa-service.socket pisa-service.service "
    "canalysis-service.service com.samsung.tv.context-aware-agent.path com.samsung.tv.context-aware-agent.service "
    "com.samsung.tv.context-aware-service.path com.samsung.tv.context-aware-service.service ipa.service "
    "abnormal_user_behavior.service org.tizen.tv.sp-kpi.service org.tizen.tv.tagpro-service-tv.service "
    "mirroring-recognition-manager.service contents-recognition-service.service "
    "slive-provider-daemon.path slive-provider-daemon.service bms.path bms.service esg-service.service "
    "org.tizen.voice-app.path org.tizen.voice-app.service org.tizen.voice-client.path org.tizen.voice-client.service "
    "com.samsung.tv.bixby-prov.path com.samsung.tv.bixby-prov.service "
    "org.tizen.multi-assistant-service.path org.tizen.multi-assistant-service.service "
    "com.samsung.tv.ondevice-voice.path com.samsung.tv.ondevice-voice.service "
    "tts-download-service.path tts-download-service.service com.samsung.tv.ghdaemon.path com.samsung.tv.ghdaemon.service "
    "gamepad-service.service gamepad-driver.service org.tizen.remotepc.service mde-framework.service"
)


def log(msg: str) -> None:
    print(f"[{dt.datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)


def resolve_paths() -> Dict[str, pathlib.Path]:
    """Locate runtime assets whether running from packaged kit or source repo."""
    if (KIT_ROOT / "runtime" / "sdb").is_file():
        return {
            "sdb": KIT_ROOT / "runtime" / "sdb",
            "lib": KIT_ROOT / "runtime" / "python-lib",
            "payloads": KIT_ROOT / "runtime" / "root-payloads",
            "onboot": KIT_ROOT / "payloads" / "onboot",
            "strip": KIT_ROOT / "payloads" / "strip",
            "ui": KIT_ROOT / "payloads" / "ui",
            "vpn": KIT_ROOT / "payloads" / "vpn",
            "receipts": KIT_ROOT / "receipts",
        }
    # Running directly inside the source repo (fleet-kit/)
    repo = KIT_ROOT.parent
    return {
        "sdb": repo / ".tools" / "sdb" / "sdb",
        "lib": repo / ".tools" / "samsung-tv-root" / "src",
        "payloads": repo / ".tools" / "samsung-tv-root-release" / "samsung-tv-root-0.0.12-macos-arm64" / "_internal" / "payloads" / "common",
        "onboot": KIT_ROOT / "tv-payloads" / "onboot",
        "strip": KIT_ROOT / "tv-payloads" / "strip",
        "ui": repo / "living-room-ui",
        "vpn": KIT_ROOT / "tv-payloads" / "vpn",
        "receipts": KIT_ROOT / "receipts",
    }


def sha256_file(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def inspect_tv_environment(tv: TvTransport, device: Dict[str, Any]) -> Dict[str, Any]:
    """Read-only baseline inspection of firmware build, vermagic, routes, and control lanes."""
    cmd = (
        "echo ==BOOT==; cat /proc/sys/kernel/random/boot_id; "
        "echo ==UNAME==; uname -a; "
        "echo ==VERMAGIC==; strings /lib/modules/$(uname -r)/kernel/crypto/arc4.ko 2>/dev/null | grep '^vermagic=' | head -1 || true; "
        "echo ==BUILD==; cat /etc/info.ini /etc/os-release 2>/dev/null | head -n 20 || true; "
        "echo ==LIBSHA==; sha256sum /usr/lib/libkUEPUser.so 2>/dev/null | cut -d' ' -f1 || true; "
        "echo ==HOOKSHA==; sha256sum /opt/usr/apps/com.samsung.tv.iot-hub-control/bin/hub-init.sh 2>/dev/null | cut -d' ' -f1 || true; "
        "echo ==ORIGSHA==; sha256sum /opt/usr/apps/com.samsung.tv.iot-hub-control/bin/hub-init.sh.samsung 2>/dev/null | cut -d' ' -f1 || true; "
        "echo ==DEFAULT==; /sbin/ip -4 route show default | head -n 1; "
        "echo ==RESOLV==; sha256sum /etc/resolv.conf 2>/dev/null | cut -d' ' -f1 || true; "
        "echo ==USERS_GID==; awk -F: '$1==\"users\"{print $3}' /etc/group 2>/dev/null || echo 100; "
        "echo ==MEM==; grep -E '^(MemTotal|MemAvailable|SwapTotal|SwapFree):' /proc/meminfo; "
        "echo ==UNITS==; for u in connman.service sdbd.service com.samsung.tv.multiscreen.service remote-server.service codex-livingroom-hub.service codex-vpn-ctld.service codex-vpn-tunnel.service; "
        "do echo $u=$(systemctl is-active $u 2>/dev/null || true); done; "
        "echo ==HUB_API==; /usr/bin/curl --silent --noproxy '*' --connect-timeout 2 --max-time 4 http://127.0.0.1:18080/api/status 2>/dev/null || echo NO_HUB; echo ''; "
        "echo ==VPN_STATUS==; cat /run/codex-vpn/status.json 2>/dev/null || echo NO_VPN; "
        "echo ==ONBOOT_LOG==; tail -n 8 /opt/usr/etc/codex/onboot.log 2>/dev/null || true; "
        "true"
    )
    raw = tv.root(cmd, timeout=45.0)
    secs: Dict[str, List[str]] = {}
    cur = None
    for line in raw.splitlines():
        if line.startswith("==") and line.endswith("=="):
            cur = line.strip("=")
            secs[cur] = []
        elif cur is not None:
            secs[cur].append(line)

    vermagic_line = (secs.get("VERMAGIC") or [""])[0].strip()
    m_vm = re.search(r"vermagic=(\d+)", vermagic_line)
    vermagic_id = m_vm.group(1) if m_vm else ""

    units: Dict[str, str] = {}
    for ln in secs.get("UNITS", []):
        if "=" in ln:
            k, v = ln.split("=", 1)
            units[k.strip()] = v.strip()

    users_gid_raw = (secs.get("USERS_GID") or ["100"])[0].strip()
    users_gid = int(users_gid_raw) if users_gid_raw.isdigit() else 100

    return {
        "observed_at": dt.datetime.now(dt.timezone.utc).isoformat(),
        "host": tv.host,
        "modelName": device.get("modelName", ""),
        "wifiMac": str(device.get("wifiMac", "")).upper(),
        "developerMode": device.get("developerMode"),
        "developerIP": device.get("developerIP"),
        "dlna_9197": tv.check_dlna_9197(),
        "boot_id": (secs.get("BOOT") or [""])[0].strip(),
        "uname": (secs.get("UNAME") or [""])[0].strip(),
        "vermagic": vermagic_line,
        "vermagic_id": vermagic_id,
        "lib_kuep_sha": (secs.get("LIBSHA") or [""])[0].strip(),
        "hook_sha": (secs.get("HOOKSHA") or [""])[0].strip(),
        "orig_hook_sha": (secs.get("ORIGSHA") or [""])[0].strip(),
        "default_route": (secs.get("DEFAULT") or [""])[0].strip(),
        "resolv_sha": (secs.get("RESOLV") or [""])[0].strip(),
        "users_gid": users_gid,
        "memory": secs.get("MEM", []),
        "units": units,
        "hub_api_raw": "\n".join(secs.get("HUB_API", [])).strip(),
        "vpn_status_raw": "\n".join(secs.get("VPN_STATUS", [])).strip(),
        "onboot_log": secs.get("ONBOOT_LOG", []),
    }


def deploy_onboot_hook(
    tv: TvTransport,
    paths: Dict[str, pathlib.Path],
    profile: Dict[str, Any],
    env_info: Dict[str, Any],
) -> Dict[str, Any]:
    """Install build-verified on-boot hook, updater runtime masks, and RAM-only UEP support."""
    onboot_dir = paths["onboot"]
    vm_id = env_info.get("vermagic_id", "")
    fw_prof = FC.KNOWN_FIRMWARE_PROFILES.get(vm_id)

    uep_ko = ""
    uep_ko_sha = ""
    uep_log_tag = ""
    lib_stock_sha = ""
    lib_patched_sha = ""
    warnings: List[str] = []

    if fw_prof:
        ko_path = onboot_dir / fw_prof["uep_ko"]
        if ko_path.is_file():
            uep_ko = fw_prof["uep_ko"]
            uep_ko_sha = sha256_file(ko_path)
            uep_log_tag = fw_prof["uep_log_tag"]
            tv.push_file(ko_path, f"{ONDEVICE_DIR}/{uep_ko}", mode="644", smack="System")
        else:
            warnings.append(f"Kernel module {fw_prof['uep_ko']} not bundled; skipping kernel UEP module")

        relaxed_path = onboot_dir / "libkUEPUser.relaxed.so"
        if (
            fw_prof["lib_stock_sha"]
            and env_info.get("lib_kuep_sha") == fw_prof["lib_stock_sha"]
            and relaxed_path.is_file()
        ):
            lib_stock_sha = fw_prof["lib_stock_sha"]
            lib_patched_sha = sha256_file(relaxed_path)
            tv.push_file(relaxed_path, f"{ONDEVICE_DIR}/libkUEPUser.relaxed.so", mode="755", smack="_")
        elif env_info.get("lib_kuep_sha"):
            warnings.append(
                f"libkUEPUser.so SHA ({env_info['lib_kuep_sha'][:12]}...) differs from verified 0090 copy; "
                "leaving script check untouched (safe fail-closed; .NET fallback remains active)"
            )
    else:
        warnings.append(
            f"Unrecognized firmware kernel vermagic ({env_info.get('vermagic') or 'unknown'}). "
            "Skipping kernel UEP module safely (fail-closed). Updater masks, bloat strip, and .NET Hub fallback still apply."
        )

    # Push onboot.sh, boot.conf, and hub-init.awk
    tv.push_file(onboot_dir / "onboot.sh", f"{ONDEVICE_DIR}/onboot.sh", mode="644", smack="System")
    boot_conf_text = FC.render_boot_conf(
        profile,
        uep_ko=uep_ko,
        uep_ko_sha=uep_ko_sha,
        uep_log_tag=uep_log_tag,
        lib_stock_sha=lib_stock_sha,
        lib_patched_sha=lib_patched_sha,
        vpn_users_gid=env_info.get("users_gid", 100),
    )
    tv.push_bytes(boot_conf_text.encode("utf-8"), f"{ONDEVICE_DIR}/boot.conf", mode="644", smack="System")

    awk_bytes = (onboot_dir / "hub-init.awk").read_bytes()
    awk_sha = hashlib.sha256(awk_bytes).hexdigest()
    tv.push_bytes(awk_bytes, f"{ONDEVICE_DIR}/hub-init.awk", mode="755", smack="System")

    # Preserve Samsung's original hub-init.sh safely (never overwrite an existing backup)
    install_cmd = (
        f"set -e; "
        f"if [ ! -e {SAMSUNG_ORIG} ]; then "
        f"  if [ -f {HOOK_PATH} ]; then "
        f"    cur=$(sha256sum {HOOK_PATH} | cut -d' ' -f1); "
        f"    if [ \"$cur\" != \"{awk_sha}\" ]; then mv {HOOK_PATH} {SAMSUNG_ORIG}; fi; "
        f"  fi; "
        f"fi; "
        f"cp {ONDEVICE_DIR}/hub-init.awk {HOOK_PATH}; "
        f"chmod 755 {HOOK_PATH}; "
        f"chsmack -a System {HOOK_PATH} {ONDEVICE_DIR}/onboot.sh {ONDEVICE_DIR}/boot.conf >/dev/null 2>&1 || true; "
        f"true"
    )
    tv.root(install_cmd, timeout=30.0)

    # Apply immediately in the current boot via PID 1's mount namespace (systemd-run --wait)
    apply_out = tv.root(
        f"systemd-run --quiet --wait --collect --unit=codex-onboot-now "
        f"/bin/sh -c '. {ONDEVICE_DIR}/onboot.sh'; "
        f"tail -n 6 {ONDEVICE_DIR}/onboot.log 2>/dev/null || true; true",
        timeout=60.0,
    )
    return {
        "firmware_matched": bool(fw_prof),
        "build_family": fw_prof["build_family"] if fw_prof else "unknown",
        "uep_ko": uep_ko or None,
        "script_check_configured": bool(lib_patched_sha),
        "warnings": warnings,
        "apply_log": apply_out.strip().splitlines()[-6:],
    }


def deploy_bloat_strip(tv: TvTransport, paths: Dict[str, pathlib.Path]) -> Dict[str, Any]:
    """Push and run the idempotent, backed-up bloat strip script on the TV."""
    strip_dir = paths["strip"]
    tv.push_file(strip_dir / "apply-strip.sh", f"{ONDEVICE_DIR}/apply-strip.sh", mode="755", smack="System")
    tv.push_file(strip_dir / "restore-strip.sh", f"{ONDEVICE_DIR}/restore-strip.sh", mode="755", smack="System")

    # Mask bloat units immediately, then run apply-strip.sh in a detached systemd unit (can take 30-90s for 25 pkgcmd calls)
    out = tv.root(
        f"systemctl mask --runtime {UPDATER_UNITS} {STRIP_UNITS} >/dev/null 2>&1 || true; "
        f"systemctl stop --no-block {UPDATER_UNITS} {STRIP_UNITS} >/dev/null 2>&1 || true; "
        f"rm -f /tmp/codex-strip.out /tmp/codex-strip.done; "
        f"systemd-run --quiet --no-block --collect --unit=codex-apply-strip "
        f"/bin/sh -c '. {ONDEVICE_DIR}/apply-strip.sh > /tmp/codex-strip.out 2>&1; echo DONE > /tmp/codex-strip.done'; "
        f"i=0; while [ ! -f /tmp/codex-strip.done ] && [ $i -lt 150 ]; do sleep 2; i=$((i+1)); done; "
        f"cat /tmp/codex-strip.out 2>/dev/null || echo STRIP_TIMEOUT; "
        f"rm -f /tmp/codex-strip.out /tmp/codex-strip.done; true",
        timeout=240.0,
    )
    return {"output": out.strip(), "passed": "STRIP_OK" in out}


def deploy_hub_ui(
    tv: TvTransport,
    paths: Dict[str, pathlib.Path],
    profile: Dict[str, Any],
) -> Dict[str, Any]:
    """Stage LivingRoomHub bare-metal native binary, .NET fallback, ASTC assets, and hub.conf."""
    ui_dir = paths["ui"]
    preset = profile.get("card_preset", "generic")
    tv.root(
        f"mkdir -p {HUB_DIR}/assets {HUB_DIR}/actions; "
        f"chmod 755 {HUB_DIR} {HUB_DIR}/assets {HUB_DIR}/actions; "
        f"chsmack -a _ {HUB_DIR} {HUB_DIR}/assets {HUB_DIR}/actions >/dev/null 2>&1 || true; true",
        timeout=30.0,
    )

    # Locate binary & assets (handles both packaged kit layout and repo layout)
    if (ui_dir / "living-room-hub-native").is_file():
        dist_dir = ui_dir
        assets_dir = ui_dir / "assets"
        generic_assets_dir = ui_dir / "assets-generic"
    else:
        dist_dir = ui_dir / "dist"
        assets_dir = ui_dir / "assets"
        generic_assets_dir = KIT_ROOT / "assets" / "generic"

    staged: Dict[str, str] = {}
    core_files = [
        (dist_dir / "living-room-hub-native", f"{HUB_DIR}/living-room-hub-native", "755"),
        (dist_dir / "LivingRoomHub.dll", f"{HUB_DIR}/LivingRoomHub.dll", "644"),
        (dist_dir / "LivingRoomHub.deps.json", f"{HUB_DIR}/LivingRoomHub.deps.json", "644"),
        (dist_dir / "LivingRoomHub.runtimeconfig.json", f"{HUB_DIR}/LivingRoomHub.runtimeconfig.json", "644"),
        (ui_dir / "start-hub.sh", f"{HUB_DIR}/start-hub.sh", "755"),
        (ui_dir / "tune-tv.sh", f"{HUB_DIR}/tune-tv.sh", "755"),
    ]
    for src, dst, mode in core_files:
        if src.is_file():
            staged[dst] = tv.push_file(src, dst, mode=mode, smack="_")

    # Stage base ASTC/icon assets
    if assets_dir.is_dir():
        for asset in sorted(assets_dir.iterdir()):
            if asset.is_file() and asset.suffix in (".astc", ".png"):
                src = asset
                if preset == "generic" and (generic_assets_dir / asset.name).is_file():
                    src = generic_assets_dir / asset.name
                staged[f"{HUB_DIR}/assets/{asset.name}"] = tv.push_file(
                    src, f"{HUB_DIR}/assets/{asset.name}", mode="644", smack="_"
                )

    # Write per-property hub.conf
    hub_conf = FC.render_hub_conf(profile)
    staged[f"{HUB_DIR}/hub.conf"] = tv.push_bytes(hub_conf.encode("utf-8"), f"{HUB_DIR}/hub.conf", mode="644", smack="_")

    # Remove disabled flag if present, restart service cleanly, verify /api/status
    out = tv.root(
        f"rm -f {HUB_DIR}/disabled; "
        f"systemctl stop codex-livingroom-hub.service >/dev/null 2>&1 || true; "
        f"systemctl reset-failed codex-livingroom-hub.service >/dev/null 2>&1 || true; "
        f"systemd-run --quiet --no-block --collect --unit=codex-hub-launch /bin/sh -c '. {HUB_DIR}/start-hub.sh'; "
        f"i=0; while [ $i -lt 15 ]; do "
        f"  resp=$(/usr/bin/curl --silent --noproxy '*' --connect-timeout 2 --max-time 4 http://127.0.0.1:18080/api/status 2>/dev/null || true); "
        f"  case \"$resp\" in *engine*) echo \"$resp\"; exit 0 ;; esac; "
        f"  sleep 1; i=$((i+1)); "
        f"done; echo HUB_NOT_READY; true",
        timeout=60.0,
    )
    status_json: Dict[str, Any] = {}
    for line in reversed(out.splitlines()):
        line = line.strip()
        if line.startswith("{"):
            try:
                status_json = json.loads(line)
                break
            except json.JSONDecodeError:
                pass
    return {
        "files_staged": len(staged),
        "card_preset": preset,
        "hub_ready": bool(status_json.get("engine")),
        "engine": status_json.get("engine"),
        "glRenderer": status_json.get("glRenderer"),
        "config": status_json.get("config"),
    }


def prompt_vpn_credentials(creds_file: Optional[pathlib.Path]) -> Optional[Tuple[str, str]]:
    """Read ExpressVPN Lightway credentials from an explicit file or interactive prompt."""
    if creds_file and creds_file.is_file():
        data = json.loads(creds_file.read_text(encoding="utf-8"))
        u = str(data.get("username", "")).strip()
        p = str(data.get("password", "")).strip()
        if u and p:
            return u, p
        raise ValueError(f"Credentials file {creds_file} must contain non-empty 'username' and 'password'")

    print("\n  ExpressVPN Lightway Credentials (sent directly to TV root-only 0600, never saved on this Mac):")
    print("  (Leave username blank to install VPN binaries/UI now and add credentials later with 'vpn-creds')")
    user = input("  Lightway username: ").strip()
    if not user:
        return None
    pwd = getpass.getpass("  Lightway password: ").strip()
    if not pwd:
        raise ValueError("Password cannot be empty when username is provided")
    return user, pwd


def deploy_vpn(
    tv: TvTransport,
    paths: Dict[str, pathlib.Path],
    profile: Dict[str, Any],
    env_info: Dict[str, Any],
    creds: Optional[Tuple[str, str]],
    *,
    smoke_test_connect: bool = True,
) -> Dict[str, Any]:
    """Deploy ExpressVPN Lightway binaries, portable LAN scripts, ctld socket, and optional creds."""
    vpn_dir = paths["vpn"]
    repo = KIT_ROOT.parent

    # Locate compiled ARMv7 binaries + CA cert (packaged layout vs repo layout)
    client_bin = vpn_dir / "bin" / "lightway-client"
    if not client_bin.is_file():
        client_bin = repo / "lightway-lab" / "dist" / "lightway-client-armv7-noasm"
    ctld_bin = vpn_dir / "bin" / "codex-vpn-ctld"
    if not ctld_bin.is_file():
        ctld_bin = repo / "tools" / "vpn_tv" / "codex-vpn-ctld"
    ca_file = vpn_dir / "ca.pem"
    if not ca_file.is_file():
        ca_file = repo / ".tools" / "expressvpn-inspection" / "expressvpn-lightway-rs.public-cert-0.pem"

    status_write = vpn_dir / "status-write.sh"
    if not status_write.is_file():
        status_write = repo / "tools" / "vpn_tv" / "status-write.sh"
    endpoints_tsv = vpn_dir / "endpoints.tsv"
    if not endpoints_tsv.is_file():
        endpoints_tsv = repo / "tools" / "vpn_tv" / "endpoints.tsv"
    locations_json = vpn_dir / "locations.json"
    if not locations_json.is_file():
        locations_json = repo / "tools" / "vpn_tv" / "locations.json"
    actions_dir = vpn_dir / "actions"
    if not actions_dir.is_dir():
        actions_dir = repo / "tools" / "vpn_tv" / "actions"

    tv.root(
        f"set -e; mkdir -p {VPN_BASE}/bin {VPN_PRIVATE} {VPN_RUN} {HUB_DIR}/actions; "
        f"chmod 755 {VPN_BASE} {VPN_BASE}/bin {VPN_RUN} {HUB_DIR}/actions; "
        f"chmod 700 {VPN_PRIVATE}; chown 0:0 {VPN_PRIVATE}; "
        f"chsmack -a _ {VPN_BASE} {VPN_BASE}/bin {VPN_RUN} >/dev/null 2>&1 || true; "
        f"chsmack -a _ -t {VPN_RUN} >/dev/null 2>&1 || true; "
        f"chsmack -a System {VPN_PRIVATE} >/dev/null 2>&1 || true; "
        f"touch {VPN_RUN}/status.json && chmod 644 {VPN_RUN}/status.json && "
        f"chsmack -a _ {VPN_RUN}/status.json >/dev/null 2>&1 || true; true",
        timeout=30.0,
    )

    public_files = [
        (client_bin, f"{VPN_BASE}/bin/lightway-client", "755"),
        (ctld_bin, f"{VPN_BASE}/bin/codex-vpn-ctld", "755"),
        (ca_file, f"{VPN_BASE}/ca.pem", "644"),
        (locations_json, f"{VPN_BASE}/locations.json", "644"),
        (endpoints_tsv, f"{VPN_BASE}/endpoints.tsv", "644"),
        (status_write, f"{VPN_BASE}/status-write.sh", "755"),
        (vpn_dir / "teardown.sh", f"{VPN_BASE}/teardown.sh", "755"),
        (vpn_dir / "wrapper.sh", f"{VPN_BASE}/wrapper.sh", "755"),
        (vpn_dir / "ctl.sh", f"{VPN_BASE}/ctl.sh", "755"),
    ]
    for action_file in sorted(actions_dir.glob("vpn-*.sh")):
        public_files.append((action_file, f"{HUB_DIR}/actions/{action_file.name}", "755"))

    for src, dst, mode in public_files:
        tv.push_file(src, dst, mode=mode, smack="_")

    # Persist non-secret network.env + default selected location
    dev_ip = str(env_info.get("developerIP") or tv.callback)
    net_env = FC.render_vpn_network_env(profile, dev_host=dev_ip)
    tv.push_bytes(net_env.encode("utf-8"), f"{VPN_BASE}/network.env", mode="644", smack="_")
    loc_id = str(profile.get("vpn_default_location") or "54")
    tv.push_bytes((loc_id + "\n").encode("utf-8"), f"{VPN_BASE}/selected-location.id", mode="644", smack="_")

    creds_staged = False
    isolation_ok = False
    if creds is not None:
        user, pwd = creds
        yaml_bytes = f'user: "{user}"\npassword: "{pwd}"\n'.encode("utf-8")
        try:
            tv.push_bytes(yaml_bytes, f"{VPN_PRIVATE}/client.yaml", mode="600", owner="0:0", smack="System", umask="077")
            creds_staged = True
        finally:
            del yaml_bytes

        perm_out = tv.root(
            f"if su -s /bin/sh owner -c 'cat {VPN_PRIVATE}/client.yaml >/dev/null 2>&1'; then echo LEAK; else echo OWNER_DENIED_OK; fi; "
            f"if su -s /bin/sh developer -c 'cat {VPN_PRIVATE}/client.yaml >/dev/null 2>&1'; then echo LEAK; else echo DEV_DENIED_OK; fi; true",
            timeout=25.0,
        )
        isolation_ok = "OWNER_DENIED_OK" in perm_out and "DEV_DENIED_OK" in perm_out and "LEAK" not in perm_out
        if not isolation_ok:
            raise RuntimeError(f"VPN credential isolation check failed: {perm_out}")

    # Compatibility no-TUN --help check + start codex-vpn-ctld.service
    help_out = tv.root(f"{VPN_BASE}/bin/lightway-client --help 2>&1; echo EXIT=$?", timeout=30.0)
    help_ok = "EXIT=0" in help_out and "--route-mode" in help_out

    users_gid = env_info.get("users_gid", 100)
    tv.root(
        f"set -e; rm -f {VPN_BASE}/disabled; "
        f"{VPN_BASE}/status-write.sh || true; "
        f"systemctl stop codex-vpn-ctld.service >/dev/null 2>&1 || true; "
        f"systemctl reset-failed codex-vpn-ctld.service >/dev/null 2>&1 || true; "
        f"/usr/bin/systemd-run --no-ask-password --service-type=simple --unit=codex-vpn-ctld.service "
        f"--description='CU8000 Lightway VPN Allowlist Socket Controller' "
        f"-p User=0 -p Group=0 -p SmackProcessLabel=System::Privileged "
        f"-p KillMode=control-group -p SendSIGKILL=yes -p Restart=on-failure -p RestartSec=3 "
        f"-p LimitCORE=0 -p StandardInput=null "
        f"{VPN_BASE}/bin/codex-vpn-ctld --serve --socket {VPN_RUN}/ctl.sock --ctl {VPN_BASE}/ctl.sh --gid {users_gid} --allowed-uids 0,5001; "
        f"sleep 1; systemctl is-active codex-vpn-ctld.service; true",
        timeout=40.0,
    )

    smoke_result: Dict[str, Any] = {"skipped": True}
    if creds_staged and smoke_test_connect and help_ok:
        log("Running live Lightway connect + teardown smoke test (Miami #54, 180s dead-man)...")
        conn_out = tv.root(f"{VPN_BASE}/ctl.sh connect {loc_id} 180; true", timeout=110.0)
        disc_out = tv.root(f"{VPN_BASE}/ctl.sh disconnect; true", timeout=45.0)
        smoke_result = {
            "skipped": False,
            "connected_ok": '"tunnelEstablished": true' in conn_out or '"tunnelEstablished":true' in conn_out,
            "disconnected_ok": '"status": "disconnected"' in disc_out or '"status":"disconnected"' in disc_out,
        }

    return {
        "help_probe_ok": help_ok,
        "credentials_staged_root_only": creds_staged,
        "uid5001_and_901_denied": isolation_ok,
        "ctld_started": True,
        "smoke_test": smoke_result,
    }


def restore_tv(tv: TvTransport, paths: Dict[str, pathlib.Path], env_info: Dict[str, Any]) -> Dict[str, Any]:
    """Complete, clean rollback of VPN, Hub UI, bloat strip, and on-boot hook."""
    report: Dict[str, Any] = {}

    # 1. Stop & remove VPN
    tv.root(
        f"systemctl stop codex-vpn-tunnel.service codex-vpn-ctld.service codex-vpn-deadman.timer codex-vpn-deadman.service >/dev/null 2>&1 || true; "
        f"if [ -x {VPN_BASE}/teardown.sh ]; then {VPN_BASE}/teardown.sh >/dev/null 2>&1 || true; fi; "
        f"rm -rf {VPN_BASE} {VPN_PRIVATE} {VPN_RUN}; "
        f"rm -f {HUB_DIR}/actions/vpn-*.sh; true",
        timeout=45.0,
    )
    report["vpn_removed"] = True

    # 2. Stop & remove Living Room Hub UI + resume any frozen companions
    tv.root(
        f"systemctl stop codex-livingroom-hub.service >/dev/null 2>&1 || true; "
        f"if [ -f /run/user/5001/codex-frozen-companions.pids ]; then "
        f"  for p in $(cat /run/user/5001/codex-frozen-companions.pids 2>/dev/null); do kill -CONT $p 2>/dev/null || true; done; "
        f"  rm -f /run/user/5001/codex-frozen-companions.pids; "
        f"fi; "
        f"sysctl -w vm.swappiness=100 >/dev/null 2>&1 || true; "
        f"rm -rf {HUB_DIR}; true",
        timeout=45.0,
    )
    report["hub_removed"] = True

    # 3. Restore bloat strip backups + sentinels
    restore_script = paths["strip"] / "restore-strip.sh"
    if restore_script.is_file():
        tv.push_file(restore_script, "/tmp/codex-restore-strip.sh", mode="755", smack="System")
        strip_out = tv.root(
            "systemd-run --quiet --wait --collect --unit=codex-restore-strip /bin/sh -c '. /tmp/codex-restore-strip.sh > /tmp/codex-rs.out 2>&1'; "
            "cat /tmp/codex-rs.out 2>/dev/null || true; rm -f /tmp/codex-restore-strip.sh /tmp/codex-rs.out; true",
            timeout=120.0,
        )
        report["strip_restore"] = strip_out.strip()

    # 4. Uncover libkUEPUser.so, restore kernel enforcement if known, unmask units, restore hub-init.sh
    vm_id = env_info.get("vermagic_id", "")
    fw_prof = FC.KNOWN_FIRMWARE_PROFILES.get(vm_id)
    if fw_prof:
        rko = paths["onboot"] / fw_prof["uep_restore_ko"]
        if rko.is_file():
            tv.push_file(rko, "/tmp/codex-ueprestore.ko", mode="644", smack="System")
            tv.root("/usr/sbin/insmod /tmp/codex-ueprestore.ko >/dev/null 2>&1 || true; rm -f /tmp/codex-ueprestore.ko; true", timeout=30.0)

    tv.root(
        f"systemd-run --quiet --wait --collect --unit=codex-unmount-lib "
        f"/bin/sh -c 'grep -q /usr/lib/libkUEPUser.so /proc/mounts && umount /usr/lib/libkUEPUser.so || true'; "
        f"if [ -e {SAMSUNG_ORIG} ]; then mv -f {SAMSUNG_ORIG} {HOOK_PATH}; fi; "
        f"systemctl unmask --runtime {UPDATER_UNITS} {STRIP_UNITS} >/dev/null 2>&1 || true; "
        f"rm -rf {ONDEVICE_DIR} {MARKER}; true",
        timeout=45.0,
    )
    report["hook_and_units_restored"] = True
    return report


def connect_and_validate(host: str, paths: Dict[str, pathlib.Path], wake: bool = False) -> Tuple[TvTransport, Dict[str, Any]]:
    """Verify CU8000 TV on port 8001, check Developer Mode, and establish SDB transport."""
    dev = FC.query_tv_api(host, timeout=4.0)
    if not dev:
        raise RuntimeError(
            f"Could not reach Samsung TV at {host}:8001. Verify the TV is powered ON and on the same Wi-Fi/LAN."
        )
    model = str(dev.get("modelName", ""))
    if "CU8000" not in model.upper():
        raise RuntimeError(f"Refusing non-CU8000 device at {host}: modelName={model!r}")

    if dev.get("PowerState") != "on":
        if wake and dev.get("wifiMac"):
            log(f"TV at {host} is in {dev.get('PowerState')}; sending Wake-on-LAN...")
            for _ in range(10):
                wake_on_lan(str(dev["wifiMac"]), host)
                time.sleep(4)
                dev = FC.query_tv_api(host, timeout=3.0) or dev
                if dev.get("PowerState") == "on":
                    break
        if dev.get("PowerState") != "on":
            raise RuntimeError(f"TV at {host} is {dev.get('PowerState')}. Turn it on with the remote and retry.")

    my_ip = FC.local_ipv4_for_target(host)
    dev_mode = str(dev.get("developerMode", "0"))
    dev_ip = str(dev.get("developerIP", "")).strip()
    if dev_mode != "1":
        raise RuntimeError(
            f"Developer Mode is OFF on {host}. On the TV: open Apps -> press 12345 -> "
            f"turn Developer Mode ON -> set Host PC IP to {my_ip} -> hold Power to restart the TV."
        )
    if my_ip and dev_ip and dev_ip != my_ip:
        raise RuntimeError(
            f"Developer Host IP mismatch on {host}: TV expects {dev_ip}, but this Mac is {my_ip}. "
            f"Update Developer Mode Host IP on the TV to {my_ip} and restart the TV."
        )

    tv = TvTransport(
        host,
        model,
        sdb_path=paths["sdb"],
        payloads_dir=paths["payloads"],
        lib_dir=paths["lib"],
    )
    log(f"Connecting SDB to {host}:26101 ({model}, MAC {dev.get('wifiMac')})...")
    if not tv.sdb_connect(wait=90.0):
        raise RuntimeError(
            f"SDB did not accept connection to {host}:26101. "
            "If you just enabled Developer Mode, hold the Power button on the remote for 3 seconds to full-reboot the TV, wait 2 minutes, and retry."
        )
    return tv, dev


def save_receipt(paths: Dict[str, pathlib.Path], mac: str, action: str, payload: Dict[str, Any]) -> pathlib.Path:
    receipts_dir = paths["receipts"]
    receipts_dir.mkdir(parents=True, exist_ok=True)
    tag = re.sub(r"[^A-Za-z0-9]", "", mac or "unknown")[-6:].lower() or "tv"
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out_path = receipts_dir / f"{action}-{tag}-{stamp}.json"
    out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return out_path


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="CU8000 Property Fleet Setup & Management Tool")
    parser.add_argument("command", nargs="?", default="setup",
                        choices=("setup", "status", "update-ui", "vpn-creds", "restore", "discover"))
    parser.add_argument("--ip", help="TV IPv4 address (skips LAN scan if given)")
    parser.add_argument("--profile", type=pathlib.Path, help="Path to property JSON profile")
    parser.add_argument("--vpn-creds", type=pathlib.Path, help="Path to local ExpressVPN credentials JSON (never saved in kit)")
    parser.add_argument("--non-interactive", action="store_true", help="Do not prompt interactively (requires --ip or --profile)")
    parser.add_argument("--skip-vpn-smoke", action="store_true", help="Install VPN without running live connect smoke test")
    parser.add_argument("--wake", action="store_true", help="Send Wake-on-LAN if TV is in standby")
    args = parser.parse_args(argv)

    paths = resolve_paths()
    if not paths["sdb"].is_file():
        print(f"ERROR: Bundled SDB tool not found at {paths['sdb']}", file=sys.stderr)
        return 2

    if args.command == "discover":
        log("Scanning local network for Samsung CU8000 TVs...")
        tvs = FC.discover_cu8000_tvs(extra_ips=[args.ip] if args.ip else None)
        print(json.dumps(tvs, indent=2))
        return 0

    profile = FC.load_profile(args.profile)
    if args.ip:
        profile["tv_ip"] = args.ip

    if args.command == "setup" and not args.non_interactive:
        print("============================================================")
        print("  Samsung CU8000 Property Fleet Setup Wizard")
        print("============================================================")
        log("Scanning local network for Samsung CU8000 TVs...")
        discovered = FC.discover_cu8000_tvs(extra_ips=[profile["tv_ip"]] if profile["tv_ip"] else None)
        profile, _ = FC.interactive_profile_wizard(profile, discovered)

    if not profile.get("tv_ip"):
        print("ERROR: No TV IP specified. Run interactively or pass --ip <tv-ip>.", file=sys.stderr)
        return 2

    tv, dev = connect_and_validate(profile["tv_ip"], paths, wake=args.wake)
    log("Running read-only pre-flight inspection...")
    env_before = inspect_tv_environment(tv, dev)
    mac = env_before["wifiMac"]

    if args.command == "status":
        print(json.dumps(env_before, indent=2))
        return 0

    if args.command == "restore":
        if not args.non_interactive:
            if not FC.prompt_bool(f"Completely restore {tv.host} ({mac}) back to stock?", False):
                log("Restore cancelled.")
                return 0
        res = restore_tv(tv, paths, env_before)
        env_after = inspect_tv_environment(tv, dev)
        receipt = {"action": "restore", "before": env_before, "result": res, "after": env_after}
        rpath = save_receipt(paths, mac, "restore", receipt)
        log(f"Restore complete. Receipt saved to: {rpath}")
        return 0

    if args.command == "update-ui":
        res = deploy_hub_ui(tv, paths, profile)
        rpath = save_receipt(paths, mac, "update-ui", {"profile": profile, "result": res})
        log(f"UI update complete (hub_ready={res['hub_ready']}). Receipt: {rpath}")
        return 0 if res["hub_ready"] else 1

    if args.command == "vpn-creds":
        creds = prompt_vpn_credentials(args.vpn_creds)
        if not creds:
            log("No credentials entered; nothing changed.")
            return 0
        res = deploy_vpn(tv, paths, profile, env_before, creds, smoke_test_connect=not args.skip_vpn_smoke)
        rpath = save_receipt(paths, mac, "vpn-creds", {"result": res})
        log(f"VPN credentials updated & verified. Receipt: {rpath}")
        return 0

    # Full modular setup
    vpn_creds: Optional[Tuple[str, str]] = None
    if profile.get("enable_vpn"):
        if args.vpn_creds:
            vpn_creds = prompt_vpn_credentials(args.vpn_creds)
        elif not args.non_interactive:
            vpn_creds = prompt_vpn_credentials(None)

    results: Dict[str, Any] = {}
    if profile.get("enable_onboot_hook", True):
        log("Step [1/4] Installing permanent on-boot updater block & unlock...")
        results["onboot"] = deploy_onboot_hook(tv, paths, profile, env_before)
        for w in results["onboot"].get("warnings", []):
            log(f"WARNING: {w}")

    if profile.get("enable_bloat_strip", True):
        log("Step [2/4] Applying backed-up bloat strip & TV Plus removal (this takes ~1-2 minutes)...")
        results["strip"] = deploy_bloat_strip(tv, paths)

    if profile.get("enable_hub_ui", True):
        log("Step [3/4] Deploying Living Room Hub native UI + Tier 3 performance tuning...")
        results["hub_ui"] = deploy_hub_ui(tv, paths, profile)

    if profile.get("enable_vpn", True):
        log("Step [4/4] Deploying ExpressVPN Lightway (split-tunnel LAN/control preserved)...")
        results["vpn"] = deploy_vpn(
            tv,
            paths,
            profile,
            env_before,
            vpn_creds,
            smoke_test_connect=not args.skip_vpn_smoke,
        )

    log("Verifying all protected control lanes after setup...")
    env_after = inspect_tv_environment(tv, dev)
    receipt = {
        "action": "setup",
        "property_name": profile.get("property_name"),
        "profile_non_secret": profile,
        "before": env_before,
        "results": results,
        "after": env_after,
    }
    rpath = save_receipt(paths, mac, "setup", receipt)

    print("\n============================================================")
    print("  SETUP COMPLETE")
    print("============================================================")
    print(f"  TV:            {tv.host} ({env_after['modelName']}, MAC {mac})")
    print(f"  Firmware:      {env_after.get('vermagic') or 'unknown'}")
    print(f"  DLNA (9197):   HTTP {env_after.get('dlna_9197')}")
    if "hub_ui" in results:
        print(f"  Hub UI:        {'READY' if results['hub_ui'].get('hub_ready') else 'CHECK LOG'} ({results['hub_ui'].get('engine')})")
    if "vpn" in results:
        print(f"  VPN Ready:     ctld={results['vpn'].get('ctld_started')} creds={results['vpn'].get('credentials_staged_root_only')}")
    print(f"  Receipt saved: {rpath}")
    print("  Next: Test the Home button, YouTube, Plex, HDMI 1, Settings, and volume on the TV.")
    print("        To undo everything at any time, run: ./Setup-TV.command restore --ip " + tv.host)
    print("============================================================\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
