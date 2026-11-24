#!/bin/sh
# Living Room Hub action (UID 5001): toggle Lightway VPN on/off via allowlist socket.
exec /opt/usr/etc/codex-vpn/bin/codex-vpn-ctld --send "TOGGLE" --timeout 90
