#!/bin/bash
# Double-clickable macOS launcher for the Samsung CU8000 Property Fleet Kit.
set -euo pipefail

KIT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$KIT_DIR"

# Clear macOS Gatekeeper quarantine on the bundled SDB helper if downloaded from Finder/AirDrop/email
if [ -f "$KIT_DIR/runtime/sdb" ]; then
    xattr -dr com.apple.quarantine "$KIT_DIR/runtime/sdb" >/dev/null 2>&1 || true
    chmod +x "$KIT_DIR/runtime/sdb" >/dev/null 2>&1 || true
fi

# Prefer Python 3.12/3.11 if installed, otherwise macOS built-in /usr/bin/python3 (3.9+ verified)
PY=""
for cand in /opt/homebrew/bin/python3 /usr/local/bin/python3 /usr/bin/python3; do
    if [ -x "$cand" ]; then
        PY="$cand"
        break
    fi
done

if [ -z "$PY" ]; then
    echo "ERROR: Python 3 not found on this Mac."
    read -r -p "Press Enter to close..." _
    exit 1
fi

"$PY" "$KIT_DIR/bin/cu8000_setup.py" "$@"
rc=$?

# Keep Terminal open if double-clicked from Finder (no CLI args)
if [ $# -eq 0 ]; then
    echo ""
    read -r -p "Finished (exit code $rc). Press Enter to close this window..." _
fi
exit $rc
