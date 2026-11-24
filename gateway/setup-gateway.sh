#!/usr/bin/env bash
# Portable 1-command launcher for the CU8000 Unified Media OS Gateway.
# Works on macOS (Docker Desktop), Linux, or NAS Docker hosts.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$DIR"

if ! command -v docker >/dev/null 2>&1; then
    echo "ERROR: Docker is not installed or not in PATH."
    echo "Install Docker Desktop (macOS) or Docker Engine (Linux/NAS) and re-run."
    exit 1
fi

if [ ! -f .env ]; then
    echo "No .env found — creating one from .env.example."
    cp .env.example .env
    chmod 600 .env
    DEFAULT_IP=$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || hostname -I 2>/dev/null | awk '{print $1}' || echo "127.0.0.1")
    read -r -p "Enter this computer/server's LAN IPv4 address [${DEFAULT_IP}]: " HOST_IP
    HOST_IP="${HOST_IP:-$DEFAULT_IP}"
    read -r -p "Optional Plex server URL (e.g. http://${HOST_IP}:32400, blank to skip): " P_URL
    P_TOK=""
    if [ -n "$P_URL" ]; then
        read -r -s -p "Plex token (X-Plex-Token): " P_TOK
        echo ""
    fi
    python3 - "$HOST_IP" "$P_URL" "$P_TOK" <<'PY'
import pathlib, sys
host_ip, p_url, p_tok = sys.argv[1], sys.argv[2], sys.argv[3]
p = pathlib.Path(".env")
txt = p.read_text()
txt = txt.replace("PUBLIC_BASE_URL=http://CHANGE_ME_TO_HOST_LAN_IP:8765", f"PUBLIC_BASE_URL=http://{host_ip}:8765")
if p_url:
    txt = txt.replace("PLEX_URL=", f"PLEX_URL={p_url}", 1)
    txt = txt.replace("PLEX_PUBLIC_URL=", f"PLEX_PUBLIC_URL={p_url}", 1)
if p_tok:
    txt = txt.replace("PLEX_TOKEN=", f"PLEX_TOKEN={p_tok}", 1)
p.write_text(txt)
PY
    echo "Saved .env (mode 0600)."
fi

mkdir -p data/config data/media
chmod 700 data/config

echo "Building and starting cu8000-media-gateway..."
docker compose up -d --build

PORT=$(awk -F= '$1=="GATEWAY_PORT"{print $2}' .env | tail -1)
PORT="${PORT:-8765}"
PUB_URL=$(awk -F= '$1=="PUBLIC_BASE_URL"{print $2}' .env | tail -1)

echo "Waiting for gateway health check on http://127.0.0.1:${PORT}/v1/health..."
for _ in $(seq 1 25); do
    if curl -fsS "http://127.0.0.1:${PORT}/v1/health" >/dev/null 2>&1; then
        echo ""
        curl -fsS "http://127.0.0.1:${PORT}/v1/health"
        echo ""
        echo "============================================================"
        echo "  Gateway is LIVE!"
        echo "  TV Gateway URL (enter in Setup-TV wizard): ${PUB_URL}"
        echo "  Open Companion UI (YouTube sign-in):       ${PUB_URL}/companion"
        echo "  SECURITY NOTE: Keep TCP port ${PORT} restricted to your LAN."
        echo "============================================================"
        exit 0
    fi
    sleep 1
done

echo "Health check timed out. Recent container logs:" >&2
docker logs --tail 40 cu8000-media-gateway >&2
exit 1
