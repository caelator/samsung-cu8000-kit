# Samsung CU8000 Setup Guide (macOS Operator / Property Manager)

This guide explains how to deploy the CU8000 Fleet Kit onto additional Samsung CU8000 televisions using a Mac.

## What This Setup Does

1. **Permanent On-Boot Updater Block & Runtime Setup** — Masks Samsung firmware/app auto-updaters on every boot via a lightweight `/opt` boot hook. Never writes to boot partitions, read-only system partitions, or DRM areas.
2. **Bloat Strip & TV Plus Removal (~300 MB extra free RAM)** — Backs up and removes Samsung TV Plus, advertising/ACR/telemetry daemons, Bixby/Alexa voice bloat, Game Hub, and SmartThings bloat, and disables Home recommendations & autoplay sound. Keeps **AirPlay, DLNA/AllShare casting, HDMI/CEC, Bluetooth remote, Settings, Plex, and YouTube** intact.
3. **Custom Bare-Metal `LivingRoomHub` Home Screen + Performance Tuning** — Replaces the sluggish stock Smart Hub with a sub-5 MB, 60 fps native Mali GPU launcher (`Plex`, `YouTube`, `HDMI 1`, plus bottom dock for `Inputs`, `VPN`, `Volume/Mute`, `Free RAM`, and `Settings`), customizable per TV via `hub.conf`.
4. **ExpressVPN Lightway (Optional, Split-Tunnel Safe)** — Runs native Lightway on the TV while keeping local LAN, casting, remote control, and optional local Plex traffic direct. Credentials are transferred straight into root-only storage (`0600`) on the TV and are never stored in this kit.
5. **Unified Media OS Gateway (Optional Docker Server)** — Included in `gateway/` for locations that want embedded ad-free YouTube (with SponsorBlock) and unified Plex browsing inside the Hub.

---

## Requirements

- A **Mac** (Apple Silicon or Intel, macOS 12+) connected to the **same Wi-Fi / LAN** as the TV.
  - Uses macOS built-in Python 3 (3.9+ verified) — no Homebrew, Xcode, or pip packages needed when using a packaged distribution zip.
- Physical Samsung TV remote (for the one-time 2-minute Developer Mode step).
- Optional: ExpressVPN Lightway manual credentials (username & password) if enabling VPN.

---

## Step 1 — One-Time TV Preparation (2 Minutes per TV)

1. Find your **Mac's local IP address**:
   - **System Settings → Wi-Fi → Details… → IP Address** (for example `192.168.1.42`).
2. On the TV remote, open **Apps** on Samsung's Home screen.
3. While on the Apps screen, type **`12345`** on the remote (use the `123` button on the SolarCell remote).
4. In the **Developer Mode** popup:
   - Toggle **Developer Mode** to **ON**.
   - Enter your **Mac's IP address** in **Host PC IP**.
   - Press **OK**.
5. **Fully restart the TV:** Hold the **Power** button on the Samsung remote for 3–5 seconds until the Samsung logo appears (a quick tap only sleeps the TV).
6. Wait **2 minutes** after the TV powers back on so its debug service finishes starting.

---

## Step 2 — Run the Setup Wizard on Your Mac

1. Unzip the distribution folder on your Mac.
2. Double-click **`Setup-TV.command`**.
   - *First time on macOS?* If Gatekeeper warns about an unidentified developer, **Right-click → Open → Open** on `Setup-TV.command`.
3. The wizard automatically discovers CU8000 TVs on the LAN and walks through:
   - Selecting the TV
   - Setting the room/unit header label (e.g. `LIVING ROOM`, `SUITE 01`)
   - Selecting the 3rd card label (`HDMI 1`, `Apple TV`, `Cable Box`, etc.)
   - Optionally entering a local Plex server IP and/or Unified Media OS Gateway URL (leave blank to launch stock Plex & YouTube apps)
   - Optionally entering ExpressVPN Lightway credentials (transferred directly to TV root-only `0600` storage; never saved on the Mac)
4. Wait ~2–3 minutes while the installer verifies identity, applies each module, and validates all protected control lanes.
5. A non-secret verification receipt is saved in `receipts/`.

---

## Step 3 — Quick Verification Checklist (On the TV)

- [ ] Press **Home** → Custom Hub appears (`Plex`, `YouTube`, `HDMI 1`).
- [ ] Open **YouTube** and **Plex** → Video and audio play normally.
- [ ] Press **Inputs** (or card 3) → Switches cleanly to HDMI 1; pressing **Home** brings the Hub right back.
- [ ] Press **Vol+ / Vol- / Mute** → Volume indicator updates accurately.
- [ ] Press **Settings** in the dock → Samsung Settings opens normally.
- [ ] (If VPN enabled) Press **VPN** in the dock → Connect / Disconnect works and displays active exit status in the top bar while keeping AirPlay/Cast and LAN direct.

---

## CLI Shortcuts for Multi-TV Rollouts

```bash
# Discover CU8000 TVs on this LAN
./Setup-TV.command discover

# Check status of a TV (read-only)
./Setup-TV.command status --ip 192.168.1.50

# Deploy non-interactively using a per-TV JSON file (copy property.example.json)
./Setup-TV.command setup --profile unit-01.json --non-interactive

# Push a future UI or hub.conf update without re-running bloat strip or VPN
./Setup-TV.command update-ui --profile unit-01.json

# Add or rotate ExpressVPN Lightway credentials later
./Setup-TV.command vpn-creds --ip 192.168.1.50

# Full rollback to stock Samsung behavior
./Setup-TV.command restore --ip 192.168.1.50
```

---

## Troubleshooting

- **"Developer Host IP mismatch"**: Your Mac's IP changed or does not match the TV's setting. Update **Apps → 12345 → Host PC IP** on the TV to your Mac's current IP, then hold Power to reboot the TV.
- **"SDB did not accept connection"**: A short tap on Power only enters standby. Hold Power until the Samsung logo appears, wait 2 minutes, and try again.
- **"TV not found"**: Confirm the TV is powered ON and the Mac is on the same Wi-Fi/VLAN (not an isolated guest network).
- **Unseen firmware build warning**: By design, the installer refuses to load an unverified kernel module on unrecognized firmware builds. Updater masks, bloat strip, and rollback still work cleanly; check the saved receipt in `receipts/` for the TV's kernel `vermagic`.
