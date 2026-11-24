#!/bin/sh
# Living Room Hub action (UID 5001): connect/switch to USA - Miami - 2 (202).
exec /opt/usr/etc/codex-vpn/bin/codex-vpn-ctld --send "SWITCH 202" --timeout 90
