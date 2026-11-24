# Safety Model, Firmware Compatibility & Rollback

Modifying a smart TV carries real risk if done carelessly. Every tool in this kit is built around strict safety invariants designed to keep your TV recoverable at all times.

---

## Non-Negotiable Safety Invariants

1. **Zero boot-area writes:** Never flash `uImage`, kernel partitions (`mmcblk0p5`/`p6`), NOR flash, boot slots, or bootloader environment variables. Samsung CU8000 kernel partitions carry appended verification blocks; patching them on disk can permanently brick the television.
2. **Zero read-only rootfs edits:** Everything installed by this kit lives in RAM (`/run`, `/tmp`, bind mounts) or on the writable `/opt` partition (`/opt/usr/etc/codex*`, `/opt/usr/share/systemd/system/*`).
3. **Zero DRM / provisioning / calibration access:** Partition `p11`, `/mnt/drm`, `unified-drm-*`, `nagsam-daemon`, `dseal`, and `qse-daemon` are strictly untouched.
4. **Protected control lanes preserved:** Before and after setup, the tool checks continuous health of:
   - Channel API v2 (`TCP 8001`) & paired WebSocket (`TCP 8002`)
   - DLNA renderer (`TCP 9197`) & AirPlay / AllShare casting
   - Developer Mode / SDB (`TCP 26101`)
   - Bluetooth physical remote (`bt-service`, `remote-server`)
   - HDMI / picture / audio / CEC & Samsung Settings (`org.tizen.menu`)
   - Samsung's original `hub-init.sh.samsung` and `com.samsung.tv.csfs` Home fallback
5. **No automatic reboots:** The setup CLI never reboots or powers off the TV automatically.
6. **Zero credentials in repo, UI, logs, or receipts:** ExpressVPN credentials (if used) are sent in memory via a one-shot TV-IP-restricted HTTP listener straight into `/opt/usr/codex-vpn-private/client.yaml` (`0700`/`0600 root:root`, Smack `System`) and verified unreadable by both UID `5001` (`owner`) and UID `901` (`developer`).

---

## Firmware Build Gating (Fail-Closed)

The CU8000 family ships multiple firmware builds over time (`T-X22UDCUABC-0080`, `0090`, and newer):

- **Verified builds (`0090` / vermagic `13432690` and `0080` / vermagic `12344917`):** `cu8000_setup.py` inspects the running TV's kernel `vermagic` and `/usr/lib/libkUEPUser.so` SHA-256 before staging any boot hook artifacts. Anchor-gated `.ko` modules verify two 16-byte ARM instruction fingerprints in kernel memory before modifying RAM.
- **Unseen firmware builds:** The installer **refuses** to load any unverified `.ko` or library overlay. It safely skips the kernel module (`UEP_KO=''` in `boot.conf`), still applies the firmware-independent updater runtime masks, bloat strip, and `.NET` fallback launcher, and records the observed `vermagic` in `receipts/` for offline review.

---

## SDB & Root Transport Quirks (Developer Reference)

- **Never put `%` or `\` in root command strings:** Samsung's archive-root callback channel strips backslashes and mangles `%`. `tv_client.py` rejects commands containing either character; stage scripts via `push_bytes()` instead.
- **Mount namespace isolation:** Commands run in the root callback shell have an isolated mount namespace. System-wide mounts (or unmounts) must execute via `systemd-run --wait` in PID 1's namespace.
- **Avoid oversized single-shot SDB commands:** Always transfer binaries via the one-shot local HTTP transfer (`push_bytes()`), never huge base64 strings over `sdb`, which can wedge `sdbd` until power-cycled.

---

## Instant Rollback & Complete Restore

### Quick Per-Feature Disable (On TV, No Mac Needed via SDB)
- **Disable Hub UI autostart:** `touch /opt/usr/etc/codex-ui/disabled && systemctl stop codex-livingroom-hub`
- **Disable Tier 3 tuning & resume frozen companions:**
  `touch /opt/usr/etc/codex-ui/no-tune && kill -CONT $(cat /run/user/5001/codex-frozen-companions.pids)`
- **Disconnect VPN immediately:** `/opt/usr/etc/codex-vpn/ctl.sh disconnect`

### Full 1-Command Restore to Stock (From Mac)
```bash
./Setup-TV.command restore --ip <TV_IP>
```

This cleanly:
1. Stops and removes the Lightway VPN tunnel, `codex-vpn-ctld`, dead-man timer, scripts, and `/opt/usr/codex-vpn-private/`.
2. Stops `codex-livingroom-hub`, resumes any frozen stock-Home companions, resets `vm.swappiness=100`, and removes `/opt/usr/etc/codex-ui/`.
3. Removes all 12 `/opt` systemd sentinels, removes zero-byte package path blockers, restores all backed-up packages from `/opt/usr/etc/codex-strip-backup/`, and resets Buxton visual/auto-update settings.
4. Unmounts any in-memory `libkUEPUser.so` cover, restores kernel UEP enforcement in RAM (if a matching restore `.ko` is present), unmasks updater and bloat units, restores Samsung's original `hub-init.sh.samsung` back to `hub-init.sh`, and removes `/opt/usr/etc/codex/`.
