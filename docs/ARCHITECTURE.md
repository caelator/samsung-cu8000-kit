# Technical Architecture

Samsung CU8000 TVs run **Tizen 9.0 (Linux 5.4.77, ARMv7 EABI5 softfp, Mali-G52 GPU)** with ~1.5 GiB RAM visible to Linux, a read-only `vdfs` root filesystem, and a writable `/opt` partition (~1.6 GiB free).

Every component in this kit operates exclusively in **RAM and writable `/opt`**. No boot partitions, kernel flash blocks, read-only system files, or DRM/provisioning partitions are ever modified.

```text
+-------------------------------------------------------------------+
| Samsung CU8000 Boot (iot-hub-control-setup -> hub-init.sh awk)    |
+-------------------------------------------------------------------+
        |  (async, zero boot delay)
        v
/opt/usr/etc/codex/onboot.sh  (+ per-TV boot.conf)
  |-- 1. Runtime-mask 14 updater units + 38 bloat/telemetry units
  |-- 2. Optional anchor-gated RAM-only UEP module (build-verified)
  |-- 3. Optional RAM-only bind mount for unsigned shell scripts
  |-- 4. Detached 120s post-boot Samsung TV Plus re-fetch sweep
  |-- 5. Start codex-vpn-ctld.service (/run/codex-vpn/ctl.sock)
  +-- 6. Launch LivingRoomHub once Wayland socket is ready (14s)
            |
            v
  codex-livingroom-hub.service (UID 5001, Nice=-5, OOMScoreAdjust=-300)
  ├── Tier 4 Bare-Metal ARMv7 C Engine (Wayland + Mali-G52 GLES2 + ASTC)
  │     ├── EGL_KHR_partial_update dirty-rect rendering (~2-3 ms swap)
  │     ├── FreeType UTF-8 glyph atlas + hardware ASTC texture pool
  │     ├── XF86Home OverrideExclusive grab via tizen_keyrouter
  │     ├── Dynamic Button Mapping HTTP API (127.0.0.1:18080)
  │     ├── Auto Smart Memory Guard (low-RAM + HDMI background trims)
  │     └── Embedded Unified Media OS (optional LAN Docker Gateway)
  │           ├── Plex Direct Play & eARC EAC3 5.1 Remux
  │           ├── Ad-Free YouTube (InnerTube + SponsorBlock)
  │           └── Zero-Ecore libcapi-media-player + RGBA8888 GLES2 HUD
  └── Tier 3 Reversible Tuning (/opt/usr/etc/codex-ui/tune-tv.sh)
        ├── vm.swappiness=40, vm.vfs_cache_pressure=75
        └── SIGSTOP 5 unused stock-Home companions (auto-SIGCONT on fallback)
```

---

## Layer 1 — On-Boot Hook & Firmware Build Gating

- **Carrier:** Samsung's stock `iot-hub-control-setup` boot services execute `/opt/usr/apps/com.samsung.tv.iot-hub-control/bin/hub-init.sh` ~13 seconds after power-on.
- **Awk entry point (`fleet-kit/tv-payloads/onboot/hub-init.awk`):** Because Samsung's shell rejects unsigned script files prior to RAM relaxation, the entry point is a 13-line `awk` script that launches `systemd-run --no-block --unit=codex-onboot` and immediately executes Samsung's untouched original script (`hub-init.sh.samsung`).
- **Fail-closed kernel modules (`kernel-uep/`):**
  - Compiled from clean ARM32 assembly (`uepwrite5.S` for build `0090`, `uepclear6.S` for build `0080`) using the owner's own TV's `/lib/modules/.../arc4.ko` vermagic skeleton via `build_uep_module.py`.
  - Before touching RAM, `init_module` validates **two 16-byte ARM instruction anchors** in kernel memory. On any mismatch, it logs an abort and returns `-ENODEV` without writing a single byte. Even on success, it returns `-ENODEV` so nothing stays resident in kernel memory.
- **Per-TV `boot.conf`:** Written by `cu8000_setup.py` based on observed `vermagic` and file hashes. Unseen firmware builds set `UEP_KO=''` and safely skip kernel loading.

---

## Layer 2 — Reversible Bloat Strip & Visual Cleanup

Implemented idempotently in `fleet-kit/tv-payloads/strip/apply-strip.sh` with full restoration in `restore-strip.sh`:

