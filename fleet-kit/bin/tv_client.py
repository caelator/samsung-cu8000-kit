#!/usr/bin/env python3
"""TV transport client for CU8000 Fleet Kit.

Wraps macOS SDB, authenticated root command execution, and one-shot TV-IP-restricted
HTTP binary transfer (never puts credentials in argv, env, or /tmp).
"""
from __future__ import annotations

import asyncio
import base64
import hashlib
import ipaddress
import os
import pathlib
import re
import secrets
import socket
import subprocess
import sys
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, Tuple


def _find_kit_root() -> pathlib.Path:
    here = pathlib.Path(__file__).resolve()
    # Inside packaged kit: <kit>/bin/tv_client.py -> <kit>
    # Inside source repo: <repo>/fleet-kit/bin/tv_client.py -> <repo>/fleet-kit
    return here.parent.parent


class TvTransport:
    """Managed SDB + authenticated root transport bound to a single verified CU8000 TV."""

    def __init__(
        self,
        host: str,
        model: str,
        *,
        sdb_path: pathlib.Path,
        payloads_dir: pathlib.Path,
        lib_dir: pathlib.Path,
    ) -> None:
        self.host = str(ipaddress.IPv4Address(host))
        self.model = model
        self.sdb_path = sdb_path
        self.payloads_dir = payloads_dir

        os.environ["SDB"] = str(self.sdb_path)
        if str(lib_dir) not in sys.path:
            sys.path.insert(0, str(lib_dir))

        from samsung_tv_root.archive_root import probe  # type: ignore
        from samsung_tv_root.sdb import SdbClient, route_callback_host  # type: ignore

        self._probe = probe
        self._SdbClient = SdbClient
        self.callback = str(ipaddress.IPv4Address(route_callback_host(self.host)))

    def sdb_connect(self, wait: float = 90.0) -> bool:
        """Connect SDB to TV:26101 and wait until it appears as a ready device."""
        started = time.monotonic()
        while True:
            subprocess.run(
                [str(self.sdb_path), "connect", f"{self.host}:26101"],
                capture_output=True,
                text=True,
                timeout=30,
            )
            devices = subprocess.run(
                [str(self.sdb_path), "devices"],
                capture_output=True,
                text=True,
                timeout=15,
            ).stdout
            if re.search(rf"^{re.escape(self.host)}:26101\s+device\s+\S+", devices, re.M):
                return True
            if time.monotonic() - started > wait:
                return False
            time.sleep(5)

    @staticmethod
    def _parse_sdk_output(raw: str) -> Tuple[str, int]:
        m = re.search(r"\n\[exit:(\d+)\]\s*$", raw)
        if not m:
            raise RuntimeError("TV SDK callback carried no completion marker")
        return raw[: m.start()].rstrip(), int(m.group(1))

    def sdk(self, command: str, timeout: float = 60.0) -> Tuple[str, int]:
        client = self._SdbClient(self.sdb_path, self.host, timeout=10)
        client.connect()
        client.require_device()
        raw = client.capture(
            command,
            callback_host=self.callback,
            bind_host=self.callback,
            timeout=timeout,
        ).output
        return self._parse_sdk_output(raw)

    def root(self, command: str, timeout: float = 90.0) -> str:
        """Run a verified root command. Command must not contain '%' or backslashes."""
        if "%" in command or "\\" in command:
            raise ValueError("Root channel strips backslashes and mangles '%'; stage scripts via push_bytes instead")
        result: Dict[str, Any] = asyncio.run(
            self._probe(
                self.host,
                model=self.model,
                callback_host=self.callback,
                bind_host=self.callback,
                sdb_timeout=20,
                accept_timeout=45,
                command_timeout=timeout,
                commands=(command,),
                payloads=self.payloads_dir,
            )
        )
        if not (
            result.get("authenticated")
            and result.get("uid") == 0
            and result.get("euid") == 0
            and result.get("staging_cleaned")
            and result.get("system_files_unchanged")
        ):
            raise RuntimeError("Authenticated root verification failed on the TV")
        entry = result["commands"][0]
        if entry.get("exit_code") != 0:
            raise RuntimeError(
                f"Root command exited {entry.get('exit_code')}: {str(entry.get('stderr'))[:240]}"
            )
        return str(entry.get("stdout", ""))

    def push_bytes(
        self,
        data: bytes,
        remote_path: str,
        *,
        mode: str = "644",
        owner: str = "0:0",
        smack: str = "_",
        umask: str = "022",
        timeout: float = 90.0,
    ) -> str:
        """One-shot TV-IP-restricted HTTP push directly to remote_path with SHA-256 verification."""
        digest = hashlib.sha256(data).hexdigest()
        tv_ip = self.host
        endpoint = "/" + secrets.token_hex(24)
        served = {"count": 0}

        class OneShotHandler(BaseHTTPRequestHandler):
            def do_GET(self):  # noqa: N802
                self.connection.settimeout(20)
                if self.client_address[0] != tv_ip or self.path != endpoint or served["count"] > 0:
                    self.send_error(404)
                    return
                served["count"] += 1
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def log_message(self, *_args):
                pass

        server = ThreadingHTTPServer((self.callback, 0), OneShotHandler)
        server.daemon_threads = True
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            url = f"http://{self.callback}:{server.server_port}{endpoint}"
            parent = str(pathlib.PurePosixPath(remote_path).parent)
            smack_cmd = f"chsmack -a {smack} {remote_path} >/dev/null 2>&1 || true;" if smack else ""
            cmd = (
                f"set -e; umask {umask}; mkdir -p {parent}; "
                f"curl --fail --silent --show-error --noproxy '*' --connect-timeout 5 --max-time 60 "
                f"--output {remote_path} {url}; "
                f"chmod {mode} {remote_path}; chown {owner} {remote_path}; "
                f"{smack_cmd} sha256sum {remote_path}"
            )
            out = self.root(cmd, timeout=timeout)
            last = out.strip().splitlines()[-1] if out.strip() else ""
            if not last.startswith(digest + " "):
                raise RuntimeError(f"SHA-256 mismatch writing {remote_path}: got {last!r}")
            if served["count"] != 1:
                raise RuntimeError(f"Unexpected transfer serve count {served['count']} for {remote_path}")
            return digest
        finally:
            server.shutdown()
            server.server_close()
            thread.join(5)

    def push_file(
        self,
        local_path: pathlib.Path,
        remote_path: str,
        *,
        mode: str = "644",
        owner: str = "0:0",
        smack: str = "_",
        umask: str = "022",
    ) -> str:
        return self.push_bytes(
            local_path.read_bytes(),
            remote_path,
            mode=mode,
            owner=owner,
            smack=smack,
            umask=umask,
        )

    def stage_sdk(self, local_path: pathlib.Path, remote_tmp: str) -> str:
        """Small file copy over SDB SDK lane with SHA-256 verification."""
        data = local_path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        b64 = base64.b64encode(data).decode("ascii")
        out, code = self.sdk(f"printf %s '{b64}' | base64 -d > {remote_tmp} && sha256sum {remote_tmp}")
        if code != 0 or not out.startswith(digest):
            raise RuntimeError(f"Staged copy of {local_path.name} failed SHA-256 check")
        return digest

    def check_dlna_9197(self) -> int:
        try:
            with urllib.request.urlopen(f"http://{self.host}:9197/dmr", timeout=4.0) as resp:
                return int(resp.status)
        except Exception:
            return 0


def wake_on_lan(mac: str, target_ip: str = "255.255.255.255") -> None:
    """Send a Wake-on-LAN magic packet for mac."""
    hex_mac = re.sub(r"[^0-9A-Fa-f]", "", mac)
    if len(hex_mac) != 12:
        return
    packet = b"\xff" * 6 + bytes.fromhex(hex_mac) * 16
    bcast = "255.255.255.255"
    local = ""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            probe.connect((target_ip, 9))
            local = probe.getsockname()[0]
            bcast = re.sub(r"\.\d+$", ".255", local)
    except OSError:
        pass
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try:
            if local:
                sock.bind((local, 0))
            sock.sendto(packet, (bcast, 9))
        except OSError:
            pass
