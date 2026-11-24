#!/bin/sh
# Living Room Hub action (UID 5001): cleanly disconnect Lightway VPN and restore routes.
exec /opt/usr/etc/codex-vpn/bin/codex-vpn-ctld --send "DISCONNECT" --timeout 30
