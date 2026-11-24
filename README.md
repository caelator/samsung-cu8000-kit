# Samsung CU8000 TV Optimization & Fleet Kit

A modular, reversible, open-source toolkit for **Samsung CU8000 (Tizen 9 / ARMv7 / Mali-G52)** televisions that replaces heavy Smart Hub bloat with:

1. **Permanent On-Boot Updater Block & Runtime Unlock** — Disables firmware/app auto-updaters on every boot via a lightweight `/opt`-only boot hook. **Zero boot-partition writes, zero read-only rootfs edits, zero DRM tampering.**
2. **Backed-Up Bloat Strip & TV Plus Removal (~300 MiB extra free RAM)** — Backs up and removes Samsung TV Plus, advertising/ACR/telemetry daemons, Bixby/Alexa voice bloat, Game Hub, and SmartThings bloat, and disables Home recommendations & autoplay sound while keeping **AirPlay, DLNA/AllShare casting, HDMI/CEC, Bluetooth remote, Settings, Plex, and YouTube** intact.
3. **Bare-Metal C `LivingRoomHub` Home Screen (~3.5–4.8 MiB PSS, 60 fps, 0% idle CPU)** — A native ARMv7 C Wayland + Mali-G52 GLES2 + hardware ASTC launcher (`Plex`, `YouTube`, `HDMI 1`, plus bottom dock for `Inputs`, `VPN`, `Volume/Mute`, `Free RAM`, and `Settings`) with runtime `hub.conf` per-TV customization, instant `XF86Home` wake (`< 10 ms` from YouTube/Settings), and a built-in Dynamic Button Mapping Service.
4. **Unified Media OS + Optional Docker Gateway** — Embedded native hardware video playback (`libcapi-media-player` + transparent GLES2 HUD + FreeType SRT subtitles) paired with a LAN-only Docker gateway serving **Plex** (Direct Play + eARC EAC3 5.1 audio remux), **Ad-Free YouTube** (InnerTube + SponsorBlock), Kids Hub, Unified Search, and Jellyseerr integration — or seamlessly falls back to stock Tizen Plex & YouTube apps when no gateway is configured.
5. **Split-Tunnel ExpressVPN Lightway on TV (~7.5 MiB HWM)** — Native Lightway tunnel in an isolated routing table (`table 51820`) that keeps your main default route, `/etc/resolv.conf`, local LAN, RFC1918 subnets, AirPlay/DLNA casting, remote control, and optional local Plex traffic 100% direct. Controlled from the TV UI via a strict `SO_PEERCRED`-authenticated Unix socket (`codex-vpn-ctld`) with root-only `0600` credentials and an automatic PID-1 dead-man rollback timer.
6. **Multi-TV Fleet Setup Wizard & macOS Packager** — Guided macOS CLI & double-clickable `Setup-TV.command` launcher with LAN auto-discovery, per-TV JSON profiles, automated deployment receipts, and full 1-command rollback (`restore`).

---

## Measured On-Device Performance (CU8000 / Tizen 9 / 1.5 GiB RAM)

| Metric | Stock Smart Hub / WebView | Bare-Metal C Hub + Bloat Strip |
| --- | --- | --- |
| Available RAM after boot | ~430 MiB | **~700–730 MiB (+300 MiB)** |
| Launcher memory (PSS) | ~203 MiB (3 processes) | **3.3–4.8 MiB (1 process, 0 GC)** |
| Idle CPU usage | ~7.3% | **0.00% (`epoll` infinite sleep)** |
| D-pad key-to-GPU swap | ~1,500 ms | **~2.0–3.5 ms (11 draw calls)** |
| `HOME` wake from YouTube / Settings | ~1,000+ ms | **8–11 ms** |
| Lightway VPN memory (cgroup HWM) | — | **~7.5 MiB (96 MiB watchdog cap)** |

---

## Repository Layout

