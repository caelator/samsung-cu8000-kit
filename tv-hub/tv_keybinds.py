#!/usr/bin/env python3
"""Dynamic Button Mapping Service CLI for Living Room Hub (Tier 4).

Controls the live tizen_keyrouter button mapping service running inside
living-room-hub-native on 127.0.0.1:18080 over SDB — zero extra TV daemons.

Examples:
  tv_keybinds.py --host 192.168.1.50 list
  tv_keybinds.py --host 192.168.1.50 learn 15
  tv_keybinds.py --host 192.168.1.50 set --evdev 240 --action launch_plex --label "Quick Plex"
  tv_keybinds.py --host 192.168.1.50 set --xkb 249 --action source --arg 13 --label "HDMI 1"
  tv_keybinds.py --host 192.168.1.50 del --xkb 249
  tv_keybinds.py --host 192.168.1.50 clear
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import pathlib
import shutil
import sys
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "fleet-kit" / "bin"))

import fleet_config as FC  # noqa: E402
from tv_client import TvTransport  # noqa: E402


def tv_http(tv: TvTransport, method: str, path: str, payload=None):
    if payload is None:
        out, rc = tv.sdk(f"curl -sS --max-time 5 -X {method} http://127.0.0.1:18080{path}")
    else:
        b64 = base64.b64encode(json.dumps(payload, separators=(",", ":")).encode()).decode()
        out, rc = tv.sdk(
            f"echo {b64} | base64 -d > /tmp/kb-req.json && "
            f"curl -sS --max-time 5 -X {method} --data-binary @/tmp/kb-req.json http://127.0.0.1:18080{path}; "
            f"rm -f /tmp/kb-req.json"
        )
    if rc != 0:
        raise RuntimeError(f"curl failed (rc={rc}): {out}")
    try:
        return json.loads(out)
    except Exception:
        return {"raw": out}


def main() -> int:
    ap = argparse.ArgumentParser(description="Living Room Hub dynamic button mapping CLI")
    ap.add_argument("--host", required=True, help="Target Samsung CU8000 TV IPv4 address")
    ap.add_argument("--sdb", type=pathlib.Path, default=None, help="Path to sdb executable (default: SDB env or PATH)")
    ap.add_argument("--lib-dir", type=pathlib.Path, default=None, help="Path to samsung-tv-root/src directory")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="Show active button mappings and last captured key")
    sub.add_parser("clear", help="Remove all custom button mappings (HOME remains active)")
    sub.add_parser("reload", help="Reload /opt/usr/etc/codex-ui/keybinds.conf and re-grab")

    p_learn = sub.add_parser("learn", help="Enter live learn mode and wait for a remote press")
    p_learn.add_argument("seconds", nargs="?", type=int, default=15)

    p_set = sub.add_parser("set", help="Dynamically bind a remote button to an action/app/source/script")
    p_set.add_argument("--xkb", type=int, default=0, help="XKB keycode (evdev + 8)")
    p_set.add_argument("--evdev", type=int, default=0, help="Linux evdev keycode")
    p_set.add_argument("--action", required=True,
                       help="launch_plex|launch_youtube|switch_switch2|show_hub|open_inputs|open_settings|"
                            "open_samsung_home|clean_background|volume_up|volume_down|toggle_mute|app|source|script|service_toggle")
    p_set.add_argument("--arg", default="", help="Optional argument (app_id, sourceType, script name, unit)")
    p_set.add_argument("--label", default="", help="Human-readable description")
    p_set.add_argument("--force", action="store_true", help="Override soft-protected navigation/volume keys")

    p_del = sub.add_parser("del", help="Remove one button mapping")
    p_del.add_argument("--xkb", type=int, default=0)
    p_del.add_argument("--evdev", type=int, default=0)

    args = ap.parse_args()
    sdb = args.sdb or (pathlib.Path(os.environ["SDB"]) if os.environ.get("SDB") else None) or (
        pathlib.Path(shutil.which("sdb")) if shutil.which("sdb") else REPO_ROOT / "runtime" / "sdb"
    )
    lib_dir = args.lib_dir or (REPO_ROOT / "runtime" / "python-lib")
    dev = FC.query_tv_api(args.host, timeout=4.0) or {}
    tv = TvTransport(
        args.host,
        str(dev.get("modelName", "CU8000")),
        sdb_path=sdb,
        payloads_dir=REPO_ROOT / "runtime" / "root-payloads",
        lib_dir=lib_dir,
    )
    if not tv.sdb_connect(wait=30.0):
        sys.exit(f"Could not connect SDB to {args.host}:26101")

    if args.cmd == "list":
        print(json.dumps(tv_http(tv, "GET", "/api/keybinds"), indent=2))
    elif args.cmd == "clear":
        print(json.dumps(tv_http(tv, "POST", "/api/keybinds", {"op": "clear"}), indent=2))
    elif args.cmd == "reload":
        print(json.dumps(tv_http(tv, "POST", "/api/keybinds", {"op": "reload"}), indent=2))
    elif args.cmd == "set":
        payload = {"op": "set", "xkb": args.xkb, "evdev": args.evdev,
                   "action": args.action, "arg": args.arg, "label": args.label}
        if args.force:
            payload["force"] = 1
        print(json.dumps(tv_http(tv, "POST", "/api/keybinds", payload), indent=2))
    elif args.cmd == "del":
        print(json.dumps(tv_http(tv, "POST", "/api/keybinds", {"op": "del", "xkb": args.xkb, "evdev": args.evdev}), indent=2))
    elif args.cmd == "learn":
        tv_http(tv, "POST", "/api/keybinds", {"op": "learn", "seconds": args.seconds})
        print(f"Learn mode active for {args.seconds}s — press the remote button you want to capture...")
        deadline = time.time() + args.seconds + 1
        while time.time() < deadline:
            time.sleep(0.5)
            st = tv_http(tv, "GET", "/api/keybinds")
            if st.get("lastLearnedXkb", 0) > 0:
                print(json.dumps({
                    "captured": True,
                    "evdev": st["lastLearnedEvdev"],
                    "xkb": st["lastLearnedXkb"],
                    "exampleBind": f"tv_keybinds.py --host {args.host} set --xkb {st['lastLearnedXkb']} --action launch_plex --label Custom",
                }, indent=2))
                return 0
            if not st.get("learning"):
                break
        print(json.dumps({"captured": False, "note": "No button pressed in window."}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
