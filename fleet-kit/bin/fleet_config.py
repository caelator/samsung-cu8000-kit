#!/usr/bin/env python3
"""Property configuration, LAN discovery, and TV config renderers for CU8000 Fleet Kit.

Compatible with macOS built-in Python 3.9+ (zero external dependencies).
Never persists VPN credentials to disk inside the kit or receipts.
"""
from __future__ import annotations

import ipaddress
import json
import os
import pathlib
import re
import socket
import subprocess
import time
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Any, Dict, List, Optional, Tuple


DEFAULT_PROFILE: Dict[str, Any] = {
    "property_name": "Property TV",
    "room_title": "LIVING ROOM",
    "tv_ip": "",
    "tv_mac": "",
    "card_preset": "generic",  # "generic" (HDMI 1) or custom
    "hdmi1_name": "HDMI 1",
    "hdmi1_port": "HDMI 1",
    "hdmi1_source_type": 13,
    "plex_host": "",
    "plex_port": 32400,
    "gateway_url": "",
    "media_os_enabled": False,
    "enable_onboot_hook": True,
    "enable_bloat_strip": True,
    "enable_hub_ui": True,
    "enable_vpn": True,
    "vpn_default_location": "54",
    "vpn_bypass_rfc1918": True,
}

# Verified CU8000 firmware build profiles (fail-closed on unknown builds)
KNOWN_FIRMWARE_PROFILES: Dict[str, Dict[str, str]] = {
    "13432690": {
        "build_family": "T-X22UDCUABC-0090",
        "uep_ko": "uepclear5.ko",
        "uep_restore_ko": "ueprestore5.ko",
        "uep_ko_sha": "0a69a4add6e12a75b4e20156cc6e6b4537488304aaa2759b82ed7b8bf4564c16",
        "uep_log_tag": "codex-uep5",
        "lib_stock_sha": "3484503c53df7bba4c664812ae985eca363faecde97dd36e456beaecc3689457",
        "lib_patched_sha": "dcad0d175a8010dbb0f41a33e66db44baccb461e522d23c9f45d1d5bb78bec71",
    },
    "12344917": {
        "build_family": "T-X22UDCUABC-0080",
        "uep_ko": "uepclear6.ko",
        "uep_restore_ko": "ueprestore6.ko",
        "uep_ko_sha": "",  # populated dynamically from bundled artifact SHA
        "uep_log_tag": "uepclear6",
        "lib_stock_sha": "",
        "lib_patched_sha": "",
    },
}


def query_tv_api(host: str, timeout: float = 2.5) -> Optional[Dict[str, Any]]:
    """Read Samsung Channel API v2 device descriptor on TCP 8001."""
    try:
        ipaddress.IPv4Address(host)
    except ValueError:
        return None
    try:
        with urllib.request.urlopen(f"http://{host}:8001/api/v2/", timeout=timeout) as resp:
            payload = json.loads(resp.read().decode("utf-8", errors="replace"))
            dev = payload.get("device")
            if isinstance(dev, dict):
                dev["_host"] = host
                return dev
    except Exception:
        return None
    return None