```text
samsung-cu8000-kit/
├── README.md
├── LICENSE
├── docs/
│   ├── ARCHITECTURE.md            # Deep technical design of all 5 layers
│   ├── MANAGER-SETUP-GUIDE.md     # Plain-English macOS operator / fleet guide
│   ├── BUILDING-FROM-SOURCE.md    # Cross-compiling ARMv7 binaries, ASTC assets, & bundling a zip
│   ├── SAFETY-AND-ROLLBACK.md     # Protected lanes, fail-closed build checks, full restore
│   └── VPN-AND-GATEWAY.md         # ExpressVPN Lightway & Unified Media OS Docker gateway setup
├── fleet-kit/                     # Multi-TV discovery, setup wizard, packager, & TV shell payloads
│   ├── Setup-TV.command           # Double-clickable macOS launcher
│   ├── property.example.json      # Per-TV / per-property config template
│   ├── build_package.py           # Reproducible macOS zip builder with privacy & integrity gates
│   ├── test_fleet_kit.py          # Offline test suite
│   ├── bin/                       # cu8000_setup.py, fleet_config.py, tv_client.py
│   └── tv-payloads/               # onboot/, strip/, vpn/
├── tv-hub/                        # Bare-metal ARMv7 C Mali-G52 GLES2 + ASTC launcher & Media OS
│   ├── native-c/                  # Wayland/EGL/GLES2/FreeType core + hub.conf + keybinds HTTP API
│   ├── media-c/                   # Embedded Plex/YouTube/Kids/Search views + hardware player
│   ├── assets/                    # Generic Mali ASTC textures (Plex, YouTube, HDMI 1, dock, icons)
│   ├── bake_assets.swift          # CoreGraphics artwork baker
│   ├── start-hub.sh               # Transient systemd unit launcher
│   ├── tune-tv.sh                 # Reversible Tier 3 VM & unused Home companion tuning
│   └── tv_keybinds.py             # Remote button remapping CLI
├── vpn-controller/
│   └── codex-vpn-ctld.c           # Pure-C Unix socket allowlist controller (SO_PEERCRED, zero HTTP)
├── kernel-uep/
│   ├── build_uep_module.py        # Builds local anchor-gated .ko from your own TV's arc4.ko skeleton
│   ├── uepwrite5.S                # Anchor-gated RAM-only template (build 0090)
│   └── uepclear6.S                # Anchor-gated RAM-only template (build 0080)
└── gateway/                       # Standalone Docker Unified Media OS Gateway (Plex + Ad-Free YouTube)
    ├── Dockerfile
    ├── docker-compose.yml
    ├── .env.example
    ├── setup-gateway.sh
    ├── app/
    └── tests/
```

---

## Quick Start

> **Important:** This public repository contains **original source code, scripts, generic ASTC assets, and documentation only**. It does **not** redistribute Samsung proprietary binaries (`sdb`, `libkUEPUser.so`, or compiled `.ko` modules), third-party executables, or any credentials.

1. **Build binaries & gather local dependencies** (one time, ~5 minutes):
   Follow [docs/BUILDING-FROM-SOURCE.md](docs/BUILDING-FROM-SOURCE.md) to:
   - Install Samsung's `sdb` (from Tizen Studio) and [samsung-tv-root](https://github.com/chris-ritsen/samsung-tv-root) into `./local-runtime/`
   - Cross-compile `tv-hub/` (`living-room-hub-native`) and `vpn-controller/` (`codex-vpn-ctld`) with `zig cc`
   - Optionally build `lightway-client` (from ExpressVPN's open-source `lightway-core`) and your build-matched anchor-gated `.ko`
2. **Build a self-contained macOS distribution zip for technicians / property managers:**
   ```bash
   python3 fleet-kit/build_package.py --runtime-dir ./local-runtime --version v1.0.0
   ```
   This verifies zero leaked credentials/private files, checks every shell script with `sh -n`/`bash -n`, validates macOS Python 3.9+ imports, audits ARMv7 ELF flags (`0x05000200` softfp), generates `manifest.json` + `SHA256SUMS.txt`, and outputs `dist/cu8000-property-kit-v1.0.0.zip`.
3. **Provision each CU8000 TV:**
   Follow [docs/MANAGER-SETUP-GUIDE.md](docs/MANAGER-SETUP-GUIDE.md) — enable Developer Mode on the TV (`Apps → 12345`), double-click `Setup-TV.command`, and follow the prompts.
4. **Optional — Deploy the Unified Media OS Gateway (for Ad-Free YouTube + Embedded Plex):**
   ```bash
   cd gateway
   ./setup-gateway.sh
   ```
   See [docs/VPN-AND-GATEWAY.md](docs/VPN-AND-GATEWAY.md).

---

## Safety & Firmware Compatibility

Read [docs/SAFETY-AND-ROLLBACK.md](docs/SAFETY-AND-ROLLBACK.md) before touching any live television:

- **Never flashes boot areas or touches DRM:** Every persistent change lives on writable `/opt` with backups preserved under `/opt/usr/etc/codex-strip-backup/` and `hub-init.sh.samsung`.
- **Fail-closed firmware gating:** Kernel UEP modules validate both kernel `vermagic` and two 16-byte ARM code anchors in RAM before writing a single word, and never stay resident (`-ENODEV`). Unseen firmware builds are skipped safely without bricking risk.
- **Instant full rollback:** Run `./Setup-TV.command restore --ip <TV_IP>` at any time to stop services, restore backed-up packages/sentinels/settings, unmask units, and restore Samsung's original `hub-init.sh`.

---

## Credits & Third-Party Notices

- **[samsung-tv-root](https://github.com/chris-ritsen/samsung-tv-root)** (Unlicense) — authenticated SDB/archive-root research tooling for Samsung Tizen TVs.
- **[ExpressVPN lightway-core](https://github.com/expressvpn/lightway-core)** (GPLv2) — open-source Lightway VPN core (built separately by the user).
- **[SponsorBlock](https://sponsor.ajay.app/)** — optional community segment skipping in the gateway.
- **Tizen Studio / `sdb`** — Samsung's official Smart Development Bridge (obtained separately from Samsung/Tizen).
- **Disclaimer:** Provided as-is for educational and personal device ownership purposes on hardware you own. Use responsibly and verify firmware compatibility before modifying any TV.