1. **12 Samsung `/opt` sentinels** under `/opt/usr/share/systemd/system/` (`ConditionPathExists=!` off-switches built into Samsung's own unit files).
2. **25 backed-up, uninstalled, and path-blocked packages:** Samsung TV Plus (`HEPsqFNie0`, `com.samsung.tv.tvplus`, `tvPlusDeeplinker`), ad/ACR services, Bixby/Alexa/TTS voice stack, Game Hub, SmartThings, and MultiControl. Each package directory is archived to `/opt/usr/etc/codex-strip-backup/<pkg>.tar` before `pkgcmd -u` and blocked with a zero-byte `chmod 444` file to stop `pkg_recovery` from reinstalling it.
3. **120-second post-boot TV Plus sweep:** Handles `webappservice` re-fetching `HEPsqFNie0` / `tvPlusDeeplinker` ~60–90 seconds after boot.
4. **Buxton2 settings & Home cache cleanup:** Disables Home recommendations, autoplay sound, Smart Hub auto-update, TV Plus autorun, and Bixby updates while preserving existing non-TV-Plus apps in `db/eden/launcher/list`.

---

## Layer 3 — Bare-Metal ARMv7 C Launcher (`tv-hub/`)

- **Engine:** Pure C11 targeting `arm-linux-gnueabi.2.28` (`cortex_a53+neon`), dynamically loading Tizen's native Wayland, EGL, GLES2, FreeType, AppControl, SourceList, HDMI-CEC, and TV Sound Manager libraries (`ffi_loader.c`) so zero proprietary SDK headers are needed to compile.
- **Graphics efficiency:**
  - Zero depth buffer, zero stencil buffer, zero MSAA.
  - Pre-baked Mali hardware ASTC 4x4/6x6 textures (`0x5ca1ab13`) — 0 ms CPU image decode, ~2.2 MiB VRAM.
  - `EGL_KHR_partial_update` (`eglSetDamageRegionKHR` + `eglSwapBuffersWithDamageKHR`) for sub-3 ms frame swaps.
  - Infinite `epoll_wait` timeout when idle = **0.00% idle CPU**.
- **Runtime configuration (`/opt/usr/etc/codex-ui/hub.conf`):**
  ```ini
  room_title=LIVING ROOM
  plex_host=192.168.1.50
  plex_port=32400
  gateway_url=http://192.168.1.50:8765
  media_os_enabled=1
  hdmi1_name=HDMI 1
  hdmi1_port=HDMI 1
  hdmi1_source_type=13
  ```
  When `gateway_url` is blank (`media_os_enabled=0`), selecting Plex or YouTube launches the stock Tizen apps directly.
- **Dynamic Button Mapping Service:** Local HTTP API (`127.0.0.1:18080/api/keybinds`) + `/opt/usr/etc/codex-ui/keybinds.conf` + `tv-hub/tv_keybinds.py` for live remote-button learning and remapping via `tizen_keyrouter` (blocking `KEY_POWER` from accidental override).

---

## Layer 4 — Unified Media OS & Docker Gateway (`gateway/`)

When a local Docker host runs `gateway/`, `LivingRoomHub` embeds full native views for **Unified Home, Movies, TV Shows, Signed-In YouTube, Kids Hub, Unified Search, Detail Sheets, and Fullscreen Hardware Video Playback**:

- Video decodes in hardware via `libcapi-media-player.so.0` on a native Tizen overlay plane behind an `EGL_ALPHA_SIZE=8` transparent GLES2 HUD + FreeType SRT subtitles.
- The Docker gateway (`Python 3.12 + astcenc + ffmpeg + nodejs`) pre-bakes poster/thumbnail artwork into Mali `.astc` files on disk, proxies Plex media range requests so Plex tokens never reach the TV, remuxes DTS audio to EAC3 5.1 for eARC AVRs, and handles YouTube InnerTube + SponsorBlock segment fetching.
- All dynamic ASTC textures flush back to `0` VRAM on Hub hide or HDMI switch.

---

## Layer 5 — Split-Tunnel ExpressVPN Lightway (`vpn-controller/`, `fleet-kit/tv-payloads/vpn/`)

- **Privilege separation:**
  - `LivingRoomHub` runs as unprivileged UID `5001` (`owner`) and can only read `/run/codex-vpn/status.json` (`0644`, Smack `_`) and talk to `/run/codex-vpn/ctl.sock` (`0660 root:users`).
  - `codex-vpn-ctld` (pure C, `SO_PEERCRED` UID `0`/`5001` check, zero HTTP) enforces a strict 7-verb allowlist (`STATUS`, `SELECT`, `SELECT_NEXT`, `CONNECT`, `DISCONNECT`, `SWITCH`, `TOGGLE`) and delegates to root-owned `ctl.sh`.
  - Credentials live solely at `/opt/usr/codex-vpn-private/client.yaml` (`0700`/`0600 root:root`, Smack `System`), verified unreadable by UID `5001` and UID `901`.
- **Network invariants (`wrapper.sh`, `ctl.sh`, `teardown.sh`):**
  - Launches `lightway-client` with `--route-mode noexec --dns-config-mode noexec`.
  - Auto-detects live gateway, interface (`wlan0`/`eth0`), and LAN CIDR on every connect.
  - Installs policy rules (`pref 100..190 -> main` for developer host, optional Plex host, LAN CIDR, VPN endpoint `/32`, multicast/SSDP/broadcast, and RFC1918) **before** `pref 500 -> table 51820` (`dummylwtrial`).
  - Never modifies the main default route or `/etc/resolv.conf`.
  - Proves distinct global exit IP (`1.1.1.1/cdn-cgi/trace`) before reporting `connected`, strictly separating `selectedLocation` vs `authenticatedLocation` vs `activeLocation`.
  - Guards every session with a 10-second `VmHWM <= 96 MiB` + route watchdog, a pre-armed PID-1 dead-man timer (`codex-vpn-deadman.timer`), and idempotent `ExecStopPost=teardown.sh`.
