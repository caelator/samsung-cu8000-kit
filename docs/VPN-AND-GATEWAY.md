# ExpressVPN Lightway & Unified Media OS Gateway Guide

Both **ExpressVPN Lightway** and the **Unified Media OS Gateway** are optional modules that integrate directly into `LivingRoomHub`.

---

## Part 1 — ExpressVPN Lightway on CU8000

### How Split-Tunnel Routing Works
- Uses isolated Linux routing table `51820` on interface `dummylwtrial` with `--route-mode noexec --dns-config-mode noexec`.
- Automatically detects the TV's live default gateway, active network interface (`wlan0` or `eth0`), and LAN CIDR on every connect — works on any subnet without hardcoded IPs.
- Keeps local traffic 100% direct (`pref 100..190 -> main`):
  - Developer Mode host (`DEV_HOST`)
  - Optional local Plex server (`PLEX_HOST`)
  - Local LAN CIDR, multicast (`224.0.0.0/4`), SSDP (`239.255.255.250`), broadcast (`255.255.255.255`), and RFC1918 private networks (`10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`)
  - Pinned VPN server endpoint `/32`
- Routes public internet traffic (`pref 500 -> table 51820`) over the tunnel and proves a distinct global exit IP before reporting `connected`.
- Protects every session with:
  - Pre-armed PID-1 dead-man timer (`codex-vpn-deadman.timer`, default 8h; 180s during initial connect)
  - 10-second `VmHWM <= 96 MiB` + default-route watchdog
  - Idempotent `ExecStopPost=teardown.sh` rollback on stop or error

### Preparing Your Location Endpoints
Populate `local-runtime/vpn/endpoints.tsv` (tab-separated) using genuine ExpressVPN Lightway clear-UDP endpoints for your desired locations:

```tsv
54	USA - Miami	US	<ENDPOINT_IP>	<PORT>	<SERVER_CERT_DN>	1500
202	USA - Miami - 2	US	<ENDPOINT_IP>	<PORT>	<SERVER_CERT_DN>	1500
```

See `fleet-kit/tv-payloads/vpn/endpoints.example.tsv` and `locations.example.json` for exact schema examples.

### Providing Credentials Safely
Never commit or embed ExpressVPN credentials in the kit. Supply them in either of two ways:

1. **Interactive prompt during `Setup-TV.command`** (recommended) — entered directly in Terminal, held briefly in RAM during a one-shot TV-IP-restricted transfer into `/opt/usr/codex-vpn-private/client.yaml` (`0600 root:root`), never written to disk on your Mac.
2. **Rotate or add later:**
   ```bash
   ./Setup-TV.command vpn-creds --ip 192.168.1.50
   ```
   Or pass a local unshared JSON file (`{"username": "...", "password": "..."}`) via `--vpn-creds /path/to/local-creds.json`.

---

## Part 2 — Unified Media OS Docker Gateway (`gateway/`)

When deployed on any LAN server, NAS, or Mac running Docker, `gateway/` powers `LivingRoomHub`'s embedded **Unified Home, Movies, TV Shows, Ad-Free YouTube (with SponsorBlock), Kids Hub, Unified Search, and Jellyseerr** views.

### Quick Start
```bash
cd gateway
./setup-gateway.sh
```

The script:
1. Creates a mode-`0600` `.env` file from `.env.example` (prompting for the host's LAN IP and optional Plex URL & token).
2. Builds and starts the `cu8000-media-gateway` Docker container (Python 3.12 + `astcenc` + `ffmpeg` + `nodejs`).
3. Waits for `http://127.0.0.1:8765/v1/health` and prints your `/companion` URL.

### Connecting YouTube & Linking Your TVs
1. Open `http://<GATEWAY_LAN_IP>:8765/companion` in any browser on your LAN to sign into YouTube (OAuth token stored strictly inside `./gateway/data/config/`, mode `0600`) and inspect library health.
2. Configure each CU8000 TV with `gateway_url=http://<GATEWAY_LAN_IP>:8765` (either during `Setup-TV.command` or via `./Setup-TV.command update-ui --profile unit.json`).
3. **Network security:** Keep TCP port `8765` firewalled to your local LAN / Docker subnets only. Never expose the gateway to the public internet.