def local_ipv4_for_target(target_ip: str = "1.1.1.1") -> str:
    """Return this Mac's primary LAN IPv4 address toward target_ip."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect((target_ip, 80))
            return s.getsockname()[0]
    except OSError:
        return ""


def _arp_candidates() -> List[str]:
    try:
        out = subprocess.run(["arp", "-an"], capture_output=True, text=True, timeout=4).stdout
        return re.findall(r"\((\d+\.\d+\.\d+\.\d+)\)", out)
    except (OSError, subprocess.SubprocessError):
        return []


def _mdns_candidates(seconds: float = 3.0) -> List[str]:
    """Quick macOS dns-sd lookup for Samsung TVs advertising AirPlay or SmartView."""
    hosts: List[str] = []
    try:
        proc = subprocess.Popen(
            ["dns-sd", "-B", "_airplay._tcp", "local."],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            stdin=subprocess.DEVNULL,
        )
        time.sleep(seconds)
        proc.kill()
        browse = proc.communicate()[0]
    except (OSError, subprocess.SubprocessError):
        return hosts

    instances = sorted(set(re.findall(r"_airplay\._tcp\.\s+(.*CU8000.*)$", browse, re.M | re.I)))
    for inst in instances:
        inst = inst.strip()
        try:
            p2 = subprocess.Popen(
                ["dns-sd", "-L", inst, "_airplay._tcp", "local."],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                stdin=subprocess.DEVNULL,
            )
            time.sleep(1.5)
            p2.kill()
            res = p2.communicate()[0]
            m = re.search(r"can be reached at (\S+):\d+", res)
            if not m:
                continue
            p3 = subprocess.Popen(
                ["dns-sd", "-G", "v4", m.group(1)],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                stdin=subprocess.DEVNULL,
            )
            time.sleep(1.5)
            p3.kill()
            ans = p3.communicate()[0]
            hosts.extend(re.findall(r"\b(\d+\.\d+\.\d+\.\d+)\b", "\n".join(ans.splitlines()[4:])))
        except (OSError, subprocess.SubprocessError):
            continue
    return hosts


def discover_cu8000_tvs(
    extra_ips: Optional[List[str]] = None,
    scan_local_subnet: bool = True,
) -> List[Dict[str, Any]]:
    """Discover Samsung CU8000 TVs reachable on port 8001 on the local LAN."""
    candidates: List[str] = []
    if extra_ips:
        candidates.extend([ip.strip() for ip in extra_ips if ip.strip()])
    candidates.extend(_arp_candidates())
    candidates.extend(_mdns_candidates())

    if scan_local_subnet:
        local_ip = local_ipv4_for_target()
        if local_ip:
            parts = local_ip.split(".")
            if len(parts) == 4:
                prefix = ".".join(parts[:3])
                for host_octet in range(1, 255):
                    candidates.append(f"{prefix}.{host_octet}")

    unique_ips = list(dict.fromkeys(candidates))
    found: List[Dict[str, Any]] = []
    seen_macs = set()

    with ThreadPoolExecutor(max_workers=48) as pool:
        futs = {pool.submit(query_tv_api, ip, 1.0): ip for ip in unique_ips}
        for fut in as_completed(futs):
            dev = fut.result()
            if not dev:
                continue
            model = str(dev.get("modelName", ""))
            if "CU8000" not in model.upper():
                continue
            mac = str(dev.get("wifiMac", "")).upper()
            key = mac or dev["_host"]
            if key in seen_macs:
                continue
            seen_macs.add(key)
            found.append(dev)

    found.sort(key=lambda d: tuple(int(x) for x in d["_host"].split(".")))
    return found


def load_profile(path: Optional[pathlib.Path]) -> Dict[str, Any]:
    """Load a property JSON profile merged onto clean defaults."""
    prof = dict(DEFAULT_PROFILE)
    if path and path.is_file():
        raw = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(raw, dict):
            raise ValueError(f"Profile {path} must be a JSON object")
        for k, v in raw.items():
            if k in prof:
                prof[k] = v
    # Auto-derive media_os_enabled if gateway_url is supplied
    if prof.get("gateway_url") and not isinstance(prof.get("media_os_enabled"), bool):
        prof["media_os_enabled"] = True
    return prof


def sanitize_single_line(val: str, max_len: int = 80) -> str:
    cleaned = re.sub(r"[\r\n\t\"'`;$<>|&]", "", str(val or "")).strip()
    return cleaned[:max_len]


def render_hub_conf(profile: Dict[str, Any]) -> str:
    """Render /opt/usr/etc/codex-ui/hub.conf for LivingRoomHub native C engine."""
    room = sanitize_single_line(profile.get("room_title") or "LIVING ROOM", 40).upper()
    plex_host = sanitize_single_line(profile.get("plex_host") or "", 60)
    plex_port = int(profile.get("plex_port") or 32400)
    gw_url = sanitize_single_line(profile.get("gateway_url") or "", 120).rstrip("/")
    media_os = bool(profile.get("media_os_enabled")) and bool(gw_url)
    hdmi1_name = sanitize_single_line(profile.get("hdmi1_name") or "HDMI 1", 40)
    hdmi1_port = sanitize_single_line(profile.get("hdmi1_port") or "HDMI 1", 20)
    hdmi1_st = int(profile.get("hdmi1_source_type") or 13)

    lines = [
        "# Generated by CU8000 Fleet Kit",
        f"room_title={room}",
        f"plex_host={plex_host}",
        f"plex_port={plex_port}",
        f"gateway_url={gw_url}",
        f"media_os_enabled={'1' if media_os else '0'}",
        f"hdmi1_name={hdmi1_name}",
        f"hdmi1_port={hdmi1_port}",
        f"hdmi1_source_type={hdmi1_st}",
        "",
    ]
    return "\n".join(lines)


def render_boot_conf(
    profile: Dict[str, Any],
    *,
    uep_ko: str,
    uep_ko_sha: str,
    uep_log_tag: str,
    lib_stock_sha: str,
    lib_patched_sha: str,
    vpn_hub_smack: str = "System::Privileged",
    vpn_users_gid: int = 100,
) -> str:
    """Render /opt/usr/etc/codex/boot.conf for onboot.sh."""
    lines = [
        "# Generated by CU8000 Fleet Kit",
        f"UEP_KO='{sanitize_single_line(uep_ko, 48)}'",
        f"UEP_KO_SHA='{sanitize_single_line(uep_ko_sha, 64)}'",
        f"UEP_LOG_TAG='{sanitize_single_line(uep_log_tag, 32)}'",
        f"LIB_STOCK_SHA='{sanitize_single_line(lib_stock_sha, 64)}'",
        f"LIB_PATCHED_SHA='{sanitize_single_line(lib_patched_sha, 64)}'",
        f"STRIP_ENABLED='{'1' if profile.get('enable_bloat_strip', True) else '0'}'",
        f"HUB_ENABLED='{'1' if profile.get('enable_hub_ui', True) else '0'}'",
        f"VPN_CTLD_ENABLED='{'1' if profile.get('enable_vpn', True) else '0'}'",
        f"VPN_HUB_SMACK='{sanitize_single_line(vpn_hub_smack, 48)}'",
        f"VPN_USERS_GID='{int(vpn_users_gid)}'",
        "",
    ]
    return "\n".join(lines)


def render_vpn_network_env(
    profile: Dict[str, Any],
    *,
    dev_host: str,
) -> str:
    """Render persistent /opt/usr/etc/codex-vpn/network.env (gateway/iface/LAN auto-detected live)."""
    plex_host = sanitize_single_line(profile.get("plex_host") or "", 60)
    plex_port = int(profile.get("plex_port") or 32400)
    dev_clean = sanitize_single_line(dev_host, 60)
    bypass = "1" if profile.get("vpn_bypass_rfc1918", True) else "0"
    lines = [
        "# Persistent non-secret LAN exclusions for Lightway VPN (CU8000 Fleet Kit)",
        "# GATEWAY, IFACE, and LAN_CIDR are auto-detected live from ip route on each connect.",
        f"DEV_HOST='{dev_clean}'",
        f"PLEX_HOST='{plex_host}'",
        f"PLEX_PORT='{plex_port}'",
        f"BYPASS_RFC1918='{bypass}'",
        "",
    ]
    return "\n".join(lines)


def prompt_str(label: str, default: str = "") -> str:
    suffix = f" [{default}]" if default else ""
    ans = input(f"  {label}{suffix}: ").strip()
    return ans if ans else default


def prompt_bool(label: str, default: bool = True) -> bool:
    hint = "Y/n" if default else "y/N"
    ans = input(f"  {label} [{hint}]: ").strip().lower()
    if not ans:
        return default
    return ans in ("y", "yes", "1", "true")


def interactive_profile_wizard(base: Dict[str, Any], discovered: List[Dict[str, Any]]) -> Tuple[Dict[str, Any], Optional[Dict[str, Any]]]:
    """Friendly plain-English wizard for property managers."""
    prof = dict(base)
    selected_dev: Optional[Dict[str, Any]] = None

    print("\n=== Step 1: Select Samsung CU8000 TV ===")
    if discovered:
        for i, d in enumerate(discovered, 1):
            dev_mode = "ON" if str(d.get("developerMode")) == "1" else "OFF"
            print(
                f"  [{i}] {d.get('name', 'Samsung TV')} — {d['_host']}  "
                f"({d.get('modelName')}, MAC {d.get('wifiMac')}, DevMode={dev_mode}, DevIP={d.get('developerIP') or 'unset'})"
            )
        choice = prompt_str("Pick a TV number or type an IP address", "1")
        if choice.isdigit() and 1 <= int(choice) <= len(discovered):
            selected_dev = discovered[int(choice) - 1]
            prof["tv_ip"] = selected_dev["_host"]
            prof["tv_mac"] = str(selected_dev.get("wifiMac", "")).upper()
        else:
            prof["tv_ip"] = choice
    else:
        print("  No CU8000 TVs auto-discovered on port 8001 (check TV is powered ON and on the same Wi-Fi/LAN).")
        prof["tv_ip"] = prompt_str("Enter the TV's IPv4 address (e.g. 192.168.1.50)", prof.get("tv_ip", ""))

    if prof["tv_ip"] and selected_dev is None:
        selected_dev = query_tv_api(prof["tv_ip"], timeout=4.0)
        if selected_dev:
            prof["tv_mac"] = str(selected_dev.get("wifiMac", "")).upper()

    print("\n=== Step 2: Property & Launcher Details ===")
    prof["property_name"] = prompt_str("Property / unit name (for your log receipt)", prof["property_name"])
    prof["room_title"] = prompt_str("Header title shown on TV top-left", prof["room_title"]).upper()
    prof["enable_onboot_hook"] = prompt_bool("Install permanent on-boot updater block & unlock?", prof["enable_onboot_hook"])
    prof["enable_bloat_strip"] = prompt_bool("Strip ads, telemetry, voice bloat & Samsung TV Plus?", prof["enable_bloat_strip"])
    prof["enable_hub_ui"] = prompt_bool("Install fast custom Living Room Hub home screen?", prof["enable_hub_ui"])

    if prof["enable_hub_ui"]:
        use_generic = prompt_bool("Use generic 'HDMI 1' 3rd card?", prof["card_preset"] == "generic")
        prof["card_preset"] = "generic" if use_generic else "switch2"
        default_hdmi_name = "HDMI 1" if use_generic else "Console"
        prof["hdmi1_name"] = prompt_str("Label for HDMI 1 device (e.g. HDMI 1, Apple TV, Cable Box)", default_hdmi_name)
        prof["plex_host"] = prompt_str("Local Plex server LAN IP (leave blank if no local Plex server)", prof["plex_host"])
        if prof["plex_host"]:
            prof["plex_port"] = int(prompt_str("Plex server port", str(prof["plex_port"])))
        prof["gateway_url"] = prompt_str(
            "Unified Media OS Gateway URL (e.g. http://192.168.1.50:8765, blank to use stock Plex/YouTube apps)",
            prof["gateway_url"],
        )
        prof["media_os_enabled"] = bool(prof["gateway_url"])

    print("\n=== Step 3: ExpressVPN Lightway ===")
    prof["enable_vpn"] = prompt_bool("Install ExpressVPN Lightway on this TV?", prof["enable_vpn"])
    return prof, selected_dev
